#!/usr/bin/env python3
"""Build a realistic config blob and check the REAL od_cfg_index.c walks it.

A blob is:

    [2 bytes][version] [pkt]* [crc16]
    pkt = [packet_number][packet_type][body]

and the two writers disagree about BOTH header bytes without it mattering to
the firmware, which is exactly why this test covers all of them:

  * the leading 2 bytes are the total length in the toolbox
    (collectPacketBytes) and zero padding in py-opendisplay (serialize_config);
  * packet_number is a running counter in the toolbox and the instance index in
    py-opendisplay, and NEITHER is read -- identity is instance_number at
    body[0] plus a per-type discriminator at body[1].

Every case deliberately includes a SECURITY (0x27) packet, because that is what
broke: 0x27 sorts before the drive packets, so a missing size entry abandoned
the tail and the panel silently had no drive data.

It is wrapped in the 16-byte storage record header od_cfg_index.c expects
(magic, version, crc, data_len) so the stub NVM3 serves the same bytes the
device would.

Run: python3 cfg_index_test.py
"""
from __future__ import annotations

import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
PANELS = HERE.parent
FW = PANELS.parent
ROOT = FW.parent
sys.path.insert(0, str(ROOT / "py-opendisplay" / "src"))
sys.path.insert(1, str(FW / "scripts"))
sys.path.insert(2, str(PANELS))

MAGIC = 0xDEADBEEF
HDR_SZ = 16

# (packet_type, body_size) for the filler packets, in the order a real config
# emits them (ascending type, which is what the serializer guarantees).
FILLER = [
    (0x01, 22),   # system
    (0x02, 22),   # manufacturer
    (0x04, 30),   # power
    (0x20, 46),   # display
    (0x21, 22),   # led
    (0x27, 64),   # security  <-- the one that broke the walk
]


def crc16_ccitt(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def build_blob(drive_packets: list[bytes], *, seq_header: bool,
               length_prefix: bool = False) -> bytes:
    """A full config blob.

    seq_header=True writes a running counter in each packet's first byte, which
    is what the web toolbox does. seq_header=False writes the instance index,
    which is what py-opendisplay does. The device must find its packets either
    way -- identity lives in the body -- and this is the knob that proves it.

    length_prefix mirrors the OTHER disagreement between the two writers: the
    first two bytes of the blob are the total length in the toolbox
    (collectPacketBytes) and plain zero padding in py-opendisplay
    (serialize_config calls them "2 bytes padding"). The firmware parser skips
    them without looking, so both are legal -- but assuming the py layout is
    what made the first attempt at this test reject the toolbox's real output.
    """
    out = bytearray(b"\x00\x00" + bytes([1]))
    seq = 0
    for ptype, bsize in FILLER:
        body = bytearray(bsize)
        body[0] = 0          # instance_number
        out += bytes([seq if seq_header else 0, ptype]) + bytes(body)
        seq += 1
    for p in drive_packets:
        # p is [disc][type][body] from packets_from_yaml; re-stamp byte 0.
        out += bytes([seq if seq_header else p[0]]) + p[1:]
        seq += 1
    if length_prefix:
        total = len(out) + 2
        out[0] = total & 0xFF
        out[1] = (total >> 8) & 0xFF
    out += struct.pack("<H", crc16_ccitt(bytes(out)))
    return bytes(out)


def wrap_record(blob: bytes) -> bytes:
    """The 16-byte storage record header od_cfg_index.c reads."""
    return (
        struct.pack("<I", MAGIC)
        + struct.pack("<I", 1)
        + struct.pack("<I", crc16_ccitt(blob))
        + struct.pack("<I", len(blob))
        + blob
    )


def build_binary() -> Path:
    exe = Path(tempfile.gettempdir()) / "od_cfg_index_test"
    cmd = [
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
        f"-I{HERE / 'stubs'}", f"-I{FW}", f"-I{FW / 'include'}",
        "-o", str(exe),
        str(HERE / "cfg_index_test.c"), str(FW / "od_cfg_index.c"),
    ]
    subprocess.run(cmd, check=True)
    return exe


def run(exe: Path, record: bytes) -> dict:
    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as t:
        t.write(record)
        path = t.name
    r = subprocess.run([str(exe), path], capture_output=True, text=True)
    if r.returncode != 0:
        raise AssertionError(f"test binary failed: {r.returncode} {r.stderr}")
    out: dict = {"entries": [], "reads": []}
    for line in r.stdout.splitlines():
        if line.startswith("entries "):
            out["n"] = int(line.split()[1])
        elif line.startswith("blob_len "):
            out["blob_len"] = int(line.split()[1])
        elif line.startswith("entry "):
            f = line.split()
            out["entries"].append({
                "type": int(f[3], 16), "name": f[4],
                "instance": int(f[6]), "sub": int(f[8]),
                "offset": int(f[10]), "len": int(f[12]),
            })
        elif line.startswith("read "):
            out["reads"].append(line)
    return out


def main() -> int:
    from lut_lab.waves import default_waves
    from opendisplay.panel_drive import packets_from_yaml

    exe = build_binary()
    colour, edit = default_waves()
    waves = {"wave_colour": bytes(colour), "wave_edit": bytes(edit)}
    drive = packets_from_yaml(PANELS / "panels.yaml", 0x001E, waves)

    # The third case is not synthetic: it is the byte-for-byte output of
    # collectPacketBytes() in the running toolbox, for
    # ?display=ep29yr-168x384&driver=m3-pro-silabs. Captured as a fixture so a
    # change to the toolbox's own layout shows up here instead of on a panel.
    cases = [
        ("toolbox layout (counter + length prefix)",
         build_blob(drive, seq_header=True, length_prefix=True)),
        ("py-opendisplay layout (instance + zero padding)",
         build_blob(drive, seq_header=False)),
        ("REAL toolbox output (captured fixture)",
         (HERE / "toolbox_2v9_blob.bin").read_bytes()),
    ]

    failures = 0
    for label, blob in cases:
        res = run(exe, wrap_record(blob))
        kinds = [e["type"] for e in res["entries"]]
        problems = []

        if res.get("blob_len") != len(blob):
            problems.append(f"blob_len {res.get('blob_len')} != {len(blob)}")
        # Every filler packet plus every drive packet must be indexed.
        # The fixture carries the toolbox's full packet set, which is larger
        # than FILLER, so require "every drive packet plus at least the filler"
        # rather than an exact count.
        n_drive = sum(1 for e in res["entries"] if e["type"] in (0x2E, 0x2F, 0x31))
        if n_drive != len(drive):
            problems.append(f"indexed {n_drive} drive packets, expected "
                            f"{len(drive)} -- the walk stopped early")
        for want in (0x27, 0x31, 0x2E, 0x2F):
            if want not in kinds:
                problems.append(f"packet 0x{want:02X} not indexed")
        # The two 0x2E packets must be distinguishable by `sub` (mode_index),
        # and the two 0x2F by `sub` (lut_slot). If identity were taken from the
        # header byte, the toolbox variant would collapse them.
        modes = [e["sub"] for e in res["entries"] if e["type"] == 0x2E]
        luts = [e["sub"] for e in res["entries"] if e["type"] == 0x2F]
        if sorted(modes) != [0, 1]:
            problems.append(f"mode discriminators {modes}, expected [0, 1]")
        if sorted(luts) != [0, 1]:
            problems.append(f"lut slots {luts}, expected [0, 1]")
        # Offsets must land on the body: verify against the blob directly.
        for e in res["entries"]:
            body = blob[e["offset"]:e["offset"] + e["len"]]
            if len(body) != e["len"]:
                problems.append(f"0x{e['type']:02X} offset {e['offset']} "
                                f"runs past the blob")
            elif body[0] != e["instance"]:
                problems.append(f"0x{e['type']:02X} offset {e['offset']} does "
                                f"not point at the body (instance {body[0]} "
                                f"vs {e['instance']})")

        if problems:
            failures += 1
            print(f"FAIL {label}")
            for p in problems:
                print(f"   {p}")
        else:
            print(f"ok   {label}: {res['n']} packets indexed, "
                  f"blob {res['blob_len']} B, modes {modes}, luts {luts}")

    print(f"\n{len(cases) - failures}/{len(cases)} layout(s) walked correctly")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
