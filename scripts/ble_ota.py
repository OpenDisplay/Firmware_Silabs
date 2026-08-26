#!/usr/bin/env python3
"""Targeted Silicon Labs BLE OTA for OpenDisplay BG22.

Finds a device by GAP name (substring), sends CMD_ENTER_DFU (0x0051), then
flashes a .gbl with silabs-ble-ota (AppLoader).

  pip install silabs-ble-ota bleak cryptography

Example:
  ./scripts/ble_ota.py --name ODDEE7 --gbl artifacts/opendisplay-bg22.gbl
"""

from __future__ import annotations

import argparse
import asyncio
import os
import sys
import time
from pathlib import Path

CHAR_UUID = "00002446-0000-1000-8000-00805f9b34fb"
CMD_ENTER_DFU = bytes([0x00, 0x51])


def _die(msg: str, code: int = 1) -> None:
    print(f"ERROR: {msg}", file=sys.stderr)
    raise SystemExit(code)


def _require_deps() -> None:
    missing = []
    try:
        import bleak  # noqa: F401
    except ImportError:
        missing.append("bleak")
    try:
        import silabs_ble_ota  # noqa: F401
    except ImportError:
        missing.append("silabs-ble-ota")
    if missing:
        root = Path(__file__).resolve().parents[1]
        _die(
            "missing packages: "
            + ", ".join(missing)
            + "\n  python3 -m venv "
            + str(root / ".venv-ble-ota")
            + "\n  "
            + str(root / ".venv-ble-ota/bin/pip")
            + " install -r "
            + str(root / "scripts/requirements-ble-ota.txt")
        )


def _normalize_name(s: str) -> str:
    return "".join(ch for ch in s.strip().upper() if ch.isalnum())


async def _scan_named(needle: str, timeout: float) -> list[tuple[str, str, object]]:
    """Return (name, address, BLEDevice) matches for needle (substring, case-insensitive)."""
    from bleak import BleakScanner

    want = _normalize_name(needle)
    if not want:
        _die("empty --name")

    print(f"==> Scanning BLE up to {timeout:.0f}s for name containing '{needle}' …")
    devices = await BleakScanner.discover(timeout=timeout, return_adv=True)
    matches: list[tuple[str, str, object]] = []
    for _addr, (dev, adv) in devices.items():
        name = dev.name or adv.local_name or ""
        if not name:
            continue
        if want in _normalize_name(name):
            matches.append((name, dev.address, dev))
    return matches


async def _pick_device(needle: str, timeout: float):
    matches = await _scan_named(needle, timeout)
    if not matches:
        _die(f"no advertising device matched '{needle}'")
    if len(matches) > 1:
        print("Multiple matches:", file=sys.stderr)
        for name, addr, _ in matches:
            print(f"  {name}  ({addr})", file=sys.stderr)
        _die("refine --name so it matches exactly one device")
    name, addr, dev = matches[0]
    print(f"==> Target: {name} @ {addr}")
    return name, addr, dev


async def _find_by_address(address: str, timeout: float):
    """Return a fresh BLEDevice for ``address``, or None if not seen."""
    from bleak import BleakScanner

    try:
        found = await BleakScanner.find_device_by_address(address, timeout=timeout)
        if found is not None:
            return found
    except Exception as ex:  # noqa: BLE001 - fall through to discover
        print(f"==> find_device_by_address: {ex}")

    # Fallback: one-shot discover (some bleak/BlueZ builds are flaky on finder)
    remaining = max(1.0, min(timeout, 5.0))
    devices = await BleakScanner.discover(timeout=remaining, return_adv=True)
    for addr, (dev, _adv) in devices.items():
        if addr.lower() == address.lower():
            return dev
    return None


async def _wait_address(address: str, timeout: float):
    """Re-discover BLEDevice by address after AppLoader reboot."""
    deadline = time.monotonic() + timeout
    print(f"==> Waiting for AppLoader at {address} (up to {timeout:.0f}s) …")
    while time.monotonic() < deadline:
        remaining = max(1.0, min(8.0, deadline - time.monotonic()))
        dev = await _find_by_address(address, remaining)
        if dev is not None:
            print(f"==> Seen again: {dev.name or '(no name)'} @ {dev.address}")
            return dev
        await asyncio.sleep(0.25)
    _die(f"device {address} did not reappear (AppLoader?) within {timeout:.0f}s")


def _is_transient_ota_connect_error(exc: BaseException) -> bool:
    """True when silabs-ble-ota never got a usable AppLoader link (safe to retry)."""
    msg = str(exc).lower()
    # perform_silabs_ota wraps connect failures as "Could not connect to AppLoader: …".
    # Mid-transfer errors must not be retried — a successful connect arms reboot-on-drop.
    if "could not connect" in msg:
        return True
    needles = ("disappeared", "not found", "device disappeared")
    return any(n in msg for n in needles) and "silabs ota failed" not in msg


async def _authenticate(client, master_key: bytes, timeout: float = 5.0) -> object:
    """OpenDisplay 0x0050 handshake (matches Firmware/tools/od-device-cli.py)."""
    tools = Path(__file__).resolve().parents[2] / "Firmware" / "tools"
    if str(tools) not in sys.path:
        sys.path.insert(0, str(tools))
    try:
        from ble_crypto import (  # type: ignore
            BleSession,
            compute_challenge_response,
            compute_server_response,
            derive_session_id,
            derive_session_key,
        )
    except ImportError:
        _die("auth requested but Firmware/tools/ble_crypto.py is unavailable")

    import os as _os

    box: dict[str, bytes] = {}
    ev = asyncio.Event()

    def on_notify(_handle, data: bytearray) -> None:
        box["data"] = bytes(data)
        ev.set()

    await client.start_notify(CHAR_UUID, on_notify)

    async def exchange(payload: bytes) -> bytes:
        ev.clear()
        await client.write_gatt_char(CHAR_UUID, payload, response=False)
        await asyncio.wait_for(ev.wait(), timeout=timeout)
        return box["data"]

    session = BleSession(master_key=master_key)
    resp1 = await exchange(bytes([0x00, 0x50, 0x00]))
    status1 = resp1[2] if len(resp1) > 2 else 0xFF
    if status1 == 0x03:
        raise RuntimeError("device does not have encryption enabled (omit --key)")
    if status1 == 0x04:
        raise RuntimeError("authentication rate-limited by device")
    if status1 != 0x00 or len(resp1) < 23:
        raise RuntimeError(f"auth challenge failed (status 0x{status1:02X})")
    server_nonce = resp1[3:19]
    device_id = resp1[19:23]
    client_nonce = _os.urandom(16)
    proof = compute_challenge_response(master_key, server_nonce, client_nonce, device_id)
    resp2 = await exchange(bytes([0x00, 0x50]) + client_nonce + proof)
    status2 = resp2[2] if len(resp2) > 2 else 0xFF
    if status2 == 0x01:
        raise RuntimeError("authentication failed (wrong key)")
    if status2 != 0x00 or len(resp2) < 19:
        raise RuntimeError(f"auth response failed (status 0x{status2:02X})")
    server_response = resp2[3:19]
    session.session_key = derive_session_key(master_key, client_nonce, server_nonce, device_id)
    session.session_id = derive_session_id(session.session_key, client_nonce, server_nonce)
    expected = compute_server_response(session.session_key, server_nonce, client_nonce, device_id)
    if expected != server_response:
        raise RuntimeError("mutual authentication failed")
    session.authenticated = True
    session.counter = 0
    print("==> Authenticated")
    return session


async def _enter_dfu(dev, master_key: bytes | None) -> str:
    from bleak import BleakClient

    address = dev.address
    print("==> Connecting (application) …")
    async with BleakClient(dev, timeout=20.0) as client:
        if not client.is_connected:
            _die("failed to connect to application")
        session = None
        if master_key is not None:
            session = await _authenticate(client, master_key)
        if session is not None:
            wire = session.encrypt(0x00, 0x51, b"")
        else:
            wire = CMD_ENTER_DFU
        print("==> Sending CMD_ENTER_DFU (0x0051)")
        try:
            await client.write_gatt_char(CHAR_UUID, wire, response=False)
        except Exception as ex:
            # Device may reset immediately after scheduling DFU.
            print(f"==> Write ended ({ex}); assuming DFU reset")
        # Let the ATT write / DFU schedule settle before we drop the link.
        await asyncio.sleep(0.5)
    # App firmware only enters the bootloader after the BLE link fully closes.
    print("==> Waiting for app disconnect / AppLoader boot …")
    await asyncio.sleep(1.5)
    return address


async def _flash_address(gbl: bytes, address: str, *, fast: bool, attempts: int = 5) -> None:
    """Flash AppLoader at ``address``, refreshing the BlueZ device between tries.

    ``silabs-ble-ota`` sleeps several seconds before the first connect. On direct
    BlueZ that often ages the scanned ``BLEDevice`` out of D-Bus ("device
    disappeared"). Re-scan immediately before each attempt and shrink the
    library's pre-connect delay once AppLoader has already been seen.
    """
    from silabs_ble_ota import SilabsOTAError, perform_silabs_ota
    import silabs_ble_ota.ota as silabs_ota_mod

    # We only call perform_silabs_ota after AppLoader is advertising; the stock
    # 6s boot delay mostly creates a BlueZ disappearance race on Linux.
    if hasattr(silabs_ota_mod, "APPLOADER_BOOT_DELAY"):
        silabs_ota_mod.APPLOADER_BOOT_DELAY = 0.75

    def on_progress(pct: float) -> None:
        print(f"\r==> OTA {pct:5.1f}%", end="", flush=True)

    def on_log(msg: str) -> None:
        print(f"\n==> {msg}")

    print(f"==> Flashing {len(gbl)} bytes via silabs-ble-ota (fast={fast})")
    last_err: BaseException | None = None
    for attempt in range(1, attempts + 1):
        print(f"\n==> AppLoader connect attempt {attempt}/{attempts} …")
        ble_device = await _find_by_address(address, timeout=12.0)
        if ble_device is None:
            last_err = RuntimeError(f"AppLoader at {address} not advertising")
            print(f"==> {last_err}; rescanning …")
            await asyncio.sleep(0.5)
            continue
        print(f"==> Using {ble_device.name or '(no name)'} @ {ble_device.address}")
        try:
            await perform_silabs_ota(
                gbl,
                ble_device,
                on_progress=on_progress,
                on_log=on_log,
                fast=fast,
            )
            print("\n==> OTA complete")
            return
        except SilabsOTAError as ex:
            print()
            last_err = ex
            # Only retry pure connect failures. A mid-transfer failure means the
            # AppLoader may already have armed reboot-on-disconnect.
            if attempt < attempts and _is_transient_ota_connect_error(ex):
                print(f"==> Connect failed ({ex}); getting a fresh advertisement …")
                await asyncio.sleep(1.0)
                continue
            _die(f"OTA failed: {ex}")
    _die(f"OTA failed: {last_err}")


async def async_main(args: argparse.Namespace) -> None:
    _require_deps()
    gbl_path = Path(args.gbl)
    if not gbl_path.is_file():
        _die(f"GBL not found: {gbl_path}")
    gbl = gbl_path.read_bytes()
    if len(gbl) < 64:
        _die(f"GBL looks empty/too small: {gbl_path} ({len(gbl)} bytes)")

    master_key = None
    if args.key:
        raw = args.key.strip().replace(" ", "")
        try:
            master_key = bytes.fromhex(raw)
        except ValueError:
            _die("--key must be hex")
        if len(master_key) != 16:
            _die("--key must be 16 bytes (32 hex chars)")

    if args.already_in_ota:
        _name, addr, _ble_device = await _pick_device(args.name, args.scan_timeout)
    else:
        _name, addr, app_dev = await _pick_device(args.name, args.scan_timeout)
        await _enter_dfu(app_dev, master_key)
        await _wait_address(addr, args.apploader_timeout)

    await _flash_address(gbl, addr, fast=not args.slow)


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--name", "-n", required=True, help="GAP name substring (e.g. ODDEE7 or OD)")
    p.add_argument("--gbl", "-g", required=True, help="Path to OpenDisplay .gbl OTA image")
    p.add_argument("--key", "-k", help="Optional 16-byte security master key (hex) if encryption is enabled")
    p.add_argument("--scan-timeout", type=float, default=12.0, help="BLE scan timeout seconds")
    p.add_argument("--apploader-timeout", type=float, default=45.0, help="Wait for AppLoader re-advertise")
    p.add_argument(
        "--already-in-ota",
        action="store_true",
        help="Skip CMD_ENTER_DFU; device is already advertising AppLoader",
    )
    p.add_argument(
        "--slow",
        action="store_true",
        help="Disable silabs-ble-ota fast mode (use when flashing via ESPHome BLE proxy)",
    )
    args = p.parse_args()
    try:
        asyncio.run(async_main(args))
    except KeyboardInterrupt:
        print("\nInterrupted", file=sys.stderr)
        raise SystemExit(130) from None


if __name__ == "__main__":
    main()
