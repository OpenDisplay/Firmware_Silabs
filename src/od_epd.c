/* od_epd -- config-driven e-paper driver. See od_epd.h for the rationale.
 *
 * The whole point of this file is that it contains NO panel knowledge. Search
 * it for a panel name, a register number or a delay and you will not find one:
 * every byte it writes came out of the device config.
 */
#include "od_epd.h"

#include <string.h>

#define OD_OK           OD_EPD_RC_OK
#define OD_ERR_ARG      OD_EPD_RC_ARG
#define OD_ERR_MODE     OD_EPD_RC_MODE
#define OD_ERR_SEQ      OD_EPD_RC_SEQ
#define OD_ERR_LUT      OD_EPD_RC_LUT
#define OD_ERR_BUSY     OD_EPD_RC_BUSY
#define OD_ERR_STATE    OD_EPD_RC_STATE
#define OD_ERR_UNSUP    OD_EPD_RC_UNSUP

/* Streaming chunk for the waveform. 64 B of stack against a 535-byte upload:
 * the alternative is a resident 535-byte buffer, which is 1.7% of this part's
 * RAM held permanently for bytes that are only ever clocked straight out. */
#define LUT_CHUNK 64u

/* ------------------------------------------------------------------ helpers */

bool od_epd_is_busy(od_epd_t *e)
{
    if (!e->desc->busy_usable) {
        return false;
    }
    return e->io->busy_level() != (e->desc->busy_idle_level != 0u);
}

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
    while (od_epd_is_busy(e)) {
        if (waited >= timeout_ms) {
            return false;
        }
        e->io->delay_ms(5);
        waited += 5;
    }
    return true;
}

/* Substitute a byte that the sequence takes from the waveform's analog header.
 * PWRR/PLL/VCOM are properties of the WAVEFORM, not of the init sequence, so a
 * mode that uploads a custom waveform must drive the analog rails that waveform
 * was authored for. Sending a custom waveform with the stock rails plays the
 * right waveform at the wrong voltages -- which does not fail, it renders a
 * plausible-but-wrong image. */
static uint8_t wave_byte(const uint8_t *hdr, bool have_hdr, uint8_t idx)
{
    if (!have_hdr) {
        return 0u;
    }
    if (idx == OD_EPD_WAVE_VCOM) {
        return (uint8_t)(hdr[5] ^ 0x80u);
    }
    return (idx < 7u) ? hdr[idx] : 0u;
}

/* Bytes of escape arguments, -1 for variable (SUBST) or unknown. Shared by the
 * validator and the runtime walk so the two cannot disagree about framing. */
static int esc_arg_bytes(uint8_t esc)
{
    switch (esc) {
        case OD_EPD_SEQ_END:
        case OD_EPD_ESC_BUSY:
        case OD_EPD_ESC_IMAGE:
        case OD_EPD_ESC_LUT:           return 0;
        case OD_EPD_ESC_DELAY:
        case OD_EPD_ESC_SET_PIN:
        case OD_EPD_ESC_IMAGE_SLICE:   return 2;
        case OD_EPD_ESC_CS_SELECT:     return 1;
        case OD_EPD_ESC_READ_SCRATCH:  return 2;
        case OD_EPD_ESC_WRITE_SCRATCH: return 3;
        case OD_EPD_ESC_LUT_SLICE:     return 4;
        default:                       return -1;
    }
}

int od_epd_validate(const od_epd_t *e)
{
    const uint8_t *seq;
    uint16_t len;
    uint16_t i = 0;
    uint16_t steps = 0;

    if (e == NULL || e->io == NULL) {
        return OD_ERR_ARG;
    }
    seq = e->seq;
    len = e->seq_len;

    while (i < len) {
        if (++steps > OD_EPD_MAX_SEQ_STEPS) {
            return OD_ERR_SEQ;
        }
        uint8_t n = seq[i];
        if (n != OD_EPD_ESC) {
            if ((uint32_t)i + 1u + n > len) {
                return OD_ERR_SEQ;
            }
            i += 1u + n;
            continue;
        }
        if (i + 1u >= len) {
            return OD_ERR_SEQ;
        }
        uint8_t esc = seq[i + 1u];
        if (esc == OD_EPD_SEQ_END) {
            return OD_OK;
        }
        if (esc == OD_EPD_ESC_WAVE) {
            if (i + 2u >= len) {
                return OD_ERR_SEQ;
            }
            uint8_t cnt = seq[i + 2u];
            if (cnt == 0u || cnt > 32u ||
                (uint32_t)i + 3u + (uint32_t)cnt * 2u > len) {
                return OD_ERR_SEQ;
            }
            i += 3u + (uint16_t)cnt * 2u;
            continue;
        }
        int args = esc_arg_bytes(esc);
        if (args < 0) {
            /* Unknown escape -- including the reserved 0x80+ length-carrying
             * range, which this executor does not implement either. Refusing
             * the WHOLE sequence here (nothing has touched the panel yet) is
             * the contract behind OD_ERR_DRIVE_SEQ_UNSUPPORTED. */
            return OD_ERR_UNSUP;
        }
        if ((uint32_t)i + 2u + (uint32_t)args > len) {
            return OD_ERR_SEQ;
        }
        /* Known but not implemented by THIS io table. */
        if ((esc == OD_EPD_ESC_SET_PIN && e->io->set_pin == NULL) ||
            (esc == OD_EPD_ESC_CS_SELECT && e->io->cs_select == NULL) ||
            esc == OD_EPD_ESC_READ_SCRATCH ||
            esc == OD_EPD_ESC_WRITE_SCRATCH) {
            return OD_ERR_UNSUP;
        }
        if (esc == OD_EPD_ESC_IMAGE_SLICE) {
            uint8_t part = seq[i + 2u];
            uint8_t of = seq[i + 3u];
            if (of == 0u || part >= of) {
                return OD_ERR_SEQ;
            }
        }
        if (esc == OD_EPD_ESC_SET_PIN && seq[i + 3u] > 2u) {
            return OD_ERR_SEQ;      /* level is 0, 1 or 2 (Hi-Z) */
        }
        i += 2u + (uint16_t)args;
    }
    /* Running off the end without an END escape is legal -- the runtime walk
     * treats it as end-of-sequence, so the validator must agree with it. */
    return OD_OK;
}

/* Stream the active pass's waveform straight to the panel.
 *
 * vsn1 patching happens here rather than at authoring time because the same
 * waveform slot is shared by more than one pass with DIFFERENT patches: the
 * 16-colour mode's passes 1 and 2 both use the edit waveform but patch byte 3
 * to 0x14 and 0x41 respectively. Storing them pre-patched would cost a second
 * 535-byte copy in the config to hold one differing byte. */
static int stream_lut(od_epd_t *e, uint8_t slot, uint8_t vsn1,
                      uint16_t start, uint16_t len)
{
    uint8_t buf[LUT_CHUNK];
    uint16_t off = start;
    uint16_t end = (uint16_t)(start + len);

    while (off < end) {
        uint16_t want = (uint16_t)((uint16_t)(end - off) < (uint16_t)LUT_CHUNK
                                 ? (uint16_t)(end - off) : (uint16_t)LUT_CHUNK);
        uint16_t got = e->io->lut_read(slot, off, buf, want);
        if (got != want) {
            return OD_ERR_LUT;
        }
        /* The vsn1 patch targets ABSOLUTE slot offset 3 -- also when the slot
         * is streamed in register-sized slices (LUT_SLICE). */
        if (vsn1 != 0u && off <= 3u && (uint16_t)(off + got) > 3u) {
            buf[3u - off] = vsn1;
        }
        e->io->data(buf, (size_t)got);
        off += got;
    }
    return OD_OK;
}

/* Forward-only walk over a length-prefixed sequence.
 *
 * Deliberately NOT a bytecode VM. Sequences arrive in the config, which is
 * parsed pre-auth, so this is untrusted input. There are no jumps, no loops and
 * no addressing: the walk only moves forward, every step is bounded by the
 * sequence length, and the step count is capped. The worst a malformed sequence
 * can do is clock out nonsense and time out -- which the raw-SPI debug command
 * can already do deliberately.
 *
 * Returns OD_OK at end of sequence, 1 when it paused on the image marker, or a
 * negative error. `e->seq_pos` is where to resume.
 */
static int run_sequence(od_epd_t *e, const uint8_t *hdr, bool have_hdr,
                        uint8_t slot, uint8_t vsn1, uint16_t lut_len)
{
    const uint8_t *seq = e->seq;
    uint16_t len = e->seq_len;
    uint16_t i = e->seq_pos;
    uint16_t steps = 0;
    uint8_t  buf[32];

    while (i < len) {
        if (++steps > OD_EPD_MAX_SEQ_STEPS) {
            e->seq_pos = i;
            return OD_ERR_SEQ;
        }
        uint8_t n = seq[i];

        if (n != OD_EPD_ESC) {
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

        if (esc == OD_EPD_SEQ_END) {
            e->seq_pos = len;
            return OD_OK;
        }
        if (esc == OD_EPD_ESC_BUSY) {
            if (!od_epd_wait_idle(e, e->desc->busy_timeout_ms)) {
                return OD_ERR_BUSY;
            }
            i += 2u;
            continue;
        }
        if (esc == OD_EPD_ESC_DELAY) {
            if ((uint32_t)i + 4u > len) {
                return OD_ERR_SEQ;
            }
            e->io->delay_ms((uint32_t)seq[i + 2u] | ((uint32_t)seq[i + 3u] << 8));
            i += 4u;
            continue;
        }
        if (esc == OD_EPD_ESC_IMAGE) {
            e->img_part = 0u;
            e->img_of = 1u;
            e->seq_pos = (uint16_t)(i + 2u);
            return 1;            /* caller streams the frame, then resumes */
        }
        if (esc == OD_EPD_ESC_IMAGE_SLICE) {
            if ((uint32_t)i + 4u > len) {
                return OD_ERR_SEQ;
            }
            e->img_part = seq[i + 2u];
            e->img_of = seq[i + 3u];
            e->seq_pos = (uint16_t)(i + 4u);
            return 1;            /* caller streams frame_bytes / img_of bytes */
        }
        if (esc == OD_EPD_ESC_SET_PIN) {
            if ((uint32_t)i + 4u > len) {
                return OD_ERR_SEQ;
            }
            if (e->io->set_pin == NULL ||
                !e->io->set_pin(seq[i + 2u], seq[i + 3u])) {
                /* Validation keeps a NULL hook from ever landing here; a false
                 * return means the pin reference is unwired on this board. */
                return OD_ERR_UNSUP;
            }
            i += 4u;
            continue;
        }
        if (esc == OD_EPD_ESC_CS_SELECT) {
            if ((uint32_t)i + 3u > len) {
                return OD_ERR_SEQ;
            }
            if (e->io->cs_select == NULL || !e->io->cs_select(seq[i + 2u])) {
                return OD_ERR_UNSUP;
            }
            i += 3u;
            continue;
        }
        if (esc == OD_EPD_ESC_LUT) {
            /* Loud, not silent: a mode whose sequence uploads a waveform but
             * whose pass resolved no slot would otherwise refresh with whatever
             * the panel had in its waveform register. */
            if (!have_hdr) {
                return OD_ERR_LUT;
            }
            int rc = stream_lut(e, slot, vsn1, 0u, lut_len);
            if (rc != OD_OK) {
                return rc;
            }
            i += 2u;
            continue;
        }
        if (esc == OD_EPD_ESC_LUT_SLICE) {
            if ((uint32_t)i + 6u > len) {
                return OD_ERR_SEQ;
            }
            uint16_t s_off = (uint16_t)(seq[i + 2u] | ((uint16_t)seq[i + 3u] << 8));
            uint16_t s_len = (uint16_t)(seq[i + 4u] | ((uint16_t)seq[i + 5u] << 8));
            if (!have_hdr) {
                return OD_ERR_LUT;
            }
            if ((uint32_t)s_off + s_len > lut_len) {
                return OD_ERR_SEQ;   /* slice runs past the stored slot */
            }
            int rc = stream_lut(e, slot, vsn1, s_off, s_len);
            if (rc != OD_OK) {
                return rc;
            }
            i += 6u;
            continue;
        }
        if (esc == OD_EPD_ESC_WAVE) {
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
                       ? wave_byte(hdr, have_hdr, p[k * 2u + 1u])
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
    e->seq_pos = len;
    return OD_OK;
}

/* -------------------------------------------------------------------- API */

int od_epd_init(od_epd_t *e, const od_panel_desc_t *d, const od_epd_io_t *io)
{
    if (!e || !d || !io || !io->cmd || !io->data || !io->reset || !io->delay_ms ||
        !io->busy_level || !io->seq_read || !io->mode_read) {
        return OD_ERR_ARG;
    }
    memset(e, 0, sizeof(*e));
    e->desc = d;
    e->io = io;
    return OD_OK;
}

int od_epd_select_mode(od_epd_t *e, uint8_t mode)
{
    if (!e || mode >= e->desc->mode_count || !e->io->mode_read) {
        return OD_ERR_MODE;
    }
    if (!e->io->mode_read(mode, &e->active)) {
        return OD_ERR_MODE;
    }
    e->mode = mode;
    e->pass = 0;
    e->paused = false;
    return OD_OK;
}

int od_epd_begin_pass(od_epd_t *e, uint8_t pass)
{
    const od_epd_mode_t *m = &e->active;
    uint8_t  hdr[7];
    bool     have_hdr = false;
    uint16_t lut_len = 0;
    uint8_t  slot;
    uint8_t  vsn1;

    if (pass >= m->passes) {
        return OD_ERR_MODE;
    }
    if (m->seq_len == 0u || m->seq_len > OD_EPD_SEQ_MAX) {
        return OD_ERR_SEQ;
    }
    e->pass = pass;
    e->paused = false;

    /* Pull this pass's recipe out of NVM3. One read, 128 B max. */
    e->seq_len = e->io->seq_read(e->mode, 0u, e->seq, m->seq_len);
    if (e->seq_len != m->seq_len) {
        return OD_ERR_SEQ;
    }
    e->seq_pos = 0u;

    /* Capability + structure pre-scan BEFORE any panel I/O. A sequence using an
     * escape this target does not implement is refused wholesale here -- no
     * reset, no power, nothing half-executed (wire: SEQ_UNSUPPORTED). */
    {
        int vrc = od_epd_validate(e);
        if (vrc != OD_OK) {
            return vrc;
        }
    }

    /* Resolve this pass's waveform. Only the 7-byte analog header is read into
     * RAM -- the 528-byte body is streamed later, at the marker. */
    slot = m->lut_slot[pass];
    vsn1 = m->vsn1[pass];
    if (slot != 0xFFu && e->io->lut_info && e->io->lut_read &&
        e->io->lut_info(slot, &lut_len)) {
        if (lut_len < 7u) {
            return OD_ERR_LUT;   /* too short to carry an analog header */
        }
        if (e->io->lut_read(slot, 0u, hdr, 7u) != 7u) {
            return OD_ERR_LUT;
        }
        if (vsn1 != 0u) {
            hdr[3] = vsn1;
        }
        have_hdr = true;
    }

    e->io->reset(10);
    if (!od_epd_wait_idle(e, e->desc->busy_timeout_ms)) {
        return OD_ERR_BUSY;
    }

    int rc = run_sequence(e, hdr, have_hdr, slot, vsn1, lut_len);
    if (rc < 0) {
        return rc;
    }
    if (rc == 1) {
        e->paused = true;        /* stopped on the image marker, as expected */
        return OD_OK;
    }
    /* A sequence with no image marker is a programming error in the config,
     * not something to paper over: the frame would have nowhere to go. */
    return OD_ERR_SEQ;
}

int od_epd_write_image_chunk(od_epd_t *e, const uint8_t *buf, size_t n)
{
    if (!e || (!buf && n)) {
        return OD_ERR_ARG;
    }
    if (!e->paused) {
        return OD_ERR_STATE;
    }
    if (n) {
        e->io->data(buf, n);   /* streamed; never buffered */
    }
    return OD_OK;
}

int od_epd_next_plane(od_epd_t *e)
{
    /* Panels that take two 1bpp planes (3-colour black/white/red) have a second
     * image command between them -- bb_epaper spelled this startWrite(PLANE_1).
     * Here it is a second image marker in the same sequence, so the register
     * that opens plane 1 is config data like every other byte.
     *
     * No panel in this branch uses it: the three BWRY panels are single-plane
     * 2bpp. It exists so a 3-colour panel is EXPRESSIBLE in a sequence rather
     * than blocked on a driver change. */
    if (!e || !e->paused) {
        return OD_ERR_STATE;
    }
    e->paused = false;
    int rc = run_sequence(e, NULL, false, 0xFFu, 0u, 0u);
    if (rc < 0) {
        return rc;
    }
    if (rc != 1) {
        return OD_ERR_SEQ;      /* caller wanted another plane; sequence has none */
    }
    e->paused = true;
    return OD_OK;
}

int od_epd_finish_pass(od_epd_t *e)
{
    if (!e || !e->paused) {
        return OD_ERR_STATE;
    }
    e->paused = false;
    /* The tail -- power-on, refresh opcode, BUSY wait -- is the remainder of
     * the same sequence. A panel needing a different refresh command or an
     * extra register poke after the frame needs no code change here. */
    int rc = run_sequence(e, NULL, false, 0xFFu, 0u, 0u);
    return (rc < 0) ? rc : OD_OK;
}

int od_epd_sleep(od_epd_t *e)
{
    if (!e || e->desc->sleep_seq_len == 0u) {
        return OD_OK;            /* panel declares no sleep sequence */
    }
    if (e->desc->sleep_seq_len > OD_EPD_SLEEP_SEQ_MAX) {
        return OD_ERR_SEQ;
    }
    memcpy(e->seq, e->desc->sleep_seq, e->desc->sleep_seq_len);
    e->seq_len = e->desc->sleep_seq_len;
    e->seq_pos = 0u;
    e->paused = false;
    int rc = run_sequence(e, NULL, false, 0xFFu, 0u, 0u);
    return (rc < 0) ? rc : OD_OK;
}
