/* od_epd -- config-driven e-paper driver. ZERO panel constants compiled in.
 *
 * WHY THIS REPLACES bb_epaper
 * ---------------------------
 * Panel behaviour used to be a property of a vendored C library version: init
 * byte arrays, LUTs, BUSY quirks and colour order all lived in bb_epaper, were
 * mirrored by hand into py-opendisplay's display_palettes.py, and were selected
 * through a per-target switch. Nothing forced those three to agree, and a
 * library bump silently changed init traffic on four shipped panels
 * (Firmware_Unified targets/nordic-zephyr/PANEL_DIVERGENCE.md) while compiling
 * cleanly and passing every gate.
 *
 * Here, EVERYTHING the driver needs arrives in the device config. There is no
 * panel table, no panel enum, and no per-target mapping switch. A new panel is
 * a config change, not a firmware release.
 *
 * WHAT THE DRIVER KNOWS vs WHAT CONFIG TELLS IT
 *   knows:   how to clock bytes out of SPI, toggle D/C, RST and CS, read BUSY
 *   told:    every command byte, every data byte, which register accepts the
 *            waveform, where the waveform upload sits relative to power-on,
 *            where the image goes, how many passes, the colour code map,
 *            whether BUSY is trustworthy and which level means idle
 *
 * NO FRAMEBUFFER. Image data streams row-by-row from the transport straight to
 * the panel, as bb_epaper did. A 800x480 2bpp frame is 96000 bytes; this target
 * has 32 KB of RAM.
 *
 * NO RESIDENT CONFIG. The stored config lives in NVM3 and nowhere else (see
 * od_cfg_index.h). Sequences and waveforms are therefore fetched through
 * callbacks, not pointed at: a 535-byte waveform is streamed to the panel in
 * chunks rather than copied into RAM first.
 *
 * SAFETY. Sequences are now UNTRUSTED INPUT on the pre-auth config path. The
 * interpreter is deliberately not a VM: it is a forward-only walk over
 * length-prefixed steps with a hard cap on step count and total length, no
 * jumps, no loops, no addressing. The worst a malformed sequence can do is
 * clock out garbage and time out -- which raw SPI can already do.
 */
#ifndef OD_EPD_H
#define OD_EPD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OD_EPD_MAX_MODES        4u
#define OD_EPD_SEQ_MAX        128u   /* measured: real sequences are 62-101 B */
#define OD_EPD_MAX_SEQ_STEPS   64u
#define OD_EPD_LUT_BYTES      535u   /* 7-byte analog header + 528-byte body */
#define OD_EPD_SLEEP_SEQ_MAX   16u

/* Sequence step encoding, produced by the host from panels.yaml.
 *
 *   [len][cmd][data...]        len counts cmd+data, len >= 1
 *   0x00 0x01                  wait for BUSY to clear
 *   0x00 0x02 [n] [pairs...]   step with wave-header substitutions; n pairs of
 *                              (is_wave, value): is_wave=0xFF -> value is an
 *                              index into the waveform's 7-byte analog header,
 *                              0xFF index means VCOM = header[5] ^ 0x80
 *   0x00 0x03                  STREAM THE IMAGE HERE (walk pauses)
 *   0x00 0x04                  STREAM THE WAVEFORM HERE
 *   0x00 0x05 [lo] [hi]        wait N ms (little-endian)
 *   0x00 0x00                  end of sequence
 *
 * The explicit delay exists because a BUSY wait is not a substitute for it.
 * The 4.2" GDEM042F52's BUSY line is unusable, so its sequence needs a SHORT
 * settle after power-on (2 s) and a LONG wait after refresh (35 s); a single
 * per-panel fixed delay cannot be both. Without this step the only way to
 * express the short one would be to hard-code it in the driver.
 *
 * A zero length is otherwise impossible (a step always has a command byte), so
 * it is free as an escape marker.
 *
 * ONE SEQUENCE IS THE WHOLE RECIPE FOR A PASS: registers, waveform upload,
 * image, power-on, refresh. That is deliberate, and it is what let an entire
 * `lut_placement` enum be deleted.
 *
 * The 7.5" GDEM075F52 discards a waveform uploaded before power-on SILENTLY --
 * two different waveforms then render identically to within 0.2/255, which
 * looks like conclusive proof the panel ignores custom waveforms and is not.
 * It must be uploaded after the entire init, booster tail included. The 2.9"
 * and 4.2" need it before power-on. Encoding that as a driver-side enum meant
 * the driver had to know which panel it was talking to; encoding it as the
 * POSITION OF 0x00 0x04 IN THE SEQUENCE means the fact is written down exactly
 * where it takes effect, and the driver needs no opinion at all.
 *
 * The register that accepts the waveform is likewise not a field: the sequence
 * contains a plain `[1][0x20]` step immediately before the marker.
 */
#define OD_SEQ_ESC            0x00u
#define OD_SEQ_ESC_BUSY       0x01u
#define OD_SEQ_ESC_WAVE       0x02u
#define OD_SEQ_ESC_IMAGE      0x03u
#define OD_SEQ_ESC_LUT        0x04u
#define OD_SEQ_ESC_DELAY      0x05u
#define OD_SEQ_END            0x00u
#define OD_SEQ_WAVE_VCOM      0xFFu

/* One drive mode. Up to 4 per display. */
typedef struct {
    char     name[12];
    uint8_t  passes;             /* 1 for normal refresh, 3 for 16-colour */
    uint8_t  lut_slot[4];        /* per-pass waveform slot, 0xFF = none */
    uint8_t  vsn1[4];            /* per-pass waveform byte 3 patch, 0 = none */
    uint16_t seq_len;
    int8_t   temp_min_c;         /* advisory; host enforces */
    int8_t   temp_max_c;
    uint8_t  temp_policy;
} od_epd_mode_t;

/* Everything the driver needs about a panel. Populated from config ONLY. */
typedef struct {
    uint16_t width, height;
    uint8_t  colour_scheme;
    uint8_t  code_map[4];        /* logical colour -> panel 2-bit code */
    bool     busy_usable;        /* false: this panel's BUSY line lies */
    uint8_t  busy_idle_level;    /* pin level that means "not busy" */
    uint32_t busy_timeout_ms;
    uint32_t busy_fixed_delay_ms;
    uint8_t  sleep_seq[OD_EPD_SLEEP_SEQ_MAX];
    uint8_t  sleep_seq_len;
    uint8_t  mode_count;
    od_epd_mode_t modes[OD_EPD_MAX_MODES];
} od_panel_desc_t;

/* Target-provided hardware access plus config-backed reads.
 *
 * The reads are callbacks rather than pointers because there is no resident
 * copy of the config to point at -- NVM3 is the only source of truth. A
 * waveform is 535 bytes; on a 32 KB part it is streamed, never buffered. */
typedef struct {
    void (*cmd)(uint8_t c);
    void (*data)(const uint8_t *d, size_t n);
    void (*reset)(uint32_t hold_ms);
    bool (*busy_level)(void);           /* RAW pin level; idle sense is config */
    void (*delay_ms)(uint32_t ms);
    uint16_t (*seq_read)(uint8_t mode, uint16_t off, uint8_t *dst, uint16_t len);
    bool     (*lut_info)(uint8_t slot, uint16_t *len_out);
    uint16_t (*lut_read)(uint8_t slot, uint16_t off, uint8_t *dst, uint16_t len);
} od_epd_io_t;

typedef struct {
    const od_panel_desc_t *desc;
    const od_epd_io_t     *io;
    uint8_t                mode;
    uint8_t                pass;
    /* The active pass's recipe, copied out of NVM3 once per pass. Held because
     * the walk PAUSES at the image marker and resumes after the caller has
     * streamed the frame -- the bytes must outlive that gap. 128 B bounded. */
    uint8_t                seq[OD_EPD_SEQ_MAX];
    uint16_t               seq_len;
    uint16_t               seq_pos;
    bool                   paused;
} od_epd_t;

int  od_epd_init(od_epd_t *e, const od_panel_desc_t *d, const od_epd_io_t *io);
int  od_epd_select_mode(od_epd_t *e, uint8_t mode);
int  od_epd_find_mode(const od_panel_desc_t *d, const char *name);

/* Run a pass up to the image marker: reset, registers, waveform upload. The
 * caller then streams the frame with od_epd_write_image_chunk and calls
 * od_epd_finish_pass to run the tail (power-on, refresh, BUSY wait). */
int  od_epd_begin_pass(od_epd_t *e, uint8_t pass);
int  od_epd_write_image_chunk(od_epd_t *e, const uint8_t *buf, size_t n);
/* Advance to the next image marker, for panels that take two 1bpp planes. */
int  od_epd_next_plane(od_epd_t *e);
int  od_epd_finish_pass(od_epd_t *e);
int  od_epd_sleep(od_epd_t *e);
bool od_epd_wait_idle(od_epd_t *e, uint32_t timeout_ms);
bool od_epd_is_busy(od_epd_t *e);

/* Decode a panel descriptor out of the stored config. Returns 0 on success,
 * negative when the config carries no drive data for this display -- there is
 * no built-in table to fall back to, and saying so is the honest answer. */
int  od_epd_desc_from_config(od_panel_desc_t *out, uint8_t display_instance);

/* Why the last od_epd_desc_from_config() call failed, packed for a one-byte
 * debug channel: (-rc) in bits 7..5, indexed packet count in bits 4..0.
 *
 * This exists because all four failure branches are invisible from the host:
 * CMD_SPI_RAW and CMD_DIRECT_WRITE_START both collapse them into one NACK, and
 * the only other diagnostic is printf over RTT -- which needs a wired
 * programmer. On a BLE-only device that made "the panel does not come up"
 * unbisectable. */
uint8_t od_epd_desc_diag(void);

#ifdef __cplusplus
}
#endif
#endif /* OD_EPD_H */
