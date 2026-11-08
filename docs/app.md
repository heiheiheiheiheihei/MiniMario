# MiniMario application guide

MiniMario runs NES ROMs on the Waveshare ESP32-C6-LCD-1.47 (ESP32-C6FH4,
160 MHz, 512 KB SRAM, no PSRAM). This document covers ROM loading, the menu, the
serial console and the host automation protocol.

---

## 1. ROM workflow

There are two ways to get ROMs onto the board:

### TF card (recommended for quick testing)

* Format the card as FAT16/FAT32 and copy `*.nes` files into the **root**
  (`/sdcard/NAME.nes`). Subdirectories are not scanned.
* The card slot shares the LCD's SPI2 bus; it is mounted at boot and can be
  re-scanned with `B` in the menu or the `rescan` console command.
* TF-card ROMs are copied into RAM: **max 256 KB** per ROM (the emulator needs
  the remaining SRAM). Larger games must be flashed (below).
* ROMs are validated on scan (iNES header `NES\x1a`, `.nes` extension, case
  insensitive). Invalid files are skipped and logged.
* Without a card the board still boots and uses the embedded ROMs.

### Embedded `roms` flash partition (recommended for permanent games)

The `roms` data partition starts at **0x1F0000** and is 1.9375 MB (2,031,616 B;
`partitions.csv`: `roms, data, 0x40` → type = data/0x01, subtype = 0x40).
The shipped [`roms/`](../roms/README.md) directory has 16 free / open-source
homebrew ROMs (1,631,120 B container) that are embedded in the firmware image.
Two layouts are supported:

* **MMROM1 container** (preferred, holds several ROMs)

  | field | type |
  |---|---|
  | magic `"MMROM1\0\0"` | 8 bytes |
  | count | u32 |
  | `count × { u32 offset, u32 size, char name[32] }` | 40 bytes each |
  | iNES blobs (16-byte aligned) | |

  Offsets are relative to the partition start. Build it with
  `tools/pack_roms.py` and flash it with `tools/flash_rom.py`:

  ```bash
  python3 tools/flash_rom.py                       # pack + flash all roms/*.nes
  python3 tools/flash_rom.py --pack a.nes b.nes
  ```

* **Legacy raw iNES image** at the very start of the partition (single ROM):

  ```bash
  python3 tools/flash_rom.py --file game.nes
  ```

Flashed ROMs are **memory-mapped** (`esp_partition_mmap`), so they cost no heap
and can be up to the partition size. This is why the bundled 256 KB
`nova.nes` lives in the container rather than on the TF card.

---

## 2. Menu (landscape 320×172)

At boot the board initialises NVS, the display, the BLE gamepad backend
(Bluepad32), the ROM store and the serial console, then shows the menu.

```
 MiniMario                              v0.1.0
------------------------------------------------
  1 croom.nes                       24K [flash]
  2 driar.nes                      128K [flash]
  3 falling.nes                     40K [flash]
  ...
 16 twin_dragons.nes                40K [flash]
 SD:OK ROM:1/16 HEAP:210K PAD:none           FPS:--
 croom.nes  [embedded]
------------------------------------------------
 A/START play   B rescan   START+SEL quit
 serial: help | up/down/select | shot
```

The shipped `roms/` set contains 16 free / open-source homebrew games (the list
scrolls; up to 64 entries fit). Their sources, licenses and SHA-256 checksums are
in [`roms/README.md`](../roms/README.md); the firmware contains no
Nintendo-copyrighted ROM.

* **D-pad up/down** – move the cursor
* **A** or **START** – start the selected ROM
* **B** – rescan the TF card and the flash partition
* **START+SELECT** (hold 0.8 s) – quit the running game, back to the menu
* **BOOT button** (hold 1.5 s) – quit the running game
* No ROMs: a friendly screen explains how to copy `.nes` files to the TF card or
  flash them into the `roms` partition.
* Defaults on a fresh device: rotation 1 (landscape), scale 0 (FIT, 210x172
  centred), backlight 80 %, RGB colour order. All are persisted in NVS.

The BLE gamepad backend (PS5/Xbox/Switch Pro/8BitDo, …) is scanned
automatically; until a controller is connected the fallback BOOT button maps to
NES `A`, and the serial console can inject any button or start a game.

While the emulator runs the 55 KB UI canvas is released (RAM for the ROM and
emulator), the frame sink is `display_show_nes_frame()`, and the input source is
`gamepad_read()` plus the quit combos. On exit the ROM is unmapped/freed, the
canvas is re-acquired and the menu is redrawn.

---

## 3. Serial console

The native USB-Serial-JTAG port (`/dev/ttyACM0`) carries both ESP-IDF logs and
this line-based console (115200 baud is irrelevant for USB CDC). Commands are
terminated by `\n`, echoed back, and every response starts with
`OK`/`ERR`/`INFO`/`ROM`/`NOTE`/`HINT` for easy scripting.

| command | description |
|---|---|
| `help` | list commands |
| `info` | chip, heap, ROM counts, SD state, display, gamepad, emulator fps |
| `roms` | list ROMs: `ROM <index> <flash|sd> <size> <name>` |
| `play <n\|flash\|path>` | start ROM by list index, first embedded ROM, or TF path |
| `menu` / `quit` | stop the game and return to the menu |
| `up` / `down` / `select` | menu cursor / start selection over serial |
| `rescan` | rescan TF card + flash partition |
| `sdtest [clean]` | TF self-test: 32 KB write/read verify, copy the smallest embedded ROM to `/sdcard/mm_sdtest.nes`, rescan and confirm it is listed; `clean` removes it |
| `key <a\|b\|select\|start\|up\|down\|left\|right\|none> [+…]` | inject pad buttons |
| `pad` | current pad mask and controller info |
| `rot <0-3>` | rotation (0/2 portrait 172×320, 1/3 landscape 320×172), persisted |
| `scale <0-2>` | NES scaling 0=FIT 1=FILL 2=PIXEL, persisted |
| `bl <0-100>` | backlight, persisted |
| `bgr <0\|1>` | MADCTL colour-order fallback (red/blue swap), persisted |
| `test` | display test pattern for 4 s |
| `clear` | clear the screen |
| `shot` | dump the frame (emulator) or UI canvas (menu) |
| `shotraw` | same, raw indexed bytes without palette |
| `ping` | `OK pong` liveness check |
| `reboot` | restart the board |

Examples:

```text
roms
play flash
key right+a
quit
rot 1
```

`key` ORs the mask into the pad state until it is changed again: send
`key none` to release. Buttons can be combined with `+`.

Settings (`rot`, `scale`, `bl`, `bgr`) are stored in NVS namespace `mm` and
restored at boot.

---

## 4. Automation protocol

Boot marker (printed once the menu is up):

```text
MMBOOT ready fw=0.1.0 roms=16 sd=0 heap=164732
```

Screenshot protocol (`shot` / `shotraw`):

```text
SHOT 256 224 57344 512\n      # NES frame: 57344 indexed bytes, 512 palette bytes
SHOTUI 320 172 55040 32\n     # menu canvas (dims = display_logical_*())
SHOTRAW 320 172 55040\n       # indexed bytes only
<payload bytes><palette bytes>SHOTEND\n
```

The palette is RGB565 little-endian (256 entries for NES, 16 for the UI). The
dump is sent with the USB-Serial-JTAG driver and ESP logs are muted during the
burst, so it is not interleaved. While a game is running the console pauses the
emulation (`nes_port_set_paused`) around the ~57 KB transfer, so screenshots are
tear-free and the emulator stays responsive to `quit`. Use `tools/shot.py`
rather than parsing by hand.
The console task polls in 50 ms slices at a low priority, so it never starves
the emulator.

`tools/serial.py` tails logs, sends commands and waits for markers;
`tools/shot.py` saves PNGs (`--json` for machine-readable metadata);
`tools/README.md` documents all host tools.

---

## 5. Troubleshooting

| symptom | fix |
|---|---|
| `SD:--` / `INFO sd_error=mount failed` | no TF card inserted, or the card is not FAT/exFAT; the board works without it |
| `roms` shows 0 ROMs | flash the container: `python3 tools/flash_rom.py` |
| `ERR load: ROM too large …` | TF-card ROM > 256 KB; flash it instead |
| `ERR load: out of memory` | close other features / use the embedded (mmap) copy |
| `bgr` colours | `bgr 1` (default) selects the panel BGR colour order; `bgr 0` for RGB. Also reachable from the menu with a BOOT double tap (tap = BGR, double tap = RGB565 byte order, hold = exit). |
| menu in portrait after `rot 0` | the menu is designed for landscape; use `rot 1` or `rot 3` |
| board stuck in "waiting for download" | `flock /tmp/mm_flash.lock python -m esptool --chip esp32c6 -p /dev/ttyACM0 --before default_reset --after hard_reset chip_id` |

Serial port access must be serialized with
`flock /tmp/mm_flash.lock <command>` when several agents share the board.
