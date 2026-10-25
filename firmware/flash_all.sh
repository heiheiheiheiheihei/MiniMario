#!/bin/bash
# Flash MiniMario (firmware + built-in free ROMs) to an ESP32-C6 board.
# Usage: ./flash_all.sh [/dev/ttyACM0]
set -e
PORT="${1:-/dev/ttyACM0}"
DIR="$(cd "$(dirname "$0")" && pwd)"
echo "==> Flashing MiniMario to $PORT"
python -m esptool --chip esp32c6 -p "$PORT" -b 460800 \
    write_flash --flash_mode dio --flash_freq 80m --flash_size 4MB \
    0x0 "$DIR/MiniMario-full-esp32c6.bin"
echo "==> Done. The board reboots into the MiniMario menu."
echo "    If your board already has a different partition layout, run:"
echo "    python -m esptool --chip esp32c6 -p $PORT erase_flash   # then rerun this script"
