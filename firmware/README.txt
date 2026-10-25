MiniMario firmware for Waveshare ESP32-C6-LCD-1.47
==================================================

Files
  MiniMario-full-esp32c6.bin  complete image (bootloader + partition table + app + built-in ROMs)
  MiniMario-app.bin           application image only (flash at 0x10000)
  bootloader.bin              bootloader (0x0)
  partition-table.bin         partition table (0x8000)
  roms.bin                    built-in ROM container (0x1F0000)

Flash everything with one command:
  ./flash_all.sh /dev/ttyACM0

or manually:
  python -m esptool --chip esp32c6 -p /dev/ttyACM0 -b 460800 \
      write_flash 0x0 MiniMario-full-esp32c6.bin

Built-in free games: Twin Dragons (zlib/CC-BY), Nova the Squirrel (GPLv3/CC-BY-NC-SA).
No Nintendo copyrighted ROMs are included.
Add your own .nes ROMs with tools/flash_rom.py or copy them to a TF card.
