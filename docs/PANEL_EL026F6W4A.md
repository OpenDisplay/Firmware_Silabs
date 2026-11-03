# 2.6" BW panel (newton PRO EL026F6W4A) — driving notes for the config-driven driver

**Status:** driving it is SOLVED and verified end to end. Text, filled and
outlined shapes, and real ArUco markers render; the markers decode back off the
chamber camera with the correct ids. This document is the implementation spec,
plus the traps, so nobody re-derives them.

**It is NOT yet expressible in `panels.yaml`** — see [§4](#4-what-the-new-driver-cannot-express-yet).
That is the one thing needing a protocol/driver change.

---

## 1. Identity

| | |
|---|---|
| Label | `newton PRO 2.6" BW Freezer`, model **EL026F6W4A**, 3 V 0.1 A |
| Controller | **UC81xx, UC8176-style** (two-byte TRES) |
| Colour | 1 bpp mono |
| Full refresh | **2474–2478 ms**, very repeatable |
| Hardware | EE04 / XIAO ESP32-S3 lab board — see `epd_spi_probe/` |

> The `IC:22800-...` on the label is an **Industry Canada** certification
> number, not the display controller. The label identifies the product only.

**SSD16xx is ruled out.** bb_epaper's own 2.6" BW entry
`epd266_init_sequence_full` (SSD16xx, 152×296) never completes — BUSY never
releases — while UC81xx inits drive the glass cleanly. Do not reuse that entry
for this part.

---

## 2. The verified drive sequence

```
power on; hardware reset
w 04                  PON    -> BUSY low ~109-258 ms, then released
(wait BUSY)
w 00 1F 0B            PSR
w 61 01 18 00 78      TRES   (nominally 280 x 120 — see the caveat below)
w 50 97               CDI
<8400 bytes to 0x10>  DTM1
w 92                  PTOU   update the WHOLE panel, not the last window
w 12                  DRF
(wait BUSY)           -> releases in ~2475 ms
```

Notes that matter for the implementation:

- **The image plane is DTM1 (`0x10`).** With this init, `0x13` is ignored.
- **`PTOU` (`0x92`) is required before `DRF`.** Without it only part of the
  glass updates.
- **Do the BUSY wait yourself.** bb_epaper's internal wait is bounded near
  10 s; its "5039 ms" / "10039 ms" results are timeouts, not completions, and
  leave a half-drawn frame.
- **BUSY idles HIGH, is LOW while busy** (UC81xx convention).

### ⚠ TRES does not describe the real geometry

`61 01 18 00 78` is 280×120, which at 1 bpp would be 4200 bytes — but the panel
consumes **8400** and the usable image is **336×200**. So on this part TRES is
an accepted magic pair, not a frame descriptor. **This is not understood**, and
it is the biggest open question. What *is* established:

- `280 x 120` is accepted; **`120 x 280` is rejected** (identical byte count).
- The rejection is silent and vicious: BUSY still cycles a normal 2475 ms while
  nothing on the glass changes. It looks exactly like a dead panel. During this
  work it was misdiagnosed as hardware damage needing physical intervention.

---

## 3. Framebuffer mapping — the hard part

**The data is COLUMN-MAJOR.**

```
byte k        -> column k // 25, vertical byte-offset k % 25
25 bytes per column = 200 pixels tall
8400 bytes          = 336 columns
1 bpp, MSB = topmost pixel of the byte
bit 1 = WHITE, bit 0 = BLACK
column order is MIRRORED (byte 0 lands at the RIGHT edge)
```

Reference implementation: `epd_spi_probe/panel.py::pack()`.

### How it was settled (so it can be re-checked, not re-guessed)

Split every **column** into a black top half and a white bottom half, assuming
*N* bytes per column, and sweep *N*:

| N | result |
|---|---|
| 15, 20, 24, 30 | shears into vertical stripes |
| **25** | **clean straight horizontal boundary, no stripes** |

It agrees independently with byte-prefix linearity: prefixes of
2100/4200/6300/8400 black bytes covered 23/47/71/**94 %** of the width in a
clean full-height band, implying ~357 columns total.

> **Everything before this assumed row-major**, which produced stripes and
> phantom "4× repeats" that tracked no parameter, and sent the investigation
> through 2 bpp, 4 bpp and a dozen candidate geometries. **If structured
> content stripes while uniform fills are clean, suspect the major order before
> the pixel format.**

### Known gap

336 columns covers ~94 % of the glass; the full width is ~357 columns
(~8900 bytes). The remaining strip is currently undriven. Writing more than
8400 bytes has not been made to work.

---

## 4. What the new driver cannot express yet

`panels.yaml` currently offers only `plane_format: 2bpp_packed`, and the
packing itself is decided host-side from `color_scheme` via
`opendisplay_color_bits_per_pixel()`. Both assume **row-major**. This panel
needs **1 bpp column-major, MSB-top, mirrored columns**.

Three things are needed:

1. **A scan-order flag.** `DisplayPanelConfig` (0x31) has
   `panel_flags` bits 2–7 free and 7 reserved bytes. Suggested:
   ```c
   #define OD_PANEL_FLAG_COLUMN_MAJOR (1u << 2) /* bytes fill down a column */
   #define OD_PANEL_FLAG_MIRROR_X     (1u << 3) /* byte 0 at the right edge */
   ```
   This costs no packet-size change, which matters — the ceiling is 4080 B.
2. **A `plane_format: 1bpp_column_major`** value in `panels.yaml`, carried to
   the host packer.
3. **Host-side packing** in `py-opendisplay` / the web encoder. The device
   streams bytes unchanged, so this is purely a host concern — the firmware
   only needs the flag so it can reject a mismatched frame size.

Nothing in the sequence encoding needs to change: the init above expresses
cleanly as a `panels.yaml` `init_sequences` entry with `@image` before `PTOU`.

---

## 5. Draft `panels.yaml` entry

Blocked on §4 — the `plane_format` value does not exist yet. Recorded so the
register data is not lost.

```yaml
  - id: 0x001C_BW          # provisional; the real panel_ic_type is unconfirmed
    name: EL026F6W4A_336x200
    part: EL026F6W4A
    controller: UC8176
    geometry: { width: 336, height: 200 }   # NOT the TRES values
    colour:
      scheme: MONO
      plane_format: 1bpp_column_major       # <-- NEW, see section 4
      column_bytes: 25
      mirror_x: true
    busy:
      usable: true
      idle_level: 1
      timeout_ms: 30000
    lut: { supported: false }
    sleep_sequence:
      - "02 00"            # POF
      - "07 A5"            # deep sleep
    modes:
      - name: full
        init: full
        passes: 1
        measured_drf_ms: [2475]
    init_sequences:
      full:
        - "04"             # PON
        - "@busy"
        - "00 1F 0B"       # PSR
        - "61 01 18 00 78" # TRES — magic pair, not the geometry
        - "50 97"          # CDI
        - "10"             # DTM1
        - "@image"
        - "92"             # PTOU
        - "12"             # DRF
        - "@busy"
```

---

## 6. Not established

- **`panel_ic_type`.** No enum value is confirmed for this part. Do not reuse
  `0x001C` (`EP266YR_184X360`) — that is the 2.6" **BWRY** sibling, a different
  panel. A new enum entry is needed.
- **Why TRES 280×120 works and 120×280 does not**, and why the accepted frame
  is 8400 B rather than the 4200 B that TRES implies.
- **The last ~6 % of the width** (columns 336…~357).
- **Partial / fast refresh.** Only the full refresh has been exercised.
- **Temperature behaviour.** Everything was measured at chamber ambient; no
  temperature sweep was done. The part is marked "Freezer", so this likely
  matters.

---

## 7. Traps, each of which cost real time

| Trap | Symptom | Reality |
|---|---|---|
| Row-major assumption | stripes, phantom 4× repeats | data is column-major |
| TRES field order | panel looks dead, BUSY normal | `120x280` rejected, `280x120` fine |
| 1-byte vs 2-byte TRES | refresh "succeeds", panel blank | this part needs the 2-byte form |
| Missing `PTOU` | only part of the glass updates | `0x92` before `DRF` |
| bb_epaper's BUSY wait | "5039/10039 ms" results | those are timeouts, not completions |
| Half/half test to pick bpp | "proved" 2 bpp | bytes are identical under 1/2 bpp — it cannot distinguish |
| Camera ROI | polarity appeared inverted | **the panel had physically moved**; re-derive the ROI after any disturbance |
| CS raised between cmd and data | data silently dropped | hold CS low across the whole write |

**Polarity ground truth** (uniform fills, correct ROI): `0x00` → 0.133 (black),
`0x55`/`0xAA` → ~0.26 (mid-grey, alternating pixels — which is itself proof of
1 bpp), `0xFF` → 0.474 (white).

---

## 8. Reproducing and the acceptance test

Tools live in `epd_spi_probe/` (separate project; bit-banged SPI, panel-agnostic):

- `panel.py` — drawing library: `blank / text / rect / aruco / pack / send`
- `verify.py` — renders text + shapes + cv2 `DICT_4X4_50` markers, photographs
  through the chamber's **wide-angle `usb` camera**, and decodes them back
- `probe.py` — CLI driver; stream frames with `wbegin 10` / `wdata` / `wend`

**Use this as the acceptance test for the driver port.** It passes only if the
whole chain is right — packing, geometry, polarity, drive sequence and optics —
rather than merely "something dark appeared", which several earlier wrong
configurations also achieved:

```
$ python3 verify.py
rendering 336x200, markers (7, 23)
refresh: wait 2478ms 0->1
ArUco ids detected: [7, 23]
RESULT: PASS - both markers decoded
```

> Camera note: the chamber has two. `hq` faces the **back** of the tag (the
> barcode label); only the wide-angle `usb` camera sees the screen. Check
> `GET /api/cameras` rather than assuming.
