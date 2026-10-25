/*
 * MiniMario - display driver for Waveshare ESP32-C6-LCD-1.47
 * ST7789, 172x320 native (portrait), used in landscape 320x172.
 *
 * The panel itself is kept in the exact portrait mode that Waveshare's own
 * ESP-IDF demo uses (verified working):
 *   MADCTL = 0x40 (mirror X), CASET = x + 34, RASET = y, COLMOD = RGB565,
 *   inversion on.
 * Landscape content is produced by transposing the image in software, so the
 * panel configuration never has to change.
 *
 * Rotation: logical content is always authored in "board landscape" space
 * (DISPLAY_W x DISPLAY_H = 320x172) for rotations 90/270.  Rotations 0/180 are
 * the portrait mount: the logical space is then DISPLAY_H x DISPLAY_W
 * (172x320) and the UI canvas is drawn 1:1 (no transposition).  Always use
 * display_logical_width()/display_logical_height() to lay out UI content.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Landscape logical resolution */
#define DISPLAY_W 320
#define DISPLAY_H 172

/* NES visible frame (nofrendo default) */
#define NES_FRAME_W 256
#define NES_FRAME_H 224

/* Rotation: which way the board is held. 90 = rotated clockwise, 270 = ccw. */
typedef enum {
    DISPLAY_ROT_0   = 0, /* portrait, 172x320 */
    DISPLAY_ROT_90  = 1, /* landscape, board rotated clockwise (default) */
    DISPLAY_ROT_180 = 2, /* portrait flipped */
    DISPLAY_ROT_270 = 3, /* landscape, board rotated counter-clockwise */
} display_rotation_t;

typedef enum {
    DISPLAY_SCALE_FIT   = 0, /* aspect correct 4:3, letterboxed (210x172 centered) */
    DISPLAY_SCALE_FILL  = 1, /* stretch to the full 320x172 panel */
    DISPLAY_SCALE_PIXEL = 2, /* square pixels, letterboxed (197x172 centered) */
    DISPLAY_SCALE_COUNT
} display_scale_t;

typedef struct {
    uint32_t frames;
    uint32_t us_last;
    uint32_t us_avg;
    uint32_t us_max;
    /* Added: average split of the frame cost between indexed->RGB565 conversion
     * and SPI/DMA transfer (incl. command overhead and waiting for the DMA). */
    uint32_t us_convert;
    uint32_t us_spi;
} display_stats_t;

/* Fixed 16 colour palette used by the UI canvas */
enum {
    UI_BLACK = 0,
    UI_WHITE,
    UI_RED,
    UI_GREEN,
    UI_BLUE,
    UI_YELLOW,
    UI_CYAN,
    UI_MAGENTA,
    UI_GRAY,
    UI_DGRAY,
    UI_ORANGE,
    UI_LGREEN,
    UI_LBLUE,
    UI_LGRAY,
    UI_BROWN,
    UI_NAVY,
};

/* Initialize SPI bus + panel. Owns SPI2_HOST; the SD card attaches to the same bus. */
esp_err_t display_init(void);

void display_set_rotation(display_rotation_t rot);
display_rotation_t display_get_rotation(void);
void display_set_scale(display_scale_t scale);
display_scale_t display_get_scale(void);
int display_logical_width(void);
int display_logical_height(void);

/* Backlight, 0..100 % */
void display_set_backlight(int percent);
int display_get_backlight(void);

/* MADCTL colour-order toggle.  false (default) = RGB, true = BGR.
 * Only bit 0x08 of MADCTL is changed; the 0x40 mirror-X bit is always kept. */
void display_set_bgr(bool enable);
bool display_get_bgr(void);

/* SPI clock in MHz (10..80; default 80).  Re-creates the panel IO, so only use
 * it while nothing else drives the panel.  Returns ESP_OK or an ESP-IDF error. */
esp_err_t display_set_spi_clock(int mhz);
int display_get_spi_clock(void);

/* Fill the whole physical panel with one colour (portrait orientation) */
void display_fill(uint16_t rgb565);

/* Blit a RGB565 image into physical (portrait) coordinates.  Only for debug. */
void display_blit(int x, int y, int w, int h, const uint16_t *pixels);

/* Present one NES frame: indexed = NES_FRAME_W*NES_FRAME_H palette indices,
 * palette565 = 256 RGB565 entries.  Blocks until the frame has been handed to
 * the SPI DMA pipeline (the last band's transfer is waited for). */
void display_show_nes_frame(const uint8_t *indexed, const uint16_t *palette565);

/* UI canvas: DISPLAY_W x DISPLAY_H (landscape) or DISPLAY_H x DISPLAY_W
 * (portrait rotations) indexed canvas, committed to the panel with the current
 * rotation.  Released while the emulator runs to save RAM.  All draw calls are
 * no-ops while the canvas is released.  Drawing functions are not thread safe
 * with each other; display_ui_commit()/display_show_nes_frame() are serialized
 * internally against each other. */
esp_err_t display_ui_acquire(void);
void display_ui_release(void);
void display_ui_clear(uint8_t color);
void display_ui_pixel(int x, int y, uint8_t color);
void display_ui_rect(int x, int y, int w, int h, uint8_t color);
void display_ui_frame(int x, int y, int w, int h, uint8_t color);
void display_ui_text(int x, int y, const char *text, uint8_t fg, uint8_t bg, int scale);
int  display_ui_text_width(const char *text, int scale);
/* Dithered black->white gradient rectangle (4x4 ordered dithering of the 5
 * grey palette entries).  horizontal=true ramps left->right. */
void display_ui_shade(int x, int y, int w, int h, bool horizontal);
void display_ui_commit(void);

/* Raw canvas access for debug / host-side screenshot dumps.  The buffer is
 * display_logical_width() * display_logical_height() bytes, row stride =
 * display_logical_width().  NULL while released. */
const uint8_t *display_ui_canvas(void);
/* The 16 entry RGB565 UI palette (UI_BLACK .. UI_NAVY). */
const uint16_t *display_ui_palette(void);

void display_get_stats(display_stats_t *out);
void display_reset_stats(void);

/* Convenience: full panel test pattern (landscape, with orientation markers) */
void display_test_pattern(void);

/* Micro benchmark.  Runs the NES presentation matrix (scale x rotation x SPI
 * clock 80/60/40 MHz), measures display_ui_commit() and checks that 100
 * acquire/release cycles leave the heap unchanged.  Logs everything at INFO
 * level.  Needs ~120 KB of temporary heap; blocks for a few seconds. */
void display_benchmark(int frames);

/* Debug: render the current UI canvas through the normal rotation/transpose
 * pipeline into a freshly malloc'd physical portrait RGB565 framebuffer
 * (PANEL_W*PANEL_H = 172x320 pixels).  Caller frees it with heap_caps_free().
 * Does not touch SPI.  Returns NULL if no canvas is acquired. */
uint16_t *display_debug_render_panel(void);

#ifdef __cplusplus
}
#endif
