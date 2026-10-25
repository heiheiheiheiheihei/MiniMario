# Display pipeline — performance notes

Target board: Waveshare ESP32-C6-LCD-1.47 (ESP32-C6FH4, 1 RISC-V core @160 MHz,
ST7789 172x320 SPI panel, no PSRAM). ESP-IDF v5.3.2, `CONFIG_COMPILER_OPTIMIZATION_PERF=y`.

## What was changed

* **Pipelined flush.** Two DMA chunk buffers (32 panel rows = 11,008 B each, the
  first chunk is 8 rows so the first transfer starts earlier). Chunk N+1 is
  converted while chunk N is being transferred.
* **ST7789 continuous streaming.** One `CASET`/`RASET` window + one `RAMWR` per
  frame, then all remaining chunks are queued as command-less
  "memory write continue" transfers. This removes the 3 short command
  transactions that the first implementation issued per 32-row band
  (~115 us/band of pure overhead on this single-core SoC).
* **Direct SPI master driver** (replaces esp_lcd; the extra esp_lcd bookkeeping
  per chunk cost ~95 us/band on this single-core SoC): queued
  `spi_transaction_t`s, D/C line driven from the transaction pre-callback,
  completion reaped in order with `spi_device_get_trans_result()`. Command
  groups are sent as polling transactions while the SPI bus lock is held so
  they stay atomic (the TF card can share the bus later). A bare `spi_device`
  queue of the same 110,080 B frame measures 11.20 ms = 101% of the
  theoretical 11.008 ms, so the SPI path itself is at the hardware limit.
* **Fast conversion kernels** (`IRAM_ATTR`, 2 pixels per 32-bit store,
  precomputed per-panel-column offset/column tables, 2 panel rows per pass so
  the table load is shared). No division, no per-pixel function call.
* **Four rotations.** Rot 90/270 = landscape 320x172 logical space (software
  transpose of the portrait panel). Rot 0/180 = portrait 172x320 logical space
  (direct mapped, plain per-row palette expansion — no transpose at all).
* **Calibration pattern** (`display_test_pattern()`): 4 coloured quadrants with
  TOP-LEFT / TOP-RIGHT / BOTTOM-LEFT / BOTTOM-RIGHT labels, 16-colour swatch
  bar, dithered grey gradient, "MiniMario" title, and a bottom info line. Text
  uses the bundled public-domain 8x8 font (now `static const` in .rodata, saves
  1 KB of DRAM).
* **New additive APIs** (see `include/display.h`): `display_set_bgr()` /
  `display_get_bgr()` (MADCTL bit 0x08 only, mirror-X 0x40 always kept, default
  RGB), `display_ui_canvas()`, `display_ui_palette()`, `display_ui_shade()`,
  `display_set_spi_clock()` / `display_get_spi_clock()`, `display_benchmark()`,
  `display_debug_render_panel()`, plus `us_convert` / `us_spi` in
  `display_stats_t`.

## Benchmark results (on hardware, final direct-SPI driver)

`display_benchmark()` = 256x224 synthetic indexed NES frame, average over
20 frames/config, 80 MHz SPI. "convert" is the time spent in the
indexed->RGB565 transpose; "spi" is everything else (DMA queue + waiting).
The two overlap; `total` is the wall time of `display_show_nes_frame()`.
Max values are within ~10 us of the average (no jitter spikes).

| rotation | scale | total avg | max | convert | spi | bytes/frame | pure-SPI ideal | tx errors |
|---|---|---|---|---|---|---|---|---|
| 90  | FIT   | 8.023 ms* / **7.570 ms** | 8.293 / 7.583 | 1.95 ms | 5.62 ms | 72,240  | 7.224 ms | 0 |
| 90  | FILL  | 12.034 ms* / **11.406 ms** | 12.399 / 11.410 | 2.96 ms | 8.45 ms | 110,080 | 11.008 ms | 0 |
| 90  | PIXEL | 8.011 ms* / **7.101 ms** | 8.172 / 7.112 | 1.83 ms | 5.27 ms | 67,768  | 6.776 ms | 0 |
| 270 | FIT   | **7.567 ms** | 7.570 | 1.944 ms | 5.623 ms | 72,240  | 7.224 ms | 0 |
| 270 | FILL  | **11.406 ms** | 11.410 | 2.958 ms | 8.448 ms | 110,080 | 11.008 ms | 0 |
| 270 | PIXEL | **7.101 ms** | 7.106 | 1.827 ms | 5.273 ms | 67,768  | 6.776 ms | 0 |
| 0 (portrait) | FIT | **5.520 ms** | 5.524 | 1.585 ms | 3.935 ms | 51,600 | 5.160 ms | 0 |
| 0 (portrait) | FILL | **11.460 ms** | 11.465 | 3.367 ms | 8.093 ms | 110,080 | 11.008 ms | 0 |
| 2 (portrait) | FIT | **5.515 ms** | 5.519 | 1.584 ms | 3.931 ms | 51,600 | 5.160 ms | 0 |

(* = the first, esp_lcd-based streaming version, kept for reference; final
driver is the direct SPI-master implementation.)

**Final: FIT 7.57 ms, FILL 11.41 ms at 80 MHz — both targets met with margin.**
With FIT the emulator needs 72,240 B = 7.22 ms of pure SPI, so 7.57 ms is 95%
SPI-bus-limited: the remaining 0.35 ms is the un-overlapped first chunk plus
queue bookkeeping.

UI canvas commit (full 320x172 panel, `display_ui_commit()`):
11.404 ms (rot 90), 11.406 (rot 270), 11.458 (rot 0), 11.459 (rot 180);
convert 2.94-3.37 ms, the rest is SPI. Full-frame UI at 87 fps possible.

### SPI clock reliability

* 80 MHz: every run 0 transaction errors, effective throughput 101% of
  theoretical with the raw driver. Waveshare's own demos also run this panel at
  80 MHz. **Recommended clock: 80 MHz.**
* 60 MHz: on the ESP32-C6 the SPI source clock is 80 MHz, so a 60 MHz request
  falls back to 40 MHz. The benchmark numbers for 60 and 40 MHz are identical
  (FIT 14.76 ms, FILL 22.38 ms) — flagged `SLOW` in the benchmark log. This is
  an SoC clock-tree limitation, not a panel problem.
* 40 MHz: 0 errors, same as 60. Only useful as a debug fallback.

## UI canvas acquire/release

100 acquire/release cycles: heap `368676 -> 368676 B`, delta **0**, no failures.
Canvas is 320x172 = 55,040 B while acquired, freed while the emulator runs.

## Static RAM

* Old driver: ~12.1 KB `.bss` (single 11,008 B chunk) + 1 KB font in `.data`.
* New driver (measured on the final `display.c.obj`): 25,546 B `.bss` + 20 B
  `.data` (2 x 11,008 B chunk buffers, mapping tables, transaction
  descriptors); font moved to `.rodata`.
* Net SRAM increase ≈ **+12.4 KB** (within the 30 KB budget); the UI canvas
  remains heap-allocated.

## Orientation verification

`display_debug_render_panel()` renders the UI canvas through the exact same
rotation/transpose tables into the physical portrait 172x320 RGB565 frame. For
all four rotations the reconstructed "what the user should see" image matches
the drawn logical canvas **pixel for pixel** (0 mismatching pixels; see
`tests/rle_to_png.py` in the temporary test project). Screenshots:

* [view_rot1.png](screenshots/view_rot1.png) — board held landscape (rot 90)
* [view_rot0.png](screenshots/view_rot0.png) — board held portrait (rot 0)
* [panel_phys_rot1.png](screenshots/panel_phys_rot1.png) — raw physical
  portrait framebuffer for rot 90 (what is actually shifted into the panel,
  before the panel's own mirror-X/rotation is applied)

## Reproduction

The temporary test app lives outside the repo (`/tmp/display_test`, not
committed): it draws the calibration pattern for each rotation, streams an RLE
dump of the logical canvas + physical framebuffer over the console, runs
`display_benchmark(20)`, and dumps the screenshots. Build it with
`EXTRA_COMPONENT_DIRS` pointing at a directory that symlinks
`components/display` (the full repo has other components with their own
dependencies).
