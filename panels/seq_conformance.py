#!/usr/bin/env python3
"""Diff the shipping C sequence interpreter against the Python one.

run_sequence() in src/od_epd.c is what the device executes. walk() in
drive_from_packets.py is what the hardware validation executes. Both read the
same bytes out of the same config packets, and if they diverge the hardware
test passes while the device misbehaves -- invisibly, because either way the
panel just sees a plausible stream of register writes.

So: compile the real C, run it over the real packet bytes for every panel and
every mode, and require the operation streams to be identical. No mock of
od_epd, only of the GPIO/SPI under it.

Run: python3 seq_conformance.py
"""
from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / "py-opendisplay" / "src"))
sys.path.insert(1, str(HERE.parent / "scripts"))
sys.path.insert(2, str(HERE))

BIN = Path(tempfile.gettempdir()) / "od_seqconf"


def build() -> None:
    cmd = [
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
        f"-I{HERE.parent / 'include'}",
        "-o", str(BIN), str(HERE / "seq_conformance.c"),
        str(HERE.parent / "src" / "od_epd.c"),
    ]
    subprocess.run(cmd, check=True)


def python_ops(seq: bytes, lut: bytes | None, passes: int,
               slots: list[int], vsn1: list[int],
               busy_usable: bool, fixed_ms: int) -> list[str]:
    """Same op stream the C harness prints, produced by the Python walk."""
    from drive_from_packets import walk

    out: list[str] = []
    for p in range(passes):
        out.append(f"pass {p}")
        this_lut = None
        if slots[p] != 0xFF and lut is not None:
            b = bytearray(lut)
            if vsn1[p]:
                b[3] = vsn1[p]
            this_lut = bytes(b)

        out.append("reset 10")
        if not busy_usable:
            out.append(f"delay {fixed_ms}")
        # busy_usable: io_busy_level() returns the idle level, so the C poll
        # exits immediately and emits nothing. Matching that exactly is the
        # point -- a spurious delay here would mask a real one there.

        paused = False
        for op in walk(seq):
            if op[0] == "cmd":
                out.append(f"cmd {op[1]:02x}")
                if op[2]:
                    out.append(_data(op[2]))
            elif op[0] == "wave":
                hdr = this_lut if this_lut else b"\x00" * 7
                pr, cnt = op[1], op[2]
                vals = [
                    (hdr[5] ^ 0x80 if pr[k * 2 + 1] == 0xFF else hdr[pr[k * 2 + 1]])
                    if pr[k * 2] == 0xFF else pr[k * 2 + 1]
                    for k in range(cnt)
                ]
                out.append(f"cmd {vals[0]:02x}")
                if cnt > 1:
                    out.append(_data(bytes(vals[1:])))
            elif op[0] == "busy":
                if not busy_usable:
                    out.append(f"delay {fixed_ms}")
            elif op[0] == "delay":
                out.append(f"delay {op[1]}")
            elif op[0] == "lut":
                assert this_lut is not None
                for i in range(0, len(this_lut), 64):
                    out.append(_data(this_lut[i:i + 64]))
            elif op[0] == "image":
                out.append("IMAGE")
                paused = True
        assert paused, "sequence never reached an image marker"
    return out


def _data(b: bytes) -> str:
    return "data " + str(len(b)) + "".join(f" {x:02x}" for x in b[:8])


def main() -> int:
    import yaml
    from lut_lab.waves import default_waves
    from opendisplay.panel_drive import packets_from_yaml
    from drive_from_packets import parse_packets

    build()
    colour, edit = default_waves()
    waves = {"wave_colour": bytes(colour), "wave_edit": bytes(edit)}
    doc = yaml.safe_load((HERE / "panels.yaml").read_text())

    failures = 0
    checked = 0
    for pdef in doc["panels"]:
        pid = int(str(pdef["id"]), 0)
        packets = packets_from_yaml(HERE / "panels.yaml", pid, waves)
        panel, modes, luts = parse_packets(packets)
        for mi, m in sorted(modes.items()):
            slots = m["lut_slot"]
            lut = luts.get(slots[0]) if slots[0] != 0xFF else None
            with tempfile.TemporaryDirectory() as td:
                sq = Path(td) / "seq.bin"
                sq.write_bytes(m["seq"])
                args = [
                    str(BIN), str(sq), str(m["passes"]),
                    ",".join(str(s) for s in slots),
                    ",".join(str(v) for v in m["vsn1"]),
                    "1" if panel["busy_usable"] else "0",
                    str(panel["busy_idle_level"]),
                    str(panel["busy_timeout_ms"]),
                    str(panel["busy_fixed_delay_ms"]),
                ]
                if lut is not None:
                    lf = Path(td) / "lut.bin"
                    lf.write_bytes(lut)
                    args.append(str(lf))
                r = subprocess.run(args, capture_output=True, text=True)
                if r.returncode != 0:
                    print(f"FAIL {pdef['name']} {m['name']}: C harness exited "
                          f"{r.returncode}: {r.stderr.strip()}")
                    failures += 1
                    continue
                c_ops = r.stdout.strip().splitlines()

            py_ops = python_ops(m["seq"], lut, m["passes"], slots, m["vsn1"],
                                panel["busy_usable"], panel["busy_fixed_delay_ms"])
            checked += 1
            if c_ops == py_ops:
                print(f"ok   {pdef['name']:18s} {m['name']:9s} "
                      f"{len(c_ops):4d} ops identical")
            else:
                failures += 1
                print(f"FAIL {pdef['name']} {m['name']}: C and Python diverge")
                for i, (a, b) in enumerate(zip(c_ops, py_ops)):
                    if a != b:
                        print(f"   first difference at op {i}:")
                        print(f"     C      {a!r}")
                        print(f"     Python {b!r}")
                        break
                else:
                    print(f"   length differs: C {len(c_ops)} vs "
                          f"Python {len(py_ops)}")
                    n = min(len(c_ops), len(py_ops))
                    extra = (c_ops if len(c_ops) > n else py_ops)[n:]
                    print(f"   trailing: {extra[:5]}")

    print(f"\n{checked - failures}/{checked} mode(s) conformant")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
