# Setting this up from scratch — handover for another agent

Everything below has been done on hardware. Where something is *expected* but
unverified it says so; treat the difference as load-bearing.

All work is on branches. **Nothing here is on `main` and nothing should be
pushed there.**

| repo | branch |
|---|---|
| `OpenDisplay/opendisplay-protocol` | `feat/od-epd-driver` |
| `OpenDisplay/Firmware_Silabs` (newdriver checkout) | `feat/od-epd-driver` |
| `OpenDisplay/py-opendisplay` | `feat/od-epd-driver` |
| `OpenDisplay/opendisplay.org` | `feat/od-epd-driver` |
| `OpenDisplay/Firmware_Silabs` (rig tooling checkout) | `feat/rig-remote-tooling` |

Two things deliberately NOT in git: the thermal-chamber ESP firmware (not a
repo — see §7), and several scripts under `Firmware_Silabs/scripts/` that
embed landing links containing **real device encryption keys**
(`show16_75.py`, `calib_geometry.py`, `overnight.py`, `scan64.py`). They are
untracked and **not gitignored**, so never `git add -A` in that repo.

---

## 1. Layout

The checkouts sit side by side; several tools resolve siblings by relative
path, so keep the names.

    opendisplay/
      newdriver/
        Firmware_Silabs/      EFR32BG22 firmware + panels.yaml + lab tooling
        py-opendisplay/       host library (BLE, config, encoding)
        opendisplay.org/      static website incl. the web toolbox
      OpenDisplay_org/
        opendisplay-protocol/ canonical protocol.h / structs.h
      Firmware_Silabs/        older checkout; holds the rig tooling + venvs
      en05_ep397yr_uart/      lab host code; `lut_lab` lives here

## 2. Build the firmware

    cd newdriver/Firmware_Silabs/cmake_gcc
    ninja -C build

Produces `build/base/opendisplay-bg22.gbl` (the OTA image) plus `.hex`/`.s37`.
Needs the Silicon Labs GSDK and `arm-none-eabi-gcc` already configured — this
tree builds against an existing SDK install, it does not vendor one.

Sanity check without hardware:

    cd newdriver/Firmware_Silabs/panels
    PYTHONPATH=../../../en05_ep397yr_uart/host python3 seq_conformance.py

Expect `12/12 mode(s) conformant (+ escape cases)`. This runs every
panels.yaml mode through the real C executor and an independent Python model
and diffs the op streams, so a divergence here means the firmware and the host
disagree about the wire — fix that before touching a panel.

## 3. Host library

`py-opendisplay` uses **uv**, and only uv:

    cd newdriver/py-opendisplay
    uv sync --all-extras
    uv run pytest tests/unit

`uv sync --all-extras` first is not optional — without it `uv run pytest`
silently falls back to system site-packages and tests pass against the wrong
code.

For the lab scripts, `PYTHONPATH` must include the lab host dir so `lut_lab`
imports:

    export PYTHONPATH=/path/to/opendisplay/en05_ep397yr_uart/host

## 4. Devices

### 4.1 Landing links

Tools take a `https://opendisplay.org/l/?<base64>` link. **Unencrypted units
do not need a stored link — mint one:**

```python
from opendisplay import landing
landing.build_landing_url(0, bytes.fromhex("D81EBB"), None, 3)
```

The device name is `OD` + the three hex bytes. A link whose key slot is all
zero parses back as `encryption_key=None`, which is what an unencrypted unit
wants. Encrypted units need their real link — those are **not** in any repo.

### 4.2 Flash

    cd newdriver/Firmware_Silabs
    export PYTHONPATH=/path/to/opendisplay/Firmware/tools
    /path/to/Firmware_Silabs/.venv-ble-ota/bin/python scripts/ble_ota.py \
        --name ODXXXXXX --gbl cmake_gcc/build/base/opendisplay-bg22.gbl

Omit `--key` for unencrypted units; passing a key that parses to `None` kills
the wrapper silently.

**Two failure modes, opposite fixes — check whether the transfer started:**

* **Progress reached some % then died** → the unit is stranded in AppLoader and
  no longer advertises its app name. Resume with `--name OTA --already-in-ota`.
* **No percentages at all**, repeating "AppLoader OTA characteristics not
  found" → AppLoader is advertising before its GATT database is up. Retrying
  and clearing the BlueZ cache both fail. Trigger DFU, wait for the
  advertisement, then sleep ~6 s before connecting
  (`scratchpad/ota_slowboot.py` does this).

Verify the flash took: `CMD_READ_IDENT` (0x0046) must answer. The version
string alone is **not** enough — a pre-Rev-2 dev build also reports
`bg22-dev`, and the only reliable tell is that 0x0046 times out on it.

### 4.3 Write the 16-colour config

    cd newdriver/Firmware_Silabs/panels
    python3 migrate_drive_config.py '<landing-url>' 0x001E

Panel IDs: `0x001E` 2.9", `0x001C` 2.6", `0x0036` 3.5", `0x0038` 1.54",
`0x003A` 4.2", `0x0040` 7.5". Read the device's own `panel_ic_type` rather than
guessing — `probe.py` prints it.

Pass `--measured` **only** on ODBB1B76; the palette was measured on that unit
and is borrowed everywhere else (`measured=0`).

The tool is idempotent (models are replaced, not appended) and preserves
FindMy (`0x2D`). It read-back-verifies and prints `VERIFIED`.

### 4.4 Render

    python3 render16.py '<landing-url>' --mode 1 --width 168 --height 384 \
        --pattern index16 --top 0

`--pattern index16` writes exact palette indices with no dithering — the only
pattern directly comparable to a calibration capture. `--rect x0,y0,x1,y1`
confines the grid to the camera's visible area (the rig sees about a third of
the 7.5"). The ACK is `[mode][passes][temp_c]`; the script aborts if the device
substituted a mode on temperature grounds.

## 5. Local website

Pure static, no build step and no dependencies:

    cd newdriver/opendisplay.org/httpdocs
    python3 -m http.server 8787 --bind 127.0.0.1

**Bind to loopback.** Web Bluetooth requires a secure context; `http://localhost`
qualifies, a LAN IP over plain HTTP does not — serve it on `0.0.0.0` and the
Bluetooth buttons fail silently with no error.

* toolbox: <http://127.0.0.1:8787/firmware/toolbox/>
* display tool: <http://127.0.0.1:8787/firmware/display/index.html>

Confirm it is the Rev 2 build: `simple-config-presets.json` must contain drive
packet pids **174/175/177** and must NOT contain 46–49 (the burned ids).

## 6. The measurement rig

`Firmware_Silabs/scripts/rig.py` is the single handle — chamber, lamp and
camera are coupled, so do not poke them separately.

    export RIG_PI_HOST=10.0.38.217 RIG_CHAMBER_HOST=10.0.66.142
    python3 rig.py status | health | snap out.jpg | capture TAG | temp 25 --wait

Addresses are DHCP and the rig moves; both are env-overridable, so a
relocation is an export rather than an edit.

* `snap` pulls from the live go2rtc stream — does not open the camera, so it
  cannot disturb a capture. Auto-exposed H264: fine for "did it render" and
  ghosting, **not** for colour numbers.
* `capture` is the photometric path (12-bit DNG). Develop with
  `dcraw -a -q 3 -T`.
* `recover` clears a wedged camera (`Failed to queue buffer for Unicam Image`),
  which otherwise needs shell access and blocks every capture.

Gotchas that cost real time:
* `psu_i` is the **heater** current, not the lamp (~1.1 A warming, ~0.01 A at
  setpoint). It is not the lamp failing.
* `preview_alive: false` is normal with no browser preview attached.
* **Re-derive the measurement ROI after any move** — a relocation rotated the
  camera's view 90° and changed the illumination, so absolute L\* from before a
  move is not comparable to after.
* Never kill a script mid-`CMD_SPI_RAW` session. It leaves the panel powered
  mid-refresh and the unit stops advertising for ~6 minutes. It recovers on its
  own; wait ~10 minutes before concluding anything is bricked.

## 7. Thermal chamber (NOT in git)

`thermal_chamber/` is not a repository and is excluded deliberately. One change
lives there uncommitted: cooling current clamped to **3.8 A**
(`CTRL_COOL_I_MAX` in `esp32/src/control/block_temp_ctrl.h`, and `CTRL_I_MAX`
in `control_supervisor.h` so the characterisation path cannot exceed it
either). 4.6 A browned out the shared PSU and took the Pi off the network.

    cd thermal_chamber/esp32
    pio run -e esp32dev_ota -t upload --upload-port <chamber-ip>

Power the TEC down first (`air_ctrl enable:false`) — the PSU holds its own
output state, so an ESP reset mid-cooling leaves it driving current unattended.

## 8. What is settled, so you do not re-run it

* **16 colour works** on 2.9", 2.6", 3.5", 1.54", 4.2", 7.5".
* **Palette codes are panel-specific.** The reference palette holds *raw panel
  codes* measured on the 2.9" (code 2 = red); the F52 family is reversed
  (code 2 = yellow). See `colour.palette_code_swap`.
* **Faster BWRY on the 2.6": PLL register `0x30` = `0x07`** — 19433 → 10341 ms
  (1.88×) for dE 5–6 (red −21%, white picks up a slight tint). The
  `E0/E6/A5` booster tail is a separate −27% but costs more chroma per unit of
  speed. Real panel temperature is also a big lever (−28% at 32 °C), so size
  BUSY timeouts for the **cold** case.
* **No fast BW mode exists** with the available waveforms. Ghosting is binary;
  repetition does not converge; truncating whole groups does not shorten a
  waveform; truncating *within* groups does but either cannot make black
  (cut ≤11) or paints yellow (cut 13–22). A purpose-built waveform is the only
  remaining path.
* **Partial refresh is absent** — 14 hypotheses, all refused.

**Two measurement traps that produced confident wrong answers here:**
1. "Clear to uniform white and read the residual" scores *perfect* for a
   waveform that can only drive white. Always render content with blacks
   **and** whites.
2. L\*-contrast alone scored a visibly **yellow** panel as "stock-equivalent".
   Always measure chroma too.

Both were caught only by looking at the photograph. Timing and single-channel
metrics cannot detect a wrong image — the camera is not optional.
