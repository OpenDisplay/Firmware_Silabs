#!/usr/bin/env python3
"""Generate the `drivePackets` block for simple-config-presets.json.

The toolbox builds a config from three presets (driver board, display, power).
Until now a display preset carried only geometry, so a device configured by the
toolbox had no drive data at all -- and with bb_epaper removed from the
firmware, no drive data means no panel.

This writes the 0xB1 / 0xAE / 0xAF packets -- plus, for a panel with a
multi-pass mode, the 0xB0 palette that mode needs -- into each display preset
that panels.yaml describes, as {"pid": "<decimal>", "raw": "<body hex>"} entries.
Since structs 2.2 the drive packets are LENGTH-PREFIXED with variable bodies,
so the toolbox carries them as OPAQUE blobs and frames them itself
([seq][id][length:2 LE][body]) -- the schema is never consulted, which also
retires the whole class of field-type mis-encoding this script used to have
to assert against (per-pass byte arrays serialized as little-endian ints).

GENERATED, NOT HAND-EDITED. A waveform is 535 bytes, so one preset carries
1070 characters of hex. Hand-maintaining that is how the yellow/red swap and
the four silently-diverged panels happened in the first place. Re-run this
whenever panels.yaml or the waveforms change:

    python3 gen_simple_config_drive.py            # writes the presets file
    python3 gen_simple_config_drive.py --check    # CI: fail if stale
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

PKT_DISPLAY_MODE = "174"   # 0xAE
PKT_DISPLAY_LUT = "175"    # 0xAF
PKT_DISPLAY_PANEL = "177"  # 0xB1
PKT_DISPLAY_CAL = "176"    # 0xB0

# The reference 16-colour palette, the same file migrate_drive_config.py writes.
# A multi-pass mode with no 0xB0 is not uploadable from the web tool (the
# per-pass codes are what the encoder needs), so a preset without it configures
# a device whose colour16 mode can never be used.
PALETTE_JSON = HERE / "palette16_measured.json"


def _hex(b: bytes) -> str:
    return b.hex()


def drive_packets_for(panel_id: int, waves: dict[str, bytes]) -> list[dict]:
    """-> toolbox preset entries for one panel, built by the SHIPPING encoder.

    packets_from_yaml() emits fully framed length-prefixed packets
    ([number][id][length:2 LE][body]); the preset carries pid + the BODY hex,
    and the toolbox re-frames on serialize. Round-tripping through
    parse_packets() first asserts the blob is self-consistent (CRCs, head
    sizes) before it is ever shipped to a browser.
    """
    from opendisplay.panel_drive import packets_from_yaml

    raw = packets_from_yaml(HERE / "panels.yaml", panel_id, waves)
    sys.path.insert(0, str(HERE))
    from drive_from_packets import parse_packets

    parse_packets(raw)   # validation only: raises on any inconsistency

    out: list[dict] = []
    for p in raw:
        pid = p[1]
        body_len = int.from_bytes(p[2:4], "little")
        body = p[4:]
        if len(body) != body_len:
            raise SystemExit(f"packet 0x{pid:02X}: framed length disagrees with body")
        out.append({"pid": str(pid), "raw": _hex(body)})
    return out


def calibration_packets_for(panel_entry: dict, drive: list[dict]) -> list[dict]:
    """-> the 0xB0 entry for each multi-pass mode in `drive`, as migrate_drive_config.py builds it.

    The palette is BORROWED (measured on ODBB1B76), so it ships measured=0. Its
    codes are raw panel codes and the raw code -> colour assignment is
    panel-specific, so a panel declaring palette_code_swap gets them permuted.
    """
    from opendisplay.models.config import DisplayCalibrationInfo
    from opendisplay.protocol.config_parser import _parse_display_mode
    from opendisplay.protocol.config_serializer import serialize_display_calibration

    pal = json.loads(PALETTE_JSON.read_text())
    swap = (panel_entry.get("colour") or {}).get("palette_code_swap")

    def codes_for(e):
        c = tuple(e["codes"])
        return tuple(swap[v] for v in c) if swap else c

    out: list[dict] = []
    for dp in drive:
        if dp["pid"] != PKT_DISPLAY_MODE:
            continue
        mode = _parse_display_mode(bytes.fromhex(dp["raw"]))
        if mode.passes <= 1:
            continue
        if len(pal["entries"][0]["codes"]) != mode.passes:
            raise SystemExit(f"palette has {len(pal['entries'][0]['codes'])}-pass codes, "
                             f"mode {mode.name!r} has {mode.passes} passes")
        cal = DisplayCalibrationInfo(
            instance_number=mode.instance_number,
            mode_index=mode.mode_index,
            entry_count=len(pal["entries"]),
            measured=False,
            lab=[tuple(e["lab"]) for e in pal["entries"]],
            codes=[codes_for(e) for e in pal["entries"]],
        )
        out.append({"pid": PKT_DISPLAY_CAL, "raw": _hex(serialize_display_calibration(cal))})
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true",
                    help="exit non-zero if the presets file is out of date")
    a = ap.parse_args()

    import yaml
    from lut_lab.waves import default_waves

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
        new += calibration_packets_for(by_ic[ic_num], new)
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
