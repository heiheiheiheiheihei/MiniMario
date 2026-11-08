# Bundled NES ROMs

This directory holds the **free / open-source NES homebrew ROMs that are embedded
in the MiniMario `roms` flash partition** (and flashed by `tools/flash_rom.py`).

* **16 ROMs, 1,630,464 bytes** (MMROM1 container = 1,631,120 bytes, fits the
  2,031,616-byte `roms` partition with ~400 KB spare).
* All of them are standard iNES images using mappers implemented by nofrendo
  (0, 1 and 2), and all were boot-tested in the same patched nofrendo core the
  firmware runs (headless host harness: non-blank, rendering frames, no crash;
  600 frames each plus 3600-frame stability runs).
* **No Nintendo-copyrighted ROMs are included.** Every file is a homebrew game
  released by its author under a license that permits redistribution.

| # | File | Game | Author | Size | Mapper | License |
|---|------|------|--------|-----:|:------:|---------|
| 1 | `croom.nes` | Concentration Room | Damian Yerrick | 24 KB | 0 | GPL-3.0-or-later (iNES-copy exception) |
| 2 | `driar.nes` | Driar | S. Adolfsson & D. Eriksson | 128 KB | 1 | CC-BY-NC-3.0 |
| 3 | `falling.nes` | Falling | Jesse Williams | 40 KB | 0 | MIT |
| 4 | `floatmancer.nes` | Floatmancer | NotExactlySiev | 40 KB | 0 | CC0-1.0 |
| 5 | `mega_mountain.nes` | Mega Mountain: NES Edition | adrian09_01 / nesdoug / pinobatch | 40 KB | 0 | GPL-3.0 + MIT |
| 6 | `minekart_madness.nes` | MineKart Madness (MMC1 build) | Matthew Hughson | 256 KB | 1 | MIT |
| 7 | `nes_runner.nes` | NES Runner | Zorchenhimer | 80 KB | 1 | MIT |
| 8 | `nova.nes` | Nova the Squirrel | Nova Squirrel | 256 KB | 1 | GPL-3.0 (code) + CC-BY-NC-SA-4.0 (assets) |
| 9 | `rainfall.nes` | Rainfall | bbossa | 40 KB | 0 | CC0-1.0 |
| 10 | `rhde.nes` | RHDE: Furniture Fight | Damian Yerrick | 32 KB | 0 | FSFAP |
| 11 | `robotfindskitten.nes` | robotfindskitten | Damian Yerrick | 32 KB | 0 | Zlib |
| 12 | `ruder.nes` | Zap Ruder | Damian Yerrick | 24 KB | 0 | FSFAP |
| 13 | `sliding_blaster.nes` | Big City Sliding Blaster | Nova Squirrel | 24 KB | 0 | Zlib |
| 14 | `super_tilt_bro.nes` | Super Tilt Bro. (no-network UxROM build) | Sylvain Gadrat | 512 KB | 2 | WTFPL |
| 15 | `thwaite.nes` | Thwaite | Damian Yerrick | 24 KB | 0 | GPL-3.0-or-later |
| 16 | `twin_dragons.nes` | Twin Dragons | Garydos / surt / Shiru | 40 KB | 0 | Zlib (code) + CC-BY (art/music), see caveat below |

Exact upstream URLs, commits/releases, SHA-256 checksums and per-ROM notes:
[`MANIFEST.md`](MANIFEST.md). Rejected candidates and the reasons:
[`PROBLEMS.md`](PROBLEMS.md).

## Licensing

Each ROM keeps the license of its authors. Verbatim license texts (plus a
provenance header) are in [`licenses/`](licenses/), one `<rom>.txt` per shipped
ROM (plus `celeste.txt` and `roborun.txt`, kept for the record although those
two are not shipped).

Notable caveats:

* **Twin Dragons** — the game is distributed by its author as a free homebrew
  ROM (code zlib, some art CC-BY-3.0, most music CC-BY-4.0). Its FamiTone
  music/SFX by Shiru is marked *all rights reserved* upstream; the compiled ROM
  is redistributed here unchanged, byte-for-byte as published by its author, and
  is also the ROM that the shipped MiniMario firmware already contained. It is
  included for personal, non-commercial use of this project.
* **Nova the Squirrel** — GPL-3.0 code, assets CC-BY-NC-SA-4.0.
* **Driar** — CC-BY-NC-3.0 (non-commercial share-alike with attribution).
* **Concentration Room / Thwaite / Mega Mountain / MineKart Madness** —
  GPL/MIT; the ROMs are unmodified upstream binaries, full source is linked in
  `MANIFEST.md` and `licenses/`.
* **Excluded after boot testing** — *Celeste NES (demo 1.00)*: nofrendo never
  enables its PPU (solid black; jsnes renders the title, so it is a core
  incompatibility, not a bad download). *RoboRun-NES 1.1*: PAL/Dendy-only build
  (shows "WRONG VIDEO REGION SELECTED" on the NTSC core). License texts are kept
  for the record. See `PROBLEMS.md`.

## Rebuilding / flashing

`tools/flash_rom.py` packs every `roms/*.nes` (sorted by filename) into the
MMROM1 container and flashes it:

```bash
python3 tools/flash_rom.py                          # pack roms/*.nes + flash to /dev/ttyACM0
python3 tools/flash_rom.py --dry-run --out firmware/roms.bin   # rebuild the container only
python3 tools/pack_roms.py --list firmware/roms.bin # inspect a container
```

The firmware menu shows these files in sorted order (7 rows, scrolls). TF-card
ROMs (`/sdcard/*.nes`) are listed as well, but are RAM-loaded with a 256 KB
hard cap; the practical limit is the free heap (see ../docs/app.md). The
embedded ROMs above use `esp_partition_mmap` and cost no heap
(`Super Tilt Bro.` is 512 KB and therefore flash-only).

To add your own legally-owned ROM, either copy it to the TF card or drop it in
this directory (it is packed by the command above). Keep names ≤ 31 characters.
