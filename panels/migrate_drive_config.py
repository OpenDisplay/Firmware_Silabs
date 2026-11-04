#!/usr/bin/env python3
"""Rebuild a device's drive data as length-prefixed 0xAE-0xB1 packets (Rev 2).

This is the fleet-migration tool DRIVER_MIGRATION.md §7/§8 calls for:

    interrogate            -> the parser DROPS the burned Rev 1 packets
                              (0x2E-0x31) on sight, so clearing is implicit
    panels.yaml            -> fresh 0xAE/0xAF/0xB1 via the SHIPPING encoder
    palette JSON           -> 0xB0 calibration (measured on ODBB1B76; every
                              other unit carries it as a borrowed placeholder,
                              measured=0, same as before the renumbering)
    write_config           -> read back and verify the drive data is present

Idempotent by construction: the models are REPLACED, not appended, so running
it twice cannot duplicate packets (the Rev 1 splice tooling could).

Usage:
    migrate_drive_config.py <landing-link> <panel_id> [--measured]
    panel_id: 0x001E (2.9"), 0x001C (2.6"), 0x003A (4.2"), 0x0040 (7.5")
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

PALETTE_JSON = HERE / "palette16_measured.json"


async def run(a) -> int:
    from show16_raw import _shim_epaper_dithering, load_landing
    _shim_epaper_dithering()
    from lut_lab.waves import default_waves
    from opendisplay import OpenDisplayDevice
    from opendisplay.models.capabilities import DeviceCapabilities
    from opendisplay.models.config import DisplayCalibrationInfo
    from opendisplay.panel_drive import packets_from_yaml
    from opendisplay.protocol.config_parser import (
        _parse_display_lut,
        _parse_display_mode,
        _parse_display_panel,
    )
    from epaper_dithering import ColorScheme

    colour, edit = default_waves()
    waves = {"wave_colour": bytes(colour), "wave_edit": bytes(edit)}
    packets = packets_from_yaml(HERE / "panels.yaml", a.panel_id, waves)

    panels, modes, luts = [], [], []
    for p in packets:
        pid, body = p[1], p[4:]
        if pid == 0xB1:
            panels.append(_parse_display_panel(body))
        elif pid == 0xAE:
            modes.append(_parse_display_mode(body))
        elif pid == 0xAF:
            luts.append(_parse_display_lut(body))

    pal = json.loads(PALETTE_JSON.read_text())

    # The palette's codes are RAW PANEL CODES, and the raw code -> colour
    # assignment is panel-specific: the reference was measured on the 2.9"
    # (code 2 = red, 3 = yellow) while the F52 family is the other way round.
    # A panel that declares palette_code_swap gets the codes permuted into its
    # own order, so the SAME Lab is reached by whatever code reaches it here.
    import yaml  # noqa: PLC0415
    doc = yaml.safe_load((HERE / "panels.yaml").read_text(encoding="utf-8"))
    entry = next(p for p in doc["panels"] if int(str(p["id"]), 0) == a.panel_id)
    swap = (entry.get("colour") or {}).get("palette_code_swap")
    if swap:
        print(f"  palette codes permuted through {swap} for this panel")

    def codes_for(e):
        c = tuple(e["codes"])
        return tuple(swap[v] for v in c) if swap else c

    colour16 = next((m for m in modes if m.name == "colour16"), None)
    cals = []
    if colour16 is not None:
        cals.append(DisplayCalibrationInfo(
            instance_number=0,
            mode_index=colour16.mode_index,
            entry_count=len(pal["entries"]),
            measured=bool(a.measured),
            lab=[tuple(e["lab"]) for e in pal["entries"]],
            codes=[codes_for(e) for e in pal["entries"]],
        ))

    info = load_landing().parse_landing_url(a.link)
    caps = DeviceCapabilities(width=168, height=384,
                              color_scheme=ColorScheme.BWRY, rotation=0)
    dev = OpenDisplayDevice(device_name=info.device_name,
                            encryption_key=info.encryption_key,
                            capabilities=caps, timeout=180.0)
    async with dev:
        cfg = await dev.interrogate()
        print(f"{info.device_name}: config in, {len(cfg.display_modes)} Rev2 modes, "
              f"{len(cfg.unknown_ex_packets)} unknown ex, findmy={bool(cfg.findmy_raw)}")
        cfg.display_panels = panels
        cfg.display_modes = modes
        cfg.display_luts = luts
        cfg.display_calibrations = cals
        await dev.write_config(cfg)
        print("config written; reading back...")
        back = await dev.interrogate()
        ok = (len(back.display_modes) == len(modes)
              and len(back.display_luts) == len(luts)
              and len(back.display_panels) == len(panels)
              and len(back.display_calibrations) == len(cals)
              and all(b.init_seq == m.init_seq
                      for b, m in zip(sorted(back.display_modes, key=lambda x: x.mode_index),
                                      sorted(modes, key=lambda x: x.mode_index)))
              and all(b.data == l.data
                      for b, l in zip(sorted(back.display_luts, key=lambda x: x.lut_slot),
                                      sorted(luts, key=lambda x: x.lut_slot))))
        print(f"read-back: modes={len(back.display_modes)} luts={len(back.display_luts)} "
              f"panel={len(back.display_panels)} cal={len(back.display_calibrations)} "
              f"-> {'VERIFIED' if ok else 'MISMATCH'}")
        return 0 if ok else 1


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("link")
    ap.add_argument("panel_id", type=lambda s: int(s, 0))
    ap.add_argument("--measured", action="store_true",
                    help="palette was measured on THIS unit (ODBB1B76 only)")
    return asyncio.run(run(ap.parse_args()))


if __name__ == "__main__":
    sys.exit(main())
