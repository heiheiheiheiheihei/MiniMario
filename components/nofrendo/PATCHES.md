# Changes to nofrendo for the ESP32-C6 / ESP-IDF 5.3 port

Source: `components/nofrendo` from Espressif's `esp32-nesemu`
(https://github.com/espressif/esp32-nesemu), itself nofrendo 0.9x
(GPLv2, (c) 1998-2000 Matthew Conte).  Only the changes below were made; the
files keep their original licenses/headers.

The platform layer (OSD, video driver, input, timing) lives in
`components/nes_port`, not here.

## Build system

- Added `CMakeLists.txt` (the reference used an IDF v3 `component.mk`).
  Compiles all core sources including `mappers/*.c` and `nes/mmclist.c`, with
  `-O2 -fno-strict-aliasing` and `-Wno-error=...` for warning classes that the
  1998-era code cannot avoid (`stringop-truncation`, `misleading-indentation`,
  `array-bounds`, ...).  Never built with `NOFRENDO_DEBUG` (the memguard debug
  allocator and PC style debug helpers are not wanted on the MCU).

## Portability fixes

1. `noftypes.h`: replaced the 1998 `typedef enum { false = 0, true = 1 } bool;`
   with `#include <stdbool.h>` / `<stddef.h>`.  The enum definition clashes with
   C17/stdbool (and with every ESP-IDF header that includes `<stdbool.h>`).
2. `memguard.h`: include `<stdlib.h>` and `<string.h>` *before* defining the
   `malloc`/`free`/`strdup` wrapper macros.  Without this, any libc header
   included later gets its prototypes macro-expanded
   (`-Werror=stringop-truncation` / "expected declaration specifiers" in
   `libsnss.c`, `event.c`, ...).
3. `log.c`: `log_assert()` used the ARM/DOS `asm("break.n 1")`; removed (not
   valid on RISC-V, and `ASSERT()` is compiled out anyway without
   `NOFRENDO_DEBUG`).
4. `osd.h`: include `<string.h>`/`<strings.h>` for `strcasecmp`/`stricmp` and
   null-terminate `osd_fullname()`.

## Memory / rendering changes

5. `vid_drv.c`, `vid_drv.h`: added `vid_setstaticbuffer(data, w, h)` and
   `vid_getstaticbuffer()`.  When a static buffer is registered, `vid_setmode()`
   wraps it in a hardware `bitmap_t` instead of allocating a 57 KB heap buffer,
   so `vid_getbuffer()` (what the ESP32 fork's PPU renders into) points at the
   port's `.bss` framebuffer.  `vid_shutdown()` drops the cached bitmap.
6. `nes.c`, `nes.h`: added

   ```c
   void nes_emulate_frame(bool draw);
   ```

   Runs exactly one NTSC frame: `nes_renderframe(draw)`, then `vid_flush()` and
   `osd_getinput()` once per frame.  `draw == false` is a frame skip (no pixel
   rendering, but CPU, PPU state, NMI and sprite-0 hit detection still run at
   full speed).  The original timer-interrupt/autoframeskip `nes_emulate()`
   loop is left in place but unused: the port drives its own 60.0988 Hz
   `esp_timer`-based frame loop.
   The GUI overlay drawing (`gui_frame`) is not called by the port, so frames
   are clean; the GUI message/palette machinery is still initialised because
   the core reports ROM/mapper information through it.

7. `nes_rom.c`:
   - `rom_free()` no longer frees `rominfo->rom` / `rominfo->vrom`.  In the
     Espressif fork these point directly into the caller-owned image (RAM
     buffer or memory-mapped SPI flash, which is read-only), so freeing them
     crashed/asserted on every eject or ROM reload.
   - `rom_load()` stores the supplied filename in `rominfo->filename` so
     `rom_getinfo()` and the ROM loading messages make sense.
   - removed a leftover debug `printf` of the ROM header in `rom_getheader()`
     and fixed a misleading-indentation warning in `rom_load()`.

## Performance optimisation (FPU-less ESP32-C6)

8. `nes.c`, `nes.h`: the 6502/PPU scanline timing accumulator
   (`nes.scanline_cycles`) was a `float` holding 1/12 CPU-cycle units, so every
   scanline did 3 software float operations on the C6 (RV32IMAC, no FPU): 262
   scanlines/frame x ~100 cycles per soft-float op adds up.  It is now an
   `int32` in exactly the same 1/12 cycle units (`NES_SCANLINE_CYCLES 1364`,
   division by 12 at the call site compiles to a multiply).  Timing is
   bit-identical, and the frame-buffer checksums of the reference ROMs are
   unchanged, which is used as the regression test.

## Notes for maintainers

- nofrendo renders 240 scanlines (`ppu_scanline(bmp, 0..239)`), while the
  visible NES picture is 224 lines.  The port therefore supplies a 256x240
  framebuffer and passes `framebuffer + 8*256` (contiguous 224 rows) to the
  frame sink.  The reference port allocated only 224 rows and rendered 240
  into it — an out-of-bounds write on every frame.
- Memory-mapped flash ROMs are read-only from the core's point of view; no
  mapper register or CPU read path writes to `rominfo->rom`.
- `osd_getromdata()` is implemented in `components/nes_port/nes_port.c` and
  returns the pointer supplied to `nes_port_set_rom()`.
