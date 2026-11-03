# Config-driven display driver + protocol additions — review before release

**Revision 2** — response to the first review round (Nic, 2026-11-03). Rev 1 described
what was implemented; **Rev 2 additionally describes what was *agreed in review* and is
queued for implementation before release.** Everything queued is marked **[QUEUED]**;
everything else is running on hardware today. §0 lists what changed and why.

**Status:** implemented and running on hardware for the **Silabs (EFR32BG22)** target only.
Firmware / NRF54 / NRF52811 are unchanged and unaffected.
**Audience:** anyone who has to sign this off, or who maintains a host that talks to these
devices. Rev 2 was written with the Solum M3 (nRF52811) tags and dual-controller Spectra 6
panels explicitly in scope, since that is where the review came from.

This describes two coupled changes:

1. **the driver migration** — vendored `bb_epaper` is gone; the panel is now driven entirely
   from data in the device's own config;
2. **the protocol additions** that exist to carry that data — four config packets, two
   commands, one colour-scheme value, and (new in Rev 2) a length-prefixed packet format
   and six sequence escapes.

They are coupled because the firmware no longer contains a panel table. A device running
this build with no drive data in its config **cannot drive its panel at all**. That is the
single most important operational consequence, and [Migration](#7-migration) is about
sequencing around it.

---

## 0. Changes in revision 2

Each item cites the review point (R1–R9 from `driver-migration-review.md`; R10 is the
pin-level note sent separately).

| # | Review point | Decision |
|---|---|---|
| R1 | dual-controller panels: CS switching, split image streams | **Accepted.** New escapes: `select CS` (`0x08`) and `stream image part k of n` (`0x07`). The Solum 5.85" single-CS `cmd\|0x80` case needs only `0x07`. §2.3. |
| R2 | UC81xx waveform spans registers `0x20`–`0x24` | **Clarified + extended.** Yes: one `0x04` marker streams the *whole* slot to the preceding command. For multi-register waveforms a slot now holds the concatenation and a new `stream slot slice` escape (`0x0B`) sends ranges — one slot per pass still suffices. §2.3. |
| R3 | no read step | **Accepted, spec-only this round.** `read N bytes into scratch` (`0x09`) and `write command with data from scratch` (`0x0A`) are specified; no target implements them yet, and a sequence containing them is *refused cleanly* (see validation rule, §2.4). §2.3, open question §9.1. |
| R4 | where does temperature come from | **Decided: the device enforces.** `temp_policy` is executed by the firmware, which may substitute a mode; the `0x0085` ACK grows `temp_c` and reports the mode *actually used*. §3.3. |
| R5 | fixed 541 B LUT packet | **Accepted via R9's sibling proposal.** The length-prefixed packet format (`config-length-prefix-proposal.md`) is **adopted**; `0x2E`–`0x31` are renumbered to `0xAE`–`0xB1` with real-length bodies. **[QUEUED]** — the deployed fleet still runs the fixed packets. §3.1, §5. |
| R6 | a mode has no colour scheme | **Accepted.** `0xAE display_mode` gains a `color_scheme` byte (`0xFF` = inherit from `0x20`). §3.1. |
| R7 | 4-pass / 16-entry limits | **Lifted by the length prefix.** `passes ≤ 8`, per-pass arrays sized by `passes`; calibration entries ≤ 32 with 2 code bytes per entry. §3.1. |
| R8 | device identification command | **Accepted.** `CMD_READ_IDENT = 0x0046`, plaintext, answers from `0x20` config (Silabs) or factory data (Solum, later). **[QUEUED]**. §3.4. |
| R9 | canonical spec first | **Accepted as a release gate.** `0x2D`, the new packets, `0x0085`, `0x0046` and scheme 10 land in `opendisplay-protocol` (the multi-language canonical repo) *before* anything ships. §8 checklist. |
| R10 | set-pin-level step (BS select, EEPROM CS, 3-wire mode switch) | **Accepted, implement now.** `0x06 [pin_ref] [level]`, where `pin_ref` indexes pins *in the config* (the six spare pin bytes in `0x20` become `aux_pin_1..6` — a rename of reserved bytes, **no size change**), so a `panels.yaml` sequence stays board-independent exactly as requested. §2.3. |

Also changed: the "insert, never append" rule is retired for new packet types (replaced by
the length-prefix rule, §7); the §5 storage claim about variable-length packets saving "only
~6%" is corrected — that number was measured on UC8151-class 535 B waveforms and does not
transfer to SSD16xx-class ~80 B waveforms on a 512 B-config nRF52811 (R5).

**Not yet re-verified on hardware:** every §6 hardware result predates Rev 2 and was
produced with the fixed `0x2E`–`0x31` packets. The renumbering invalidates none of the
*driver* verification (the executor and sequences are unchanged) but all five configs must
be rewritten and re-verified. That is the top entry in the release checklist.

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

A mode's whole recipe for one pass is a byte string of steps:

```
[len][cmd][data...]        len counts cmd+data, len >= 1
```

Zero length is otherwise impossible (a step always has a command byte), so `0x00` is free
as an escape prefix. Escapes, Rev 2 set (`0x06`–`0x0B` are new):

```
0x00 0x00                       end of sequence
0x00 0x01                       wait for BUSY to clear
0x00 0x02 [n] [pairs...]        step with waveform-header substitutions
0x00 0x03                       STREAM THE WHOLE IMAGE HERE (executor pauses)
0x00 0x04                       STREAM THE WHOLE WAVEFORM SLOT HERE
0x00 0x05 [lo] [hi]             wait N ms (little-endian)
0x00 0x06 [pin_ref] [level]     set a config-referenced pin          (R10)  [QUEUED]
0x00 0x07 [part] [of]           stream image slice part/of           (R1)   [QUEUED]
0x00 0x08 [cs_sel]              route following steps to CS n        (R1)   [QUEUED, spec-only]
0x00 0x09 [pin_ref] [len]       read len bytes into scratch          (R3)   [spec-only]
0x00 0x0A [cmd] [off] [len]     send cmd with data from scratch      (R3)   [spec-only]
0x00 0x0B [off:2] [len:2]       stream a SLICE of the waveform slot  (R2)   [QUEUED]
0x00 0x80..0xFF [len] [..]      reserved: future escapes carry their length
```

**One sequence is the whole recipe for a pass** — registers, waveform upload, image, power-on,
refresh. This is what let the `lut_placement` enum be deleted: waveform placement is now *the
position of the `0x04` marker in the sequence*. The 7.5" puts it after its booster tail; the
2.9" and 4.2" put it before power-on. The driver has no opinion and needs none.

The explicit delay step (`0x05`) exists because a BUSY wait is not a substitute. The 4.2"
GDEM042F52's BUSY line is unusable and it needs a **2 s** settle after power-on and a **35 s**
wait after refresh — one per-panel fixed delay cannot be both.

Notes on the new escapes:

- **`0x06` set pin (R10).** `level` is `0`, `1`, or `2` = release to input/Hi-Z (needed when
  a bidirectional data line must be let go for a 3-wire read-back). `pin_ref` is **not a GPIO
  number** — it indexes pins the config already names, so a `panels.yaml` sequence stays
  board-independent:

  | `pin_ref` | pin |
  |---|---|
  | `0x00`–`0x05` | `aux_pin_1..6` — the six spare pin bytes in `DisplayConfig 0x20` (`reserved_pin_3..8`), now named. Rename only; `0x20` does not change size. |
  | `0x10` | `reset_pin` |
  | `0x11` | `dc_pin` |
  | `0x12` | `cs_pin` |
  | `0x13` | `cs_pin_2` |
  | `0x14` | `data_pin` |
  | `0x15` | `clk_pin` |

  Covers the cited cases: BS high for BWRY / low for 4-wire panels (BS wired to an aux pin),
  the UC8159 toggling BS mid-sequence around its temperature read-back, and the per-panel
  waveform EEPROM's own CS.
- **`0x07` stream image slice (R1).** Streams part `part` (0-based) of `of` equal slices of
  the frame. `0x03` is equivalent to `0x07 0 1`. Slices are defined on the *uncompressed*
  frame, so they compose with a compressed `0x0085` transfer. Covers both split-panel shapes:
  Spectra 6 `BWGBRY_SPLIT` (`0x08` to `cs_pin_2`, then `0x07 1 2`) and the Solum 5.85" —
  two controllers on one CS where the second is addressed as `cmd|0x80`, i.e.
  `w 24` + `0x07 0 2`, `w A4` + `0x07 1 2`, no CS escape needed.
- **`0x08` select CS (R1).** `cs_sel` 0 = `cs_pin`, 1 = `cs_pin_2`. Sticky until changed;
  every sequence starts at 0. Spec-only this round — we have no dual-CS panel wired to this
  firmware to test against (our Spectra 6 rig runs a different driver).
- **`0x09`/`0x0A` read into scratch / write from scratch (R3).** Scratch is 16 bytes, zeroed
  at sequence start. `0x09` clocks `len` bytes in, **sampling `pin_ref` as the data-in line**
  — explicit because the read source varies: `dc_pin` doubles as MISO on some boards, the
  waveform EEPROM has its own MISO on an aux pin. `0x0A` sends `cmd` followed by
  `scratch[off..off+len)`. Together they express the UC8159 EEPROM case (select EEPROM CS via
  `0x06`, read PLL/VCOM via `0x09`, write them into registers via `0x0A`). What they do *not*
  yet express is deriving a waveform from OTP read-back or feeding a temperature into mode
  selection — that needs logic, not a byte stream, and is deliberately left open (§9.1).
- **`0x0B` stream slot slice (R2).** For controllers whose waveform spans several registers
  (UC81xx `0x20`–`0x24`): the slot stores the concatenation, the sequence sends
  `w 20` + `0x0B off len`, `w 21` + `0x0B ...`, … One slot per pass still suffices, so the
  per-pass slot array is not consumed by register count. To answer R2's direct question:
  **one `0x04` streams one whole slot to the preceding command**; `0x0B` is the partial form.

### 2.4 Validation rule (new in Rev 2)

The executor **pre-scans a sequence before touching the panel** and refuses it wholesale —
`0x0085` NACK `07 SEQ_UNSUPPORTED` — if it contains an escape the target does not know *or*
knows but has not implemented (`0x08`/`0x09`/`0x0A` today). A refused sequence does nothing;
there is no partial execution. Reserved escapes `0x80`+ carry a length byte so a future
*optional* escape can be skipped rather than refused, per the convention in the length-prefix
proposal. **[QUEUED]**

> **Reviewers:** §2.3 is still the part to attack. If the encoding is wrong, every panel is
> wrong. `panels/seq_conformance.c/.py` cross-checks the C executor against a Python model
> and will grow cases for `0x06`/`0x07`/`0x0B` with the implementation.

---

## 3. Protocol additions

Nothing existing changed shape. **Rev 2 note:** Rev 1 shipped the new packets as fixed-size
types `0x2E`–`0x31`; review adopted the **length-prefixed packet format** instead and the
packets move to `0xAE`–`0xB1`. The fixed types are hereby **dead on arrival — they never ship
outside our lab** and will be scrubbed from the five lab units. Everything below describes
the `0xA0`-range form. **[QUEUED]**

### 3.0 Length-prefixed packets (adopted from `config-length-prefix-proposal.md`, R5/R9)

- Types `0x00`–`0x7F`: unchanged, `[packet_number][packet_type][fixed body]`. **This range is
  frozen** — no new fixed-size types, ever. (`0x2D findmy_config` is the last.)
- Types `0x80`–`0xFF`: `[packet_number][packet_type][length:2 LE][body]`. Any reader that
  knows this one rule can *skip* a type it does not know.
- Writers put all `0x80`+ packets **after** every legacy packet. An old reader stops at the
  first `0x80`+ type and keeps everything before it — which is today's behaviour, but by
  design instead of by accident.

This is what retires the "insert, never append" rule (§7), un-pads the LUT packet (an
SSD16xx waveform costs ~85 B on the wire instead of 541 — the difference between fitting and
not fitting on an nRF52811 whose whole config is 512 B today), and lifts the Rev 1 limits
R7 flagged. It can also carry a long-form extension of an existing packet later (e.g. a
`0xA0` superset of `0x20`) without breaking any reader.

### 3.1 Config packets

Identity is `instance_number` at `body[0]`, not the header packet-number byte — the header
byte is a don't-care that different writers already disagree about.

| ID | Name | Body | Repeat | Consumed by |
|---|---|---|---|---|
| `0xAE` | `display_mode` | variable | per mode | device + host |
| `0xAF` | `display_lut` | 4 + waveform | per waveform slot | device + host |
| `0xB0` | `display_calibration` | variable | per multi-pass mode | **host only** |
| `0xB1` | `display_panel` | variable | per display | device + host |

*(Layouts below are as **landed** in `opendisplay_structs.h` 2.2: each packet is a fixed
HEAD struct + one variable tail, so every field keeps a fixed offset and the codegen size
asserts hold. `lut_slot`/`vsn1` stayed fixed `[8]` arrays rather than `[passes]` — ≤ 14
bytes of padding buys fixed offsets for every parser.)*

- **`0xAE display_mode`** — 37 B head: `instance`, `mode_index`, `mode_name[12]`,
  **`color_scheme` (new, R6: what this mode renders; `0xFF` = inherit `0x20`'s scheme —
  so BWRY16 lives here and never in `0x20`, and a 16-grey mode on a BWR panel is
  expressible)**, `passes` (≤ 8, was ≤ 4 — R7), `temp_min_c`/`temp_max_c`/`temp_policy`
  (now device-enforced, §3.3), `lut_slot[8]`, `vsn1[8]`, `seq_len:2`; tail `seq[seq_len]`.
  A 1-pass mode with a 60 B sequence costs ~101 B on the wire against Rev 1's fixed 159.
- **`0xAF display_lut`** — 4 B head: `instance`, `lut_slot`, `crc16` (CRC16-CCITT,
  poly/init as the config CRC); tail = the waveform at its real size (R5). **A slot failing
  CRC is treated as ABSENT, not as garbage**, so a corrupt waveform degrades to "no custom
  waveform" rather than driving the panel with noise.
- **`0xB0 display_calibration`** — 4 B head: `instance`, `mode_index`, `cal_flags` (bit0
  measured), `entry_count` (≤ 32, was 16 — R7: 16 greys + red + headroom fits); tail =
  `entry_count` × 8 B entries: `codes:u16 LE` (2-bit panel code of pass *p* in bits
  [2p+1:2p] — 16 bits covers 8 passes) + `lab_l`/`lab_a`/`lab_b` (CIELAB ×100, int16 LE).
  **The device skips this packet**; it exists so a host image pipeline can quantise against
  real measured colour. Encoding and measurement live in *one* packet deliberately: a
  measurement whose codes are unknown cannot be reproduced, and codes with no measurement
  cannot be chosen between.
- **`0xB1 display_panel`** — `instance`, `panel_flags`, `busy_idle_level`,
  `busy_timeout_ms:2`, `busy_fixed_delay_ms:2`, `code_map[4]`, `mode_count`,
  `sleep_seq_len`, `sleep_seq[sleep_seq_len]`. This packet exists because BUSY policy, BUSY
  idle level and `code_map` were **never on the wire** — they only existed in the firmware's
  panel table. Without it the driver is unimplementable from config.

The `aux_pin_1..6` rename happens in **`0x20`** (reserved bytes get names; size unchanged),
not here — pin *wiring* is board identity and belongs with the other pins.

### 3.2 `CMD_DRIVE_WRITE_START` = `0x0085`

```
request   [0x00][0x85][mode:1][refresh:1][flags:1][rsv:2]
                [uncompressed_size:4 LE]   iff flags bit0 COMPRESSED
                [first data bytes...]
response  [0x00][0x85][mode][passes][temp_c:1]      <- mode/temp_c: see 3.3  [QUEUED]
error     [0xFF][0x85][err:1][mode:1]
```

| flag | meaning |
|---|---|
| bit0 `COMPRESSED` | payload carries `uncompressed_size` + zlib stream |
| bit1 `PIPELINED` | data via `0x0081` windowed frames. **Firmware target only** — Silabs/NRF54/NRF52811 must leave clear |
| bits2–7 | reserved, must be 0 |

Errors: `01` BAD_ARGS, `02` NO_CONFIG, `03` BAD_MODE, `04` BAD_SIZE, `05` UNSUPPORTED,
**`06` TEMP_RANGE** (policy = reject, §3.3), **`07` SEQ_UNSUPPORTED** (§2.4). *(06/07 new.)*

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
mode table.

### 3.3 Temperature (R4) **[QUEUED]**

Rev 1 said `temp_min_c`/`temp_max_c` were "advisory, the host enforces" — and the review
correctly asked what the host is supposed to enforce *with*. Decision: **the device
enforces**, because the device is the thing with a sensor.

- `temp_policy` in `0xAE`: `0` = advisory (run as asked), `1` = **substitute** — the firmware
  picks the declared mode whose range covers the current temperature (ties: lowest
  `mode_index`), `2` = **reject** with `06 TEMP_RANGE`.
- The `0x0085` ACK reports the **mode actually used** (which under policy 1 may differ from
  the requested one — a host must read it back, not assume) and `temp_c`, the device's best
  temperature estimate as int8 (`0x7F` = unknown). Source is the device's business — EFR32
  internal sensor today; a controller read-back via `0x09` scratch later.
- A host that wants the temperature *before* choosing content can issue `0x0085` cheaply?
  No — open question §9.2: whether temperature also belongs in the advertisement (MSD) so
  the host can pick modes without a round-trip. Deliberately not decided this round.

### 3.4 `CMD_READ_IDENT` = `0x0046` (R8) **[QUEUED]**

Plaintext (identity is needed precisely when the host does not yet know what it is talking
to; it carries no secrets). *Numbering note: the review suggested "like `0x0043`", but
`0x0043` **is** `CMD_FIRMWARE_VERSION`; the next free slot in the plaintext info family,
`0x0046`, is assigned instead (landed in opendisplay-protocol 2.3).*

```
request   [0x00][0x46]
response  [0x00][0x46][proto_minor:1][fw_target:1][display_count:1]
          then per display: [panel_ic_type:2 LE][width:2 LE][height:2 LE][color_scheme:1]
```

Silabs answers from its `0x20` config. Solum targets answer from factory data (UICR
CUSTOMER) once migrated — which is the actual point: it lets a host pick the right
`panels.yaml` entry for a tag it has never seen. `fw_target` enumerates
Silabs/NRF52811/NRF54/Firmware. Field set is a proposal — R8 asked for the mechanism;
whether these fields suffice for the M3 factory data is a question back to Nic (§9.3).

### 3.5 `OD_COLOR_SCHEME_BWRY16 = 10`

Packs **identically to BWRY (3)**: 2bpp, one plane. The difference is the number of frames,
which comes from the mode's pass count in `0xAE`, not from this value. It exists so a host
image pipeline can select the 16-entry palette; the device needs only the mode.

**Value 10, not 9.** The designated mirror (`epaper-dithering` `palettes.rs`) already uses 9
for a library-local `Grayscale8` that is deliberately not a wire value.

> ⚠️ **This value must not be written into `DisplayConfig 0x20 color_scheme`.** The panel's
> pixel format is BWRY; 16-colour is a property of a *mode* — and since R6 it has a proper
> home, the `color_scheme` field of `0xAE`. Writing 10 into `0x20` made every 4-colour
> consumer mis-render — the web tool's `clampScheme()` floored it to 6 (16-level grey) and
> the designer collapsed it to 0 (mono), both silently. Host guards were added, but the rule
> is the fix.

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

**What actually fits.** Waveforms dominate. For *our* UC8151-class panels the waveforms are
~535 B of real data, so the length prefix changes little and the lever remains *sharing slots
across displays*:

| displays | per-display waveform copies | shared slots |
|---|---|---|
| 1 | ~2087 | ~2087 |
| 2 | ~3529 | ~2443 |
| 3 | **~4971 (over)** | ~2799 |
| 4 | — | ~3155 |
| 5 | — | ~3511 |

*(Rev 1 numbers for the fixed packets; the `0xA0`-range equivalents land within a few
percent for these panels and will be re-measured when the renumbering is implemented.)*

**Rev 1's "variable length saves only ~6%" claim is hereby corrected** (R5): it was measured
on these 535 B waveforms and says nothing about SSD16xx-class parts, where the waveform is
~70–110 B and the fixed 541 B packet is ~85 % padding — on an nRF52811 (512 B config today,
RAM-tight next to the SoftDevice) that padding is the difference between a grey mode fitting
and not. The unchanged half of the old claim still stands: *trimming trailing-idle waveform
bytes* saves nothing and is unsafe (streaming fewer bytes than the panel's waveform register
holds leaves stale data in the tail) — `0xAF` carries the waveform at the length the panel
needs, not less.

Observed real configs: 2205–2335 B for one display with two modes (Rev 1 packets).

---

## 5. Host-side changes

| Repo | Change |
|---|---|
| `py-opendisplay` | models + byte-exact round-trip for the drive packets; `encoding/multipass.py`. **[QUEUED: `0xAE`–`0xB1` renumber + length-prefix reader]** |
| `opendisplay.org` | `0x0085` sender, multi-pass encoder, Drive Mode selector, 16-colour dither path. **[QUEUED: same renumber]** |
| `epaper-dithering` | `ColorScheme::Bwry16 = 10` across the Rust / Python / TypeScript mirrors |
| `opendisplay-protocol` | **[QUEUED, release gate — R9]** `0x2D`, length-prefix rule, `0xAE`–`0xB1`, `0x0085`, `0x0046`, scheme 10 |

Two host-side rules worth calling out, because both were learned by rendering the wrong thing:

- **Palette codes are PANEL codes. Do not apply `code_map` to them.** `code_map` (`0xB1`)
  turns a *logical* BWRY colour into a panel code for the 4-colour path. Applying it to the
  per-pass codes in `0xB0`, which are already panel codes, maps 2→3 and 3→2 and swaps yellow
  and red in every pass. `encode_frames()` now raises if given a `code_map`.
- **A multi-pass upload must be dithered against the 16-colour palette, not the 4-ink one.**
  Quantising the canvas to BWRY and then multi-pass-encoding the painted canvas throws away 12
  of the 16 colours before the encoder runs.

---

## 6. What was verified on hardware

Every panel `panels.yaml` describes has been migrated and driven through the full
`0x0085` path. Frame size is `width × height / 4` (2bpp), and the device recomputes it from
its own config — a geometry mismatch is rejected as `OD_ERR_DRIVE_BAD_SIZE` rather than
rendered skewed, so an accepted transfer is itself a geometry check.

**Rev 2 re-verification (2026-11-03, `0xAE`–`0xB1` wire, new firmware):** every
unit OTA'd to the length-prefix build, config rebuilt by
`panels/migrate_drive_config.py` (burned `0x2E`–`0x31` dropped on read — the
clearing is implicit in the parser now), 16-colour render driven through
`0x0085`. The ACK's new `temp_c` is logged per trial.

| Unit | Panel | Rev 2 render | ACK temp | Note |
|---|---|---|---|---|
| ODBB1B76 | 2.9" GDEY029F51H | 48384 B, END + `0x73` | 19 °C | read-back byte-verified |
| ODBF2156 | 2.9" GDEY029F51H | 48384 B, END + `0x73` | — | **findmy 0x2D preserved** through the rewrite |
| OD58FFC4 | 2.6" GDEY0266F51 | 49680 B, END + `0x73` | 26 °C | read-back byte-verified |
| ODCF0ED5 | 4.2" GDEM042F52 | 90000 B, END + `0x73` | 27 °C | **photographed in the rig**: clean flat-patch 16-colour grid (index16 pattern) |
| ODBF064E | 7.5" GDEM075F52 | 288000 B, END + `0x73` | 21 °C | read-back byte-verified |

The photographically confirmed unit this round is **ODCF0ED5** (the 4.2" is
what sits in the rig today); ODBB1B76's photographic record is from Rev 1.

| Other check | Result |
|---|---|
| `bb_epaper` symbols in ELF | 0 |
| NVM3 growth preserves key + config | every unit above authenticated after OTA |
| Config read-back | byte-exact after the `pipe_send_raw` retry fix |
| Sequence executor | `seq_conformance` C vs Python model |
| Config index | `panels/cfgtest/`, mutation-tested |

**Caveats the reviewers should weigh:**

- Four of five units are protocol-level this round — the rig camera sees only the 4.2".
  Protocol-level success has repeatedly reported a working transfer while the panel showed
  something wrong; treat those four as "the device accepted and executed three passes".
- Two units additionally carry **unvalidated waveforms**: the 2.6" inherits the 2.9"'s
  `wave_colour`/`wave_edit` (derived entry, see §8.9), and every unit except ODBB1B76 carries
  a borrowed palette (§8.2).

---

## 7. Migration

**Order matters.** A device on this firmware with no drive packets cannot drive its panel.

```
1. Read the existing config off the device and keep it.
2. OTA.                        # device now has no panel until step 3
3. Write the config with the drive packets (panel + modes + luts, + calibration
   for multi-pass modes).
4. Verify: read back, confirm modes/luts/panel are present.
5. Drive one frame and LOOK AT THE PANEL.
```

**Packet placement (Rev 2 — replaces "insert, never append").** The legacy range
`0x00`–`0x7F` is frozen; all new packets are `0x80`+ length-prefixed and go **after** every
legacy packet. Old readers stop at the first `0x80`+ type and keep everything before it;
new readers skip unknown `0x80`+ types by length. The Rev 1 incident that motivated the old
rule — `py-opendisplay` does not model `0x2D findmy_config` (128 B body), so appending drive
packets behind it put 1560 B behind a wall and the config read back as *no drive data at
all* — stays in the record as the reason the frozen-range rule exists. Until py-opendisplay
learns `0x2D` it still cannot read past one, so `0x2D` must remain the last legacy packet it
encounters only if nothing legacy follows it (true in practice; the drive packets now sort
behind it anyway).

**Timeouts must be sized for the slowest panel.** The 4.2" has an unusable BUSY line and runs
on a 35 s fixed delay, against the 2.9"'s ~20.6 s refresh. A pass boundary is a refresh, not a
stall, and it can land on **any** chunk. Two host bounds were silently too short and aborted
healthy uploads.

**Config blob header:** the first two bytes are padding/reserved, not a length. The firmware
takes `data_len` from the NVM3 object header and skips a 3-byte preamble. Hosts that treat
those bytes as a length will log a spurious mismatch.

---

## 8. Release checklist (new in Rev 2)

In order:

1. ~~Land the length-prefix rule + `0x2D` + `0xAE`–`0xB1` + `0x0085` (with `temp_c` ACK and
   errors 06/07) + `0x0046 READ_IDENT` + scheme 10 in **`opendisplay-protocol`** (R9).~~
   **DONE** — opendisplay-protocol commit `40729f3` (protocol 2.3 / structs 2.2), all
   language mirrors regenerated and `validate_mirrors` green; `CMD_SPI_RAW 0x0084` folded
   back from the vendored copy in the same change (it had shipped without a canonical home —
   the exact drift class this repo exists to prevent).
2. ~~Implement in Firmware_Silabs.~~ **DONE** (`de5aded`): length-prefix parser in both
   walkers, validation pre-scan, escapes `0x06`/`0x07`/`0x0B` executed (`0x08` wired but
   capability-absent, `0x09`/`0x0A` refused), device-side `temp_policy` with
   ACK `[mode][passes][temp_c]`, `CMD_READ_IDENT 0x0046` (auth-exempt).
3. ~~Mirror in `py-opendisplay` + web tool.~~ **DONE** (`5e03709`, `76fde4e`): `0x2D`
   modelled opaquely, burned ids dropped on read, unknown `0x80+` preserved verbatim;
   toolbox carries drive packets as raw blobs and frames them itself; presets regenerated.
4. ~~Rewrite + re-verify all five lab units.~~ **DONE** (`1c2acbb`) — see §6; ODCF0ED5
   photographed.
5. ~~`seq_conformance` escape cases.~~ **DONE** (`7a5fee0`): 8/8 modes conformant + the
   new escapes + five refusal cases (rc distinguishes "refused, nothing executed").
6. Second review round (this document, Rev 2) — including the questions in §9. **← YOU
   ARE HERE.** Remaining loose ends: `CMD_SPI_RAW`'s `handle_spi_raw` still predates the
   canonical fold-back (no change needed, names identical); the toolbox's schema entries
   for the old fixed drive packets (`config.yaml` pids 46–49) are now dead and can be
   pruned; Firmware/NRF54/NRF52811 unmigrated as before.

---

## 9. Known gaps and open questions

**Questions back to the reviewer (new, from this round):**

1. **Scratch → logic (R3).** `0x09`/`0x0A` move bytes, but the SSD16xx use case — read the
   OTP waveform, *derive* a fast waveform for the current temperature — is computation, not
   plumbing. Where should that live? Host-side (read scratch back over BLE, host derives,
   uploads as a normal `0xAF`)? A firmware hook? This round deliberately specs only the
   plumbing.
2. **Temperature before mode choice (R4).** The device now enforces `temp_policy` and reports
   `temp_c` in the ACK, but a host choosing *content* (e.g. skip red below −5 °C) wants the
   temperature up front. Advertisement (MSD) field, or is ACK-then-adjust-next-time enough in
   practice on the M3 tags?
3. **`0x0046 READ_IDENT` field set (R8).** Does `[panel_ic_type][w][h][color_scheme]` per display cover
   what the M3 UICR CUSTOMER data can populate (controller, resolution, colours, buttons)?
   Buttons are notably absent — separate concern or same command?
4. **`pin_ref` coverage (R10).** Six aux pins + the six named SPI pins — enough for every
   M3 board variant you have? And is level `2` (release to input) the right third state, or
   do any boards need drive-strength/pull control too?

**Blocking-ish (decide before release):**

5. **Yellow/red palette index conflict is unresolved.** `code_map [0,1,3,2]` implies panel
   code 2 = red and 3 = yellow, which matches the nominal palette's labels. A photo
   measurement of the same grid says the opposite for entries 2 and 3. Transpose and mirror
   hypotheses were both tested and fail. The measurement cannot arbitrate because the script
   white-balances against the palette's own white entry, so that cell is `L*100` *by
   construction* and carries no orientation information. **Decisive test not yet run:**
   render four full-screen flat bands of panel codes 0/1/2/3 and photograph — no palette, no
   grid orientation, no white-balance circularity.
6. **Two units ship a borrowed palette.** ODBF2156 and ODCF0ED5 carry ODBB1B76's CIELAB with
   `measured=0`. For ODBF2156 that is cross-*unit* (same part number; measured spread
   dE00 6.40). For ODCF0ED5 it is cross-*model* — different glass, pigment and waveforms —
   and should be treated as a placeholder, not an approximation.

**Non-blocking but should be written down:**

7. **Compressed multi-pass hides frame boundaries.** `frameBytes` counts uncompressed bytes;
   compressed chunks are in different units, so the host cannot locate the boundary. Current
   behaviour: use the wide ack window for every chunk of a compressed multi-pass transfer. A
   genuine stall then takes 90 s rather than 15 s to surface. Proper fix is to sync-flush
   deflate at frame boundaries so the compressed offsets are known; it touches the wire stream
   and was deliberately deferred.
8. **`0xB0` is host-only.** The device parses past it and ignores it. If a future firmware
   wants temperature-compensated palettes, this is where it would start consuming it.
9. **`PIPELINED` (flags bit1) is unimplemented outside the Firmware target** and NACKs with
   `UNSUPPORTED`.
10. **The vendored `epaper-dithering` JS bundle has not been rebuilt**, so `getPalette(10)` is
    absent from it. The web tool deliberately does not depend on that: it dithers 16-colour
    content through the custom-`ColorPalette` path against the *device's* measured palette,
    which is the more correct design anyway. Rebuilding needs a `wasm32-unknown-unknown`
    toolchain.
11. **Only the Silabs target is migrated.** Firmware / NRF54 / NRF52811 still use the old path
    and are unaffected by this change. (Rev 2 was shaped so the NRF52811 *can* follow —
    that was the point of R5.)
12. **Boot-screen latch bug**, found and fixed during this work: `s_boot_applied` was set
    *before* the attempt, so a device that booted without drive packets — the normal state
    immediately after an OTA — skipped its boot screen and could never redraw it, defeating
    the deliberate retry from the config-reload path. Devices need this build for the retry to
    work; on older builds, power-cycle after writing the config.
    **Follow-on:** making that retry real exposed an ordering bug — the redraw sat in front of
    the config-write ACK, so a ~20–35 s panel refresh pushed the ACK past py-opendisplay's 5 s
    bound and a *successful* write reported as a timeout. The index rebuild still precedes the
    ACK (the host may send another command immediately); the redraw now follows it.
13. **`panels.yaml` covers four panels**: 0x001E (2.9" 168×384), **0x001C (2.6" 184×360)**,
    0x003A (4.2" 400×300), 0x0040 (7.5" 800×480). Any other `panel_ic_type` has no drive data
    and therefore no panel on this firmware — including presets the toolbox still offers. The
    2.6" entry is **derived** from the 2.9" (bb_epaper's two init sequences are byte-identical
    except TRES) and its waveforms are inherited, not yet validated on that glass. A fifth
    panel (2.6" BW EL026F6W4A, column-major) is characterised but blocked on a `plane_format`
    addition — see `PANEL_EL026F6W4A.md`.
14. **Writing a config is not idempotent at the host level.** Re-running a splice that appends
    drive packets produces a *second* copy (observed: 1717 → 3277 B), and `find_mode`/`find_lut`
    would serve whichever they reach first. Tooling must clear existing drive packets before
    adding new ones.

---

## 10. What I most want reviewed (Rev 2)

1. **The new escapes** (§2.3, `0x06`–`0x0B`) — numbering, argument shapes, and the
   validation/refusal rule in §2.4. These are now the contract alongside the base encoding.
2. **The `0xAE`–`0xB1` body layouts** (§3.1) — they are being re-cut anyway for the length
   prefix, so field-set complaints are cheap *now* and expensive later.
3. **Device-enforced `temp_policy` + ACK reporting** (§3.3) — in particular whether
   "substitute, ties to lowest mode_index" is the right policy-1 semantics.
4. **The `0x0046 READ_IDENT` response shape** (§3.4).
5. **The release checklist order** (§8) — especially gating everything on
   `opendisplay-protocol` landing first.
