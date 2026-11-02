#!/usr/bin/env python3
"""Measure the 16 colours of the colour16 mode and emit them for the config.

Renders a 4x4 grid of the 16 palette entries through the REAL drive path (the
mode's own sequence and waveforms, read back out of the serialized config
packets), photographs it, and reports CIELAB per entry.

GRID PLACEMENT IS A PROPERTY OF THE CAMERA, NOT THE PANEL. --top 0 (the
default) fills the whole screen, which is what you want when photographing the
panel by hand. --top 136 confines the grid to the bottom ~65%, which is all the
RIG camera can see (scripts/panel_rect_ODBB1B76.json); a full-screen grid shot
through the rig puts its top row off-frame, which is exactly how an earlier
attempt measured the backdrop instead of the panel.

Whatever value is used for the render MUST be used for the measurement, or the
sampling windows land between cells -- hence one flag feeding both.

WHY MEASURE AT ALL. A palette entry is a triple of per-pass 2-bit codes; what
colour that triple actually produces is a property of the pigment and the
waveform, not something derivable. The 16 nominal values currently in the
device's 0x30 packet were written as a size test and are not measurements.
"""
from __future__ import annotations

import argparse
import asyncio
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / "py-opendisplay" / "src"))
sys.path.insert(1, "/home/jonas/Desktop/opendisplay/en05_ep397yr_uart/host")
sys.path.insert(2, str(HERE.parent / "scripts"))
sys.path.insert(3, str(HERE))

# Per-pass 2-bit codes for each palette entry: (pass0, pass1, pass2).
# This is the ENCODING half of the calibration -- without it a measured colour
# cannot be reproduced, because nothing else records which codes made it.
#
# Entry 15 is (2,1,2), not the stock "fd" (3,0,1). Measured over two units
# (results/bwry/SESSION_2026-11-02_overnight.md): gamut hull 3681 -> 4152
# (+12.8%) on ODBB1B76 and 3567 -> 3549 (-0.5%, within noise) on ODBF2156,
# with mean C* up on both and one more cell above C* 10. It also replaces the
# cell with the WORST device-to-device agreement (burgundy, dE00 19.5), so it
# should narrow unit spread as well as widen the gamut.
#
# Two further swaps were measured at the same time and are NOT applied: together
# they gave +18.0% on ODBB1B76 but -19.4% on ODBF2156, i.e. they were fitted to
# one panel. Caveat on this one too -- n = 2 units.
PALETTE = [(2, 0, 0), (1, 1, 1), (2, 1, 1), (3, 1, 1),
           (0, 1, 1), (1, 0, 3), (1, 2, 1), (2, 0, 2),
           (2, 1, 3), (2, 3, 1), (3, 1, 2), (0, 2, 1),
           (0, 3, 1), (3, 2, 1), (0, 1, 0), (2, 1, 2)]

VIS_TOP = 0            # first panel row the grid occupies; see --top
GRID = 4


def build_pass_frame(w: int, h: int, pass_no: int, top: int | None = None) -> bytes:
    """One pass's frame: a 4x4 grid of palette entries from row `top` down.

    NO code_map IS APPLIED, deliberately. PALETTE holds PANEL 2-bit codes, not
    logical BWRY colours -- show16_raw.py, the renderer these triples came from
    and which produced the reference image, writes them straight to the panel
    (`PALETTE[...][pass_no] & 3`). code_map exists to turn a LOGICAL colour into
    a panel code for the 4-colour path; running palette codes through it maps
    2->3 and 3->2, which swaps yellow and red in every pass.
    """
    top = VIS_TOP if top is None else top
    cell_h = (h - top) // GRID
    cell_w = w // GRID
    out = bytearray()
    acc = n = 0
    for y in range(h):
        for x in range(w):
            if y < top:
                logical = 1                      # white above the grid
            else:
                r = min((y - top) // cell_h, GRID - 1)
                c = min(x // cell_w, GRID - 1)
                logical = PALETTE[r * GRID + c][pass_no]
            acc = ((acc << 2) | (logical & 3)) & 0xFF
            n += 1
            if n == 4:
                out.append(acc)
                acc = n = 0
    return bytes(out)


async def render(link: str, chunk: int, ack_every: int) -> None:
    import time as _t

    from show16_raw import Raw, _shim_epaper_dithering, load_landing
    _shim_epaper_dithering()
    from lut_lab.waves import default_waves
    from opendisplay.panel_drive import packets_from_yaml
    from opendisplay import OpenDisplayDevice
    from opendisplay.models.capabilities import DeviceCapabilities
    from epaper_dithering import ColorScheme
    from drive_from_packets import parse_packets, walk

    colour, edit = default_waves()
    waves = {"wave_colour": bytes(colour), "wave_edit": bytes(edit)}
    packets = packets_from_yaml(HERE / "panels.yaml", 0x001E, waves)
    panel, modes, luts = parse_packets(packets)
    mi = next(k for k, v in modes.items() if v["name"] == "colour16")
    m = modes[mi]
    w, h = 168, 384
    print(f"colour16: {m['passes']} passes, slots {m['lut_slot'][:m['passes']]}, "
          f"code_map {panel['code_map']}")

    info = load_landing().parse_landing_url(link)
    caps = DeviceCapabilities(width=w, height=h,
                              color_scheme=ColorScheme.BWRY, rotation=0)
    dev = OpenDisplayDevice(device_name=info.device_name,
                            encryption_key=info.encryption_key,
                            capabilities=caps, timeout=180.0)

    async def wait_busy():
        if panel["busy_usable"]:
            try:
                return await raw.wait_idle(panel["busy_timeout_ms"])
            except RuntimeError:
                return -1
        await asyncio.sleep(panel["busy_fixed_delay_ms"] / 1000)
        return panel["busy_fixed_delay_ms"]

    async with dev:
        raw = Raw(dev, chunk, ack_every)
        await raw.open(panel_init=False)
        try:
            for p in range(m["passes"]):
                slot = m["lut_slot"][p]
                lut = bytearray(luts[slot])
                if m["vsn1"][p]:
                    lut[3] = m["vsn1"][p]
                lut = bytes(lut)
                await raw.reset(10)
                await wait_busy()
                t0 = _t.time()
                for op in walk(m["seq"]):
                    if op[0] == "busy":
                        await wait_busy()
                    elif op[0] == "delay":
                        await asyncio.sleep(op[1] / 1000)
                    elif op[0] == "image":
                        await raw.data(build_pass_frame(w, h, p),
                                       stream=True)
                    elif op[0] == "lut":
                        await raw.data(lut)
                    elif op[0] == "wave":
                        pr, cnt = op[1], op[2]
                        vals = [
                            (lut[5] ^ 0x80 if pr[k * 2 + 1] == 0xFF else lut[pr[k * 2 + 1]])
                            if pr[k * 2] == 0xFF else pr[k * 2 + 1]
                            for k in range(cnt)
                        ]
                        await raw.cmd(vals[0], bytes(vals[1:]))
                    else:
                        await raw.cmd(op[1], op[2])
                print(f"  pass {p} done in {_t.time()-t0:.1f}s", flush=True)
            for op in walk(panel["sleep_seq"]):
                if op[0] == "cmd":
                    await raw.cmd(op[1], op[2])
        finally:
            await raw.close()


def measure(dng: str, rect: str, top: int) -> list[dict]:
    import numpy as np
    from bwry_lab import load_raw12, demosaic, linrgb_to_lab

    cal = json.load(open(rect))
    x0, x1 = cal["active_x"]
    bottom = cal["bottom_edge_y"]
    ppr = cal["px_per_panel_row"]
    rgb = demosaic(load_raw12(dng))

    def row_to_y(r: int) -> int:
        return int(round(bottom - (384 - r) * ppr))

    cell_h = (384 - top) // GRID
    cell_w = (x1 - x0) / GRID
    inset_y, inset_x = int(cell_h * ppr * 0.25), cell_w * 0.25

    # White reference: the strip between the first visible row and the grid.
    # It is narrow -- the camera sees from panel row ~102 and the grid starts at
    # 136 -- so clamp to the frame rather than assuming headroom above it.
    # With a full-screen grid there is no white strip above it, so fall back to
    # the brightest cell (entry 1 is white in this palette) as the reference.
    if top <= 0:
        wy0, wy1 = None, None
    else:
        wy0 = max(row_to_y(cal["first_visible_panel_row"]) + 8, 4)
        wy1 = max(row_to_y(top) - 8, wy0 + 20)
    if wy0 is None:
        wr, wc = divmod(1, GRID)          # palette entry 1 is white
        wy0 = row_to_y(top + wr * cell_h) + inset_y
        wy1 = row_to_y(top + (wr + 1) * cell_h) - inset_y
        wx0 = int(x0 + wc * cell_w + inset_x)
        wx1 = int(x0 + (wc + 1) * cell_w - inset_x)
        white = rgb[wy0:wy1, wx0:wx1].reshape(-1, 3).mean(axis=0)
    else:
        white = rgb[wy0:wy1, int(x0 + cell_w):int(x1 - cell_w)].reshape(-1, 3).mean(axis=0)
    if not np.all(np.isfinite(white)) or white.max() <= 0:
        raise SystemExit(f"white reference window y={wy0}..{wy1} is empty; "
                         f"the grid or the calibration rect is wrong")
    print(f"white reference from image rows {wy0}..{wy1}: "
          f"{[round(float(v),4) for v in white]}")

    out = []
    for i, codes in enumerate(PALETTE):
        r, c = divmod(i, GRID)
        y0 = row_to_y(top + r * cell_h) + inset_y
        y1 = row_to_y(top + (r + 1) * cell_h) - inset_y
        px0 = int(x0 + c * cell_w + inset_x)
        px1 = int(x0 + (c + 1) * cell_w - inset_x)
        patch = rgb[y0:y1, px0:px1].reshape(-1, 3).mean(axis=0)
        lab = linrgb_to_lab((patch / white).reshape(1, 1, 3))[0, 0]
        out.append({"index": i, "codes": list(codes),
                    "lab": [round(float(v), 2) for v in lab],
                    "rgb_rel": [round(float(v), 4) for v in (patch / white)]})
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("link")
    ap.add_argument("--rect", default=str(ROOT.parent / "Firmware_Silabs" / "scripts"
                                          / "panel_rect_ODBB1B76.json"))
    ap.add_argument("--dng", help="skip rendering; measure this capture")
    ap.add_argument("--out", default=str(HERE / "palette16_measured.json"))
    ap.add_argument("--top", type=int, default=0,
                    help="first panel row the grid occupies. 0 = full screen "
                         "(hand-held photo); 136 = bottom 65%% (rig camera).")
    ap.add_argument("--chunk", type=int, default=192)
    ap.add_argument("--ack-every", type=int, default=2)
    a = ap.parse_args()

    global VIS_TOP
    VIS_TOP = a.top
    if not a.dng:
        asyncio.run(render(a.link, a.chunk, a.ack_every))
        print(f"  grid occupies panel rows {a.top}..384 "
              f"({'full screen' if a.top == 0 else 'rig-visible area only'})")
        print("rendered; now capture with rig.py and re-run with --dng")
        return 0

    entries = measure(a.dng, a.rect, a.top)
    print(f"\n{'#':>2}  codes      L*     a*     b*")
    for e in entries:
        L, A, B = e["lab"]
        print(f"{e['index']:2d}  {tuple(e['codes'])}  {L:6.2f} {A:6.2f} {B:6.2f}")
    Path(a.out).write_text(json.dumps(
        {"_comment": "Measured on ODBB1B76 through the colour16 drive path. "
                     "`codes` is the per-pass 2-bit encoding that produced each "
                     "colour; without it a measurement cannot be reproduced.",
         "device": "ODBB1B76", "mode": "colour16", "entries": entries}, indent=1))
    print(f"\nwrote {a.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
