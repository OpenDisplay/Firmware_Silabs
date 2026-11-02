#!/usr/bin/env python3
"""Render a 16-colour image end to end over CMD_DRIVE_WRITE_START (0x85).

This is the whole chain with nothing stubbed:

    image -> device's measured palette (0x30) -> 3 packed frames
          -> CMD_DRIVE_WRITE_START (mode index, declared size)
          -> CMD_DIRECT_WRITE_DATA x N
          -> CMD_DIRECT_WRITE_END

The palette is read OFF THE DEVICE, so this cannot silently fall back to a
hardcoded one: if the config carries no measured palette it fails loudly.
The pass count comes from the device's ACK, not from an assumption here.
"""
from __future__ import annotations

import argparse
import asyncio
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / "py-opendisplay" / "src"))
sys.path.insert(1, "/home/jonas/Desktop/opendisplay/en05_ep397yr_uart/host")
sys.path.insert(2, str(HERE.parent / "scripts"))

CMD_DRIVE_WRITE_START = 0x0085
DRIVE_WRITE_FLAG_COMPRESSED = 0x01
DRIVE_ERR = {1: "BAD_ARGS", 2: "NO_CONFIG", 3: "BAD_MODE",
             4: "BAD_SIZE", 5: "UNSUPPORTED"}


def build_image(w: int, h: int, kind: str):
    import numpy as np
    img = np.zeros((h, w, 3), np.uint8)
    if kind == "swatches":
        # 4x4 grid in the camera-visible rows, for comparison against the
        # per-entry measurements.
        top = 136
        img[:top] = 255
        ch, cw = (h - top) // 4, w // 4
        ramp = [(0, 0, 0), (255, 0, 0), (255, 255, 0), (255, 255, 255),
                (128, 0, 0), (255, 128, 0), (200, 200, 0), (200, 200, 200),
                (64, 32, 0), (255, 200, 100), (160, 160, 80), (120, 120, 120),
                (40, 40, 40), (255, 160, 160), (220, 220, 160), (80, 80, 80)]
        for i, c in enumerate(ramp):
            r, col = divmod(i, 4)
            img[top + r * ch: top + (r + 1) * ch, col * cw:(col + 1) * cw] = c
    else:
        for y in range(h):
            f = y / (h - 1)
            img[y, :] = (int(255 * f), int(200 * (1 - abs(2 * f - 1))), int(60 * (1 - f)))
    return img


async def run(a) -> int:
    import numpy as np
    from show16_raw import _shim_epaper_dithering, load_landing
    _shim_epaper_dithering()
    import opendisplay.device as devmod
    from opendisplay import OpenDisplayDevice
    from opendisplay.models.capabilities import DeviceCapabilities
    from opendisplay.encoding.multipass import MultiPassPalette, encode_frames
    from epaper_dithering import ColorScheme

    cap = {}
    real = devmod.parse_config_response
    devmod.parse_config_response = lambda raw: (cap.__setitem__("b", bytes(raw)),
                                                real(raw))[1]
    info = load_landing().parse_landing_url(a.link)
    caps = DeviceCapabilities(width=a.width, height=a.height,
                              color_scheme=ColorScheme.BWRY, rotation=0)
    dev = OpenDisplayDevice(device_name=info.device_name,
                            encryption_key=info.encryption_key,
                            capabilities=caps, timeout=180.0)

    async with dev:
        cfg = await dev.interrogate()
        cal = next((c for c in cfg.display_calibrations
                    if c.mode_index == a.mode), None)
        if cal is None or not cal.codes:
            print(f"device carries no measured palette for mode {a.mode}; "
                  f"run calibrate16.py and write it to the config first")
            return 2
        mode = next((m for m in cfg.display_modes if m.mode_index == a.mode), None)
        if mode is None:
            print(f"device declares no mode {a.mode}")
            return 2
        print(f"mode {a.mode} '{mode.name}': {mode.passes} passes; "
              f"palette {len(cal.lab)} entries measured={cal.measured}")

        pal = MultiPassPalette.from_calibration(cal, passes=mode.passes)
        panel = cfg.display_panels[0] if cfg.display_panels else None
        code_map = panel.code_map if panel else None
        if a.pattern == "index16":
            # Exact palette indices, NO quantisation and NO dithering. This is
            # the only pattern that is directly comparable to the calibration
            # capture: the RGB patterns go through nearest-colour + diffusion,
            # so a blotchy result from those is the dither doing its job, not a
            # transport fault. Distinguishing the two cost an OTA cycle once.
            import numpy as _np
            from opendisplay.encoding.multipass import pack_pass
            top, grid = 136, 4
            idx = _np.zeros((a.height, a.width), _np.uint8)
            idx[:top] = 1
            ch, cw = (a.height - top) // grid, a.width // grid
            for i in range(16):
                r, c = divmod(i, grid)
                idx[top + r * ch: top + (r + 1) * ch, c * cw:(c + 1) * cw] = i
            frames = [pack_pass(idx, pal, p, code_map) for p in range(pal.passes)]
        else:
            img = build_image(a.width, a.height, a.pattern)
            frames = encode_frames(img, pal, code_map=code_map,
                                   dither=not a.no_dither)
        payload = b"".join(frames)
        print(f"encoded {len(frames)} frames x {len(frames[0])} B "
              f"= {len(payload)} B total (code_map {code_map})")

        # START: name the mode and declare passes*frame_bytes.
        req = bytes([a.mode, 0x00, 0x00, 0x00, 0x00])
        await dev._write(CMD_DRIVE_WRITE_START.to_bytes(2, "big") + req)
        r = await dev._read(timeout=60.0)
        if not r or r[0] != 0x00:
            err = DRIVE_ERR.get(r[2] if len(r) > 2 else 0, "?")
            print(f"START rejected: {r.hex(' ')} ({err})")
            return 1
        passes = r[3] if len(r) > 3 else 0
        print(f"START ok: device expects {passes} passes")
        if passes != mode.passes:
            print(f"  device says {passes}, config says {mode.passes} -- aborting")
            return 1

        sent = 0
        for i in range(0, len(payload), a.chunk):
            part = payload[i:i + a.chunk]
            await dev._write((0x0071).to_bytes(2, "big") + part)
            rr = await dev._read(timeout=30.0)
            if not rr or rr[0] != 0x00:
                print(f"DATA rejected at {sent} B: {rr.hex(' ')}")
                return 1
            sent += len(part)
            if sent % (a.chunk * 60) == 0:
                print(f"  {sent}/{len(payload)} B", flush=True)
        print(f"  {sent}/{len(payload)} B sent")

        await dev._write((0x0072).to_bytes(2, "big") + bytes([0]))
        r = await dev._read(timeout=180.0)
        print(f"END -> {r.hex(' ')}")
        try:
            note = await dev._read(timeout=180.0)
            print(f"refresh notification -> {note.hex(' ')}")
        except Exception as e:
            print(f"(no refresh notification: {type(e).__name__})")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("link")
    ap.add_argument("--mode", type=int, default=1)
    ap.add_argument("--width", type=int, default=168)
    ap.add_argument("--height", type=int, default=384)
    ap.add_argument("--pattern", default="index16", choices=("index16", "swatches", "gradient"))
    ap.add_argument("--no-dither", action="store_true")
    ap.add_argument("--chunk", type=int, default=180)
    return asyncio.run(run(ap.parse_args()))


if __name__ == "__main__":
    sys.exit(main())
