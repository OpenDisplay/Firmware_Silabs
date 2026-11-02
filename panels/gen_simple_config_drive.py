#!/usr/bin/env python3
"""Generate the `drivePackets` block for simple-config-presets.json.

The toolbox builds a config from three presets (driver board, display, power).
Until now a display preset carried only geometry, so a device configured by the
toolbox had no drive data at all -- and with bb_epaper removed from the
firmware, no drive data means no panel.

This writes the 0x31 / 0x2E / 0x2F packets into each display preset that
panels.yaml describes, in exactly the shape the toolbox's own `extraPackets`
mechanism already uses: {"pid": "<decimal>", "fields": {...}}.

GENERATED, NOT HAND-EDITED. A waveform is 535 bytes, so one preset carries
1070 characters of hex. Hand-maintaining that is how the yellow/red swap and
the four silently-diverged panels happened in the first place. Re-run this
whenever panels.yaml or the waveforms change:

    python3 gen_simple_config_drive.py            # writes the presets file
    python3 gen_simple_config_drive.py --check    # CI: fail if stale

FIELD TYPES MATTER HERE. The toolbox serializes a field of size <= 8 with no
declared type as ONE little-endian integer. lut_slot and vsn1 are size-4
PER-PASS BYTE ARRAYS, and code_map is a size-4 map, so all three are declared
`type: bytes` in config.yaml; without that the passes come out reversed and
pass 0's slot lands on pass 3. That is why this script emits them as hex
strings and asserts the declared types before writing anything.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / "py-opendisplay" / "src"))
sys.path.insert(1, str(HERE.parent / "scripts"))

TOOLBOX = ROOT / "opendisplay.org" / "httpdocs" / "firmware" / "toolbox"
PRESETS = TOOLBOX / "simple-config-presets.json"
SCHEMA = TOOLBOX / "config.yaml"

# panel_ic_type -> the preset id that declares it. Taken from the presets file
# itself rather than assumed, so a renamed preset fails loudly.
PID_FIELD = "panelIcType"

PKT_DISPLAY_MODE = "46"   # 0x2E
PKT_DISPLAY_LUT = "47"    # 0x2F
PKT_DISPLAY_PANEL = "49"  # 0x31


def _hex(b: bytes) -> str:
    return b.hex()


def check_schema_types() -> None:
    """Refuse to generate against a schema that would mis-encode the bytes."""
    import yaml

    doc = yaml.safe_load(SCHEMA.read_text())

    def find(node):
        if isinstance(node, dict):
            for k, v in node.items():
                if isinstance(v, dict) and "fields" in v and "name" in v:
                    yield str(k), v
                elif isinstance(v, (dict, list)):
                    yield from find(v)
        elif isinstance(node, list):
            for x in node:
                yield from find(x)

    packets = dict(find(doc))
    want = {
        (PKT_DISPLAY_MODE, "lut_slot"): "bytes",
        (PKT_DISPLAY_MODE, "vsn1"): "bytes",
        (PKT_DISPLAY_MODE, "mode_name"): "text",
        (PKT_DISPLAY_PANEL, "code_map"): "bytes",
    }
    for (pid, fname), typ in want.items():
        if pid not in packets:
            raise SystemExit(f"config.yaml has no packet {pid}")
        fld = next((f for f in packets[pid]["fields"] if f["name"] == fname), None)
        if fld is None:
            raise SystemExit(f"packet {pid} has no field {fname}")
        if fld.get("type") != typ:
            raise SystemExit(
                f"packet {pid} field {fname} must be `type: {typ}`; it is "
                f"{fld.get('type')!r}. A size-{fld['size']} field without it is "
                f"serialized as a little-endian integer, which silently "
                f"reverses per-pass bytes."
            )


def drive_packets_for(panel_id: int, waves: dict[str, bytes]) -> list[dict]:
    """-> toolbox preset entries for one panel, built by the SHIPPING encoder."""
    from opendisplay.panel_drive import packets_from_yaml

    raw = packets_from_yaml(HERE / "panels.yaml", panel_id, waves)
    sys.path.insert(0, str(HERE))
    from drive_from_packets import parse_packets

    panel, modes, luts = parse_packets(raw)

    out: list[dict] = [{
        "pid": PKT_DISPLAY_PANEL,
        "fields": {
            "instance_number": "0x0",
            "panel_flags": hex(
                (0x01 if panel["busy_usable"] else 0)
                | (0x02 if panel["lut_supported"] else 0)
            ),
            "busy_idle_level": str(panel["busy_idle_level"]),
            "busy_timeout_ms": str(panel["busy_timeout_ms"]),
            "busy_fixed_delay_ms": str(panel["busy_fixed_delay_ms"]),
            "code_map": _hex(bytes(panel["code_map"])),
            "mode_count": str(panel["mode_count"]),
            "sleep_seq_len": str(len(panel["sleep_seq"])),
            "sleep_seq": _hex(panel["sleep_seq"]),
            "reserved": "0" * 14,
        },
    }]
    for mi, m in sorted(modes.items()):
        out.append({
            "pid": PKT_DISPLAY_MODE,
            "fields": {
                "instance_number": "0x0",
                "mode_index": str(mi),
                "mode_name": m["name"],
                "passes": str(m["passes"]),
                "lut_slot": _hex(bytes(m["lut_slot"])),
                "vsn1": _hex(bytes(m["vsn1"])),
                "mode_flags": "0x0",
                "temp_min_c": str(m["temp"][0]),
                "temp_max_c": str(m["temp"][1]),
                "temp_policy": str(m["temp"][2]),
                "seq_len": str(len(m["seq"])),
                "init_seq": _hex(m["seq"]),
            },
        })
    for slot, data in sorted(luts.items()):
        from opendisplay.panel_drive import crc16_ccitt
        out.append({
            "pid": PKT_DISPLAY_LUT,
            "fields": {
                "instance_number": "0x0",
                "lut_slot": str(slot),
                "lut_len": str(len(data)),
                "crc16": hex(crc16_ccitt(data)),
                "data": _hex(data),
            },
        })
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true",
                    help="exit non-zero if the presets file is out of date")
    a = ap.parse_args()

    import yaml
    from lut_lab.waves import default_waves

    check_schema_types()
    colour, edit = default_waves()
    waves = {"wave_colour": bytes(colour), "wave_edit": bytes(edit)}

    panels_doc = yaml.safe_load((HERE / "panels.yaml").read_text())
    by_ic = {int(str(p["id"]), 0): p for p in panels_doc["panels"]}

    presets = json.loads(PRESETS.read_text())
    changed: list[str] = []
    for entry in presets["displays"]:
        ic = entry.get(PID_FIELD)
        if ic is None:
            continue
        ic_num = int(str(ic), 0)
        if ic_num not in by_ic:
            continue
        new = drive_packets_for(ic_num, waves)
        if entry.get("drivePackets") != new:
            entry["drivePackets"] = new
            changed.append(f"{entry['id']} (panel_ic_type {ic_num}, "
                           f"{len(new)} packets)")

    unmatched = set(by_ic) - {
        int(str(e[PID_FIELD]), 0) for e in presets["displays"] if e.get(PID_FIELD)
    }
    if unmatched:
        print("WARNING: panels.yaml describes panels with no display preset: "
              + ", ".join(hex(u) for u in sorted(unmatched)))

    if a.check:
        if changed:
            print("simple-config-presets.json is STALE; re-run without "
                  "--check:\n  " + "\n  ".join(changed))
            return 1
        print("simple-config-presets.json drive data is up to date")
        return 0

    if not changed:
        print("no change")
        return 0
    PRESETS.write_text(json.dumps(presets, indent=2, ensure_ascii=False) + "\n")
    print("updated:\n  " + "\n  ".join(changed))
    print(f"presets file now {PRESETS.stat().st_size} B")
    return 0


if __name__ == "__main__":
    sys.exit(main())
