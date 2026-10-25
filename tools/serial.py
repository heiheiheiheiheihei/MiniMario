#!/usr/bin/env python3
"""MiniMario serial helper: tail logs, send console commands, wait for markers.

Examples:
    python3 tools/serial.py                       # tail the console until Ctrl-C
    python3 tools/serial.py --wait-boot 20        # wait for the boot log line
    python3 tools/serial.py --send help --until "reboot" --timeout 3
    python3 tools/serial.py -s info -s roms --timeout 2
    python3 tools/serial.py --send "play flash" --until "OK running" --timeout 20

The firmware prints a `MMBOOT ready fw=... roms=... sd=... heap=...` line once
the menu is up; use --wait-boot to synchronise automated runs.
"""
import argparse
import os
import subprocess
import sys
import time

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

PORT_ENV = "MINIMARIO_PORT"
DEFAULT_PORT = "/dev/ttyACM0"


def parse_args(argv):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-p", "--port", default=os.environ.get(PORT_ENV, DEFAULT_PORT))
    ap.add_argument("-b", "--baud", type=int, default=115200,
                    help="ignored by the USB CDC console, accepted for convenience")
    ap.add_argument("-s", "--send", action="append", default=[], metavar="CMD",
                    help="send a console command (repeatable)")
    ap.add_argument("--send-delay", type=float, default=0.3,
                    help="delay between --send commands in seconds (default 0.3)")
    ap.add_argument("--until", action="append", default=[], metavar="MARKER",
                    help="stop reading once a line contains MARKER (repeatable)")
    ap.add_argument("--wait-boot", type=float, default=0.0,
                    help="wait up to N s for 'MMBOOT ready' before sending")
    ap.add_argument("--boot-marker", default="MMBOOT ready")
    ap.add_argument("--timeout", type=float, default=0.0,
                    help="stop after N s (0 = run until Ctrl-C when nothing to wait for)")
    ap.add_argument("--quiet", action="store_true", help="do not echo device output")
    ap.add_argument("--timestamps", action="store_true", help="prefix lines with elapsed time")
    ap.add_argument("--capture", metavar="FILE", help="also write the raw stream to FILE")
    ap.add_argument("--reset", action="store_true",
                    help="hard-reset the chip with esptool before opening the port")
    ap.add_argument("--json", action="store_true", help="print a JSON summary at the end")
    return ap.parse_args(argv)


def hard_reset(port):
    cmd = [sys.executable, "-m", "esptool", "--chip", "esp32c6", "-p", port,
           "--after", "hard_reset", "flash_id"]
    print("+ " + " ".join(cmd), file=sys.stderr)
    try:
        return subprocess.call(cmd, stdout=subprocess.DEVNULL)
    except OSError as exc:
        print(f"warning: reset failed: {exc}", file=sys.stderr)
        return 1


def main(argv=None):
    args = parse_args(argv)

    if args.reset:
        hard_reset(args.port)
        time.sleep(0.3)

    if not os.path.exists(args.port):
        print(f"error: serial port {args.port} does not exist", file=sys.stderr)
        return 2
    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.05, write_timeout=3)
    except Exception as exc:
        print(f"error: cannot open {args.port}: {exc}", file=sys.stderr)
        return 2

    capture = open(args.capture, "wb") if args.capture else None
    started = time.time()
    markers = [m.encode() for m in args.until]
    seen = {m: False for m in args.until}
    sent = 0
    line_buf = bytearray()
    boot_seen = False
    last_activity = time.time()
    stop = False
    exit_code = 0

    def emit(raw: bytes):
        nonlocal last_activity
        last_activity = time.time()
        if capture:
            capture.write(raw + b"\n")
        if args.quiet:
            return
        text = raw.decode("utf-8", "replace")
        if args.timestamps:
            text = f"[{time.time() - started:8.3f}] {text}"
        print(text)

    try:
        while not stop:
            now = time.time()
            chunk = ser.read(4096)
            if chunk:
                line_buf += chunk
                while True:
                    idx = line_buf.find(b"\n")
                    if idx < 0:
                        break
                    line = bytes(line_buf[:idx]).rstrip(b"\r")
                    del line_buf[:idx + 1]
                    emit(line)
                    if args.boot_marker.encode() in line:
                        boot_seen = True
                    for m in markers:
                        if m in line:
                            seen[m] = True
                            stop = True

            # sending schedule
            if not args.send or stop:
                pass
            elif now - started >= args.send_delay * (sent + 1) and sent < len(args.send):
                cmd = args.send[sent]
                ser.write(cmd.encode() + b"\n")
                ser.flush()
                sent += 1
                print(f"[host] {cmd}", file=sys.stderr)

            if args.wait_boot and not boot_seen and now - started > args.wait_boot:
                print(f"error: '{args.boot_marker}' not seen in {args.wait_boot}s",
                      file=sys.stderr)
                exit_code = 3
                break

            if markers and all(seen.values()):
                stop = True
            if args.timeout and now - started > args.timeout:
                if markers and not all(seen.values()):
                    print("error: timeout waiting for marker(s): " +
                          ", ".join(m for m, ok in seen.items() if not ok), file=sys.stderr)
                    exit_code = 3
                stop = True
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()
        if capture:
            capture.close()

    if args.wait_boot and not boot_seen and exit_code == 0:
        exit_code = 3
    summary = {
        "port": args.port,
        "seconds": round(time.time() - started, 3),
        "sent": sent,
        "boot_seen": boot_seen,
        "markers": {m: ok for m, ok in seen.items()},
    }
    if args.json:
        import json
        print(json.dumps(summary))
    else:
        print(f"[summary] {summary}", file=sys.stderr)
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
