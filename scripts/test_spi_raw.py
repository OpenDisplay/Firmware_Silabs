#!/usr/bin/env python3
"""End-to-end test of CMD_SPI_RAW 0x0084 against a live device.

Deliberately SAFE by default: the happy path only OPENs (pins + power, which
clocks nothing to the panel), polls BUSY, and CLOSEs. No arbitrary bytes are
pushed into the panel unless --poke is given, because a bad raw sequence can
leave a display mid-drive or DC-imbalanced.

It also exercises the failure paths, which is where a protocol handler is most
likely to be wrong:
  * CMD before OPEN          -> OD_ERR_SPI_RAW_NOT_OPEN (0x02)
  * unknown sub-opcode       -> OD_ERR_SPI_RAW_UNKNOWN_SUB (0x05)
  * BUSY with no args        -> OD_ERR_SPI_RAW_BAD_ARGS (0x01)

Encryption is not special-cased here: _write/_read in the client encrypt
transparently once authenticate() has run, which is exactly how the firmware
expects it (the handler sits behind the normal dispatch gate).
"""
import argparse
import asyncio
import importlib.util
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "py-opendisplay" / "src"))

CMD = bytes([0x00, 0x84])
SUB = {"OPEN": 0x00, "CMD": 0x01, "DATA": 0x02,
       "BUSY": 0x03, "RESET": 0x04, "CLOSE": 0x05}
ERRS = {0x01: "BAD_ARGS", 0x02: "NOT_OPEN", 0x03: "NO_CONFIG",
        0x04: "PANEL", 0x05: "UNKNOWN_SUB"}


def load_landing():
    spec = importlib.util.spec_from_file_location(
        "landing", REPO / "py-opendisplay" / "src" / "opendisplay" / "landing.py")
    m = importlib.util.module_from_spec(spec)
    sys.modules["landing"] = m          # dataclass(slots=True) needs this
    spec.loader.exec_module(m)
    return m


def describe(rsp: bytes) -> str:
    if not rsp:
        return "<empty>"
    if rsp[0] == 0x00:
        body = rsp[3:]
        return (f"ACK sub=0x{rsp[2]:02X}"
                + (f" data={body.hex(' ')}" if body else ""))
    if rsp[0] == 0xFF:
        err = rsp[2] if len(rsp) > 2 else 0xFF
        return (f"NACK err=0x{err:02X}({ERRS.get(err,'?')}) "
                f"sub=0x{rsp[3]:02X}" if len(rsp) > 3 else f"NACK err=0x{err:02X}")
    if rsp[0] == 0xFE:
        return "AUTH_REQUIRED (gate working, but we should be authenticated!)"
    return f"? {rsp.hex(' ')}"


async def call(dev, sub_name, args=b"", timeout=20.0, label=None):
    frame = CMD + bytes([SUB[sub_name]]) + args
    await dev._write(frame)
    try:
        rsp = await dev._read(timeout=timeout)
    except Exception as exc:                       # noqa: BLE001
        print(f"  {label or sub_name:<22} -> EXC {type(exc).__name__}: {exc}")
        return None
    print(f"  {label or sub_name:<22} -> {describe(rsp)}")
    return rsp


def _shim_epaper_dithering():
    """Work around a version skew, locally and only for this test.

    py-opendisplay imports SPECTRA_7_3_6COLOR_V2, which the installed
    epaper_dithering predates (it has SPECTRA_7_3_6COLOR). Aliasing the name
    before importing opendisplay avoids touching the environment just to run a
    firmware test. This is a test-harness workaround, NOT something to ship --
    if the palettes actually differ, anything depending on the V2 values would
    be wrong, so nothing here relies on palette content.
    """
    try:
        import epaper_dithering as ed             # noqa: PLC0415
    except ImportError:
        return
    if not hasattr(ed, "SPECTRA_7_3_6COLOR_V2") and hasattr(ed, "SPECTRA_7_3_6COLOR"):
        ed.SPECTRA_7_3_6COLOR_V2 = ed.SPECTRA_7_3_6COLOR
        print("[shim] aliased SPECTRA_7_3_6COLOR_V2 -> SPECTRA_7_3_6COLOR")
    cs = getattr(ed, "ColorScheme", None)
    if cs is None:
        return
    # The installed epaper_dithering (0.6.4) predates several ColorScheme
    # members that py-opendisplay references at import time (BWGBRY_SPLIT,
    # SEVEN_COLOR, ...). Naming them one by one is whack-a-mole, so synthesise
    # ANY missing member on first access.
    #
    # Safe for THIS test only: the members are used purely as dict keys in
    # palette lookup tables and nothing here renders an image. Do not reuse
    # this shim for anything that depends on palette VALUES -- it would
    # silently produce wrong colours rather than failing.
    meta = type(cs)
    if getattr(meta, "_od_test_shim", False):
        return
    # Python 3.12+ removed EnumType.__getattr__, so there may be nothing to
    # chain to; install one in that case.
    orig_getattr = getattr(meta, "__getattr__", None)

    def _getattr(klass, name):
        if orig_getattr is not None:
            try:
                return orig_getattr(klass, name)
            except AttributeError:
                pass
        if True:
            if name.startswith("_") or klass is not cs:
                raise AttributeError(name)
            member = object.__new__(klass)
            member._name_ = name
            member._value_ = 1000 + len(klass._member_map_)
            klass._member_map_[name] = member
            klass._value2member_map_[member._value_] = member
            print(f"[shim] synthesised ColorScheme.{name}")
            return member

    meta.__getattr__ = _getattr
    meta._od_test_shim = True


async def main_async(args):
    _shim_epaper_dithering()
    from opendisplay import OpenDisplayDevice     # noqa: PLC0415

    land = load_landing()
    info = land.parse_landing_url(args.link)
    name = info.device_name
    key = info.encryption_key
    print(f"device {name}  key_present={key is not None}")

    # The key must go to the CONSTRUCTOR: __aenter__ calls interrogate(), which
    # happens before any manual authenticate() and fails with
    # AuthenticationRequiredError on an encrypted device.
    #
    # `capabilities` is supplied purely to SKIP interrogate(): __aenter__ only
    # auto-interrogates when both config and capabilities are None. The config
    # read is unrelated to CMD_SPI_RAW and currently truncates on this device
    # (679/775 bytes), which would otherwise block the test. The values below
    # are placeholders -- nothing in this test reads them; _write/_read do not
    # consult capabilities.
    from opendisplay.models.capabilities import DeviceCapabilities  # noqa: PLC0415
    from epaper_dithering import ColorScheme                        # noqa: PLC0415
    caps = DeviceCapabilities(width=1, height=1,
                              color_scheme=ColorScheme.MONO, rotation=0)
    dev = OpenDisplayDevice(device_name=name, encryption_key=key,
                            capabilities=caps, timeout=args.timeout)
    async with dev:
        print("connected + authenticated\n")

        print("-- failure paths (before OPEN) --")
        await call(dev, "CMD", bytes([0x00]), label="CMD before OPEN")
        await call(dev, "BUSY", b"", label="BUSY no args")
        bad = CMD + bytes([0x7F])
        await dev._write(bad)
        try:
            print(f"  {'unknown sub 0x7F':<22} -> {describe(await dev._read(timeout=10))}")
        except Exception as exc:                   # noqa: BLE001
            print(f"  unknown sub -> EXC {exc}")

        print("\n-- happy path --")
        # flags=0: power + pins only, nothing clocked to the panel
        await call(dev, "OPEN", bytes([0x00]), timeout=30, label="OPEN (pins only)")
        await call(dev, "BUSY", bytes([0xE8, 0x03]), timeout=30,
                   label="BUSY (1000ms)")

        if args.poke:
            # Only with --poke: a real (still benign) command byte.
            print("\n-- optional poke --")
            await call(dev, "CMD", bytes([args.poke_cmd]), timeout=20,
                       label=f"CMD 0x{args.poke_cmd:02X}")
            await call(dev, "BUSY", bytes([0xE8, 0x03]), timeout=30,
                       label="BUSY after cmd")

        await call(dev, "CLOSE", b"", timeout=30, label="CLOSE")
        print("\n-- after CLOSE, session must be gone --")
        await call(dev, "CMD", bytes([0x00]), label="CMD after CLOSE")
    print("\ndone")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("link", help="opendisplay.org/l/?... device link")
    ap.add_argument("--timeout", type=float, default=15.0)
    ap.add_argument("--poke", action="store_true",
                    help="also send one real command byte to the panel")
    ap.add_argument("--poke-cmd", type=lambda s: int(s, 0), default=0x71,
                    help="command byte for --poke (default 0x71, a benign read-ish op)")
    asyncio.run(main_async(ap.parse_args()))


if __name__ == "__main__":
    main()
