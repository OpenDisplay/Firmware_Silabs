#!/usr/bin/env python3
"""Render the OEPL 16-colour chart on a GDEY029F51H via CMD_SPI_RAW.

Firmware_Silabs has no 16-colour mode, so the whole 3-pass JD79661 sequence is
driven from the host through the raw SPI debug command. Transcribed from the
UART lab firmware's color16.cpp (c16WriteInitJd / c16Pass / cellIndex /
kPaletteFd) so the register order and per-pass constants match a known-good
implementation rather than being guessed.

Per pass:
  RESET -> wait idle -> init registers -> LUT (0x20, 535 B) -> image (0x10)
        -> PON (0x04) -> wait idle -> DRF (0x12 00) -> wait idle

Three passes use waves {colour, edit, edit}; passes 1 and 2 patch wave header
byte 3 (vsn1) to 0x14 / 0x41. IMPORTANT: the image bytes DIFFER per pass --
each pixel's 2-bit code is palette[cell][pass], not a constant image re-sent
three times.

Image is 168x384 at 2bpp = 16128 bytes per pass, chunked over BLE.
"""
import argparse
import asyncio
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(REPO / "py-opendisplay" / "src"))
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(REPO / "en05_ep397yr_uart" / "host"))

from test_spi_raw import _shim_epaper_dithering, load_landing  # noqa: E402

CMD = bytes([0x00, 0x84])
SUB_OPEN, SUB_CMD, SUB_DATA, SUB_BUSY, SUB_RESET, SUB_CLOSE = range(6)
SUB_DATA_NOACK = 0x06

PANEL_W, PANEL_H = 168, 384
PSR_B1, PSR_B2 = 0x0F, 0x29
VSN1 = {1: 0x14, 2: 0x41}          # per-pass wave header byte 3
WAVE_SIZE = 535

# kPaletteFd from color16.cpp: [colour][pass] -> 2-bit code
PALETTE = [
    (2, 0, 0), (1, 1, 1), (2, 1, 1), (3, 1, 1),      # black white yellow red
    (0, 1, 1), (1, 0, 3), (1, 2, 1), (2, 0, 2),      # orange stone slate sand
    (2, 1, 3), (2, 3, 1), (3, 1, 2), (0, 2, 1),      # olive mocha caramel terracotta
    (0, 3, 1), (3, 2, 1), (0, 1, 0), (3, 0, 1),      # brown rust darkbrown burgundy
]
NAMES = ["black", "white", "yellow", "red", "orange", "stone_grey",
         "slate_grey", "sand", "olive", "mocha", "caramel", "terracotta",
         "brown", "rust", "dark_brown", "burgundy"]


def cell_index(x, y):
    """Full-bleed 4x4, no gutters (color16.cpp cellIndex)."""
    cw, ch = PANEL_W // 4, PANEL_H // 4
    return min(y // ch, 3) * 4 + min(x // cw, 3)


def build_image(pass_no):
    """2bpp image for one pass: each pixel's code is palette[cell][pass]."""
    out = bytearray()
    acc = 0
    n = 0
    for y in range(PANEL_H):
        for x in range(PANEL_W):
            acc = ((acc << 2) | (PALETTE[cell_index(x, y)][pass_no] & 3)) & 0xFF
            n += 1
            if n == 4:
                out.append(acc)
                acc = 0
                n = 0
    assert len(out) == PANEL_W * PANEL_H // 4, len(out)
    return bytes(out)


def init_regs(w):
    """c16WriteInitJd: (cmd, data) pairs. LUT_EN is PSR byte2 bit7."""
    b2 = PSR_B2 | 0x80
    return [
        (0x4D, bytes([0x78])),
        (0x00, bytes([PSR_B1, b2])),
        (0x01, bytes([0x07, w[0], w[1], w[2], w[3], w[4]])),
        (0x03, bytes([0x10, 0x54, 0x44])),
        (0x06, bytes([0x05, 0x00, 0x3F, 0x0A, 0x25, 0x12, 0x1A])),
        (0x30, bytes([w[6]])),
        (0x50, bytes([0x37])),
        (0x60, bytes([0x02, 0x02])),
        (0x61, bytes([(PANEL_W >> 8) & 0xFF, PANEL_W & 0xFF,
                      (PANEL_H >> 8) & 0xFF, PANEL_H & 0xFF])),
        (0xE7, bytes([0x1C])),
        (0xE3, bytes([0x22])),
        (0xB4, bytes([0xD0])),
        (0xB5, bytes([0x03])),
        (0xE9, bytes([0x01])),
        (0x82, bytes([w[5] ^ 0x80])),
    ]


class Raw:
    """CMD_SPI_RAW helper. Raises on NACK so a bad step stops the sequence
    instead of silently corrupting the rest of the render."""

    def __init__(self, dev, chunk, ack_every=8):
        self.dev = dev
        self.chunk = chunk
        self.ack_every = ack_every
        self.frames = 0

    async def _call(self, sub, args=b"", timeout=30.0):
        await self.dev._write(CMD + bytes([sub]) + args)
        self.frames += 1
        rsp = await self.dev._read(timeout=timeout)
        if not rsp or rsp[0] != 0x00:
            raise RuntimeError(
                f"CMD_SPI_RAW sub=0x{sub:02X} failed: {rsp.hex(' ') if rsp else '<none>'}")
        return rsp

    async def open(self, panel_init=False):
        await self._call(SUB_OPEN, bytes([1 if panel_init else 0]), timeout=40)

    async def close(self):
        await self._call(SUB_CLOSE, timeout=40)

    async def reset(self, hold_ms=10):
        await self._call(SUB_RESET, bytes([hold_ms]), timeout=30)

    async def wait_idle(self, timeout_ms):
        t = min(timeout_ms, 0xFFFF)
        rsp = await self._call(SUB_BUSY, bytes([t & 0xFF, (t >> 8) & 0xFF]),
                               timeout=timeout_ms / 1000.0 + 20.0)
        elapsed = int.from_bytes(rsp[3:5], "little")
        still = bool(rsp[5])
        if still:
            raise RuntimeError(f"BUSY still asserted after {elapsed} ms")
        return elapsed

    async def _stream(self, payload):
        """Stream with WINDOWED flow control: N unacked frames, then one ACKed.

        Pure write-without-response corrupts the image. BLE gives it no
        flow control, so once the controller's buffer fills, frames are
        dropped SILENTLY -- the render came out streaky while every DRF time
        stayed identical. That is the trap: refresh duration depends only on
        the LUT, never on the pixel content, so TIMING CANNOT DETECT A
        CORRUPTED UPLOAD. Only the camera caught it.

        A periodic ACKed frame both paces the sender (the reply cannot arrive
        until the device has drained the preceding writes) and gives a
        checkpoint. This is the same shape as CMD_PIPE_WRITE's window/ACK
        scheme, which exists for exactly this reason.
        """
        pending = 0
        for i in range(0, len(payload), self.chunk):
            part = payload[i:i + self.chunk]
            if pending >= self.ack_every:
                await self._call(SUB_DATA, part)   # ACKed: drains the window
                pending = 0
            else:
                await self.dev._write(CMD + bytes([SUB_DATA_NOACK]) + part,
                                      response=False)
                self.frames += 1
                pending += 1

    async def data(self, payload, stream=False):
        """Data bytes with NO command byte.

        Needed because a config sequence sends the register as an ordinary step
        ("10" for the image, "20" for the waveform) and then hits a marker that
        streams the body. Folding the register into this call would re-send it
        and put the panel one byte out of step.
        """
        if not payload:
            return
        if stream:
            await self._stream(payload)
            await self._call(SUB_BUSY, bytes([0x64, 0x00]), timeout=20)
            return
        for i in range(0, len(payload), self.chunk):
            await self._call(SUB_DATA, payload[i:i + self.chunk])

    async def cmd(self, c, data=b"", stream=False):
        """Command byte plus optional inline data, chunking the tail."""
        head = data[: self.chunk]
        await self._call(SUB_CMD, bytes([c]) + head)
        rest = data[len(head):]
        if not rest:
            return
        if stream:
            await self._stream(rest)
            # Resync: a BUSY poll is ACKed, so reaching here proves the device
            # is still answering after the unacknowledged burst.
            await self._call(SUB_BUSY, bytes([0x64, 0x00]), timeout=20)
        else:
            for i in range(0, len(rest), self.chunk):
                await self._call(SUB_DATA, rest[i:i + self.chunk])


async def run(args):
    _shim_epaper_dithering()
    from opendisplay import OpenDisplayDevice                       # noqa: PLC0415
    from opendisplay.models.capabilities import DeviceCapabilities  # noqa: PLC0415
    from epaper_dithering import ColorScheme                        # noqa: PLC0415
    from lut_lab.waves import default_waves                         # noqa: PLC0415

    colour, edit = default_waves()
    waves = [colour, edit, edit]

    info = load_landing().parse_landing_url(args.link)
    caps = DeviceCapabilities(width=PANEL_W, height=PANEL_H,
                              color_scheme=ColorScheme.BWRY, rotation=0)
    dev = OpenDisplayDevice(device_name=info.device_name,
                            encryption_key=info.encryption_key,
                            capabilities=caps, timeout=20.0)

    images = [build_image(p) for p in range(3)]
    print(f"image {len(images[0])} B/pass, chunk {args.chunk} B "
          f"-> ~{-(-len(images[0]) // args.chunk)} frames/pass")

    async with dev:
        raw = Raw(dev, args.chunk, args.ack_every)
        t_all = time.time()
        await raw.open(panel_init=False)      # pins+power only; we own the sequence
        try:
            for p in range(3):
                w = bytearray(waves[p])
                if p in VSN1:
                    w[3] = VSN1[p]
                t0 = time.time()
                print(f"\npass {p}: wave={'colour' if p == 0 else 'edit'} "
                      f"vsn1=0x{w[3]:02X}")
                await raw.reset(10)
                await raw.wait_idle(3000)
                for c, d in init_regs(w):
                    await raw.cmd(c, d)
                await raw.cmd(0x20, bytes(w))              # 535-byte LUT (ACKed)
                await raw.cmd(0x10, images[p],             # 16128 B image
                              stream=not args.no_stream)
                await raw.cmd(0x04)                        # PON
                await raw.wait_idle(10000)
                await raw.cmd(0x12, bytes([0x00]))         # DRF
                el = await raw.wait_idle(args.drf_timeout)
                print(f"  pass {p} done: drf {el} ms, {time.time()-t0:.1f}s wall")
            await raw.cmd(0x07, bytes([0xA5]))             # deep sleep
        finally:
            await raw.close()
        print(f"\nTOTAL {time.time()-t_all:.1f}s, {raw.frames} BLE frames")
        print("colours:", ", ".join(NAMES))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("link")
    ap.add_argument("--chunk", type=int, default=192,
                    help="data bytes per BLE frame (244-byte pipe buffer minus "
                         "opcode+sub and encryption overhead)")
    ap.add_argument("--drf-timeout", type=int, default=60000)
    ap.add_argument("--ack-every", type=int, default=2,
                    help="unacked frames between ACKed ones (default 2). "
                         "MEASURED on GDEY029F51H: 2 renders correctly in "
                         "40.1s; 8 and pure no-ACK both CORRUPT the image "
                         "while producing identical DRF times, so only a "
                         "camera catches it. Raise with care.")
    ap.add_argument("--no-stream", action="store_true",
                    help="use ACKed DATA for the image (slow path, for A/B "
                         "comparison against DATA_NOACK streaming)")
    asyncio.run(run(ap.parse_args()))


if __name__ == "__main__":
    main()
