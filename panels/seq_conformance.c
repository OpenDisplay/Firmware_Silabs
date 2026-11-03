/* seq_conformance -- compile src/od_epd.c natively and dump what it clocks out.
 *
 * WHY. There are two interpreters of the sequence encoding: run_sequence() in
 * src/od_epd.c, which ships, and walk() in drive_from_packets.py, which is how
 * the design is validated on hardware. If they disagree, the hardware test
 * passes while the device misbehaves -- and the disagreement is invisible,
 * because both produce a plausible-looking stream of register writes.
 *
 * This runs the REAL C interpreter against the REAL packet bytes and prints
 * every operation. seq_conformance.py diffs that against the Python walk.
 * Nothing here mocks od_epd itself; only the hardware beneath it.
 *
 * Build: cc -I../include -o /tmp/seqconf seq_conformance.c ../src/od_epd.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "od_epd.h"

static uint8_t  g_seq[OD_EPD_SEQ_MAX];
static uint16_t g_seq_len;
static uint8_t  g_lut[OD_EPD_LUT_BYTES];
static uint16_t g_lut_len;
static int      g_have_lut;

static void io_cmd(uint8_t c)                 { printf("cmd %02x\n", c); }
static void io_reset(uint32_t ms)             { printf("reset %u\n", ms); }
static void io_delay(uint32_t ms)             { printf("delay %u\n", ms); }
static bool io_busy_level(void)               { return true; /* idle_level=1 */ }

static void io_data(const uint8_t *d, size_t n)
{
    /* Summarise rather than dump: a 528-byte waveform body would drown the
     * diff, and what matters is the length and the first bytes (the analog
     * header, where the vsn1 patch lands). */
    printf("data %zu", n);
    for (size_t i = 0; i < n && i < 8u; i++) {
        printf(" %02x", d[i]);
    }
    printf("\n");
}

static uint16_t io_seq_read(uint8_t mode, uint16_t off, uint8_t *dst, uint16_t len)
{
    (void)mode;
    if ((uint32_t)off + len > g_seq_len) {
        return 0u;
    }
    memcpy(dst, &g_seq[off], len);
    return len;
}

static bool io_lut_info(uint8_t slot, uint16_t *len_out)
{
    (void)slot;
    if (!g_have_lut) {
        return false;
    }
    *len_out = g_lut_len;
    return true;
}

static uint16_t io_lut_read(uint8_t slot, uint16_t off, uint8_t *dst, uint16_t len)
{
    (void)slot;
    if (!g_have_lut || (uint32_t)off + len > g_lut_len) {
        return 0u;
    }
    memcpy(dst, &g_lut[off], len);
    return len;
}

static size_t load(const char *path, uint8_t *dst, size_t cap)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        exit(2);
    }
    size_t n = fread(dst, 1, cap, f);
    fclose(f);
    return n;
}

/* The mode header now reaches the executor through io->mode_read -- there is
 * no resident modes[] array in od_panel_desc_t any more. */
static od_epd_mode_t g_mode;

static bool io_mode_read(uint8_t index, od_epd_mode_t *out)
{
    if (index != 0u) {
        return false;
    }
    *out = g_mode;
    return true;
}

/* SET_PIN backing for conformance: prints the operation (diffed against the
 * Python model) and accepts every reference, so sequences exercising the
 * escape run end to end. --no-set-pin leaves the hook NULL to prove the
 * validator refuses such sequences on a target without the capability. */
static bool io_set_pin(uint8_t pin_ref, uint8_t level)
{
    printf("setpin %02x %u\n", pin_ref, level);
    return true;
}

static bool io_cs_select(uint8_t cs)
{
    printf("cssel %u\n", cs);
    return true;
}

int main(int argc, char **argv)
{
    /* argv: seq.bin passes slot0,slot1,... vsn1_0,vsn1_1,... busy_usable
     *       idle_level timeout fixed [lut.bin] [--no-set-pin] [--no-cs] */
    if (argc < 9) {
        fprintf(stderr, "usage: %s seq.bin passes slots vsn1s busy idle "
                        "timeout fixed [lut.bin] [--no-set-pin] [--no-cs]\n",
                argv[0]);
        return 2;
    }
    g_seq_len = (uint16_t)load(argv[1], g_seq, sizeof(g_seq));

    od_panel_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.width = 168;
    desc.height = 384;
    desc.busy_usable = (uint8_t)atoi(argv[5]) != 0u;
    desc.busy_idle_level = (uint8_t)atoi(argv[6]);
    desc.busy_timeout_ms = (uint32_t)atoi(argv[7]);
    desc.busy_fixed_delay_ms = (uint32_t)atoi(argv[8]);
    desc.mode_count = 1;

    memset(&g_mode, 0, sizeof(g_mode));
    strcpy(g_mode.name, "t");
    g_mode.passes = (uint8_t)atoi(argv[2]);
    g_mode.seq_len = g_seq_len;
    for (unsigned i = 0; i < OD_EPD_MAX_PASSES; i++) {
        g_mode.lut_slot[i] = 0xFFu;
    }
    {   /* "s0,s1,.." and "v0,v1,.." */
        char *p = strtok(argv[3], ",");
        for (unsigned i = 0; i < OD_EPD_MAX_PASSES && p; i++, p = strtok(NULL, ",")) {
            g_mode.lut_slot[i] = (uint8_t)strtol(p, NULL, 0);
        }
        p = strtok(argv[4], ",");
        for (unsigned i = 0; i < OD_EPD_MAX_PASSES && p; i++, p = strtok(NULL, ",")) {
            g_mode.vsn1[i] = (uint8_t)strtol(p, NULL, 0);
        }
    }

    bool with_set_pin = true;
    bool with_cs = true;
    for (int a = 9; a < argc; a++) {
        if (strcmp(argv[a], "--no-set-pin") == 0) {
            with_set_pin = false;
        } else if (strcmp(argv[a], "--no-cs") == 0) {
            with_cs = false;
        } else {
            g_lut_len = (uint16_t)load(argv[a], g_lut, sizeof(g_lut));
            g_have_lut = 1;
        }
    }

    od_epd_io_t io = {
        .cmd = io_cmd, .data = io_data, .reset = io_reset,
        .busy_level = io_busy_level, .delay_ms = io_delay,
        .seq_read = io_seq_read, .mode_read = io_mode_read,
        .lut_info = io_lut_info, .lut_read = io_lut_read,
        .set_pin = with_set_pin ? io_set_pin : NULL,
        .cs_select = with_cs ? io_cs_select : NULL,
    };

    od_epd_t e;
    if (od_epd_init(&e, &desc, &io) != 0) {
        fprintf(stderr, "od_epd_init failed\n");
        return 3;
    }
    if (od_epd_select_mode(&e, 0) != 0) {
        fprintf(stderr, "select_mode failed\n");
        return 3;
    }
    for (uint8_t p = 0; p < g_mode.passes; p++) {
        printf("pass %u\n", p);
        int rc = od_epd_begin_pass(&e, p);
        if (rc != 0) {
            /* rc -7 (OD_EPD_RC_UNSUP) is a DISTINCT outcome the Python side
             * asserts on: sequence refused at validation, nothing executed. */
            fprintf(stderr, "begin_pass %u rc=%d\n", p, rc);
            return (rc == OD_EPD_RC_UNSUP) ? 5 : 4;
        }
        /* Stream point. IMAGE pauses as slice 0/1; IMAGE_SLICE as [part][of].
         * Walk every marker in the sequence, as the transfer path does. */
        printf("IMAGE part %u of %u\n", e.img_part, e.img_of);
        while (e.img_part + 1u < e.img_of) {
            rc = od_epd_next_plane(&e);
            if (rc != 0) {
                fprintf(stderr, "next_plane rc=%d\n", rc);
                return 4;
            }
            printf("IMAGE part %u of %u\n", e.img_part, e.img_of);
        }
        rc = od_epd_finish_pass(&e);
        if (rc != 0) {
            fprintf(stderr, "finish_pass %u rc=%d\n", p, rc);
            return 4;
        }
    }
    return 0;
}
