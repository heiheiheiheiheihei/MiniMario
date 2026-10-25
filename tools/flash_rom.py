#!/usr/bin/env python3
"""Flash NES ROMs into the MiniMario "roms" data partition (0x1F0000).

Two modes:
  * default / --pack:  build an MMROM1 container from the given .nes files and
    flash it (without arguments it packs the two bundled homebrew demos from
    /tmp/roms: twin_dragons.nes + nova.nes),
  * --file rom.nes:    flash a single raw iNES image at the partition start
                       (legacy layout, the firmware detects "NES\\x1a").

The partition offset/size are read from partitions.csv in the project root.

Examples:
    python3 tools/flash_rom.py                       # pack + flash the demos
    python3 tools/flash_rom.py --pack a.nes b.nes
    python3 tools/flash_rom.py --file mygame.nes
    python3 tools/flash_rom.py --pack a.nes --dry-run
"""
import argparse
import os
import subprocess
import sys
from pathlib import Path

try:
    from pack_roms import build_container, check_rom, describe
except ImportError:  # running from another directory
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from pack_roms import build_container, check_rom, describe

PORT_ENV = "MINIMARIO_PORT"
DEFAULT_PORT = "/dev/ttyACM0"
DEFAULT_ROMS = ["/tmp/roms/twin_dragons.nes", "/tmp/roms/nova.nes"]
FALLBACK_OFFSET = 0x1F0000
FALLBACK_SIZE = 0x1F0000


def partition_info():
    """Read the 'roms' row from partitions.csv (offset, size)."""
    csv = Path(__file__).resolve().parent.parent / "partitions.csv"
    try:
        for line in csv.read_text().splitlines():
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            cols = [c.strip() for c in line.split(",")]
            if len(cols) >= 5 and cols[0] == "roms":
                return int(cols[3], 0), int(cols[4], 0)
    except (OSError, ValueError):
        pass
    return FALLBACK_OFFSET, FALLBACK_SIZE


def esptool_write(port: str, offset: int, image: str, no_reset: bool, baud: int | None):
    # esptool global options must precede the subcommand
    cmd = [sys.executable, "-m", "esptool", "--chip", "esp32c6", "-p", port,
           "--after", "no_reset" if no_reset else "hard_reset"]
    if baud:
        cmd += ["-b", str(baud)]
    cmd += ["write_flash", hex(offset), image]
    print("+ " + " ".join(cmd))
    return subprocess.call(cmd)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--file", help="flash one raw .nes image (legacy layout)")
    ap.add_argument("--pack", nargs="+", metavar="ROM",
                    help="pack these .nes files into an MMROM1 container and flash it")
    ap.add_argument("--port", default=os.environ.get(PORT_ENV, DEFAULT_PORT))
    ap.add_argument("--offset", type=lambda v: int(v, 0), default=None,
                    help="partition offset (default: from partitions.csv)")
    ap.add_argument("--no-reset", action="store_true", help="do not reset after flashing")
    ap.add_argument("--baud", type=int, default=None, help="esptool baud rate (USB-JTAG ignores it)")
    ap.add_argument("--out", default=None, help="write the packed container to this file")
    ap.add_argument("--dry-run", action="store_true", help="build/check only, do not flash")
    args = ap.parse_args(argv)

    part_offset, part_size = partition_info()
    offset = args.offset if args.offset is not None else part_offset
    print(f"roms partition: offset=0x{offset:06x} size=0x{part_size:06x} ({part_size} bytes)")

    if args.file and args.pack:
        ap.error("use either --file or --pack, not both")

    if args.file:
        data = check_rom(args.file)
        if len(data) > part_size:
            raise SystemExit(f"error: {args.file} ({len(data)} bytes) does not fit the "
                             f"roms partition ({part_size} bytes)")
        image = args.file
        legacy = True
    else:
        roms = args.pack if args.pack else DEFAULT_ROMS
        missing = [r for r in roms if not os.path.isfile(r)]
        if missing:
            raise SystemExit("error: missing ROM(s): " + ", ".join(missing) +
                             "\nProvide .nes files (e.g. --pack a.nes b.nes) or put the "
                             "bundled demos into /tmp/roms/.")
        container = build_container(roms)
        if len(container) > part_size:
            raise SystemExit(f"error: container ({len(container)} bytes) does not fit the "
                             f"roms partition ({part_size} bytes)")
        describe(container)
        image = args.out or "/tmp/minimario_roms.bin"
        with open(image, "wb") as f:
            f.write(container)
        print(f"packed {len(roms)} ROM(s) -> {image} ({len(container)} bytes)")
        legacy = False

    if args.dry_run:
        print("dry-run: not flashing")
        return 0

    rc = esptool_write(args.port, offset, image, args.no_reset, args.baud)
    if rc != 0:
        print(f"error: esptool failed (exit {rc})", file=sys.stderr)
        return rc
    kind = "legacy RAW" if legacy else "MMROM1 container"
    print(f"OK: flashed {kind} to 0x{offset:06x} on {args.port}")
    print("The board resets after flashing; wait for 'MMBOOT ready' then use "
          "'roms' / 'play flash' on the serial console.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
