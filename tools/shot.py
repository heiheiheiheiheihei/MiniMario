#!/usr/bin/env python3
"""Capture a screenshot from the MiniMario NES emulator over the serial console.

The firmware console answers the `shot` command with one of these framed dumps:

    SHOT    <w> <h> <nbytes> <palette_bytes>\\n   (emulator frame, 256x224)
    SHOTUI  <w> <h> <nbytes> <palette_bytes>\\n   (menu / UI canvas)
    SHOTRAW <w> <h> <nbytes>\\n                    (`shotraw`: no palette)

followed by <nbytes> indexed pixels, <palette_bytes> bytes of little-endian
RGB565 palette entries and the 8-byte marker "SHOTEND\\n".

Usage:
    python3 tools/shot.py                       # booted board, save shot.png
    python3 tools/shot.py -o menu.png --scale 2
    python3 tools/shot.py --json                # machine readable metadata
    python3 tools/shot.py --wait-boot 20        # wait for "MMBOOT ready" first

Exit code 0 on success, non-zero on timeout / protocol error.
"""
import argparse
import hashlib
import json
import os
import struct
import sys
import time
import zlib

import os
import sys

# The script directory is tools/, which contains serial.py: when this file is
# executed directly Python would import *this* file as the pyserial module.
# Remove the script directory from sys.path before importing pyserial.
_SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path[:] = [p for p in sys.path if os.path.abspath(p or os.getcwd()) != _SCRIPT_DIR]

try:
    import serial
except ImportError:  # pragma: no cover
    sys.exit("error: pyserial is required (pip install pyserial)")

try:
    from PIL import Image
    HAVE_PIL = True
except ImportError:  # pragma: no cover - the pure-python PNG writer is equivalent
    HAVE_PIL = False

PORT_ENV = "MINIMARIO_PORT"
DEFAULT_PORT = "/dev/ttyACM0"
SHOTEND = b"SHOTEND"


# --------------------------------------------------------------------------- #
# serial link with a small line/binary reader                                 #
# --------------------------------------------------------------------------- #
class Link:
    def __init__(self, port: str, baud: int):
        self.ser = serial.Serial(port, baud, timeout=0.05, write_timeout=3)
        self.buf = bytearray()

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass

    def drain(self, seconds: float):
        end = time.time() + seconds
        while time.time() < end:
            self.ser.timeout = 0.02
            chunk = self.ser.read(8192)
            if not chunk:
                time.sleep(0.01)
            self.buf.clear()

    def _pump(self, deadline: float):
        remaining = deadline - time.time()
        if remaining <= 0:
            return False
        self.ser.timeout = max(0.01, min(0.1, remaining))
        chunk = self.ser.read(65536)
        if chunk:
            self.buf += chunk
            return True
        return False

    def read_line(self, timeout: float):
        deadline = time.time() + timeout
        while True:
            idx = self.buf.find(b"\n")
            if idx >= 0:
                line = bytes(self.buf[:idx])
                del self.buf[:idx + 1]
                return line.rstrip(b"\r")
            if not self._pump(deadline):
                if time.time() >= deadline:
                    return None

    def read_exact(self, count: int, timeout: float) -> bytes:
        deadline = time.time() + timeout
        out = bytearray()
        while len(out) < count:
            if self.buf:
                take = min(count - len(out), len(self.buf))
                out += self.buf[:take]
                del self.buf[:take]
                continue
            if not self._pump(deadline):
                if time.time() >= deadline:
                    raise TimeoutError(f"timeout: read {len(out)}/{count} payload bytes")
        return bytes(out)

    def wait_for(self, marker: bytes, timeout: float, echo=True) -> bool:
        deadline = time.time() + timeout
        while time.time() < deadline:
            line = self.read_line(max(0.1, deadline - time.time()))
            if line is None:
                continue
            if echo:
                print(f"[dev] {line.decode('utf-8', 'replace')}", file=sys.stderr)
            if marker in line:
                return True
        return False


# --------------------------------------------------------------------------- #
# protocol                                                                    #
# --------------------------------------------------------------------------- #
def parse_header(line: bytes):
    parts = line.split()
    if len(parts) < 4:
        return None
    kind = parts[0].decode("ascii", "replace")
    if kind not in ("SHOT", "SHOTUI", "SHOTRAW"):
        return None
    try:
        w, h, nbytes = int(parts[1]), int(parts[2]), int(parts[3])
        pal_len = int(parts[4]) if len(parts) > 4 else 0
    except ValueError:
        return None
    if w <= 0 or h <= 0 or nbytes != w * h or nbytes > 1 << 20:
        return None
    if pal_len not in (0, 32, 512):
        return None
    return {"kind": kind, "w": w, "h": h, "nbytes": nbytes, "pal_len": pal_len}


def grab(link: Link, timeout: float, attempts: int):
    last_error = "no SHOT header received"
    for attempt in range(attempts):
        if attempt:
            print(f"[shot] retry {attempt + 1}/{attempts} ({last_error})", file=sys.stderr)
        link.drain(0.15)
        link.ser.write(b"shot\n")
        link.ser.flush()
        deadline = time.time() + timeout
        hdr = None
        while time.time() < deadline:
            line = link.read_line(min(2.0, max(0.1, deadline - time.time())))
            if line is None:
                continue
            if line.startswith(b"ERR"):
                last_error = line.decode("utf-8", "replace")
                print(f"[dev] {last_error}", file=sys.stderr)
                break
            if line.startswith(b"SHOT"):
                hdr = parse_header(line)
                if hdr:
                    break
                last_error = f"bad header: {line!r}"
        if not hdr:
            continue
        try:
            payload = link.read_exact(hdr["nbytes"], timeout)
            palette = link.read_exact(hdr["pal_len"], timeout) if hdr["pal_len"] else b""
            tail = b""
            end = time.time() + timeout
            while SHOTEND not in tail and time.time() < end:
                line = link.read_line(max(0.1, end - time.time()))
                if line is None:
                    continue
                tail += line
            if SHOTEND not in tail:
                last_error = "missing SHOTEND marker"
                continue
            hdr["payload"] = payload
            hdr["palette"] = palette
            return hdr
        except TimeoutError as exc:
            last_error = str(exc)
    raise TimeoutError(last_error)


# --------------------------------------------------------------------------- #
# image output                                                                #
# --------------------------------------------------------------------------- #
def rgb565_to_rgb(value: int):
    r = (value >> 11) & 0x1F
    g = (value >> 5) & 0x3F
    b = value & 0x1F
    return (r * 255 + 15) // 31, (g * 255 + 31) // 63, (b * 255 + 15) // 31


def palette_rgb(palette: bytes):
    return [rgb565_to_rgb(v) for (v,) in struct.iter_unpack("<H", palette)]


def write_png_pure(path: str, w: int, h: int, indexed: bytes, palette, scale: int):
    """Fallback PNG writer: 8-bit indexed PNG with an optional palette."""
    if scale > 1:
        big = bytearray()
        for y in range(h):
            row = indexed[y * w:(y + 1) * w]
            line = bytearray()
            for x in range(w):
                line += bytes((row[x],)) * scale
            for _ in range(scale):
                big += line
        indexed = bytes(big)
        w, h = w * scale, h * scale
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw += indexed[y * w:(y + 1) * w]
    if palette is None:
        palette = [(i, i, i) for i in range(256)]
    pal = bytearray()
    for i in range(256):
        rgb = palette[i] if i < len(palette) else (0, 0, 0)
        pal += bytes(rgb)
    def chunk(tag: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + tag + data + struct.pack(
            ">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 3, 0, 0, 0)
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"PLTE", bytes(pal)) +
           chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)


def write_image(path: str, w: int, h: int, kind: str, indexed: bytes, palette: bytes,
                scale: int):
    palette_colors = palette_rgb(palette) if palette else None
    if HAVE_PIL:
        if palette_colors is not None:
            img = Image.frombytes("P", (w, h), indexed)
            flat = []
            for rgb in palette_colors:
                flat.extend(rgb)
            img.putpalette(flat + [0, 0, 0] * (256 - len(palette_colors)))
        else:
            img = Image.frombytes("L", (w, h), indexed)
        if scale > 1:
            img = img.resize((w * scale, h * scale), Image.NEAREST)
        img.save(path)
    else:
        write_png_pure(path, w, h, indexed, palette_colors, scale)


# --------------------------------------------------------------------------- #
# main                                                                        #
# --------------------------------------------------------------------------- #
def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-p", "--port", default=os.environ.get(PORT_ENV, DEFAULT_PORT))
    ap.add_argument("-b", "--baud", type=int, default=115200,
                    help="ignored by the USB CDC console, accepted for convenience")
    ap.add_argument("-o", "--out", default="shot.png", help="output PNG (default: shot.png)")
    ap.add_argument("--raw-out", help="also write the raw indexed bytes here")
    ap.add_argument("--scale", type=int, default=2, help="nearest-neighbour upscale (default 2)")
    ap.add_argument("--timeout", type=float, default=15.0, help="overall read timeout in seconds")
    ap.add_argument("--attempts", type=int, default=3, help="retries on a bad/short dump")
    ap.add_argument("--wait-boot", type=float, default=0.0,
                    help="first wait up to N s for a line containing 'MMBOOT ready'")
    ap.add_argument("--boot-marker", default="MMBOOT ready")
    ap.add_argument("--json", action="store_true", help="print machine readable JSON")
    args = ap.parse_args(argv)

    if args.scale < 1 or args.scale > 8:
        ap.error("--scale must be 1..8")

    if not os.path.exists(args.port):
        print(f"error: serial port {args.port} does not exist", file=sys.stderr)
        return 2
    try:
        link = Link(args.port, args.baud)
    except Exception as exc:
        print(f"error: cannot open {args.port}: {exc}", file=sys.stderr)
        return 2

    try:
        if args.wait_boot > 0:
            if not link.wait_for(args.boot_marker.encode(), args.wait_boot):
                print(f"error: '{args.boot_marker}' not seen within {args.wait_boot}s",
                      file=sys.stderr)
                return 3
        shot = grab(link, args.timeout, max(1, args.attempts))
    except (TimeoutError, OSError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 3
    finally:
        link.close()

    w, h = shot["w"], shot["h"]
    kind = shot["kind"]
    indexed = shot["payload"]
    palette = shot["palette"]
    write_image(args.out, w, h, kind, indexed, palette, args.scale)
    if args.raw_out:
        with open(args.raw_out, "wb") as f:
            f.write(indexed)
    digest = hashlib.sha256(indexed).hexdigest()

    if args.json:
        print(json.dumps({
            "mode": "emu" if kind == "SHOT" else ("menu" if kind == "SHOTUI" else "raw"),
            "kind": kind,
            "width": w,
            "height": h,
            "bytes": len(indexed),
            "palette_bytes": len(palette),
            "palette_entries": len(palette) // 2,
            "sha256": digest,
            "png": os.path.abspath(args.out),
            "png_scale": args.scale,
        }))
    else:
        mode = "NES frame" if kind == "SHOT" else ("menu/UI canvas" if kind == "SHOTUI" else "raw")
        print(f"OK {kind} {w}x{h} ({mode}), {len(indexed)} bytes indexed, "
              f"palette={len(palette) // 2} entries -> {args.out} (scale {args.scale})")
        print(f"   sha256={digest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
