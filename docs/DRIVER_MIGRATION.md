# Config-driven display driver + protocol additions — review before release

**Status:** implemented and running on hardware for the **Silabs (EFR32BG22)** target only.
Firmware / NRF54 / NRF52811 are unchanged and unaffected.
**Audience:** anyone who has to sign this off, or who maintains a host that talks to these devices.

This describes two coupled changes:

1. **the driver migration** — vendored `bb_epaper` is gone; the panel is now driven entirely
   from data in the device's own config;
2. **the protocol additions** that exist to carry that data — four config packets, one
   command, one colour-scheme value.

They are coupled because the firmware no longer contains a panel table. A device running
this build with no drive data in its config **cannot drive its panel at all**. That is the
single most important operational consequence, and [Migration](#6-migration) is about
sequencing around it.

---

## 1. Why

`bb_epaper` carried a compiled-in table of panel quirks: init register sequences, waveform
placement, BUSY semantics, colour code ordering. Adding a panel meant a firmware change, and
every target carried every panel. Three concrete failures came out of that model:

- **Panel data and firmware drifted.** The 2.9" BWRY panel had `yellow` and `red` crossed in
  the vendored table for months because the table said one thing and the hardware did another.
- **Quirks became driver opinions.** The 7.5" GDEM075F52 *silently discards* a waveform
  uploaded before power-on, while the 2.9" and 4.2" require it before power-on. Under the old
  model that was a `lut_placement` enum the driver had to branch on — i.e. the driver had to
  know which panel it was talking to.
- **No framebuffer, but a framebuffer API.** The driver exposed `fillScreen`, `drawPixel` etc.
  against RAM these parts do not have.

The new model: **the firmware knows how to execute a sequence; the config knows what the
sequence is.** The firmware has no notion of which panel is attached beyond what it reads
back from NVM3.

---

## 2. The driver

### 2.1 What was removed

- `bb_ep.inl`, `bb_epaper` panel tables, and the framebuffer-shaped API.
- `silabs_efr32_io.inl`'s dependence on `BBEPDISP` (it read pin state out of that struct in
  61 places); rebased onto `od_epd_io_t`.
- The `lut_placement` enum (see 2.3).
- IT8951 framing, `BBEP_CS_EVERY_BYTE`, and chip-type-based BUSY polarity.

Verification: `nm` over the linked ELF reports **0** symbols matching `bbep`/`bb_ep`.

### 2.2 New components

| File | Role |
|---|---|
| `include/od_epd.h`, `src/od_epd.c` | panel-agnostic sequence executor |
| `src/od_epd_io_silabs.c` | EFR32 HAL behind `od_epd_io_t` |
| `src/od_epd_adapter.cpp` | joins `od_epd` to the config index |
| `od_cfg_index.c/.h` | 256-byte RAM map of packet offsets in the NVM3 blob |
| `panels/panels.yaml` | **the panel database — now host-side, not firmware-side** |

`panels.yaml` is the source of truth. `py-opendisplay`'s `packets_from_yaml()` compiles it
into wire packets. The firmware never sees the YAML.

### 2.3 Sequence encoding — the load-bearing design decision

A mode's whole recipe for one pass is a byte string:

```
[len][cmd][data...]        len counts cmd+data, len >= 1
0x00 0x01                  wait for BUSY to clear
0x00 0x02 [n] [pairs...]   step with waveform-header substitutions
0x00 0x03                  STREAM THE IMAGE HERE   (executor pauses)
0x00 0x04                  STREAM THE WAVEFORM HERE
0x00 0x05 [lo] [hi]        wait N ms (little-endian)
0x00 0x00                  end of sequence
```

Zero length is otherwise impossible (a step always has a command byte), so it is free as an
escape.

**One sequence is the whole recipe for a pass** — registers, waveform upload, image, power-on,
refresh. This is what let the `lut_placement` enum be deleted: waveform placement is now *the
position of the `0x00 0x04` marker in the sequence*. The 7.5" puts it after its booster tail;
the 2.9" and 4.2" put it before power-on. The driver has no opinion and needs none.

The explicit delay step (`0x00 0x05`) exists because a BUSY wait is not a substitute. The
4.2" GDEM042F52's BUSY line is unusable and it needs a **2 s** settle after power-on and a
**35 s** wait after refresh — one per-panel fixed delay cannot be both.

> **Reviewers:** this is the part to attack. If the encoding is wrong, every panel is wrong.
> `panels/seq_conformance.c/.py` cross-checks the C executor against a Python model.

---

## 3. Protocol additions

Nothing existing changed shape. Four packets and one command are **added**; one colour-scheme
value is **assigned**. No existing packet, command or field was resized, renumbered or
repurposed.

Be precise about what that buys, though: an **older host** still pairs, authenticates, reads
and writes config against this firmware, and an **older config** still parses. What an older
host cannot do is *produce drive data* — so the panel will not render until something writes
`0x31`/`0x2E`/`0x2F`. This build says so rather than guessing around it. "Backward compatible"
applies to the wire format, not to the ability to drive a panel.

### 3.1 Config packets

Wire layout is unchanged in form: `[packet_number][packet_type][body]`. **Identity is
`instance_number` at `body[0]`, not the header byte** — the header byte is a don't-care that
different writers already disagree about.

| ID | Name | Body | Repeat | Consumed by |
|---|---|---|---|---|
| `0x2E` | `display_mode` | 157 | per mode | device + host |
| `0x2F` | `display_lut` | 541 | per waveform slot | device + host |
| `0x30` | `display_calibration` | 116 | per multi-pass mode | **host only** |
| `0x31` | `display_panel` | 36 | per display | device + host |

- **`0x2E display_mode`** — `mode_index`, `mode_name[12]`, `passes`, `lut_slot[4]`,
  `vsn1[4]`, advisory `temp_min_c`/`temp_max_c`/`temp_policy`, `seq_len`, `seq[128]`.
- **`0x2F display_lut`** — `lut_slot`, `lut_len`, `crc16`, `data[535]`. **A slot failing CRC
  is treated as ABSENT, not as garbage**, so a corrupt waveform degrades to "no custom
  waveform" rather than driving the panel with noise.
- **`0x30 display_calibration`** — `mode_index`, entry count, `measured` flag, `codes[16]`
  (per-pass 2-bit codes packed one byte per entry), `lab[16][3]` (CIELAB ×100, int16 LE).
  **The device skips this packet**; it exists so a host image pipeline can quantise against
  real measured colour. Encoding and measurement live in *one* packet deliberately: a
  measurement whose codes are unknown cannot be reproduced, and codes with no measurement
  cannot be chosen between.
- **`0x31 display_panel`** — `panel_flags`, `busy_idle_level`, `busy_timeout_ms`,
  `busy_fixed_delay_ms`, `code_map[4]`, `mode_count`, `sleep_seq_len`, `sleep_seq[16]`.
  This packet exists because BUSY policy, BUSY idle level and `code_map` were **never on the
  wire** — they only existed in the firmware's panel table. Without it the driver is
  unimplementable from config.

### 3.2 `CMD_DRIVE_WRITE_START` = `0x0085`

```
request   [0x00][0x85][mode:1][refresh:1][flags:1][rsv:2]
                [uncompressed_size:4 LE]   iff flags bit0 COMPRESSED
                [first data bytes...]
response  [0x00][0x85][mode][passes]
error     [0xFF][0x85][err:1][mode:1]
```

| flag | meaning |
|---|---|
| bit0 `COMPRESSED` | payload carries `uncompressed_size` + zlib stream |
| bit1 `PIPELINED` | data via `0x0081` windowed frames. **Firmware target only** — Silabs/NRF54/NRF52811 must leave clear |
| bits2–7 | reserved, must be 0 |

Errors: `01` BAD_ARGS, `02` NO_CONFIG, `03` BAD_MODE, `04` BAD_SIZE, `05` UNSUPPORTED.

**Why not reuse `0x0070`.** The mode must be known at START, because this driver uploads the
waveform as part of the init sequence — *before* the frame. `0x0072` END does carry a
`refresh` byte, but it arrives after the pixels are already in the panel; that was a leftover
from bb_epaper re-sending init at refresh time. `refresh` is carried at START here, where it
can still affect anything.

**Multi-pass is ONE transfer, not three.** A 3-pass mode drives the panel three times with
different pixel data. The host declares `passes × frame_bytes` and streams the frames back to
back; the device advances pass at each frame boundary. One session means one zlib stream
across all three frames — which matters, since consecutive passes of the same image are highly
correlated — and keeps the panel rail up without a keep-power flag.

The `passes` value in the ACK is read **from the config**, so a host need not duplicate the
mode table. `rsv` is deliberately spare; the next thing likely to need it is per-upload
temperature.

### 3.3 `OD_COLOR_SCHEME_BWRY16 = 10`

Packs **identically to BWRY (3)**: 2bpp, one plane. The difference is the number of frames,
which comes from the mode's pass count in `0x2E`, not from this value. It exists so a host
image pipeline can select the 16-entry palette; the device needs only the mode.

**Value 10, not 9.** The designated mirror (`epaper-dithering` `palettes.rs`) already uses 9
for a library-local `Grayscale8` that is deliberately not a wire value.

> ⚠️ **This value must not be written into `DisplayConfig 0x20 color_scheme`.** The panel's
> pixel format is BWRY; 16-colour is a property of a *mode*. Writing 10 there made every
> 4-colour consumer mis-render — the web tool's `clampScheme()` floored it to 6 (16-level
> grey) and the designer collapsed it to 0 (mono), both silently. Host guards were added, but
> the rule is the fix.

---

## 4. Storage

| Setting | Was | Now |
|---|---|---|
| `NVM3_DEFAULT_NVM_SIZE` | 24576 (3 pages) | **32768** (4 pages) |
| `NVM3_DEFAULT_MAX_OBJECT_SIZE` | 2112 | **4096** |
| `MAX_CONFIG_SIZE` | 2096 | **4080** |

**These three move together.** `nvm3_open()` rejects when `nvmSize < thrSize`, and `thrSize`
scales with max-object: `thrSize(2112) = 20588`, `thrSize(4096) = 24556`. Raising max-object
to 4096 while the region was still 24576 cleared that by **20 bytes** and failed in practice —
NVM3 would not open, config read back as 0 bytes, every write NACKed.

**Growing the region is non-destructive.** The region is pinned to `__main_flash_end__` and
grows *downward* (0x50000 → 0x4E000, end fixed at 0x56000), so existing pages stay in range
with valid headers. Verified on ODBB1B76; two further units were migrated and authenticated
afterwards with their existing keys. *(The header comment claiming otherwise was stale and has
been corrected in this change.)*

**4080 is a hard wall.** `NVM3_MAX_OBJECT_SIZE_HIGH_LIMIT` (4096) is enforced inside
`nvm3_open()` regardless of region size, so adding pages buys the config nothing.

**What actually fits.** Waveforms dominate at 541 B each. The lever is *sharing slots across
displays*, not variable-length packets:

| displays | per-display waveform copies | shared slots |
|---|---|---|
| 1 | 2087 | 2087 |
| 2 | 3529 | 2443 |
| 3 | **4971 (over)** | 2799 |
| 4 | — | 3155 |
| 5 | — | 3511 |

Measured dead ends: variable-length `0x2E`/`0x2F` saves ~6% and does not change the display
count; trimming trailing-idle waveform bytes saves nothing and would be *unsafe* (streaming
fewer bytes than the panel's waveform register holds leaves stale data in the tail).

Observed real configs: 2205–2335 B for one display with two modes.

---

## 5. Host-side changes

| Repo | Change |
|---|---|
| `py-opendisplay` | models + byte-exact round-trip for `0x2E/0x2F/0x30/0x31`; `encoding/multipass.py` |
| `opendisplay.org` | `0x0085` sender, multi-pass encoder, Drive Mode selector, 16-colour dither path |
| `epaper-dithering` | `ColorScheme::Bwry16 = 10` across the Rust / Python / TypeScript mirrors |

Two host-side rules worth calling out, because both were learned by rendering the wrong thing:

- **Palette codes are PANEL codes. Do not apply `code_map` to them.** `code_map` (`0x31`)
  turns a *logical* BWRY colour into a panel code for the 4-colour path. Applying it to the
  per-pass codes in `0x30`, which are already panel codes, maps 2→3 and 3→2 and swaps yellow
  and red in every pass. `encode_frames()` now raises if given a `code_map`.
- **A multi-pass upload must be dithered against the 16-colour palette, not the 4-ink one.**
  Quantising the canvas to BWRY and then multi-pass-encoding the painted canvas throws away 12
  of the 16 colours before the encoder runs.

---

## 6. Migration

**Order matters.** A device on this firmware with no drive packets cannot drive its panel.

```
1. Read the existing config off the device and keep it.
2. OTA.                        # device now has no panel until step 3
3. Write the config with 0x31 + 0x2E + 0x2F (+ 0x30 for multi-pass modes).
4. Verify: read back, confirm modes/luts/panel are present.
5. Drive one frame and LOOK AT THE PANEL.
```

**Insert new packets ahead of any packet type the reader does not know — never append.**
A TLV reader cannot skip an unknown type (it cannot know its length), so it must abandon the
rest of the blob. `py-opendisplay` does not model `0x2D findmy_config` (128 B body, 130 B on
the wire), which some units carry; appending drive packets after it put 1560 B behind a wall
and the config read back as *no drive data at all*. The firmware and the web tool both know
`0x2D`; py-opendisplay is the odd one out.

**Timeouts must be sized for the slowest panel.** The 4.2" has an unusable BUSY line and runs
on a 35 s fixed delay, against the 2.9"'s ~20.6 s refresh. A pass boundary is a refresh, not a
stall, and it can land on **any** chunk. Two host bounds were silently too short and aborted
healthy uploads.

**Config blob header:** the first two bytes are padding/reserved, not a length. The firmware
takes `data_len` from the NVM3 object header and skips a 3-byte preamble. Hosts that treat
those bytes as a length will log a spurious mismatch.

---

## 7. What was verified on hardware

| Check | Result |
|---|---|
| `bb_epaper` symbols in ELF | 0 |
| 16-colour render, 2.9" (ODBB1B76) | 3 passes, 48384 B, photographed as a clean 4×4 grid |
| 16-colour render, 2.9" (ODBF2156) | 3 passes, 48384 B, END + refresh notification |
| 16-colour render, 4.2" (ODCF0ED5) | 3 passes, 90000 B, END + refresh notification |
| NVM3 growth preserves key + config | 3 units migrated, all authenticated afterwards |
| Config read-back | byte-exact after the `pipe_send_raw` retry fix |
| Sequence executor | `seq_conformance` C vs Python model |
| Config index | `panels/cfgtest/`, mutation-tested |

**Caveat the reviewers should weigh:** only ODBB1B76 has been confirmed *photographically*.
The other two were confirmed at protocol level only — the rig camera is not aimed at them.
In this project, protocol-level success has repeatedly reported a working transfer while the
panel showed something wrong.

---

## 8. Known gaps and open questions

**Blocking-ish (decide before release):**

1. **Yellow/red palette index conflict is unresolved.** `code_map [0,1,3,2]` implies panel
   code 2 = red and 3 = yellow, which matches the nominal palette's labels. A photo
   measurement of the same grid says the opposite for entries 2 and 3. Transpose and mirror
   hypotheses were both tested and fail. The measurement cannot arbitrate because the script
   white-balances against the palette's own white entry, so that cell is `L*100` *by
   construction* and carries no orientation information. **Decisive test not yet run:**
   render four full-screen flat bands of panel codes 0/1/2/3 and photograph — no palette, no
   grid orientation, no white-balance circularity.
2. **Two units ship a borrowed palette.** ODBF2156 and ODCF0ED5 carry ODBB1B76's CIELAB with
   `measured=0`. For ODBF2156 that is cross-*unit* (same part number; measured spread
   dE00 6.40). For ODCF0ED5 it is cross-*model* — different glass, pigment and waveforms —
   and should be treated as a placeholder, not an approximation.

**Non-blocking but should be written down:**

3. **Compressed multi-pass hides frame boundaries.** `frameBytes` counts uncompressed bytes;
   compressed chunks are in different units, so the host cannot locate the boundary. Current
   behaviour: use the wide ack window for every chunk of a compressed multi-pass transfer. A
   genuine stall then takes 90 s rather than 15 s to surface. Proper fix is to sync-flush
   deflate at frame boundaries so the compressed offsets are known; it touches the wire stream
   and was deliberately deferred.
4. **`0x30` is host-only.** The device parses past it and ignores it. If a future firmware
   wants temperature-compensated palettes, this is where it would start consuming it.
5. **`PIPELINED` (flags bit1) is unimplemented outside the Firmware target** and NACKs with
   `UNSUPPORTED`.
6. **The vendored `epaper-dithering` JS bundle has not been rebuilt**, so `getPalette(10)` is
   absent from it. The web tool deliberately does not depend on that: it dithers 16-colour
   content through the custom-`ColorPalette` path against the *device's* measured palette,
   which is the more correct design anyway. Rebuilding needs a `wasm32-unknown-unknown`
   toolchain.
7. **Only the Silabs target is migrated.** Firmware / NRF54 / NRF52811 still use the old path
   and are unaffected by this change.
8. **Boot-screen latch bug**, found and fixed during this work: `s_boot_applied` was set
   *before* the attempt, so a device that booted without drive packets — the normal state
   immediately after an OTA — skipped its boot screen and could never redraw it, defeating the
   deliberate retry from the config-reload path. Devices need this build for the retry to
   work; on older builds, power-cycle after writing the config.
   **Follow-on:** making that retry real exposed an ordering bug — the redraw sat in front of
   the config-write ACK, so a ~20–35 s panel refresh pushed the ACK past py-opendisplay's 5 s
   bound and a *successful* write reported as a timeout. The index rebuild still precedes the
   ACK (the host may send another command immediately); the redraw now follows it.
9. **`panels.yaml` covers four panels**: 0x001E (2.9" 168×384), **0x001C (2.6" 184×360)**,
   0x003A (4.2" 400×300), 0x0040 (7.5" 800×480). Any other `panel_ic_type` has no drive data
   and therefore no panel on this firmware — including presets the toolbox still offers. The
   2.6" entry is **derived** from the 2.9" (bb_epaper's two init sequences are byte-identical
   except TRES) and its waveforms are inherited, not yet validated on that glass.
10. **Writing a config is not idempotent at the host level.** Re-running a splice that appends
    drive packets produces a *second* copy (observed: 1717 → 3277 B), and `find_mode`/`find_lut`
    would serve whichever they reach first. Tooling must clear existing
    `0x2E`/`0x2F`/`0x30`/`0x31` before adding new ones.

---

## 9. What I most want reviewed

1. **The sequence encoding** (§2.3). Everything else is replaceable; this is the contract.
2. **`0x31`'s field set.** It exists because those facts were never on the wire. If something
   is still missing, now is the time — it is `@repeatable max=4` with 7 reserved bytes.
3. **The `0x0085` ACK carrying `passes` from config.** It removes a host-side mode table, but
   it also means a host cannot validate the pass count before sending. Deliberate; worth a
   second opinion.
4. **The 4080 B ceiling against real multi-display deployments.** The sharing table in §4 says
   5 displays fit. Nobody has built a 5-display config.
5. **Whether `0x30` should be device-visible.** It is host-only today.
