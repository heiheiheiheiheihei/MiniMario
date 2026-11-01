# MiniMario host tools

All tools talk to the board's **native USB-Serial-JTAG console** (`/dev/ttyACM0`)
and need `pyserial` (`tools/shot.py` uses Pillow when available, otherwise it
falls back to a pure-python PNG writer). Override the port with
`--port …` or the `MINIMARIO_PORT` environment variable.

> **Board sharing rule:** every access to `/dev/ttyACM0` (flash, esptool,
> pyserial, monitor) must be wrapped in
> `flock /tmp/mm_flash.lock <command>` so concurrent agents do not reset the
> board under each other. Tools never touch DTR/RTS.

## `pack_roms.py` — build the embedded ROM container

```bash
python3 tools/pack_roms.py roms/*.nes -o /tmp/mmroms.bin
python3 tools/pack_roms.py --list /tmp/mmroms.bin
```

Container layout (little endian, offsets relative to the partition start):

| field | type | notes |
|---|---|---|
| magic | 8 bytes | `"MMROM1\0\0"` |
| count | u32 | number of ROMs (≤ 64) |
| entries[count] | 40 bytes each | `u32 offset`, `u32 size`, `char name[32]` (NUL terminated) |
| blobs | bytes | iNES images, each 16-byte aligned |

A partition that starts directly with `NES\x1a` is also accepted by the
firmware as one legacy raw ROM.

## `flash_rom.py` — write ROMs to the `roms` partition (0x1F0000)

```bash
python3 tools/flash_rom.py                      # pack + flash every roms/*.nes
python3 tools/flash_rom.py --pack a.nes b.nes   # custom container
python3 tools/flash_rom.py --file game.nes      # single legacy raw ROM
python3 tools/flash_rom.py --pack a.nes --dry-run
python3 tools/flash_rom.py --dry-run --out firmware/roms.bin   # rebuild the container only
```

The tool reads offset/size from `partitions.csv`, refuses images larger than the
partition and calls esptool (`--chip esp32c6 -p <port>`) itself. The board
resets after flashing.

## `shot.py` — screenshot over the serial console

```bash
flock /tmp/mm_flash.lock python3 tools/shot.py -o /tmp/menu.png
flock /tmp/mm_flash.lock python3 tools/shot.py --json -o /tmp/game.png
flock /tmp/mm_flash.lock python3 tools/shot.py --wait-boot 20 --scale 3
```

It sends `shot\n`, parses the framed answer and writes a PNG (indexed, nearest
upscaled by `--scale`, default 2). In the menu the UI canvas is dumped, while a
running emulator returns its 256x224 frame; both include the palette (the UI
palette has 16 entries, the NES palette 256).

Protocol (device → host):

```text
SHOT 256 224 57344 512\n     <- NES frame: 57344 indexed bytes + 512 palette bytes
SHOTUI 320 172 55040 32\n     <- menu canvas (dims depend on rotation)
SHOTRAW 320 172 55040\n       <- `shotraw`: indexed pixels only
<payload><palette>SHOTEND\n
```

`--json` prints
`{"mode": "menu"|"emu"|"raw", "width", "height", "bytes", "palette_entries", "sha256", "png"}`.
The tool retries `--attempts` times if a dump is truncated.

## `serial.py` — log tail / command runner

```bash
flock /tmp/mm_flash.lock python3 tools/serial.py                        # tail logs
flock /tmp/mm_flash.lock python3 tools/serial.py --reset --wait-boot 20
flock /tmp/mm_flash.lock python3 tools/serial.py -s help -s info -s roms --timeout 4
flock /tmp/mm_flash.lock python3 tools/serial.py -s "play flash" --until "OK running" --timeout 25
```

Options: `-s/--send` (repeatable), `--send-delay`, `--until MARKER` (stops and
returns 0 when seen), `--wait-boot N`, `--timeout`, `--timestamps`,
`--capture FILE`, `--reset`, `--json`.

The firmware prints `MMBOOT ready fw=… roms=… sd=… heap=…` once the menu is up,
which is the recommended synchronisation point for automated tests.

## Console command reference

See `docs/app.md`. In short:

```text
help  info  roms  play <n|flash|path>  menu  quit  up  down  select  rescan
key <a|b|select|start|up|down|left|right|none>  pad
rot <0-3>  scale <0-2>  bl <0-100>  bgr <0|1>  test  clear
shot  shotraw  ping  reboot
```
