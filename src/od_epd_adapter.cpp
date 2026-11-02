/* od_epd_adapter -- see od_epd_adapter.h.
 *
 * This file is the join between "where the config is stored" and "how a panel
 * is driven". od_epd knows nothing about NVM3; od_cfg_index knows nothing about
 * panels. The io callbacks below are the only place the two meet.
 */
#include "od_epd_adapter.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

extern "C" {
#include "od_cfg_index.h"
#include "od_epd_io_silabs.h"
#include "opendisplay_ble.h"   /* opendisplay_get_global_config */
#include "opendisplay_config_parser.h"
#include "opendisplay_constants.h"
}

/* Body offsets inside a 0x2E display_mode packet. These mirror
 * py-opendisplay's DisplayMode.serialize() and are asserted against the wire
 * struct below, so a layout change fails the build rather than the panel. */
#define MODE_OFF_NAME      2u
#define MODE_OFF_PASSES   14u
#define MODE_OFF_SLOT     15u
#define MODE_OFF_VSN1     19u
#define MODE_OFF_TMIN     24u
#define MODE_OFF_SEQLEN   27u
#define MODE_OFF_SEQ      29u

/* 0x2F display_lut */
#define LUT_OFF_LEN        2u
#define LUT_OFF_CRC        4u
#define LUT_OFF_DATA       6u

/* 0x31 display_panel */
#define PANEL_OFF_FLAGS      1u
#define PANEL_OFF_IDLE       2u
#define PANEL_OFF_TIMEOUT    3u
#define PANEL_OFF_FIXED      5u
#define PANEL_OFF_CODEMAP    7u
#define PANEL_OFF_MODECOUNT 11u
#define PANEL_OFF_SLEEPLEN  12u
#define PANEL_OFF_SLEEPSEQ  13u

static_assert(offsetof(struct DisplayModeConfig, seq) == MODE_OFF_SEQ,
              "0x2E sequence offset drifted from the wire struct");
static_assert(offsetof(struct DisplayLutConfig, data) == LUT_OFF_DATA,
              "0x2F data offset drifted from the wire struct");
static_assert(offsetof(struct DisplayPanelConfig, sleep_seq) == PANEL_OFF_SLEEPSEQ,
              "0x31 sleep_seq offset drifted from the wire struct");

/* The adapter is a singleton in practice (one panel, one SPI bus), and the io
 * vtable takes plain function pointers, so the lookup state lives at file
 * scope rather than being smuggled through a context pointer. */
/* mode_index -> od_cfg index. Sized by the config index, not by a mode cap:
 * how many update modes a display may have is now a config question. */
static uint8_t s_mode_entry_idx[OD_CFG_INDEX_MAX];
static uint8_t s_mode_entry_count;
static od_epd_io_t s_io;
static uint8_t s_desc_diag;   /* see od_epd_desc_diag() */

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}


/* ------------------------------------------------- config-backed io reads */

static uint16_t io_seq_read(uint8_t mode, uint16_t off, uint8_t *dst, uint16_t len)
{
    if (mode >= s_mode_entry_count) {
        return 0u;
    }
    const od_cfg_entry_t *e = od_cfg_index_entry((int)s_mode_entry_idx[mode]);
    if (e == NULL) {
        return 0u;
    }
    return (uint16_t)od_cfg_read(e, (uint16_t)(MODE_OFF_SEQ + off), dst, len);
}

/* Load one mode's header on selection. Only the ACTIVE mode is resident. */
static bool io_mode_read(uint8_t index, od_epd_mode_t *out)
{
    uint8_t h[MODE_OFF_SEQ];

    if (out == NULL || index >= s_mode_entry_count) {
        return false;
    }
    const od_cfg_entry_t *e = od_cfg_index_entry((int)s_mode_entry_idx[index]);
    if (e == NULL || od_cfg_read(e, 0u, h, sizeof(h)) != sizeof(h)) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    memcpy(out->name, &h[MODE_OFF_NAME], sizeof(out->name));
    out->name[sizeof(out->name) - 1u] = '\0';
    out->passes = h[MODE_OFF_PASSES];
    memcpy(out->lut_slot, &h[MODE_OFF_SLOT], 4);
    memcpy(out->vsn1, &h[MODE_OFF_VSN1], 4);
    out->temp_min_c = (int8_t)h[MODE_OFF_TMIN];
    out->temp_max_c = (int8_t)h[MODE_OFF_TMIN + 1u];
    out->temp_policy = h[MODE_OFF_TMIN + 2u];
    out->seq_len = rd16(&h[MODE_OFF_SEQLEN]);
    if (out->passes == 0u || out->passes > 4u ||
        out->seq_len == 0u || out->seq_len > OD_EPD_SEQ_MAX) {
        printf("[OD] mode %u rejected (passes=%u seq=%u)\r\n",
               (unsigned)index, (unsigned)out->passes, (unsigned)out->seq_len);
        return false;
    }
    return true;
}

/* A waveform slot is matched on `sub` (body[1] = lut_slot), NOT on the
 * packet-header byte. The header's first byte is a don't-care that the two
 * writers disagree about -- py-opendisplay puts the instance index there, the
 * web toolbox puts a running counter -- so matching on it would make a
 * toolbox-built config unable to find its own waveforms. */
static const od_cfg_entry_t *find_lut(uint8_t slot)
{
    for (int i = 0; i < od_cfg_index_count(); i++) {
        const od_cfg_entry_t *e = od_cfg_index_entry(i);
        if (e != NULL && e->type == CONFIG_PKT_DISPLAY_LUT && e->sub == slot) {
            return e;
        }
    }
    return NULL;
}

static uint16_t io_lut_read(uint8_t slot, uint16_t off, uint8_t *dst, uint16_t len)
{
    const od_cfg_entry_t *e = find_lut(slot);
    if (e == NULL) {
        return 0u;
    }
    return (uint16_t)od_cfg_read(e, (uint16_t)(LUT_OFF_DATA + off), dst, len);
}

/* Verify before use, and report a bad CRC as ABSENT rather than as a shorter
 * waveform. A waveform truncated by an interrupted config write still has a
 * plausible analog header, so it would drive the panel at sane voltages with
 * nonsense frames -- renders an image, just the wrong one. That is the failure
 * mode this whole driver exists to stop being silent. */
static bool io_lut_info(uint8_t slot, uint16_t *len_out)
{
    const od_cfg_entry_t *e = find_lut(slot);
    uint8_t hdr[6];
    uint8_t buf[64];

    if (e == NULL || od_cfg_read(e, 0u, hdr, sizeof(hdr)) != sizeof(hdr)) {
        return false;
    }
    uint16_t len = rd16(&hdr[LUT_OFF_LEN]);
    uint16_t want_crc = rd16(&hdr[LUT_OFF_CRC]);
    if (len == 0u || len > OD_EPD_LUT_BYTES) {
        return false;
    }

    uint16_t crc = 0xFFFFu;
    uint16_t off = 0u;
    while (off < len) {
        uint16_t chunk = (uint16_t)((len - off) < (uint16_t)sizeof(buf)
                                    ? (uint16_t)(len - off) : (uint16_t)sizeof(buf));
        if (od_cfg_read(e, (uint16_t)(LUT_OFF_DATA + off), buf, chunk) != chunk) {
            return false;
        }
        /* Running CRC over chunks: same polynomial, fed incrementally. */
        for (uint16_t i = 0; i < chunk; i++) {
            crc ^= (uint16_t)((uint16_t)buf[i] << 8);
            for (int b = 0; b < 8; b++) {
                crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u)
                                      : (uint16_t)(crc << 1);
            }
        }
        off += chunk;
    }
    if (crc != want_crc) {
        printf("[OD] lut slot %u CRC %04X != %04X -- treating as absent\r\n",
               (unsigned)slot, (unsigned)crc, (unsigned)want_crc);
        return false;
    }
    if (len_out != NULL) {
        *len_out = len;
    }
    return true;
}

/* ------------------------------------------------------ desc from config */

extern "C" uint8_t od_epd_desc_diag(void) { return s_desc_diag; }

static int desc_fail(int rc)
{
    int n = od_cfg_index_count();
    if (n < 0) { n = 0; }
    s_desc_diag = (uint8_t)(((unsigned)(-rc) << 5) | ((unsigned)n & 0x1Fu));
    return rc;
}

extern "C" int od_epd_desc_from_config(od_panel_desc_t *out, uint8_t display_instance)
{
    const struct GlobalConfig *cfg = opendisplay_get_global_config();
    uint8_t body[CONFIG_PKT_DISPLAY_PANEL_SIZE];

    if (out == NULL || cfg == NULL || cfg->display_count == 0u) {
        return desc_fail(-1);
    }
    memset(out, 0, sizeof(*out));
    s_mode_entry_count = 0u;

    /* Geometry and colour scheme come from 0x20, which is already parsed and
     * resident -- re-reading it from NVM3 would be churn for no saving. */
    const struct DisplayConfig *d = &cfg->displays[0];
    out->width = d->pixel_width;
    out->height = d->pixel_height;
    out->colour_scheme = d->color_scheme;

    if (od_cfg_index_count() <= 0 && od_cfg_index_build() < 0) {
        printf("[OD] desc: no config index\r\n");
        return desc_fail(-2);
    }

    /* 0x31: the drive properties 0x20 never carried. */
    const od_cfg_entry_t *pe = NULL;
    for (int i = 0; i < od_cfg_index_count(); i++) {
        const od_cfg_entry_t *e = od_cfg_index_entry(i);
        if (e != NULL && e->type == CONFIG_PKT_DISPLAY_PANEL &&
            e->instance == display_instance) {
            pe = e;
            break;
        }
    }
    if (pe == NULL || od_cfg_read(pe, 0u, body, sizeof(body)) != sizeof(body)) {
        printf("[OD] desc: display %u has no 0x31 panel packet\r\n",
               (unsigned)display_instance);
        return desc_fail(-3);
    }
    out->busy_usable = (body[PANEL_OFF_FLAGS] & OD_PANEL_FLAG_BUSY_USABLE) != 0u;
    out->busy_idle_level = body[PANEL_OFF_IDLE];
    out->busy_timeout_ms = rd16(&body[PANEL_OFF_TIMEOUT]);
    out->busy_fixed_delay_ms = rd16(&body[PANEL_OFF_FIXED]);
    memcpy(out->code_map, &body[PANEL_OFF_CODEMAP], 4);
    out->sleep_seq_len = body[PANEL_OFF_SLEEPLEN];
    if (out->sleep_seq_len > OD_EPD_SLEEP_SEQ_MAX) {
        out->sleep_seq_len = 0u;
    }
    memcpy(out->sleep_seq, &body[PANEL_OFF_SLEEPSEQ], OD_EPD_SLEEP_SEQ_MAX);

    /* 0x2E: one per mode. Only INDEXED here -- the header of whichever mode is
     * selected gets read later by io_mode_read, so a display may declare as
     * many modes as fit in the config rather than as many as an array held. */
    for (int i = 0; i < od_cfg_index_count() && out->mode_count < OD_CFG_INDEX_MAX; i++) {
        const od_cfg_entry_t *e = od_cfg_index_entry(i);
        uint8_t inst;
        if (e == NULL || e->type != CONFIG_PKT_DISPLAY_MODE) {
            continue;
        }
        if (od_cfg_read(e, 0u, &inst, 1u) != 1u || inst != display_instance) {
            continue;
        }
        s_mode_entry_idx[out->mode_count] = (uint8_t)i;
        out->mode_count++;
        s_mode_entry_count = out->mode_count;
    }
    if (out->mode_count == 0u) {
        printf("[OD] desc: display %u has no usable 0x2E modes\r\n",
               (unsigned)display_instance);
        return desc_fail(-4);
    }
    s_desc_diag = (uint8_t)(od_cfg_index_count() & 0x1F);   /* stage 0 = success */
    return 0;
}

/* --------------------------------------------------------------- adapter */

int OdEpdAdapter::setPanelType(uint8_t display_instance)
{
    _ready = false;
    if (od_epd_desc_from_config(&_desc, display_instance) != 0) {
        return BBEP_ERROR_BAD_PARAMETER;
    }
    s_io = *od_epd_io_silabs();
    s_io.seq_read = io_seq_read;
    s_io.mode_read = io_mode_read;
    s_io.lut_info = io_lut_info;
    s_io.lut_read = io_lut_read;
    if (od_epd_init(&_epd, &_desc, &s_io) != 0) {
        return BBEP_ERROR_BAD_PARAMETER;
    }
    /* Default to the first declared mode so a caller that never selects one
     * still gets a defined pass count. */
    (void)od_epd_select_mode(&_epd, 0u);
    _ready = true;
    printf("[OD] panel from config: %ux%u cs=%u busy=%s modes=%u\r\n",
           (unsigned)_desc.width, (unsigned)_desc.height,
           (unsigned)_desc.colour_scheme,
           _desc.busy_usable ? "poll" : "fixed-delay",
           (unsigned)_desc.mode_count);
    return BBEP_SUCCESS;
}

void OdEpdAdapter::setRotation(int degrees)
{
    /* Recorded, not acted on. bb_epaper rotated inside its framebuffer; this
     * driver has no framebuffer, so rotation is the host's job when it packs
     * the frame. Silently rotating nothing would be worse than storing it. */
    _rotation = degrees;
}

void OdEpdAdapter::initIO(uint8_t dc, uint8_t rst, uint8_t busy, uint8_t cs,
                          uint8_t mosi, uint8_t sck, uint32_t speed)
{
    od_epd_io_silabs_setup(dc, rst, busy, cs, mosi, sck, speed,
                           _desc.busy_idle_level != 0u);
}

int OdEpdAdapter::selectModeIndex(uint8_t index)
{
    if (!_ready || index >= _desc.mode_count) {
        return BBEP_ERROR_BAD_PARAMETER;
    }
    return od_epd_select_mode(&_epd, index) == 0 ? BBEP_SUCCESS
                                                 : BBEP_ERROR_BAD_PARAMETER;
}

uint8_t OdEpdAdapter::passCount(void) const
{
    return _ready ? _epd.active.passes : 0u;
}

int OdEpdAdapter::beginPass(uint8_t pass)
{
    if (!_ready) {
        return BBEP_ERROR_BAD_PARAMETER;
    }
    int rc = od_epd_begin_pass(&_epd, pass);
    if (rc != 0) {
        printf("[OD] beginPass %u failed rc=%d\r\n", (unsigned)pass, rc);
        return BBEP_ERROR_BAD_PARAMETER;
    }
    return BBEP_SUCCESS;
}

void OdEpdAdapter::writeData(const uint8_t *data, int len)
{
    if (len > 0) {
        (void)od_epd_write_image_chunk(&_epd, data, (size_t)len);
    }
}

int OdEpdAdapter::nextPlane(void)
{
    int rc = od_epd_next_plane(&_epd);
    if (rc != 0) {
        printf("[OD] nextPlane failed rc=%d (sequence has no second plane?)\r\n", rc);
        return BBEP_ERROR_BAD_PARAMETER;
    }
    return BBEP_SUCCESS;
}

int OdEpdAdapter::finishPass(void)
{
    int rc = od_epd_finish_pass(&_epd);
    if (rc != 0) {
        printf("[OD] finishPass failed rc=%d\r\n", rc);
        return BBEP_ERROR_BAD_PARAMETER;
    }
    return BBEP_SUCCESS;
}

void OdEpdAdapter::sleep(int mode)
{
    (void)mode;   /* the sequence itself says how deep the sleep is */
    if (_ready) {
        (void)od_epd_sleep(&_epd);
    }
}

bool OdEpdAdapter::isBusy(void)
{
    return _ready ? od_epd_is_busy(&_epd) : false;
}

void OdEpdAdapter::writeCmd(uint8_t c)
{
    if (_ready) {
        _epd.io->cmd(c);
    }
}

void OdEpdAdapter::writeRaw(const uint8_t *data, int len)
{
    if (_ready && data != nullptr && len > 0) {
        _epd.io->data(data, (size_t)len);
    }
}

int OdEpdAdapter::reset(uint8_t hold_ms)
{
    if (!_ready) {
        return BBEP_ERROR_BAD_PARAMETER;
    }
    _epd.io->reset(hold_ms);
    return BBEP_SUCCESS;
}
