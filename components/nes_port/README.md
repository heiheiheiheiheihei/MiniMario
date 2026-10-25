# nes_port — nofrendo NES core OSD/port layer for the ESP32-C6 (MiniMario)

This component runs the nofrendo NES core (`components/nofrendo`) on the
Waveshare ESP32-C6-LCD-1.47 board (ESP32-C6FH4: single RISC-V core @160 MHz,
512 KB SRAM, 4 MB flash, no PSRAM, **no audio hardware**) through the frozen API
in `include/nes_port.h`.

It replaces Espressif's old IDF v3 `components/nofrendo-esp32` OSD layer, which
cannot be used on the C6 (raw SPI register pokes, dual-core video queue, DAC
audio, ESP32-only headers).

**Status: final.** All core patches are documented in
[`components/nofrendo/PATCHES.md`](../nofrendo/PATCHES.md).  Verified on real
hardware, including the integrated MiniMario firmware (`play flash` ->
twin_dragons: **59.97 fps** emulation, emulator heap **20,132 B**, deterministic
checksums, clean quit back to the menu, game visually correct).

---

## 1. What it does

- **Video.** The nofrendo PPU renders directly into a static 256x240 palette
  index framebuffer in `.bss` (61,440 bytes).  The visible 256x224 picture
  (PPU lines 8..231, the standard NTSC overscan crop) is handed to the
  registered `nes_frame_sink_t` once per displayed frame.  The 256-entry
  palette is converted from nofrendo `rgb_t` to RGB565 in the video-driver
  `set_palette` hook and is available through `nes_port_palette()`.
- **Pacing.** Own frame loop with absolute `esp_timer` deadlines at 60.0988 Hz
  (16,639 us/frame, +1 us every third frame).  Emulation speed never depends on
  the display: a token bucket decides whether the next frame is drawn or
  skipped (frame skip), so a slow display only lowers the *display* rate.
- **No audio.** The APU object is created because games write APU registers and
  expect the APU frame IRQ, but the sample-processing callback is never
  installed.  The NES frame IRQ still comes from the core's per-scanline
  `nes_checkfiq()`.
- **Input.** The application's `nes_input_source_t` is polled exactly once per
  emulated frame and translated into nofrendo joypad events
  (`event_joypad1_a/b/start/select/up/down/left/right`).
- **Quit / reload.** `nes_port_request_quit()` (safe from another task) makes a
  blocked `nes_port_run()` return cleanly; a second, different ROM can then be
  loaded and run in the same boot.  Frame-loop allocations: none.
- **No dependencies besides nofrendo + `pad.h`.**  The ROM and the frame sink
  are supplied by the application (no filesystem, no display calls inside the
  component).

## 2. Integration (frozen API)

```c
display_init();                      /* platform, once */
nes_port_init();                     /* once per boot (idempotent) */
nes_port_set_rom_name("game.nes");   /* optional: used in logs/rom_getinfo */
nes_port_set_rom(rom_ptr, rom_len);  /* RAM buffer or esp_partition_mmap ptr  */
nes_port_set_frame_sink(display_show_nes_frame);
nes_port_set_input_source(my_pad_read);
nes_port_run();                      /* blocking; returns ESP_OK on quit      */
```

- The ROM pointer must stay valid for the whole run.  Both heap buffers and
  read-only SPI-flash `esp_partition_mmap()` pointers are supported and perform
  within ~4 % of each other.
- `nes_port_run()` blocks but calls `vTaskDelay()` every frame, so a menu or
  serial-console task can call `nes_port_request_quit()` any time.
- `nes_port_set_frame_sink(NULL)` = headless run (no display updates).
- ROM problems (not iNES, truncated, unsupported mapper) are reported by
  `nes_port_set_rom()` *before* the emulator starts, so the app can show a
  proper error message.

## 3. Measured on hardware (ESP32-C6 rev0.1 @160 MHz, DISPLAY_SCALE_FIT)

| ROM | mapper | path | fps (emulation) | frames drawn | avg frame = core + display sink |
|---|---|---|---|---|---|
| twin_dragons.nes 40 KB | 0 | flash mmap, no display | 59.86 | 152/152 | 9.60 ms |
| twin_dragons.nes 40 KB | 0 | RAM copy, no display | 60.31 | 121/121 | 9.37 ms |
| twin_dragons.nes 40 KB | 0 | flash mmap + display | **59.19** | 189/210 (90 %) | 16.6 = 9.08 + 8.37 ms |
| nova.nes 256 KB | 1 | flash mmap, no display | 59.91 | 151/151 | 7.78 ms |
| nova.nes 256 KB | 1 | RAM copy, no display | 60.22 | 121/121 | 7.43 ms |
| nova.nes 256 KB | 1 | flash mmap + display | **59.80** | 206/210 (98 %) | 16.1 = 8.13 + 8.13 ms |
| 01.basics.nes 16 KB (sprite hit) | 0 | flash mmap + display | **60.05** | 211/211 (100 %) | 16.0 = 7.42 + 8.55 ms |
| twin_dragons.nes, integrated MiniMario app | 0 | flash + display | **59.97** | — | Lead-verified |

- **Heap used by the emulator:** 20,160 B (NROM) / 28,328 B (MMC1 + CHR-RAM),
  including the 8 KB battery SRAM and 8 KB VRAM allocated by the core.
  Target was < 300 KB.
- **Static RAM:** 61,440 B `.bss` framebuffer; no frame-loop allocation.
- **Deterministic checksums:** static screens give a constant FNV-1a checksum
  (e.g. sprite basics `d4faff0a` for the whole run and after a fresh reload of
  the same ROM); reloading a ROM reproduces the exact checksum sequence.
- Runs were clean (no crash/WDT) and every `nes_port_run()` returned to the
  caller with the heap back at its pre-run level.

## 4. Debug / verification helpers

```c
/* FNV-1a 32 over the visible 256x224 palette-index framebuffer */
uint32_t chk = nes_port_fb_checksum();

/* 80x25 ASCII art of the NES screen, for eyeballing over a serial console.
 * Each character is the average palette luminance of a 3.2 x 9 pixel block;
 * rows are '\n' separated (2026 bytes incl. NUL).  Returns bytes needed. */
char ascii[2026];
int n = nes_port_debug_ascii(ascii, sizeof ascii);
fwrite(ascii, 1, n, stdout);            /* or raw_write() on the serial console */

/* One line per second: fps, frames/drawn/skipped, last frame us, core vs sink
 * split, free heap and the framebuffer checksum.  OFF by default so binary
 * "shot" dumps on the console cannot be corrupted. */
nes_port_set_debug_log(true);

/* Tear-free screenshots: keep the 60 Hz cadence but freeze the console. */
nes_port_set_paused(true);
dump(nes_port_framebuffer(), nes_port_palette());
nes_port_set_paused(false);

/* Extra stats (additive; frozen API untouched) */
nes_port_stats_t st;
nes_port_get_stats_ex(&st);  /* frames, displayed, skipped, fps_x100,
                                last/max/avg frame, core and sink us, mapper,
                                free heap */
```

The frozen `nes_port_get_stats(..., machine_name)` returns the iNES **mapper
number**.  ASCII dump usage from an existing console is only
`nes_port_debug_ascii(buf, sizeof buf)` and write `buf` as text; use
`nes_port_fb_checksum()` to compare runs automatically (the source of the
checksums in the table above).

## 5. How to change ROM limits / mapper support

**Flash-mapped ROMs (recommended):** `roms` is a data partition
(`partitions.csv`, type `data`/subtype `0x40`, currently at `0x1F0000`, size
`0x1F0000` = 1.9 MB).  `nes_port` maps the whole partition and accepts any iNES
image whose size (`16 + trainer?512 + PRG_banks*16384 + CHR_banks*8192`) fits
in it — no heap cost.  To allow bigger ROMs, enlarge/move that partition in
`partitions.csv` (Lead-owned file) and flash the ROM at the matching offset;
nothing in `nes_port` or `nofrendo` has to change.

**Heap RAM buffers:** limited by free heap.  The integrated firmware (Bluepad32
up) has ~165 KB free before the emulator, and the emulator itself needs ~28 KB
(+ display), so cap RAM ROMs at ~128 KB and fall back to the flash partition for
bigger ones.  In a Bluetooth-less build (342 KB free at boot) a 256 KB ROM was
successfully copied to RAM and ran at 60.22 fps.

**Adding a mapper:** drop `mappers/mapNNN.c` into `components/nofrendo/mappers/`
(the component CMakeLists globs `mappers/*.c`), then register its
`mapNNN_intf` in `components/nofrendo/nes/mmclist.c` — that is also where the
current list is.  Currently registered: 0, 1, 2, 3, 4, 5, 7, 8, 9, 11, 15, 16,
18, 19, 21, 22, 23, 24, 25, 32, 33, 34, 40, 41, 42, 46, 50, 64, 65, 66, 70, 73,
75, 78, 79, 85, 87, 93, 94, 99, 160, 229, 231.  Unsupported mappers are rejected
cleanly by `nes_port_set_rom()`.

## 6. Known limitations

- iNES format only (no UNIF); no audio output (hardware limitation); battery
  SRAM is allocated/emulated but not persisted (no filesystem in this
  component); save states exist in the core but are not exposed through the
  API; screenshots taken without `nes_port_set_paused()` can tear because the
  console dumps 57 KB asynchronously while the game keeps running.
- The nofrendo core is GPLv2 (see file headers / `AUTHORS`); the ESP32 fork
  comes from Espressif's `esp32-nesemu` (Apache-2.0 porting code, GPLv2 core).
- `vid_drv`/`nes.c`/`nes_rom.c` were modified for this port — see
  `components/nofrendo/PATCHES.md` before updating the core from upstream.

## 7. Reproducing the measurements

The standalone harness that produced the table above lives in `/tmp/emu_test`
(not part of the repo); its `README.md` documents the commands.  Short version:

```bash
/tmp/emu_test/sync.sh
flock /tmp/mm_build.lock bash -c 'source /home/myf/esp/esp-idf/export.sh >/dev/null 2>&1 \
  && cd /tmp/emu_test && idf.py -B build build'
# board access must be serialized with /tmp/mm_flash.lock and must not toggle DTR/RTS:
flock /tmp/mm_flash.lock /tmp/emu_test/run_hw.sh /tmp/roms/twin_dragons.nes 13
```

The harness runs four `nes_port_run()` sessions per boot: flash-mapped without
display (pure fps), RAM copy (fps comparison), flash + `display_show_nes_frame`
+ scripted input, and a reload after quit; a monitor task prints fps/frame
times/heap/checksums every 500 ms and 80x25 ASCII dumps at fixed points.
