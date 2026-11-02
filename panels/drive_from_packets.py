#!/usr/bin/env python3
"""Drive a panel from SERIALIZED CONFIG PACKETS, not from panels.yaml.

This closes the loop. od_epd_ref.py proved the YAML is sufficient; this proves
the WIRE FORMAT is sufficient, which is the thing the firmware will actually
receive. The path exercised here is:

    panels.yaml -> panel_drive.packets_from_yaml() -> bytes
                -> parse back out of those bytes
                -> execute over CMD_SPI_RAW

Nothing between the parse and the panel consults the YAML, so anything the
packets fail to carry shows up as a panel that does not render. That is the
only way to find a field you forgot to put on the wire.
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

SEQ_FIELD = 128
LUT_FIELD = 535


def parse_packets(packets):
    """-> (modes, luts) decoded straight from the serialized bytes."""
    modes, luts = {}, {}
    for p in packets:
        pid = p[1]
        body = p[2:]
        if pid == 0x2E:
            inst, mi = body[0], body[1]
            name = body[2:14].rstrip(b"\x00").decode()
            passes = body[14]
            lut_slot = list(body[15:19])
            vsn1 = list(body[19:23])
            flags = body[23]
            tmin, tmax = struct.unpack("<bb", body[24:26])
            policy = body[26]
            seq_len = struct.unpack("<H", body[27:29])[0]
            seq = bytes(body[29:29 + seq_len])
            modes[mi] = {
                "instance": inst, "name": name, "passes": passes,
                "lut_slot": lut_slot, "vsn1": vsn1,
                "analog_from_wave": bool(flags & 1),
                "temp": (tmin, tmax, policy), "seq": seq,
            }
        elif pid == 0x2F:
            inst, slot, ln, crc = struct.unpack("<BBHH", body[0:6])
            luts[slot] = bytes(body[6:6 + ln])
            from opendisplay.panel_drive import crc16_ccitt
            if crc16_ccitt(luts[slot]) != crc:
                raise ValueError(f"LUT slot {slot} CRC mismatch -- would be "
                                 f"treated as absent by the device")
    return modes, luts


async def run(a):
    from show16_raw import Raw, _shim_epaper_dithering, load_landing
    _shim_epaper_dithering()
    from lut_lab.waves import default_waves
    from opendisplay.panel_drive import packets_from_yaml
    from od_epd_ref import walk, build_image
    from opendisplay import OpenDisplayDevice
    from opendisplay.models.capabilities import DeviceCapabilities
    from epaper_dithering import ColorScheme

    colour, edit = default_waves()
    waves = {"wave_colour": bytes(colour), "wave_edit": bytes(edit)}
    pid = int(a.panel_id, 0)
    packets = packets_from_yaml(HERE / "panels.yaml", pid, waves)
    modes, luts = parse_packets(packets)
    print(f"  decoded {len(modes)} modes, {len(luts)} LUT slots from "
          f"{sum(len(p) for p in packets)} B of packets")

    mi = next(k for k, v in modes.items() if v["name"] == a.mode)
    m = modes[mi]
    print(f"  mode '{m['name']}': passes={m['passes']} "
          f"lut_slot={m['lut_slot'][:m['passes']]} vsn1={m['vsn1'][:m['passes']]} "
          f"analog_from_wave={m['analog_from_wave']} seq={len(m['seq'])}B "
          f"temp={m['temp'][0]}..{m['temp'][1]}C")

    # Geometry and BUSY policy still come from DisplayConfig 0x20 on a real
    # device; this harness takes them from the CLI so the test isolates the
    # NEW packets rather than re-testing the old one.
    w, h = a.width, a.height
    info = load_landing().parse_landing_url(a.link)
    caps = DeviceCapabilities(width=w, height=h,
                              color_scheme=ColorScheme.BWRY, rotation=0)
    dev = OpenDisplayDevice(device_name=info.device_name,
                            encryption_key=info.encryption_key,
                            capabilities=caps, timeout=60.0)
    import time as _t
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
                if a.busy_usable:
                    await raw.wait_idle(5000)
                else:
                    await asyncio.sleep(0.5)
                for op in walk(m["seq"], lut if lut else b"\x00" * 7):
                    if op[0] == "busy":
                        if a.busy_usable:
                            await raw.wait_idle(a.busy_timeout)
                        else:
                            await asyncio.sleep(2.0)
                    else:
                        await raw.cmd(op[1], op[2])
                if lut:
                    await raw.cmd(0x20, lut)
                await raw.cmd(0x10, build_image(w, h, p), stream=True)
                if not a.pon_in_init:
                    await raw.cmd(0x04)
                    await asyncio.sleep(2.0)
                t0 = _t.time()
                await raw.cmd(0x12, bytes([0x00]))
                if a.busy_usable:
                    try:
                        el = await raw.wait_idle(a.busy_timeout)
                    except RuntimeError:
                        el = -1
                else:
                    await asyncio.sleep(a.fixed_delay / 1000)
                    el = int((_t.time() - t0) * 1000)
                print(f"    pass {p}: slot={slot} drf={el} ms", flush=True)
            await raw.cmd(0x07, bytes([0xA5]))
        finally:
            await raw.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("link")
    ap.add_argument("--panel-id", required=True)
    ap.add_argument("--mode", default="colour16")
    ap.add_argument("--width", type=int, required=True)
    ap.add_argument("--height", type=int, required=True)
    ap.add_argument("--busy-usable", action="store_true")
    ap.add_argument("--busy-timeout", type=int, default=40000)
    ap.add_argument("--fixed-delay", type=int, default=35000)
    ap.add_argument("--pon-in-init", action="store_true")
    ap.add_argument("--chunk", type=int, default=192)
    ap.add_argument("--ack-every", type=int, default=2)
    asyncio.run(run(ap.parse_args()))


if __name__ == "__main__":
    main()
