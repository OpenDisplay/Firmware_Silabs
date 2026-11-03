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

/* Body offsets inside a 0xAE display_mode packet (fixed HEAD + seq tail; the
 * body length travels in the wire header). These mirror py-opendisplay's
 * serializer and are asserted against the wire struct below, so a layout
 * change fails the build rather than the panel. */
#define MODE_OFF_NAME      2u
#define MODE_OFF_SCHEME   14u
#define MODE_OFF_PASSES   15u
#define MODE_OFF_TMIN     16u
#define MODE_OFF_SLOT     19u
#define MODE_OFF_VSN1     27u
#define MODE_OFF_SEQLEN   35u
#define MODE_OFF_SEQ      37u

/* 0xAF display_lut: 4-byte head, then the waveform at its real size. There is
 * no stored length field any more -- the waveform length IS body_len - 4. */
#define LUT_OFF_CRC        2u
#define LUT_OFF_DATA       4u

/* 0xB1 display_panel: 13-byte head + sleep_seq[sleep_seq_len] tail. */
#define PANEL_OFF_FLAGS      1u
#define PANEL_OFF_IDLE       2u
#define PANEL_OFF_TIMEOUT    3u
#define PANEL_OFF_FIXED      5u
#define PANEL_OFF_CODEMAP    7u
#define PANEL_OFF_MODECOUNT 11u
#define PANEL_OFF_SLEEPLEN  12u
#define PANEL_OFF_SLEEPSEQ  13u

static_assert(sizeof(struct DisplayModeHead) == MODE_OFF_SEQ,
              "0xAE head size drifted from the wire struct");
static_assert(offsetof(struct DisplayModeHead, seq_len) == MODE_OFF_SEQLEN,
              "0xAE seq_len offset drifted from the wire struct");
static_assert(offsetof(struct DisplayModeHead, color_scheme) == MODE_OFF_SCHEME,
              "0xAE color_scheme offset drifted from the wire struct");
static_assert(sizeof(struct DisplayLutHead) == LUT_OFF_DATA,
              "0xAF head size drifted from the wire struct");
static_assert(offsetof(struct DisplayLutHead, crc16) == LUT_OFF_CRC,
              "0xAF crc offset drifted from the wire struct");
static_assert(sizeof(struct DisplayPanelHead) == PANEL_OFF_SLEEPSEQ,
              "0xB1 head size drifted from the wire struct");
static_assert(sizeof(((od_epd_mode_t *)0)->lut_slot)
                  == sizeof(((struct DisplayModeHead *)0)->lut_slot),
              "per-pass slot arrays disagree between executor and wire");
/* The executor's local escape values must agree with the canonical enum. */
static_assert(OD_EPD_ESC_SET_PIN == (unsigned)OD_SEQ_ESC_SET_PIN &&
              OD_EPD_ESC_IMAGE_SLICE == (unsigned)OD_SEQ_ESC_IMAGE_SLICE &&
              OD_EPD_ESC_CS_SELECT == (unsigned)OD_SEQ_ESC_CS_SELECT &&
              OD_EPD_ESC_READ_SCRATCH == (unsigned)OD_SEQ_ESC_READ_SCRATCH &&
              OD_EPD_ESC_WRITE_SCRATCH == (unsigned)OD_SEQ_ESC_WRITE_SCRATCH &&
              OD_EPD_ESC_LUT_SLICE == (unsigned)OD_SEQ_ESC_LUT_SLICE,
              "executor escape values drifted from opendisplay_structs.h");

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
    if (e == NULL || e->len < MODE_OFF_SEQ ||
        od_cfg_read(e, 0u, h, sizeof(h)) != sizeof(h)) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    memcpy(out->name, &h[MODE_OFF_NAME], sizeof(out->name));
    out->name[sizeof(out->name) - 1u] = '\0';
    out->color_scheme = h[MODE_OFF_SCHEME];
    out->passes = h[MODE_OFF_PASSES];
    memcpy(out->lut_slot, &h[MODE_OFF_SLOT], OD_EPD_MAX_PASSES);
    memcpy(out->vsn1, &h[MODE_OFF_VSN1], OD_EPD_MAX_PASSES);
    out->temp_min_c = (int8_t)h[MODE_OFF_TMIN];
    out->temp_max_c = (int8_t)h[MODE_OFF_TMIN + 1u];
    out->temp_policy = h[MODE_OFF_TMIN + 2u];
    out->seq_len = rd16(&h[MODE_OFF_SEQLEN]);
    if (out->passes == 0u || out->passes > OD_EPD_MAX_PASSES ||
        out->seq_len == 0u || out->seq_len > OD_EPD_SEQ_MAX ||
        (uint32_t)MODE_OFF_SEQ + out->seq_len > e->len) {
        printf("[OD] mode %u rejected (passes=%u seq=%u body=%u)\r\n",
               (unsigned)index, (unsigned)out->passes, (unsigned)out->seq_len,
               (unsigned)e->len);
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
    uint8_t hdr[LUT_OFF_DATA];
    uint8_t buf[64];

    if (e == NULL || e->len <= LUT_OFF_DATA ||
        od_cfg_read(e, 0u, hdr, sizeof(hdr)) != sizeof(hdr)) {
        return false;
    }
    /* The waveform length IS the body length minus the 4-byte head -- 0xAF
     * carries the waveform at its real size, no stored length to disagree. */
    uint16_t len = (uint16_t)(e->len - LUT_OFF_DATA);
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

/* ------------------------------------------------- SET_PIN escape backing */

/* pin_ref -> raw (port<<4|pin) byte, resolved from DisplayConfig 0x20 when the
 * panel descriptor is loaded. Sequences reference pins SYMBOLICALLY
 * (DriveSeqPinRef) so panels.yaml stays board-independent; this table is the
 * one place the symbol meets the wiring. */
#define PINREF_AUX_COUNT 6u
static uint8_t s_aux_pins[PINREF_AUX_COUNT];
static uint8_t s_named_pins[6];   /* RESET, DC, CS, CS_2, DATA, CLK */

static bool io_set_pin(uint8_t pin_ref, uint8_t level)
{
    uint8_t raw = 0xFFu;

    if (pin_ref < PINREF_AUX_COUNT) {
        raw = s_aux_pins[pin_ref];
        /* Aux pins: 0x00 AND 0xFF mean "not wired" -- legacy configs carry 0
         * in these ex-reserved bytes, so 0 cannot mean port A pin 0 here. */
        if (raw == 0x00u) {
            raw = 0xFFu;
        }
    } else if (pin_ref >= OD_SEQ_PINREF_RESET && pin_ref <= OD_SEQ_PINREF_CLK) {
        raw = s_named_pins[pin_ref - OD_SEQ_PINREF_RESET];
    }
    if (raw == 0xFFu) {
        printf("[OD] SET_PIN ref 0x%02X unwired on this board\r\n",
               (unsigned)pin_ref);
        return false;
    }
    return od_epd_io_silabs_pin(raw, level);
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
    uint8_t body[PANEL_OFF_SLEEPSEQ + OD_EPD_SLEEP_SEQ_MAX];
    uint16_t got;

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
    /* Variable body: 13-byte head + sleep_seq tail. Read what is there, up to
     * the head + the biggest sleep sequence the executor holds. */
    memset(body, 0, sizeof(body));
    got = (uint16_t)((pe != NULL && pe->len < sizeof(body)) ? pe->len
                                                            : sizeof(body));
    if (pe == NULL || pe->len < PANEL_OFF_SLEEPSEQ ||
        od_cfg_read(pe, 0u, body, got) != got) {
        printf("[OD] desc: display %u has no display_panel (0xB1) packet\r\n",
               (unsigned)display_instance);
        return desc_fail(-3);
    }
    out->busy_usable = (body[PANEL_OFF_FLAGS] & OD_PANEL_FLAG_BUSY_USABLE) != 0u;
    out->busy_idle_level = body[PANEL_OFF_IDLE];
    out->busy_timeout_ms = rd16(&body[PANEL_OFF_TIMEOUT]);
    out->busy_fixed_delay_ms = rd16(&body[PANEL_OFF_FIXED]);
    memcpy(out->code_map, &body[PANEL_OFF_CODEMAP], 4);
    out->sleep_seq_len = body[PANEL_OFF_SLEEPLEN];
    if (out->sleep_seq_len > OD_EPD_SLEEP_SEQ_MAX ||
        (uint32_t)PANEL_OFF_SLEEPSEQ + out->sleep_seq_len > got) {
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
    /* Resolve the SET_PIN symbol table from 0x20 while it is at hand. */
    {
        const struct GlobalConfig *cfg = opendisplay_get_global_config();
        const struct DisplayConfig *d = &cfg->displays[0];
        s_aux_pins[0] = d->aux_pin_1;
        s_aux_pins[1] = d->aux_pin_2;
        s_aux_pins[2] = d->aux_pin_3;
        s_aux_pins[3] = d->aux_pin_4;
        s_aux_pins[4] = d->aux_pin_5;
        s_aux_pins[5] = d->aux_pin_6;
        s_named_pins[0] = d->reset_pin;
        s_named_pins[1] = d->dc_pin;
        s_named_pins[2] = d->cs_pin;
        s_named_pins[3] = d->cs_pin_2;
        s_named_pins[4] = d->data_pin;
        s_named_pins[5] = d->clk_pin;
    }
    s_io = *od_epd_io_silabs();
    s_io.seq_read = io_seq_read;
    s_io.mode_read = io_mode_read;
    s_io.lut_info = io_lut_info;
    s_io.lut_read = io_lut_read;
    s_io.set_pin = io_set_pin;
    s_io.cs_select = NULL;   /* dual-CS: specified, not implemented here yet --
                              * a sequence using it is refused at validation. */
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
        _last_rc = OD_EPD_RC_STATE;
        return BBEP_ERROR_BAD_PARAMETER;
    }
    _last_rc = od_epd_begin_pass(&_epd, pass);
    if (_last_rc != 0) {
        printf("[OD] beginPass %u failed rc=%d%s\r\n", (unsigned)pass, _last_rc,
               _last_rc == OD_EPD_RC_UNSUP ? " (sequence refused: unsupported escape)"
                                           : "");
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
