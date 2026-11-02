/* od_epd -- config-driven e-paper driver. See od_epd.h for the rationale.
 *
 * The whole point of this file is that it contains NO panel knowledge. Search
 * it for a panel name, a register number or a delay and you will not find one:
 * every byte it writes came out of the device config.
 */
#include "od_epd.h"

#include <string.h>

#define OD_OK            0
#define OD_ERR_ARG      -1
#define OD_ERR_MODE     -2
#define OD_ERR_SEQ      -3
#define OD_ERR_LUT      -4
#define OD_ERR_BUSY     -5

/* ------------------------------------------------------------------ helpers */

bool od_epd_wait_idle(od_epd_t *e, uint32_t timeout_ms)
{
    /* A panel whose BUSY line lies gets a fixed delay instead of a poll.
     * This is not a workaround bolted on for one panel -- it is a declared
     * per-panel property. The 4.2" GDEM042F52 reports busy immediately after
     * power-up with nothing sent and never releases after a refresh; polling it
     * stalls forever. bb_epaper encoded the same fact as an UNDOCUMENTED local
     * patch (BBEP_SKIP_BUSY_WAIT) to its vendored tree, which is exactly the
     * kind of hidden knowledge this driver exists to make explicit. */
    if (!e->desc->busy_usable) {
        uint32_t d = e->desc->busy_fixed_delay_ms;
        e->io->delay_ms(d ? d : timeout_ms);
        return true;
    }
    uint32_t waited = 0;
    while (e->io->busy()) {
        if (waited >= timeout_ms) {
            return false;
        }
        e->io->delay_ms(5);
        waited += 5;
    }
    return true;
}

/* Substitute a byte that the mode takes from the LUT's analog header.
 * PWRR/PLL/VCOM are properties of the WAVEFORM, not of the init sequence, so a
 * mode that uploads a custom LUT must drive the analog rails that waveform was
 * authored for. Sending a custom LUT with the stock rails plays the right
 * waveform at the wrong voltages -- which does not fail, it renders a
 * plausible-but-wrong image. */
static uint8_t wave_byte(const uint8_t *lut, uint16_t lut_len, uint8_t idx)
{
    if (lut == NULL || lut_len < 7u) {
        return 0u;
    }
    if (idx == OD_SEQ_WAVE_VCOM) {
        return (uint8_t)(lut[5] ^ 0x80u);
    }
    return (idx < 7u) ? lut[idx] : 0u;
}

/* Forward-only walk over a length-prefixed sequence.
 *
 * Deliberately NOT a bytecode VM. Init sequences arrive in the config, which is
 * parsed pre-auth, so this is untrusted input. There are no jumps, no loops and
 * no addressing: the walk only moves forward, every step is bounded by the
 * blob length, and the step count is capped. The worst a malformed sequence can
 * do is clock out nonsense and time out -- which the raw-SPI debug command can
 * already do deliberately.
 */
static int run_sequence(od_epd_t *e, const uint8_t *seq, uint16_t len,
                        const uint8_t *lut, uint16_t lut_len)
{
    uint16_t i = 0;
    uint16_t steps = 0;
    uint8_t  buf[32];

    while (i < len) {
        if (++steps > OD_EPD_MAX_SEQ_STEPS) {
            return OD_ERR_SEQ;
        }
        uint8_t n = seq[i];

        if (n != OD_SEQ_ESC) {
            /* plain step: [len][cmd][data...] */
            if ((uint32_t)i + 1u + n > len) {
                return OD_ERR_SEQ;
            }
            e->io->cmd(seq[i + 1u]);
            if (n > 1u) {
                e->io->data(&seq[i + 2u], (size_t)(n - 1u));
            }
            i += 1u + n;
            continue;
        }

        /* escape */
        if (i + 1u >= len) {
            return OD_ERR_SEQ;
        }
        uint8_t esc = seq[i + 1u];
        if (esc == OD_SEQ_END) {
            return OD_OK;
        }
        if (esc == OD_SEQ_ESC_BUSY) {
            if (!od_epd_wait_idle(e, e->desc->busy_timeout_ms)) {
                return OD_ERR_BUSY;
            }
            i += 2u;
            continue;
        }
        if (esc == OD_SEQ_ESC_WAVE) {
            if (i + 2u >= len) {
                return OD_ERR_SEQ;
            }
            uint8_t cnt = seq[i + 2u];
            if (cnt == 0u || cnt > sizeof(buf) ||
                (uint32_t)i + 3u + (uint32_t)cnt * 2u > len) {
                return OD_ERR_SEQ;
            }
            const uint8_t *p = &seq[i + 3u];
            for (uint8_t k = 0; k < cnt; k++) {
                buf[k] = (p[k * 2u] == 0xFFu)
                       ? wave_byte(lut, lut_len, p[k * 2u + 1u])
                       : p[k * 2u + 1u];
            }
            e->io->cmd(buf[0]);
            if (cnt > 1u) {
                e->io->data(&buf[1], (size_t)(cnt - 1u));
            }
            i += 3u + (uint16_t)cnt * 2u;
            continue;
        }
        return OD_ERR_SEQ;
    }
    return OD_OK;
}

/* -------------------------------------------------------------------- API */

int od_epd_init(od_epd_t *e, const od_panel_desc_t *d, const od_epd_io_t *io,
                const uint8_t *seq_blob, uint16_t seq_blob_len)
{
    if (!e || !d || !io || !io->cmd || !io->data || !io->reset || !io->delay_ms) {
        return OD_ERR_ARG;
    }
    memset(e, 0, sizeof(*e));
    e->desc = d;
    e->io = io;
    e->seq_blob = seq_blob;
    e->seq_blob_len = seq_blob_len;
    return OD_OK;
}

int od_epd_select_mode(od_epd_t *e, uint8_t mode)
{
    if (!e || mode >= e->desc->mode_count) {
        return OD_ERR_MODE;
    }
    e->mode = mode;
    e->pass = 0;
    return OD_OK;
}

int od_epd_begin_pass(od_epd_t *e, uint8_t pass)
{
    const od_epd_mode_t *m = &e->desc->modes[e->mode];
    if (pass >= m->passes) {
        return OD_ERR_MODE;
    }
    e->pass = pass;

    /* Resolve this pass's LUT, if any. */
    const uint8_t *lut = NULL;
    uint16_t lut_len = 0;
    uint8_t slot = m->lut_slot[pass];
    if (slot != 0xFFu && e->desc->lut_supported && e->io->lut) {
        lut = e->io->lut(slot, &lut_len);
        if (lut && lut_len < 7u) {
            return OD_ERR_LUT;   /* too short to even carry an analog header */
        }
    }

    e->io->reset(10);
    if (!od_epd_wait_idle(e, e->desc->busy_timeout_ms)) {
        return OD_ERR_BUSY;
    }

    if ((uint32_t)m->init_off + m->init_len > e->seq_blob_len) {
        return OD_ERR_SEQ;
    }
    const uint8_t *seq = &e->seq_blob[m->init_off];

    /* LUT PLACEMENT IS PER-PANEL AND IS NOT INFERABLE.
     *
     * On the 2.9" and 4.2" the waveform must be uploaded BEFORE power-on. On the
     * 7.5" GDEM075F52 that upload is SILENTLY DISCARDED -- two different
     * waveforms then render identically to within 0.2/255, which looks like
     * conclusive proof that the panel ignores custom LUTs, and is not. It must
     * go after the ENTIRE init including the booster tail. Proof that it works:
     * each waveform then plays at its own intrinsic length (internal 20244 ms,
     * colour 20909 ms, edit 1862 ms).
     *
     * Because pre-PON is silently wrong rather than loudly wrong on one panel,
     * placement has to be declared data, not driver behaviour.
     */
    if (lut && e->desc->lut_placement == OD_LUT_PRE_PON) {
        /* The init sequence itself carries the LUT-enable bit (the host set it
         * when generating the sequence), so we only push the waveform here. */
        int rc = run_sequence(e, seq, m->init_len, lut, lut_len);
        if (rc != OD_OK) {
            return rc;
        }
        e->io->cmd(e->desc->lut_write_cmd);
        e->io->data(lut, lut_len);
    } else {
        int rc = run_sequence(e, seq, m->init_len, lut, lut_len);
        if (rc != OD_OK) {
            return rc;
        }
        if (lut) {   /* OD_LUT_POST_INIT */
            e->io->cmd(e->desc->lut_write_cmd);
            e->io->data(lut, lut_len);
        }
    }
    return OD_OK;
}

int od_epd_write_image_chunk(od_epd_t *e, const uint8_t *buf, size_t n)
{
    if (!e || (!buf && n)) {
        return OD_ERR_ARG;
    }
    if (n) {
        e->io->data(buf, n);   /* streamed; never buffered */
    }
    return OD_OK;
}

int od_epd_refresh(od_epd_t *e)
{
    /* The refresh command and its argument are panel data like everything else;
     * they live at the tail of the mode's sequence blob as a normal step, so a
     * panel needing a different refresh opcode needs no code change. Here we
     * only wait for completion. */
    if (!od_epd_wait_idle(e, e->desc->busy_timeout_ms)) {
        return OD_ERR_BUSY;
    }
    return OD_OK;
}

int od_epd_sleep(od_epd_t *e)
{
    (void)e;
    return OD_OK;   /* sleep sequence is config-provided, run by the caller */
}
