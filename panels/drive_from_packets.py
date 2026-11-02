#!/usr/bin/env python3
"""Drive a panel from SERIALIZED CONFIG PACKETS, not from panels.yaml.

This closes the loop. The path exercised here is:

    panels.yaml -> panel_drive.packets_from_yaml() -> bytes
                -> parse back out of those bytes
                -> execute over CMD_SPI_RAW

Nothing between the parse and the panel consults the YAML, so anything the
packets fail to carry shows up as a panel that does not render. That is the
only way to find a field you forgot to put on the wire -- and it has already
earned its keep: writing the firmware's od_epd_desc_from_config() revealed that
BUSY policy, BUSY idle sense and the colour code map were never serialized at
all, which is why packet 0x31 now exists.

THIS FILE REPLACES od_epd_ref.py, which drove the same panels from the YAML
directly. Two things made it a liability rather than a second opinion: it
carried its own copy of the sequence encoder (so the encoder it validated was
not the one that ships), and it implemented the `lut_placement` enum that no
longer exists. A duplicate implementation that can silently disagree is exactly
what this whole change set exists to delete.

`walk()` below is kept structurally identical to run_sequence() in
src/od_epd.c -- forward-only, length-prefixed, no jumps, step-count capped --
so a divergence is visible by reading them side by side.
"""
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

ESC = 0x00
ESC_BUSY, ESC_WAVE, ESC_IMAGE, ESC_LUT, ESC_DELAY = 0x01, 0x02, 0x03, 0x04, 0x05
END = 0x00


def parse_packets(packets):
    """-> (panel, modes, luts) decoded straight from the serialized bytes."""
    from opendisplay.panel_drive import crc16_ccitt

    panel, modes, luts = None, {}, {}
    for p in packets:
        pid, body = p[1], p[2:]
        if pid == 0x31:
            inst, flags, idle, timeout, fixed = struct.unpack("<BBBHH", body[0:7])
            sleep_len = body[12]
            panel = {
                "instance": inst,
                "busy_usable": bool(flags & 0x01),
                "lut_supported": bool(flags & 0x02),
                "busy_idle_level": idle,
                "busy_timeout_ms": timeout,
                "busy_fixed_delay_ms": fixed,
                "code_map": list(body[7:11]),
                "mode_count": body[11],
                "sleep_seq": bytes(body[13:13 + sleep_len]),
            }
        elif pid == 0x2E:
            inst, mi = body[0], body[1]
            seq_len = struct.unpack("<H", body[27:29])[0]
            modes[mi] = {
                "instance": inst,
                "name": body[2:14].rstrip(b"\x00").decode(),
                "passes": body[14],
                "lut_slot": list(body[15:19]),
                "vsn1": list(body[19:23]),
                "temp": struct.unpack("<bb", body[24:26]) + (body[26],),
                "seq": bytes(body[29:29 + seq_len]),
            }
        elif pid == 0x2F:
            _inst, slot, ln, crc = struct.unpack("<BBHH", body[0:6])
            luts[slot] = bytes(body[6:6 + ln])
            if crc16_ccitt(luts[slot]) != crc:
                raise ValueError(
                    f"LUT slot {slot} CRC mismatch -- the device would treat "
                    f"this as ABSENT, not as a short waveform"
                )
    if panel is None:
        raise ValueError("no 0x31 display_panel packet: BUSY policy and the "
                         "colour code map would have nowhere to come from")
    return panel, modes, luts


def walk(seq):
    """Mirror of run_sequence() in od_epd.c.

    Yields ('cmd', c, data) | ('busy',) | ('delay', ms) | ('image',) | ('lut',).
    Wave-header substitution is resolved by the caller, which owns the waveform.
    """
    i = steps = 0
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
        elif esc == ESC_IMAGE:
            yield ("image",)
            i += 2
        elif esc == ESC_LUT:
            yield ("lut",)
            i += 2
        elif esc == ESC_DELAY:
            yield ("delay", seq[i + 2] | (seq[i + 3] << 8))
            i += 4
        elif esc == ESC_WAVE:
            cnt = seq[i + 2]
            yield ("wave", bytes(seq[i + 3:i + 3 + cnt * 2]), cnt)
            i += 3 + cnt * 2
        else:
            raise ValueError(f"bad escape 0x{esc:02X}")


PALETTE = [(2, 0, 0), (1, 1, 1), (2, 1, 1), (3, 1, 1),
           (0, 1, 1), (1, 0, 3), (1, 2, 1), (2, 0, 2),
           (2, 1, 3), (2, 3, 1), (3, 1, 2), (0, 2, 1),
           (0, 3, 1), (3, 2, 1), (0, 1, 0), (3, 0, 1)]


def build_image(w, h, pass_no, code_map):
    """4x4 colour grid. Codes go through the config's code_map, so a wrong map
    shows up as swapped colours rather than as a plausible image -- the
    yellow/red swap that bit the lab firmware."""
    cw, ch = w // 4, h // 4
    out, acc, n = bytearray(), 0, 0
    for y in range(h):
        r = min(y // ch, 3)
        for x in range(w):
            c = min(x // cw, 3)
            logical = PALETTE[r * 4 + c][pass_no] & 3
            acc = ((acc << 2) | (code_map[logical] & 3)) & 0xFF
            n += 1
            if n == 4:
                out.append(acc)
                acc = n = 0
    return bytes(out)


async def run(a):
    import time as _t

    from show16_raw import Raw, _shim_epaper_dithering, load_landing
    _shim_epaper_dithering()
    from lut_lab.waves import default_waves
    from opendisplay.panel_drive import packets_from_yaml
    from opendisplay import OpenDisplayDevice
    from opendisplay.models.capabilities import DeviceCapabilities
    from epaper_dithering import ColorScheme

    colour, edit = default_waves()
    waves = {"wave_colour": bytes(colour), "wave_edit": bytes(edit)}
    packets = packets_from_yaml(HERE / "panels.yaml", int(a.panel_id, 0), waves)
    panel, modes, luts = parse_packets(packets)
    print(f"  decoded {len(modes)} modes, {len(luts)} waveform slots from "
          f"{sum(len(p) for p in packets)} B of packets")
    print(f"  panel: busy={'poll' if panel['busy_usable'] else 'fixed-delay'} "
          f"idle_level={panel['busy_idle_level']} "
          f"timeout={panel['busy_timeout_ms']} fixed={panel['busy_fixed_delay_ms']} "
          f"code_map={panel['code_map']} sleep={len(panel['sleep_seq'])}B")

    mi = next(k for k, v in modes.items() if v["name"] == a.mode)
    m = modes[mi]
    print(f"  mode '{m['name']}': passes={m['passes']} "
          f"slots={m['lut_slot'][:m['passes']]} vsn1={m['vsn1'][:m['passes']]} "
          f"seq={len(m['seq'])}B temp={m['temp'][0]}..{m['temp'][1]}C")

    w, h = a.width, a.height
    info = load_landing().parse_landing_url(a.link)
    caps = DeviceCapabilities(width=w, height=h,
                              color_scheme=ColorScheme.BWRY, rotation=0)
    dev = OpenDisplayDevice(device_name=info.device_name,
                            encryption_key=info.encryption_key,
                            capabilities=caps, timeout=60.0)

    async def wait_busy():
        if panel["busy_usable"]:
            try:
                return await raw.wait_idle(panel["busy_timeout_ms"])
            except RuntimeError:
                return -1
        await asyncio.sleep(panel["busy_fixed_delay_ms"] / 1000)
        return panel["busy_fixed_delay_ms"]

    async with dev:
        raw = Raw(dev, a.chunk, a.ack_every)
        await raw.open(panel_init=False)
        try:
            for p in range(m["passes"]):
                slot = m["lut_slot"][p]
                lut = None
                if slot != 0xFF:
                    lut = bytearray(luts[slot])
                    if m["vsn1"][p]:
                        lut[3] = m["vsn1"][p]
                    lut = bytes(lut)

                await raw.reset(10)
                await wait_busy()
                t_drf = None
                for op in walk(m["seq"]):
                    if op[0] == "busy":
                        el = await wait_busy()
                        if t_drf is not None:
                            t_drf = el if el != -1 else int((_t.time() - t_drf) * 1000)
                    elif op[0] == "delay":
                        await asyncio.sleep(op[1] / 1000)
                    elif op[0] == "image":
                        await raw.data(build_image(w, h, p, panel["code_map"]),
                                       stream=True)
                    elif op[0] == "lut":
                        if lut is None:
                            raise RuntimeError(
                                "sequence has @lut but this pass resolved no "
                                "waveform slot -- the panel would refresh with "
                                "whatever was already in its register")
                        await raw.data(lut)
                    elif op[0] == "wave":
                        hdr = lut if lut else b"\x00" * 7
                        pr, cnt = op[1], op[2]
                        vals = [
                            (hdr[5] ^ 0x80 if pr[k * 2 + 1] == 0xFF else hdr[pr[k * 2 + 1]])
                            if pr[k * 2] == 0xFF else pr[k * 2 + 1]
                            for k in range(cnt)
                        ]
                        await raw.cmd(vals[0], bytes(vals[1:]))
                    else:
                        if op[1] == 0x12:
                            t_drf = _t.time()
                        await raw.cmd(op[1], op[2])
                print(f"    pass {p}: slot={slot} done", flush=True)
            if panel["sleep_seq"]:
                for op in walk(panel["sleep_seq"]):
                    if op[0] == "cmd":
                        await raw.cmd(op[1], op[2])
                    elif op[0] == "delay":
                        await asyncio.sleep(op[1] / 1000)
        finally:
            await raw.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("link")
    ap.add_argument("--panel-id", required=True)
    ap.add_argument("--mode", default="colour16")
    # Geometry still comes from the CLI: it lives in DisplayConfig 0x20, which
    # this harness deliberately does not generate, so the test isolates the NEW
    # packets instead of re-testing the old one.
    ap.add_argument("--width", type=int, required=True)
    ap.add_argument("--height", type=int, required=True)
    ap.add_argument("--chunk", type=int, default=192)
    ap.add_argument("--ack-every", type=int, default=2)
    asyncio.run(run(ap.parse_args()))


if __name__ == "__main__":
    main()
