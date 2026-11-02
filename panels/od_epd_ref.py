#!/usr/bin/env python3
"""Reference implementation of od_epd, driven from panels.yaml over CMD_SPI_RAW.

WHY THIS EXISTS. Porting the firmware to a config-driven driver is a big change
to a shipping product. Before doing it blind, the DESIGN can be validated on
real hardware: this executes exactly the same sequence encoding, wave
substitution and per-panel LUT placement that `src/od_epd.c` implements, but
from the host over the raw-SPI debug command.

If all three panels render correctly from panels.yaml alone, then:
  * panels.yaml is complete -- nothing the driver needs is still hiding in
    bb_epaper or in a hardcoded constant,
  * the sequence encoding and the @w/@vcom substitution are right,
  * the per-panel lut_placement rule is right,
and the firmware port becomes a transcription of something already proven,
rather than an experiment on a device you have to reflash to retry.

This is the same encoder that generates the config blob, so what runs here is
byte-for-byte what the device would be told.
"""
import argparse
import asyncio
import sys
from pathlib import Path

import yaml

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
sys.path.insert(0, str(REPO / "scripts"))

ESC, ESC_BUSY, ESC_WAVE, END = 0x00, 0x01, 0x02, 0x00
WAVE_TOKENS = {f"@w{i}": i for i in range(7)}
WAVE_TOKENS["@vcom"] = 0xFF


# --------------------------------------------------------------- encoding
def encode_sequence(steps):
    """panels.yaml step list -> the length-prefixed blob od_epd.c walks."""
    out = bytearray()
    for raw in steps:
        s = raw.strip()
        if s == "@busy":
            out += bytes([ESC, ESC_BUSY])
            continue
        toks = s.split()
        parsed = [(1, WAVE_TOKENS[t]) if t in WAVE_TOKENS else (0, int(t, 16))
                  for t in toks]
        if any(k for k, _ in parsed):
            out += bytes([ESC, ESC_WAVE, len(parsed)])
            for is_wave, v in parsed:
                out += bytes([0xFF if is_wave else 0x00, v])
        else:
            out.append(len(parsed))
            out += bytes(v for _, v in parsed)
    out += bytes([ESC, END])
    return bytes(out)


def wave_byte(lut, idx):
    if idx == 0xFF:
        return lut[5] ^ 0x80
    return lut[idx]


def walk(seq, lut):
    """Mirror of run_sequence() in od_epd.c. Yields ('cmd',c,data) or ('busy',).

    Kept structurally identical to the C so a divergence is obvious on reading:
    forward-only, length-prefixed, no jumps, step-count capped.
    """
    i, steps = 0, 0
    while i < len(seq):
        steps += 1
        if steps > 64:
            raise ValueError("sequence step cap exceeded")
        n = seq[i]
        if n != ESC:
            if i + 1 + n > len(seq):
                raise ValueError("step overruns blob")
            yield ("cmd", seq[i + 1], bytes(seq[i + 2:i + 1 + n]))
            i += 1 + n
            continue
        esc = seq[i + 1]
        if esc == END:
            return
        if esc == ESC_BUSY:
            yield ("busy",)
            i += 2
            continue
        if esc == ESC_WAVE:
            cnt = seq[i + 2]
            p = seq[i + 3:i + 3 + cnt * 2]
            vals = [wave_byte(lut, p[k * 2 + 1]) if p[k * 2] == 0xFF
                    else p[k * 2 + 1] for k in range(cnt)]
            yield ("cmd", vals[0], bytes(vals[1:]))
            i += 3 + cnt * 2
            continue
        raise ValueError(f"bad escape 0x{esc:02X}")


# ------------------------------------------------------------------ driving
PALETTE = [(2, 0, 0), (1, 1, 1), (2, 1, 1), (3, 1, 1),
           (0, 1, 1), (1, 0, 3), (1, 2, 1), (2, 0, 2),
           (2, 1, 3), (2, 3, 1), (3, 1, 2), (0, 2, 1),
           (0, 3, 1), (3, 2, 1), (0, 1, 0), (3, 0, 1)]


def build_image(w, h, pass_no):
    cw, ch = w // 4, h // 4
    out = bytearray()
    acc = n = 0
    for y in range(h):
        r = min(y // ch, 3)
        for x in range(w):
            c = min(x // cw, 3)
            acc = ((acc << 2) | (PALETTE[r * 4 + c][pass_no] & 3)) & 0xFF
            n += 1
            if n == 4:
                out.append(acc)
                acc = n = 0
    return bytes(out)


async def drive(raw, panel, mode, waves, chunk_log):
    """Execute one mode exactly as od_epd.c would."""
    seqs = {k: encode_sequence(v) for k, v in panel["init_sequences"].items()}
    m = next(x for x in panel["modes"] if x["name"] == mode)
    w, h = panel["geometry"]["width"], panel["geometry"]["height"]
    lut_cfg = panel.get("lut") or {}
    placement = lut_cfg.get("placement", "pre_pon")
    lut_names = m.get("lut_per_pass") or [None] * m.get("passes", 1)
    vsn1 = (m.get("vsn1_per_pass") or [None] * len(lut_names))

    for p in range(m.get("passes", 1)):
        lut = None
        if lut_names[p]:
            lut = bytearray(waves[lut_names[p]])
            if vsn1[p]:
                lut[3] = vsn1[p]
            lut = bytes(lut)
        seq = seqs[m["init"]]

        await raw.reset(10)
        if panel["busy"].get("usable"):
            await raw.wait_idle(5000)
        else:
            await asyncio.sleep(0.5)

        for op in walk(seq, lut if lut else b"\x00" * 7):
            if op[0] == "busy":
                if panel["busy"].get("usable"):
                    await raw.wait_idle(panel["busy"].get("timeout_ms", 30000))
                else:
                    await asyncio.sleep(panel["busy"].get("fixed_delay_ms", 2000) / 1000)
            else:
                await raw.cmd(op[1], op[2])

        if lut and placement == "pre_pon":
            await raw.cmd(0x20, lut)
        elif lut and placement == "post_init":
            await raw.cmd(0x20, lut)

        img = build_image(w, h, p)
        await raw.cmd(0x10, img, stream=True)

        # Panels whose init omits PON need it after the image write.
        if "04" not in [s.strip() for s in panel["init_sequences"][m["init"]]]:
            await raw.cmd(0x04)
            await asyncio.sleep(2.0)

        import time as _t
        t0 = _t.time()
        await raw.cmd(0x12, bytes([0x00]))
        if panel["busy"].get("usable"):
            try:
                el = await raw.wait_idle(panel["busy"].get("timeout_ms", 40000))
            except RuntimeError:
                el = -1
        else:
            await asyncio.sleep(panel["busy"].get("fixed_delay_ms", 35000) / 1000)
            el = int((_t.time() - t0) * 1000)
        chunk_log.append((p, lut_names[p], el))
        print(f"    pass {p}: lut={lut_names[p] or 'none'} drf={el} ms", flush=True)


async def run(a):
    from show16_raw import Raw, _shim_epaper_dithering, load_landing
    _shim_epaper_dithering()
    from opendisplay import OpenDisplayDevice
    from opendisplay.models.capabilities import DeviceCapabilities
    from epaper_dithering import ColorScheme
    from lut_lab.waves import default_waves

    colour, edit = default_waves()
    waves = {"wave_colour": colour, "wave_edit": edit}

    doc = yaml.safe_load((HERE / "panels.yaml").read_text())
    panel = next(p for p in doc["panels"] if int(str(p["id"]), 0) == int(a.panel_id, 0))
    print(f"  {panel['name']} ({panel['part']})  {panel['geometry']['width']}x"
          f"{panel['geometry']['height']}  mode={a.mode}")
    print(f"  lut placement: {(panel.get('lut') or {}).get('placement')}   "
          f"busy usable: {panel['busy'].get('usable')}")
    sizes = {k: len(encode_sequence(v)) for k, v in panel["init_sequences"].items()}
    print(f"  encoded sequences: {sizes}")

    info = load_landing().parse_landing_url(a.link)
    caps = DeviceCapabilities(width=panel["geometry"]["width"],
                              height=panel["geometry"]["height"],
                              color_scheme=ColorScheme.BWRY, rotation=0)
    dev = OpenDisplayDevice(device_name=info.device_name,
                            encryption_key=info.encryption_key,
                            capabilities=caps, timeout=60.0)
    log = []
    async with dev:
        raw = Raw(dev, a.chunk, a.ack_every)
        await raw.open(panel_init=False)
        try:
            await drive(raw, panel, a.mode, waves, log)
            await raw.cmd(0x07, bytes([0xA5]))
        finally:
            await raw.close()
    print(f"  done: {len(log)} passes")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("link")
    ap.add_argument("--panel-id", required=True, help="e.g. 0x001E")
    ap.add_argument("--mode", default="colour16")
    ap.add_argument("--chunk", type=int, default=192)
    ap.add_argument("--ack-every", type=int, default=2)
    asyncio.run(run(ap.parse_args()))


if __name__ == "__main__":
    main()
