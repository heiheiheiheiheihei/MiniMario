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
void nes_port_get_stats(uint32_t *frames, uint32_t *fps_x100, uint32_t *last_frame_us,
                        uint32_t *machine_name);

#ifdef __cplusplus
}
#endif
