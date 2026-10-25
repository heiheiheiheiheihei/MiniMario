#!/usr/bin/env python3
"""Pack one or more iNES ROMs into the MiniMario "roms" partition container.

Container layout (little endian, offsets relative to partition start):
    char magic[8]   "MMROM1\\0\\0"
    u32  count
    count * { u32 offset, u32 size, char name[32] }   (name NUL terminated)
    ROM blobs (each blob start aligned to 16 bytes)

The firmware (main/rom_store.c) also accepts a partition that starts directly
with "NES\\x1a" as one legacy raw ROM; this tool always writes the container.

Usage:
    python3 tools/pack_roms.py game1.nes game2.nes -o roms.bin
    python3 tools/pack_roms.py --list roms.bin
"""
import argparse
import os
import struct
import sys

MAGIC = b"MMROM1\x00\x00"
NAME_LEN = 32
ALIGN = 16
HEADER_LEN = 8 + 4
ENTRY_LEN = 4 + 4 + NAME_LEN
INES_MAGIC = b"NES\x1a"


def ines_expected_size(data: bytes) -> int:
    """Size implied by an iNES header, or -1 if the header is invalid."""
    if len(data) < 16 or data[:4] != INES_MAGIC:
        return -1
    prg = data[4]
    chr_ = data[5]
    trainer = 512 if (data[6] & 0x04) else 0
    return 16 + trainer + prg * 16384 + chr_ * 8192


def check_rom(path: str) -> bytes:
    with open(path, "rb") as f:
        data = f.read()
    size = ines_expected_size(data)
    if size < 0:
        raise SystemExit(f"error: {path}: not an iNES ROM (missing NES\\x1a header)")
    if len(data) < size:
        raise SystemExit(f"error: {path}: truncated iNES image ({len(data)} < {size} bytes)")
    if len(data) != size:
        print(f"warning: {path}: {len(data)} bytes on disk, iNES header implies {size}; "
              f"truncating to the header size", file=sys.stderr)
        data = data[:size]
    return data


def build_container(roms, force_names=None) -> bytes:
    """roms: list of file paths (or (name, data) tuples). Returns container bytes."""
    items = []
    for i, rom in enumerate(roms):
        if isinstance(rom, tuple):
            name, data = rom
        else:
            data = check_rom(rom)
            name = os.path.basename(rom)
        if force_names and i < len(force_names):
            name = force_names[i]
        if len(name) > NAME_LEN - 1:
            raise SystemExit(f"error: ROM name too long (max {NAME_LEN - 1}): {name}")
        items.append((name, data))

    count = len(items)
    table_end = HEADER_LEN + count * ENTRY_LEN
    offset = (table_end + ALIGN - 1) // ALIGN * ALIGN

    entries = []
    blobs = []
    for name, data in items:
        entries.append((offset, len(data), name))
        blobs.append(data)
        offset = (offset + len(data) + ALIGN - 1) // ALIGN * ALIGN

    out = bytearray(offset)
    out[0:8] = MAGIC
    struct.pack_into("<I", out, 8, count)
    pos = HEADER_LEN
    for off, size, name in entries:
        struct.pack_into("<II", out, pos, off, size)
        raw = name.encode("ascii", "replace")[:NAME_LEN - 1]
        out[pos + 8:pos + 8 + len(raw)] = raw
        out[pos + 8 + len(raw)] = 0
        pos += ENTRY_LEN
    for (off, size, _name), data in zip(entries, blobs):
        out[off:off + size] = data
    return bytes(out)


def describe(container: bytes, verbose: bool = True):
    if len(container) < HEADER_LEN or container[:8] != MAGIC:
        if container[:4] == INES_MAGIC:
            size = ines_expected_size(container)
            print("legacy raw iNES image at partition start "
                  f"({len(container)} bytes on flash, header implies {size})")
            return
        raise SystemExit("error: not an MMROM1 container")
    count = struct.unpack_from("<I", container, 8)[0]
    print(f"MMROM1 container: {count} ROM(s), {len(container)} bytes")
    for i in range(count):
        pos = HEADER_LEN + i * ENTRY_LEN
        off, size = struct.unpack_from("<II", container, pos)
        name = container[pos + 8:pos + 8 + NAME_LEN].split(b"\x00", 1)[0].decode("ascii", "replace")
        print(f"  [{i}] {name:<31} offset=0x{off:06x} size={size}")
        if verbose and off + size <= len(container):
            rom = container[off:off + size]
            mapper = ((rom[6] >> 4) | (rom[7] & 0xF0)) if len(rom) >= 16 else -1
            print(f"       iNES prg={rom[4] if len(rom) > 4 else 0} "
                  f"chr={rom[5] if len(rom) > 5 else 0} mapper={mapper}")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("roms", nargs="*", help="input .nes files")
    ap.add_argument("-o", "--out", help="output container (.bin)")
    ap.add_argument("--list", metavar="CONTAINER", help="describe an existing container")
    ap.add_argument("-n", "--name", action="append", default=[],
                    help="override the display name of the nth ROM (repeatable)")
    args = ap.parse_args(argv)

    if args.list:
        with open(args.list, "rb") as f:
            describe(f.read())
        return 0

    if not args.roms:
        ap.error("no input .nes files (or use --list)")
    if not args.out:
        ap.error("-o/--out is required")

    for rom in args.roms:
        check_rom(rom)
    container = build_container(args.roms, args.name or None)
    with open(args.out, "wb") as f:
        f.write(container)
    describe(container)
    print(f"wrote {args.out} ({len(container)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
