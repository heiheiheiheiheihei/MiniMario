/*
 * MiniMario - nofrendo based NES emulator port for the ESP32-C6.
 *
 * The port exposes a very small API on top of libnofrendo:
 *   nes_port_init()            initialise the OSD layer / timer
 *   nes_port_set_rom()         give it a ROM image (RAM or memory-mapped flash)
 *   nes_port_set_frame_sink()  callback invoked once per rendered frame
 *   nes_port_set_input_source()callback returning the current NES_PAD_* mask
 *   nes_port_run()             blocking emulation loop (returns on quit)
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "pad.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Called once per emulated frame with the 256x224 palette-index framebuffer
 * and the 256 entry RGB565 palette.  The buffers stay valid until the next
 * frame is rendered.  The callback runs in the emulator task. */
typedef void (*nes_frame_sink_t)(const uint8_t *indexed, const uint16_t *palette565);

/* Returns the current NES_PAD_* button mask (called once per frame). */
typedef uint8_t (*nes_input_source_t)(void);

esp_err_t nes_port_init(void);

/* ROM buffer must remain valid for the whole emulation session. */
esp_err_t nes_port_set_rom(const uint8_t *rom, size_t len);
const uint8_t *nes_port_get_rom(size_t *len);

void nes_port_set_frame_sink(nes_frame_sink_t sink);
void nes_port_set_input_source(nes_input_source_t src);

/* Blocking.  Runs until nes_port_request_quit() is called (or the game asks to
 * exit through its own reset handling). */
esp_err_t nes_port_run(void);
void nes_port_request_quit(void);
bool nes_port_is_running(void);

/* Debug/verification helpers */
const uint8_t *nes_port_framebuffer(void); /* 256*224 palette indices */
const uint16_t *nes_port_palette(void);    /* 256 RGB565 entries */
/* machine_name receives the iNES mapper number of the loaded ROM. */
void nes_port_get_stats(uint32_t *frames, uint32_t *fps_x100, uint32_t *last_frame_us,
                        uint32_t *machine_name);

/* ------------------------------------------------------------------------- */
/* Additive helpers (not part of the original frozen API, safe to ignore).    */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint32_t frames;         /* emulated frames since nes_port_run() started   */
    uint32_t displayed;      /* frames that were rendered + handed to the sink*/
    uint32_t skipped;        /* frame-skipped frames (no rendering)           */
    uint32_t fps_x100;       /* emulated fps * 100 over the last ~2 s          */
    uint32_t last_frame_us;  /* duration of the last emulated frame            */
    uint32_t last_core_us;   /* ... minus the frame sink time                  */
    uint32_t last_sink_us;   /* time spent inside the frame sink               */
    uint32_t max_frame_us;   /* longest emulated frame since run started        */
    uint32_t total_frame_us; /* sum of all frame durations (for averages)       */
    uint32_t total_core_us;  /* sum of frame durations minus sink time          */
    uint32_t total_sink_us;  /* sum of sink times                               */
    uint32_t mapper;         /* iNES mapper number                             */
    uint32_t heap_free;      /* esp_get_free_heap_size() at the last frame     */
} nes_port_stats_t;

void nes_port_get_stats_ex(nes_port_stats_t *out);

/* Optional ROM name used in logs / rom_getinfo(). MAX 47 chars + NUL. */
void nes_port_set_rom_name(const char *name);
const char *nes_port_rom_name(void);

/* FNV-1a 32 bit checksum over the visible 256x224 indexed framebuffer. */
uint32_t nes_port_fb_checksum(void);

/* Render the current visible framebuffer as 80x25 ASCII art ('\n' separated,
 * 80 chars per line, NUL terminated).  Returns the number of bytes needed
 * (excluding the NUL); writes at most `len` bytes if `out` is not NULL. */
int nes_port_debug_ascii(char *out, size_t len);

/* Per-second stats/checksum logging from inside the emulation loop. Off by
 * default so a binary "shot" dump on the serial console cannot be corrupted;
 * enable it for standalone validation builds. */
void nes_port_set_debug_log(bool on);

/* Pause/resume emulation without leaving nes_port_run().  While paused the
 * frame loop keeps its 60 Hz cadence but does not advance the console, so the
 * framebuffer stays stable: a debug console can dump a tear-free "shot" by
 * pausing, reading nes_port_framebuffer(), then resuming.  Safe to call from
 * another task. */
void nes_port_set_paused(bool paused);
bool nes_port_is_paused(void);

#ifdef __cplusplus
}
#endif
