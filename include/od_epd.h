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
 *   told:    every command byte, every data byte, every delay, which register
 *            enables a LUT, where the LUT goes, how many passes, the colour
 *            code map, whether BUSY is trustworthy
 *
 * NO FRAMEBUFFER. Image data streams row-by-row from the transport straight to
 * the panel, as bb_epaper did. A 800x480 2bpp frame is 96000 bytes; this target
 * has 32 KB of RAM.
 *
 * SAFETY. Init sequences are now UNTRUSTED INPUT on the pre-auth config path.
 * The interpreter is deliberately not a VM: it is a forward-only walk over
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
#define OD_EPD_MAX_SEQ_BYTES  512u   /* measured: real sequences are 62-77 B */
#define OD_EPD_MAX_SEQ_STEPS   64u
#define OD_EPD_LUT_BYTES      535u   /* 7-byte analog header + 528-byte body */

/* Sequence step encoding, produced by the host from panels.yaml.
 *   [len][cmd][data...]        len counts cmd+data, len >= 1
 *   0x00 0x01                  wait for BUSY to clear
 *   0x00 0x02 [n] [pairs...]   step with wave-header substitutions; n pairs of
 *                              (is_wave, value): is_wave=0xFF -> value is an
 *                              index into the LUT's 7-byte analog header,
 *                              0xFF index means VCOM = header[5] ^ 0x80
 *   0x00 0x00                  end of sequence
 *
 * A zero length is otherwise impossible (a step always has a command byte), so
 * it is free as an escape marker.
 */
#define OD_SEQ_ESC            0x00u
#define OD_SEQ_ESC_BUSY       0x01u
#define OD_SEQ_ESC_WAVE       0x02u
#define OD_SEQ_END            0x00u
#define OD_SEQ_WAVE_VCOM      0xFFu

enum od_lut_placement {
    OD_LUT_PRE_PON   = 0, /* upload to the LUT register before power-on */
    OD_LUT_POST_INIT = 1  /* upload after the ENTIRE init, booster tail included */
};

/* One drive mode. Up to 4 per display. */
typedef struct {
    char     name[12];
    uint8_t  passes;             /* 1 for normal refresh, 3 for 16-colour */
    uint8_t  lut_slot[4];        /* per-pass LUT slot, 0xFF = none */
    uint8_t  vsn1[4];            /* per-pass wave-header byte 3 patch, 0 = none */
    bool     analog_from_wave;   /* PWRR/PLL/VCOM substituted from LUT header */
    uint16_t init_off;           /* offset into the config's sequence blob */
    uint16_t init_len;
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
    uint32_t busy_timeout_ms;
    uint32_t busy_fixed_delay_ms;
    bool     lut_supported;
    uint8_t  lut_placement;      /* enum od_lut_placement */
    uint8_t  lut_enable_reg;     /* register whose bit enables LUT use */
    uint8_t  lut_enable_mask;
    uint8_t  lut_write_cmd;      /* command that accepts the waveform (0x20) */
    uint8_t  mode_count;
    od_epd_mode_t modes[OD_EPD_MAX_MODES];
} od_panel_desc_t;

/* Target-provided hardware access. The driver does no GPIO/SPI itself. */
typedef struct {
    void (*cmd)(uint8_t c);
    void (*data)(const uint8_t *d, size_t n);
    void (*reset)(uint32_t hold_ms);
    bool (*busy)(void);                 /* true while busy */
    void (*delay_ms)(uint32_t ms);
    const uint8_t *(*lut)(uint8_t slot, uint16_t *len_out); /* NULL if absent */
} od_epd_io_t;

typedef struct {
    const od_panel_desc_t *desc;
    const od_epd_io_t     *io;
    const uint8_t         *seq_blob;    /* config's sequence region */
    uint16_t               seq_blob_len;
    uint8_t                mode;
    uint8_t                pass;
} od_epd_t;

int  od_epd_init(od_epd_t *e, const od_panel_desc_t *d, const od_epd_io_t *io,
                 const uint8_t *seq_blob, uint16_t seq_blob_len);
int  od_epd_select_mode(od_epd_t *e, uint8_t mode);
/* Run one pass: reset, init (with LUT per placement), then the caller streams
 * image bytes via od_epd_write_image_chunk and calls od_epd_refresh. */
int  od_epd_begin_pass(od_epd_t *e, uint8_t pass);
int  od_epd_write_image_chunk(od_epd_t *e, const uint8_t *buf, size_t n);
int  od_epd_refresh(od_epd_t *e);
int  od_epd_sleep(od_epd_t *e);
bool od_epd_wait_idle(od_epd_t *e, uint32_t timeout_ms);

/* Decode a panel descriptor out of the stored config. Returns 0 on success. */
int  od_epd_desc_from_config(od_panel_desc_t *out, uint8_t display_instance);

#ifdef __cplusplus
}
#endif
#endif /* OD_EPD_H */
