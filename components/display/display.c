/*
 * MiniMario - display driver for Waveshare ESP32-C6-LCD-1.47
 * ST7789 172x320 SPI panel, driven directly through the ESP-IDF SPI master
 * (DMA, queued transactions), landscape 320x172.
 *
 * The panel is left in the exact portrait configuration that Waveshare's own
 * ESP-IDF demo uses (verified against their demo screenshots):
 *   MADCTL = 0x40 (mirror X, RGB order)
 *   COLMOD = 0x55 (RGB565)
 *   CASET = x + 34, RASET = y   (visible window is 172 columns wide, centred)
 *   inversion ON (IPS panel)
 * and landscape pictures are produced by transposing the frame in software.
 * That keeps the panel setup identical to a known-good configuration.
 *
 * Presentation pipeline
 * ---------------------
 * The ST7789 keeps writing GRAM in "memory write continue" mode across SPI
 * chip-select gaps, so a whole frame is presented as:
 *
 *   1 x CASET/RASET (window covering the image)   (2 polling command groups)
 *   1 x RAMWR + first colour chunk                (kept on the bus)
 *   N-1 colour chunks queued with no command      (pure DMA)
 *
 * Two ~11 KB chunk buffers are ping-ponged: chunk N+1 is converted while chunk
 * N is being transferred; the oldest DMA result is reaped before its buffer is
 * reused.  This keeps the SPI bus busy and removes the per-chunk command
 * overhead that dominated the first implementation.
 *
 * Rotation model
 * --------------
 *   rot 90/270 : logical space is 320x172 (board held landscape).  The
 *                transposition is done with a precomputed per-panel-column
 *                table of source byte offsets, 2 pixels per 32-bit store.
 *   rot 0/180  : logical space is 172x320 (board held portrait).  The panel is
 *                direct-mapped, so presentation is a plain per-row palette
 *                expansion (no transpose) at full speed.
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "display.h"
#include "font/font8x8_basic.h"

static const char *TAG = "display";

/* ---- board wiring (Waveshare ESP32-C6-LCD-1.47) ---- */
#define PIN_SCLK 7
#define PIN_MOSI 6
#define PIN_MISO 5 /* shared with the TF card */
#define PIN_LCD_CS 14
#define PIN_DC 15
#define PIN_RST 21
#define PIN_BL 22

#define LCD_HOST SPI2_HOST

/* Default SPI clock.  The panel is known to run at 80 MHz in Waveshare's own
 * Arduino/ESP-IDF demos; display_set_spi_clock() can lower it at runtime. */
#ifndef LCD_SPI_FREQ_MHZ
#define LCD_SPI_FREQ_MHZ 80
#endif
#define LCD_SPI_MIN_MHZ 10
#define LCD_SPI_MAX_MHZ 80

#define LCD_GAP_X 34

#define PANEL_W 172 /* physical, portrait */
#define PANEL_H 320

/* DMA chunk size: 32 panel rows (~11 KB).  Two of them are ping-ponged. */
#define CHUNK_ROWS 32
#define CHUNK_PIXELS (PANEL_W * CHUNK_ROWS)
#define SPI_QUEUE_DEPTH 4

static spi_device_handle_t s_dev;
static SemaphoreHandle_t s_lock; /* recursive, serializes full panel operations */
static bool s_inited;

/* One DMA chunk buffer.  The synchronous per-band flush only needs one. */
static uint16_t s_chunk[CHUNK_PIXELS] __attribute__((aligned(4)));

static display_rotation_t s_rot = DISPLAY_ROT_90;
static display_scale_t s_scale = DISPLAY_SCALE_FIT;
static int s_backlight = 80;
static bool s_bgr = false;
static int s_spi_mhz = LCD_SPI_FREQ_MHZ;

/* ---- UI state ---- */
static uint16_t s_pal_ui[16];
static uint8_t s_canvas_mem[DISPLAY_W * DISPLAY_H] __attribute__((aligned(4)));
static uint8_t *s_canvas;          /* 8bpp, 320*172 bytes (or 172*320 when portrait) */
static int s_canvas_w = DISPLAY_W; /* logical dims of the canvas for current rotation */
static int s_canvas_h = DISPLAY_H;

/* ---- mapping tables (rebuilt on scale/rotation change) ---- */
static int16_t s_srcx[320];          /* logical x -> NES source column (-1 = letterbox) */
static int16_t s_srcy[320];          /* logical y -> NES source row    (-1 = letterbox) */
static uint32_t s_nes_off[PANEL_W];  /* landscape: source byte offset per panel column */
static uint16_t s_nes_col[PANEL_W];  /* portrait:  source column index per panel column */
static uint32_t s_ui_off[PANEL_W];   /* landscape: canvas byte offset per panel column */
static uint16_t s_ui_col[PANEL_W];   /* portrait:  canvas column index per panel column */
static int s_band_v0, s_band_v1;     /* physical rows covered by NES content */
static int s_img_x0, s_img_y0, s_img_w, s_img_h; /* game rect in logical coords */


/* ---- stats ---- */
static display_stats_t s_stats;
static uint64_t s_stats_us_sum;
static uint64_t s_stats_conv_sum;
static uint32_t s_tx_errors;
static uint32_t s_last_tx_error; /* esp_err_t of the last failed transfer */

/* ------------------------------------------------------------------------- */
/* low level panel helpers                                                   */
/* ------------------------------------------------------------------------- */

/* D/C line is driven from the transaction pre-callback: trans->user is 0 for a
 * command byte and 1 for parameter/colour data. */
static void IRAM_ATTR lcd_pre_trans(spi_transaction_t *t)
{
    gpio_set_level(PIN_DC, (uintptr_t)t->user & 1);
}

static esp_err_t lcd_dev_create(int mhz)
{
    spi_device_interface_config_t devcfg = {
        .flags = SPI_DEVICE_HALFDUPLEX,
        .clock_speed_hz = mhz * 1000 * 1000,
        .mode = 0,
        .spics_io_num = PIN_LCD_CS,
        .queue_size = SPI_QUEUE_DEPTH,
        .pre_cb = lcd_pre_trans,
    };
    return spi_bus_add_device(LCD_HOST, &devcfg, &s_dev);
}

/* Send one command byte (D/C low), CS released at the end. */
static esp_err_t lcd_cmd(uint8_t cmd)
{
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &cmd,
        .user = (void *)0,
    };
    return spi_device_polling_transmit(s_dev, &t);
}

/* Send a command byte followed by its parameters, CS kept low in between. */
static esp_err_t lcd_cmd_params(uint8_t cmd, const uint8_t *param, size_t len)
{
    uint8_t c = cmd;
    spi_transaction_t t_cmd = {
        .length = 8,
        .tx_buffer = &c,
        .user = (void *)0,
        .flags = len ? SPI_TRANS_CS_KEEP_ACTIVE : 0,
    };
    esp_err_t err = spi_device_polling_transmit(s_dev, &t_cmd);
    if (err == ESP_OK && param && len) {
        spi_transaction_t t_param = {
            .length = len * 8,
            .tx_buffer = param,
            .user = (void *)1,
        };
        err = spi_device_polling_transmit(s_dev, &t_param);
    }
    return err;
}

/* Run a command group atomically on the shared bus (SD card may use it later). */
static esp_err_t lcd_cmd_params_locked(uint8_t cmd, const uint8_t *param, size_t len)
{
    esp_err_t err = spi_device_acquire_bus(s_dev, portMAX_DELAY);
    if (err != ESP_OK) return err;
    err = lcd_cmd_params(cmd, param, len);
    spi_device_release_bus(s_dev);
    return err;
}

static void tx_err(esp_err_t err, const char *what)
{
    s_tx_errors++;
    s_last_tx_error = (uint32_t)err;
    ESP_LOGE(TAG, "%s failed: %s", what, esp_err_to_name(err));
}

/* Blit a rectangle in one go (slow, only for debug / full-panel clear). */
static esp_err_t panel_write_rect(int x, int y, int w, int h, const void *data, size_t nbytes)
{
    uint16_t x0 = (uint16_t)(x + LCD_GAP_X);
    uint16_t x1 = (uint16_t)(x + w - 1 + LCD_GAP_X);
    uint16_t y0 = (uint16_t)y;
    uint16_t y1 = (uint16_t)(y + h - 1);
    uint8_t caset[4] = {x0 >> 8, x0 & 0xff, x1 >> 8, x1 & 0xff};
    uint8_t raset[4] = {y0 >> 8, y0 & 0xff, y1 >> 8, y1 & 0xff};
    uint8_t ramwr = 0x2C;

    esp_err_t err = spi_device_acquire_bus(s_dev, portMAX_DELAY);
    if (err != ESP_OK) return err;
    err = lcd_cmd_params(0x2A, caset, 4);
    if (err == ESP_OK) err = lcd_cmd_params(0x2B, raset, 4);
    if (err == ESP_OK) {
        spi_transaction_t t_cmd = {
            .length = 8,
            .tx_buffer = &ramwr,
            .user = (void *)0,
            .flags = SPI_TRANS_CS_KEEP_ACTIVE,
        };
        err = spi_device_polling_transmit(s_dev, &t_cmd);
    }
    if (err == ESP_OK) {
        spi_transaction_t t_data = {
            .length = nbytes * 8,
            .tx_buffer = data,
            .user = (void *)1,
        };
        err = spi_device_polling_transmit(s_dev, &t_data);
    }
    spi_device_release_bus(s_dev);
    if (err != ESP_OK) tx_err(err, "write_rect");
    return err;
}

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */
/* ------------------------------------------------------------------------- */

static inline bool rot_portrait(display_rotation_t r)
{
    return r == DISPLAY_ROT_0 || r == DISPLAY_ROT_180;
}

static inline bool is_portrait(void) { return rot_portrait(s_rot); }

static void rebuild_maps(void)
{
    const bool portrait = is_portrait();

    if (!portrait) {
        /* landscape: logical 320x172, transposed onto the 172x320 panel */
        s_canvas_w = DISPLAY_W;
        s_canvas_h = DISPLAY_H;

        int w = DISPLAY_W, x0 = 0;
        switch (s_scale) {
        case DISPLAY_SCALE_FIT:   w = 210; break;
        case DISPLAY_SCALE_PIXEL: w = 197; break;
        case DISPLAY_SCALE_FILL:
        default:                  w = 320; break;
        }
        x0 = (DISPLAY_W - w) / 2;
        s_img_x0 = x0;
        s_img_y0 = 0;
        s_img_w = w;
        s_img_h = DISPLAY_H;

        for (int x = 0; x < DISPLAY_W; x++) {
            int sx;
            if (x < x0 || x >= x0 + w) {
                sx = -1;
            } else if (s_scale == DISPLAY_SCALE_FILL) {
                sx = (x * 256) / 320;
            } else if (s_scale == DISPLAY_SCALE_PIXEL) {
                sx = ((x - x0) * 224) / w;
            } else {
                sx = ((x - x0) * 256) / w;
            }
            if (sx > 255) sx = 255;
            s_srcx[x] = (int16_t)sx;
        }
        for (int y = 0; y < DISPLAY_H; y++) {
            int sy = (y * 224) / DISPLAY_H;
            if (sy > 223) sy = 223;
            s_srcy[y] = (int16_t)sy;
        }

        /* Physical row band covered by the image.  The band is centred, but
         * compute it through the actual rotation so odd widths stay exact. */
        if (s_rot == DISPLAY_ROT_270) {
            s_band_v0 = x0;
            s_band_v1 = x0 + w;
        } else { /* ROT_90 */
            s_band_v0 = PANEL_H - (x0 + w);
            s_band_v1 = PANEL_H - x0;
        }

        const bool rev = (s_rot == DISPLAY_ROT_270);
        for (int u = 0; u < PANEL_W; u++) {
            int ly = rev ? (PANEL_W - 1 - u) : u;
            s_nes_off[u] = (uint32_t)s_srcy[ly] * NES_FRAME_W;
            s_ui_off[u] = (uint32_t)ly * DISPLAY_W;
        }
    } else {
        /* portrait: logical 172x320 direct mapped; NES is shown upright and
         * letterboxed like the landscape FIT mode, but with the axes swapped. */
        s_canvas_w = DISPLAY_H;
        s_canvas_h = DISPLAY_W;

        int h = s_canvas_h, y0 = 0;
        if (s_scale != DISPLAY_SCALE_FILL) {
            h = 150; /* 172 * 224/256 -> keep the NES aspect ratio */
            y0 = (s_canvas_h - h) / 2;
        }

        for (int x = 0; x < PANEL_W; x++) {
            int sx = (x * 256) / PANEL_W;
            if (sx > 255) sx = 255;
            s_srcx[x] = (int16_t)sx;
        }
        for (int y = 0; y < s_canvas_h; y++) {
            int sy;
            if (y < y0 || y >= y0 + h) {
                sy = -1;
            } else if (s_scale == DISPLAY_SCALE_FILL) {
                sy = (y * 224) / s_canvas_h;
            } else {
                sy = ((y - y0) * 224) / h;
            }
            if (sy > 223) sy = 223;
            s_srcy[y] = (int16_t)sy;
        }

        s_img_x0 = 0;
        s_img_y0 = y0;
        s_img_w = s_canvas_w;
        s_img_h = h;

        if (s_rot == DISPLAY_ROT_0) {
            s_band_v0 = y0;
            s_band_v1 = y0 + h;
        } else { /* ROT_180 */
            s_band_v0 = PANEL_H - (y0 + h);
            s_band_v1 = PANEL_H - y0;
        }

        const bool rev = (s_rot == DISPLAY_ROT_180);
        for (int u = 0; u < PANEL_W; u++) {
            int lx = rev ? (PANEL_W - 1 - u) : u;
            s_nes_col[u] = (uint16_t)s_srcx[lx];
            s_ui_col[u] = (uint16_t)lx;
        }
    }
}

static void stats_add_frame(uint32_t total_us, uint32_t conv_us)
{
    s_stats.frames++;
    s_stats.us_last = total_us;
    s_stats_us_sum += total_us;
    s_stats_conv_sum += conv_us;
    s_stats.us_avg = (uint32_t)(s_stats_us_sum / s_stats.frames);
    s_stats.us_convert = (uint32_t)(s_stats_conv_sum / s_stats.frames);
    s_stats.us_spi = (uint32_t)((s_stats_us_sum - s_stats_conv_sum) / s_stats.frames);
    if (total_us > s_stats.us_max) s_stats.us_max = total_us;
}

/* ------------------------------------------------------------------------- */
/* backlight (PWM on GPIO22)                                                 */
/* ------------------------------------------------------------------------- */

static void backlight_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .freq_hz = 20000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    ledc_channel_config_t ch = {
        .gpio_num = PIN_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch));
}

void display_set_backlight(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s_backlight = percent;
    uint32_t duty = (uint32_t)((1023 * percent) / 100);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

int display_get_backlight(void) { return s_backlight; }

/* ------------------------------------------------------------------------- */
/* conversion kernels (IRAM so they never stall on a flash cache miss)       */
/* ------------------------------------------------------------------------- */

/* Two physical rows at once (landscape): one 32-bit store per 2 pixels and the
 * offset table load is shared by both rows. */
static IRAM_ATTR void conv_2rows(const uint8_t *base0, const uint8_t *base1,
                                 const uint16_t *pal, const uint32_t *off,
                                 uint16_t *dst0, uint16_t *dst1, int n)
{
    uint32_t *o0 = (uint32_t *)dst0;
    uint32_t *o1 = (uint32_t *)dst1;
    for (int u = 0; u < n; u += 2) {
        uint32_t a = pal[base0[off[u]]];
        uint32_t b = pal[base0[off[u + 1]]];
        uint32_t c = pal[base1[off[u]]];
        uint32_t d = pal[base1[off[u + 1]]];
        *o0++ = a | (b << 16);
        *o1++ = c | (d << 16);
    }
}

/* Single physical row (landscape fallback for an odd band height). */
static IRAM_ATTR void conv_1row(const uint8_t *base, const uint16_t *pal,
                                const uint32_t *off, uint16_t *dst, int n)
{
    uint32_t *o = (uint32_t *)dst;
    for (int u = 0; u < n; u += 2) {
        uint32_t a = pal[base[off[u]]];
        uint32_t b = pal[base[off[u + 1]]];
        *o++ = a | (b << 16);
    }
}

/* One physical row, direct mapped (portrait): each output pixel is fetched from
 * `row` through the column index table. */
static IRAM_ATTR void conv_1row_direct(const uint8_t *row, const uint16_t *pal,
                                       const uint16_t *col, uint16_t *dst, int n)
{
    uint32_t *o = (uint32_t *)dst;
    for (int u = 0; u < n; u += 2) {
        uint32_t a = pal[row[col[u]]];
        uint32_t b = pal[row[col[u + 1]]];
        *o++ = a | (b << 16);
    }
}

/* ------------------------------------------------------------------------- */
/* band converters + streaming pipeline                                      */
/* ------------------------------------------------------------------------- */

typedef void (*band_conv_fn)(void *ctx, int v, int rows, uint16_t *dst);

typedef struct {
    const uint8_t *indexed;
    const uint16_t *pal;
} nes_frame_t;

static void conv_nes_band(void *ctx, int v, int rows, uint16_t *dst)
{
    const nes_frame_t *f = (const nes_frame_t *)ctx;
    const uint8_t *indexed = f->indexed;
    const uint16_t *pal = f->pal;
    const int w = PANEL_W;

    if (!is_portrait()) {
        const bool rot90 = (s_rot == DISPLAY_ROT_90);
        int r = 0;
        for (; r + 1 < rows; r += 2) {
            int v0 = v + r;
            int v1 = v0 + 1;
            int lx0 = rot90 ? (PANEL_H - 1 - v0) : v0;
            int lx1 = rot90 ? (PANEL_H - 1 - v1) : v1;
            int sx0 = s_srcx[lx0];
            int sx1 = s_srcx[lx1];
            uint16_t *d0 = dst + (size_t)r * w;
            uint16_t *d1 = d0 + w;
            if (sx0 >= 0 && sx1 >= 0) {
                conv_2rows(indexed + sx0, indexed + sx1, pal, s_nes_off, d0, d1, w);
            } else {
                if (sx0 >= 0) conv_1row(indexed + sx0, pal, s_nes_off, d0, w);
                else memset(d0, 0, (size_t)w * 2);
                if (sx1 >= 0) conv_1row(indexed + sx1, pal, s_nes_off, d1, w);
                else memset(d1, 0, (size_t)w * 2);
            }
        }
        if (r < rows) {
            int lx = rot90 ? (PANEL_H - 1 - (v + r)) : (v + r);
            int sx = s_srcx[lx];
            uint16_t *d = dst + (size_t)r * w;
            if (sx >= 0) conv_1row(indexed + sx, pal, s_nes_off, d, w);
            else memset(d, 0, (size_t)w * 2);
        }
    } else {
        const bool rot0 = (s_rot == DISPLAY_ROT_0);
        for (int r = 0; r < rows; r++) {
            int ly = rot0 ? (v + r) : (PANEL_H - 1 - (v + r));
            int sy = s_srcy[ly];
            uint16_t *d = dst + (size_t)r * w;
            if (sy < 0) {
                memset(d, 0, (size_t)w * 2);
            } else {
                conv_1row_direct(indexed + (size_t)sy * NES_FRAME_W, pal, s_nes_col, d, w);
            }
        }
    }
}

static void conv_ui_band(void *ctx, int v, int rows, uint16_t *dst)
{
    (void)ctx;
    const int w = PANEL_W;

    if (!is_portrait()) {
        const bool rot90 = (s_rot == DISPLAY_ROT_90);
        int r = 0;
        for (; r + 1 < rows; r += 2) {
            int v0 = v + r, v1 = v0 + 1;
            int lx0 = rot90 ? (PANEL_H - 1 - v0) : v0;
            int lx1 = rot90 ? (PANEL_H - 1 - v1) : v1;
            conv_2rows(s_canvas + lx0, s_canvas + lx1, s_pal_ui, s_ui_off,
                       dst + (size_t)r * w, dst + (size_t)(r + 1) * w, w);
        }
        if (r < rows) {
            int lx = rot90 ? (PANEL_H - 1 - (v + r)) : (v + r);
            conv_1row(s_canvas + lx, s_pal_ui, s_ui_off, dst + (size_t)r * w, w);
        }
    } else {
        const bool rot0 = (s_rot == DISPLAY_ROT_0);
        for (int r = 0; r < rows; r++) {
            int ly = rot0 ? (v + r) : (PANEL_H - 1 - (v + r));
            const uint8_t *row = s_canvas + (size_t)ly * s_canvas_w;
            conv_1row_direct(row, s_pal_ui, s_ui_col, dst + (size_t)r * w, w);
        }
    }
}

/* Present `rows_total` physical rows starting at y0.  Each band gets its own
 * CASET/RASET + RAMWR sequence (the standard ST7789 write sequence); relying on
 * "memory write continue" across CS boundaries left most of the panel black in
 * the first hardware test, so we do not take that shortcut.  Returns the
 * conversion time. */
static uint32_t stream_frame(int y0, int rows_total, band_conv_fn conv, void *ctx)
{
    uint32_t conv_us = 0;
    int v = y0, end = y0 + rows_total;

    while (v < end) {
        int rows = CHUNK_ROWS;
        if (v + rows > end) rows = end - v;

        uint32_t c0 = (uint32_t)esp_timer_get_time();
        conv(ctx, v, rows, s_chunk);
        conv_us += (uint32_t)esp_timer_get_time() - c0;

        panel_write_rect(0, v, PANEL_W, rows, s_chunk, (size_t)rows * PANEL_W * 2);
        v += rows;
    }
    return conv_us;
}

/* ------------------------------------------------------------------------- */
/* NES frame presentation                                                    */
/* ------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------- */
/* letterbox bars: reuse the UI canvas outside the game image                */
/* ------------------------------------------------------------------------- */
/* While a game runs the panel is only refreshed inside the 210 px wide image,
 * so the bars beside it would keep whatever the menu left there.  The app
 * draws status text into those rectangles of the (static) UI canvas and calls
 * display_ui_commit_bars() to flush just the bar rows.  This reuses the normal
 * canvas rotation/scale mapping, so it works for every rotation. */
void display_image_rect(int *x0, int *y0, int *w, int *h)
{
    if (x0) *x0 = s_img_x0;
    if (y0) *y0 = s_img_y0;
    if (w) *w = s_img_w;
    if (h) *h = s_img_h;
}

void display_ui_commit_bars(void)
{
    if (!s_inited || !s_canvas) return;
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    if (s_scale != DISPLAY_SCALE_FILL) {
        stream_frame(0, s_band_v0, conv_ui_band, NULL);
        stream_frame(s_band_v1, PANEL_H - s_band_v1, conv_ui_band, NULL);
    }
    xSemaphoreGiveRecursive(s_lock);
}

void display_show_nes_frame(const uint8_t *indexed, const uint16_t *palette565)
{
    if (!s_inited || !indexed || !palette565) return;

    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    uint32_t t0 = (uint32_t)esp_timer_get_time();
    nes_frame_t frame = {.indexed = indexed, .pal = palette565};
    uint32_t conv_us = stream_frame(s_band_v0, s_band_v1 - s_band_v0,
                                    conv_nes_band, &frame);
    uint32_t total = (uint32_t)esp_timer_get_time() - t0;
    stats_add_frame(total, conv_us);
    xSemaphoreGiveRecursive(s_lock);
}

/* ------------------------------------------------------------------------- */
/* UI canvas                                                                 */
/* ------------------------------------------------------------------------- */

esp_err_t display_ui_acquire(void)
{
    if (s_canvas) return ESP_OK;
    /* Static DRAM buffer: the Wi-Fi/BT stacks fragment the heap enough that a
     * runtime 55 KB malloc can fail by a few hundred bytes, so the canvas lives
     * in .bss and "release" only marks it unused. */
    s_canvas = s_canvas_mem;
    memset(s_canvas, 0, (size_t)s_canvas_w * s_canvas_h);
    return ESP_OK;
}

void display_ui_release(void)
{
    s_canvas = NULL;
}

const uint8_t *display_ui_canvas(void) { return s_canvas; }

const uint16_t *display_ui_palette(void) { return s_pal_ui; }

void display_ui_clear(uint8_t color)
{
    if (s_canvas) memset(s_canvas, color, (size_t)s_canvas_w * s_canvas_h);
}

void display_ui_pixel(int x, int y, uint8_t color)
{
    if (!s_canvas) return;
    if ((unsigned)x >= (unsigned)s_canvas_w || (unsigned)y >= (unsigned)s_canvas_h) return;
    s_canvas[y * s_canvas_w + x] = color;
}

void display_ui_rect(int x, int y, int w, int h, uint8_t color)
{
    if (!s_canvas || w <= 0 || h <= 0) return;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w;
    int y1 = y + h;
    if (x1 > s_canvas_w) x1 = s_canvas_w;
    if (y1 > s_canvas_h) y1 = s_canvas_h;
    if (x0 >= x1 || y0 >= y1) return;
    for (int r = y0; r < y1; r++) {
        memset(s_canvas + (size_t)r * s_canvas_w + x0, color, (size_t)(x1 - x0));
    }
}

void display_ui_frame(int x, int y, int w, int h, uint8_t color)
{
    display_ui_rect(x, y, w, 1, color);
    display_ui_rect(x, y + h - 1, w, 1, color);
    display_ui_rect(x, y, 1, h, color);
    display_ui_rect(x + w - 1, y, 1, h, color);
}

int display_ui_text_width(const char *text, int scale)
{
    if (!text) return 0;
    if (scale < 1) scale = 1;
    return (int)strlen(text) * 8 * scale;
}

void display_ui_text(int x, int y, const char *text, uint8_t fg, uint8_t bg, int scale)
{
    if (!s_canvas || !text || scale < 1) return;
    if (scale > 4) scale = 4;

    int cx = x;
    for (const char *p = text; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 32 || c > 126) c = '?';
        const uint8_t *glyph = (const uint8_t *)font8x8_basic[c];
        for (int row = 0; row < 8; row++) {
            int py = y + row * scale;
            uint8_t bits = glyph[row];
            for (int sy = 0; sy < scale; sy++) {
                int yy = py + sy;
                if (yy < 0) continue;
                if (yy >= s_canvas_h) break;
                uint8_t *line = s_canvas + (size_t)yy * s_canvas_w;
                for (int col = 0; col < 8; col++) {
                    uint8_t color = (bits & (1u << col)) ? fg : bg;
                    int start = cx + col * scale;
                    for (int sx = 0; sx < scale; sx++) {
                        int xx = start + sx;
                        if (xx < 0) continue;
                        if (xx >= s_canvas_w) break;
                        line[xx] = color;
                    }
                }
            }
            if (py >= s_canvas_h) break;
        }
        cx += 8 * scale;
        if (cx >= s_canvas_w) break;
    }
}

/* 4x4 ordered dithering between the five grey palette entries. */
void display_ui_shade(int x, int y, int w, int h, bool horizontal)
{
    if (!s_canvas || w <= 0 || h <= 0) return;
    static const uint8_t levels[5] = {UI_BLACK, UI_DGRAY, UI_GRAY, UI_LGRAY, UI_WHITE};
    static const uint8_t bayer[4][4] = {
        {0, 8, 2, 10},
        {12, 4, 14, 6},
        {3, 11, 1, 9},
        {15, 7, 13, 5},
    };
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w;
    int y1 = y + h;
    if (x1 > s_canvas_w) x1 = s_canvas_w;
    if (y1 > s_canvas_h) y1 = s_canvas_h;
    if (x0 >= x1 || y0 >= y1) return;

    int span = (horizontal ? w : h) - 1;
    if (span < 1) span = 1;
    for (int py = y0; py < y1; py++) {
        uint8_t *row = s_canvas + (size_t)py * s_canvas_w;
        for (int px = x0; px < x1; px++) {
            int t = horizontal ? (px - x) : (py - y);
            if (t < 0) t = 0;
            if (t > span) t = span;
            int scaled = t * 4 * 255 / span; /* 0 .. 1020 */
            int idx = scaled / 255;
            int rem = scaled - idx * 255;
            if (idx > 3) idx = 3;
            int thr = (int)bayer[py & 3][px & 3] * 255 / 16;
            row[px] = levels[(rem > thr) ? (idx + 1) : idx];
        }
    }
}

void display_ui_commit(void)
{
    if (!s_canvas || !s_inited) return;

    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    uint32_t t0 = (uint32_t)esp_timer_get_time();
    uint32_t conv_us = stream_frame(0, PANEL_H, conv_ui_band, NULL);
    uint32_t total = (uint32_t)esp_timer_get_time() - t0;
    stats_add_frame(total, conv_us);
    xSemaphoreGiveRecursive(s_lock);
}

/* ------------------------------------------------------------------------- */
/* plain panel operations                                                    */
/* ------------------------------------------------------------------------- */

void display_fill(uint16_t color)
{
    if (!s_inited) return;
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    for (int i = 0; i < CHUNK_PIXELS; i++) s_chunk[i] = color;
    for (int y = 0; y < PANEL_H; y += CHUNK_ROWS) {
        int rows = PANEL_H - y;
        if (rows > CHUNK_ROWS) rows = CHUNK_ROWS;
        panel_write_rect(0, y, PANEL_W, rows, s_chunk, (size_t)rows * PANEL_W * 2);
    }
    xSemaphoreGiveRecursive(s_lock);
}

void display_blit(int x, int y, int w, int h, const uint16_t *pixels)
{
    if (!s_inited || !pixels || w <= 0 || h <= 0) return;
    if (x < 0 || y < 0 || x + w > PANEL_W || y + h > PANEL_H) return;
    int rows_per_chunk = CHUNK_PIXELS / w;
    if (rows_per_chunk < 1) return;
    if (rows_per_chunk > CHUNK_ROWS) rows_per_chunk = CHUNK_ROWS;

    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    for (int r = 0; r < h; r += rows_per_chunk) {
        int rows = h - r;
        if (rows > rows_per_chunk) rows = rows_per_chunk;
        size_t bytes = (size_t)w * rows * 2;
        memcpy(s_chunk, pixels + (size_t)r * w, bytes);
        panel_write_rect(x, y + r, w, rows, s_chunk, bytes);
    }
    xSemaphoreGiveRecursive(s_lock);
}

void display_set_rotation(display_rotation_t rot)
{
    if (rot > DISPLAY_ROT_270) rot = DISPLAY_ROT_90;
    if (!s_lock) { s_rot = rot; return; }
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    if (s_rot != rot) {
        s_rot = rot;
        rebuild_maps();
        if (s_canvas) memset(s_canvas, 0, (size_t)s_canvas_w * s_canvas_h);
        if (s_inited) display_fill(0x0000);
        ESP_LOGI(TAG, "rotation -> %d (%dx%d logical)", (int)rot, s_canvas_w, s_canvas_h);
    } else {
        rebuild_maps();
    }
    xSemaphoreGiveRecursive(s_lock);
}

display_rotation_t display_get_rotation(void) { return s_rot; }

int display_logical_width(void) { return is_portrait() ? DISPLAY_H : DISPLAY_W; }
int display_logical_height(void) { return is_portrait() ? DISPLAY_W : DISPLAY_H; }

void display_set_scale(display_scale_t scale)
{
    if (scale >= DISPLAY_SCALE_COUNT) scale = DISPLAY_SCALE_FIT;
    if (!s_lock) { s_scale = scale; return; }
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    bool changed = (s_scale != scale);
    s_scale = scale;
    rebuild_maps();
    if (s_inited && changed && scale != DISPLAY_SCALE_FILL) {
        /* wipe the letterbox bars once */
        display_fill(0x0000);
    }
    xSemaphoreGiveRecursive(s_lock);
}

display_scale_t display_get_scale(void) { return s_scale; }

/* ------------------------------------------------------------------------- */
/* colour order / SPI clock                                                  */
/* ------------------------------------------------------------------------- */

static void apply_madctl(void)
{
    uint8_t madctl = (uint8_t)(0x40 | (s_bgr ? 0x08 : 0x00));
    if (s_dev) lcd_cmd_params_locked(0x36, &madctl, 1);
}

void display_set_bgr(bool enable)
{
    s_bgr = enable;
    if (s_lock && s_dev) {
        xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
        apply_madctl();
        xSemaphoreGiveRecursive(s_lock);
    }
    ESP_LOGI(TAG, "colour order -> %s (MADCTL=0x%02x)", s_bgr ? "BGR" : "RGB",
             (unsigned)(0x40 | (s_bgr ? 0x08 : 0x00)));
}

bool display_get_bgr(void) { return s_bgr; }

esp_err_t display_set_spi_clock(int mhz)
{
    if (mhz < LCD_SPI_MIN_MHZ || mhz > LCD_SPI_MAX_MHZ) return ESP_ERR_INVALID_ARG;
    if (!s_lock) { s_spi_mhz = mhz; return ESP_OK; }

    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (s_inited) {
        if (s_dev) {
            err = spi_bus_remove_device(s_dev);
            s_dev = NULL;
        }
        if (err == ESP_OK) {
            err = lcd_dev_create(mhz);
            if (err == ESP_OK) {
                s_spi_mhz = mhz;
                ESP_LOGI(TAG, "SPI clock -> %d MHz", mhz);
            } else {
                ESP_LOGE(TAG, "re-creating SPI device at %d MHz failed: %s", mhz,
                         esp_err_to_name(err));
            }
        }
    } else {
        s_spi_mhz = mhz;
    }
    xSemaphoreGiveRecursive(s_lock);
    return err;
}

int display_get_spi_clock(void) { return s_spi_mhz; }

/* ------------------------------------------------------------------------- */
/* init                                                                      */
/* ------------------------------------------------------------------------- */

esp_err_t display_init(void)
{
    if (s_inited) return ESP_OK;

    s_lock = xSemaphoreCreateRecursiveMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    gpio_config_t rst = {
        .pin_bit_mask = 1ULL << PIN_RST,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&rst));
    gpio_set_level(PIN_RST, 1);

    gpio_config_t dc = {
        .pin_bit_mask = 1ULL << PIN_DC,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&dc));
    gpio_set_level(PIN_DC, 1);

    backlight_init();

    spi_bus_config_t bus = {
        .sclk_io_num = PIN_SCLK,
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = CHUNK_PIXELS * 2,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));
    ESP_ERROR_CHECK(lcd_dev_create(s_spi_mhz));

    /* hardware reset */
    gpio_set_level(PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    static const uint8_t b0[] = {0x00, 0xE8};
    static const uint8_t b2[] = {0x0C, 0x0C, 0x00, 0x33, 0x33};
    static const uint8_t b7[] = {0x75};
    static const uint8_t bb[] = {0x1A};
    static const uint8_t c0[] = {0x80};
    static const uint8_t c2[] = {0x01, 0xFF};
    static const uint8_t c3[] = {0x13};
    static const uint8_t c4[] = {0x20};
    static const uint8_t c6[] = {0x0F};
    static const uint8_t d0[] = {0xA4, 0xA1};
    static const uint8_t gamma_p[] = {0xD0, 0x0D, 0x14, 0x0D, 0x0D, 0x09, 0x38, 0x44,
                                      0x4E, 0x3A, 0x17, 0x18, 0x2F, 0x30};
    static const uint8_t gamma_n[] = {0xD0, 0x09, 0x0F, 0x08, 0x07, 0x14, 0x37, 0x44,
                                      0x4D, 0x38, 0x15, 0x16, 0x2C, 0x2E};

    ESP_ERROR_CHECK(lcd_cmd(0x01)); /* SWRESET */
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_ERROR_CHECK(lcd_cmd(0x11)); /* SLPOUT */
    vTaskDelay(pdMS_TO_TICKS(120));
    apply_madctl();
    ESP_ERROR_CHECK(lcd_cmd_params_locked(0x3A, (const uint8_t[]){0x55}, 1)); /* COLMOD RGB565 */
    ESP_ERROR_CHECK(lcd_cmd_params_locked(0xB0, b0, sizeof(b0)));
    ESP_ERROR_CHECK(lcd_cmd_params_locked(0xB2, b2, sizeof(b2)));
    ESP_ERROR_CHECK(lcd_cmd_params_locked(0xB7, b7, sizeof(b7)));
    ESP_ERROR_CHECK(lcd_cmd_params_locked(0xBB, bb, sizeof(bb)));
    ESP_ERROR_CHECK(lcd_cmd_params_locked(0xC0, c0, sizeof(c0)));
    ESP_ERROR_CHECK(lcd_cmd_params_locked(0xC2, c2, sizeof(c2)));
    ESP_ERROR_CHECK(lcd_cmd_params_locked(0xC3, c3, sizeof(c3)));
    ESP_ERROR_CHECK(lcd_cmd_params_locked(0xC4, c4, sizeof(c4)));
    ESP_ERROR_CHECK(lcd_cmd_params_locked(0xC6, c6, sizeof(c6)));
    ESP_ERROR_CHECK(lcd_cmd_params_locked(0xD0, d0, sizeof(d0)));
    ESP_ERROR_CHECK(lcd_cmd_params_locked(0xE0, gamma_p, sizeof(gamma_p)));
    ESP_ERROR_CHECK(lcd_cmd_params_locked(0xE1, gamma_n, sizeof(gamma_n)));
    ESP_ERROR_CHECK(lcd_cmd(0x21)); /* INVON */
    ESP_ERROR_CHECK(lcd_cmd(0x29)); /* DISPON */
    vTaskDelay(pdMS_TO_TICKS(20));

    static const uint16_t ui_pal[16] = {
        0x0000, /* black */
        0xFFFF, /* white */
        0xF800, /* red */
        0x07E0, /* green */
        0x001F, /* blue */
        0xFFE0, /* yellow */
        0x07FF, /* cyan */
        0xF81F, /* magenta */
        0x8410, /* gray */
        0x4208, /* dark gray */
        0xFD20, /* orange */
        0x87F0, /* light green */
        0x5D7F, /* light blue */
        0xC618, /* light gray */
        0xA145, /* brown */
        0x0010, /* navy */
    };
    memcpy(s_pal_ui, ui_pal, sizeof(ui_pal));

    rebuild_maps();
    s_inited = true;
    display_fill(0x0000);
    display_set_backlight(s_backlight);

    ESP_LOGI(TAG, "ST7789 up: %dx%d panel, %dMHz SPI, %s, rot=%d scale=%d chunk=%dB x2",
             PANEL_W, PANEL_H, s_spi_mhz, s_bgr ? "BGR" : "RGB", (int)s_rot, (int)s_scale,
             CHUNK_PIXELS * 2);
    return ESP_OK;
}

/* ------------------------------------------------------------------------- */
/* stats                                                                     */
/* ------------------------------------------------------------------------- */

void display_get_stats(display_stats_t *out)
{
    if (out) *out = s_stats;
}

void display_reset_stats(void)
{
    memset(&s_stats, 0, sizeof(s_stats));
    s_stats_us_sum = 0;
    s_stats_conv_sum = 0;
}

/* ------------------------------------------------------------------------- */
/* calibration / test pattern                                                */
/* ------------------------------------------------------------------------- */

static void draw_calibration_landscape(void)
{
    /* 320x172 landscape layout */
    display_ui_rect(0, 0, 160, 82, UI_RED);
    display_ui_rect(160, 0, 160, 82, UI_GREEN);
    display_ui_rect(0, 90, 160, 70, UI_BLUE);
    display_ui_rect(160, 90, 160, 70, UI_WHITE);

    display_ui_text(4, 4, "TOP-LEFT", UI_WHITE, UI_RED, 2);
    display_ui_text(DISPLAY_W - 4 - display_ui_text_width("TOP-RIGHT", 2), 4,
                    "TOP-RIGHT", UI_BLACK, UI_GREEN, 2);
    display_ui_text(4, 96, "BOTTOM", UI_WHITE, UI_BLUE, 2);
    display_ui_text(4, 120, "LEFT", UI_WHITE, UI_BLUE, 2);
    display_ui_text(164, 96, "BOTTOM", UI_BLACK, UI_WHITE, 2);
    display_ui_text(164, 120, "RIGHT", UI_BLACK, UI_WHITE, 2);

    /* title */
    display_ui_rect(86, 52, 148, 20, UI_BLACK);
    display_ui_text(88, 54, "MiniMario", UI_YELLOW, UI_BLACK, 2);
    display_ui_text(4, 148, "ESP32-C6-LCD-1.47", UI_LGRAY, UI_BLUE, 1);

    /* 16 colour swatch bar */
    for (int i = 0; i < 16; i++) {
        display_ui_rect(i * 20, 82, 20, 8, (uint8_t)i);
    }
    /* greyscale gradient */
    display_ui_shade(0, 160, DISPLAY_W, 12, true);
}

static void draw_calibration_portrait(void)
{
    /* 172x320 portrait layout */
    const int lw = display_logical_width(); /* 172 */
    const int qw = lw / 2;                  /* 86  */
    display_ui_rect(0, 0, qw, 160, UI_RED);
    display_ui_rect(qw, 0, lw - qw, 160, UI_GREEN);
    display_ui_rect(0, 168, qw, 140, UI_BLUE);
    display_ui_rect(qw, 168, lw - qw, 140, UI_WHITE);

    display_ui_text(4, 4, "TOP-", UI_WHITE, UI_RED, 1);
    display_ui_text(4, 14, "LEFT", UI_WHITE, UI_RED, 1);
    display_ui_text(qw + 4, 4, "TOP-", UI_BLACK, UI_GREEN, 1);
    display_ui_text(qw + 4, 14, "RIGHT", UI_BLACK, UI_GREEN, 1);
    display_ui_text(4, 172, "BOTTOM", UI_WHITE, UI_BLUE, 1);
    display_ui_text(4, 182, "LEFT", UI_WHITE, UI_BLUE, 1);
    display_ui_text(qw + 4, 172, "BOTTOM", UI_BLACK, UI_WHITE, 1);
    display_ui_text(qw + 4, 182, "RIGHT", UI_BLACK, UI_WHITE, 1);

    /* title */
    display_ui_rect(12, 68, lw - 24, 20, UI_BLACK);
    display_ui_text(14, 70, "MiniMario", UI_YELLOW, UI_BLACK, 2);
    display_ui_text(4, 140, "ESP32-C6", UI_LGRAY, UI_BLUE, 1);

    /* 16 colour swatch bar (10 px each = 160 px, centred) */
    int sw_x = (lw - 16 * 10) / 2;
    for (int i = 0; i < 16; i++) {
        display_ui_rect(sw_x + i * 10, 160, 10, 8, (uint8_t)i);
    }
    /* greyscale gradient */
    display_ui_shade(0, 308, lw, 12, true);
}

void display_test_pattern(void)
{
    if (display_ui_acquire() != ESP_OK) return;
    display_ui_clear(UI_BLACK);
    if (is_portrait()) {
        draw_calibration_portrait();
    } else {
        draw_calibration_landscape();
    }
    display_ui_commit();
}

/* ------------------------------------------------------------------------- */
/* debug render (no SPI)                                                     */
/* ------------------------------------------------------------------------- */

uint16_t *display_debug_render_panel(void)
{
    if (!s_canvas) return NULL;
    size_t bytes = (size_t)PANEL_W * PANEL_H * 2;
    uint16_t *fb = heap_caps_malloc(bytes, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!fb) return NULL;
    memset(fb, 0, bytes);

    if (!is_portrait()) {
        const bool rot90 = (s_rot == DISPLAY_ROT_90);
        for (int v = 0; v < PANEL_H; v++) {
            int lx = rot90 ? (PANEL_H - 1 - v) : v;
            if (lx < 0 || lx >= s_canvas_w) continue;
            const uint8_t *base = s_canvas + lx;
            uint16_t *dst = fb + (size_t)v * PANEL_W;
            for (int u = 0; u < PANEL_W; u++) {
                dst[u] = s_pal_ui[base[s_ui_off[u]]];
            }
        }
    } else {
        const bool rot0 = (s_rot == DISPLAY_ROT_0);
        for (int v = 0; v < PANEL_H; v++) {
            int ly = rot0 ? v : (PANEL_H - 1 - v);
            const uint8_t *row = s_canvas + (size_t)ly * s_canvas_w;
            uint16_t *dst = fb + (size_t)v * PANEL_W;
            for (int u = 0; u < PANEL_W; u++) {
                dst[u] = s_pal_ui[row[s_ui_col[u]]];
            }
        }
    }
    return fb;
}

/* ------------------------------------------------------------------------- */
/* benchmark                                                                 */
/* ------------------------------------------------------------------------- */

static const char *scale_name(display_scale_t s)
{
    switch (s) {
    case DISPLAY_SCALE_FIT:   return "FIT";
    case DISPLAY_SCALE_FILL:  return "FILL";
    case DISPLAY_SCALE_PIXEL: return "PIXEL";
    default:                  return "?";
    }
}

static void bench_fill_frame(uint8_t *idx, uint16_t *pal)
{
    for (int y = 0; y < NES_FRAME_H; y++) {
        uint8_t *row = idx + (size_t)y * NES_FRAME_W;
        for (int x = 0; x < NES_FRAME_W; x++) {
            row[x] = (uint8_t)(((x >> 3) * 13 + (y >> 3) * 7 + ((x * y) >> 10)) & 0xFF);
        }
    }
    for (int i = 0; i < 256; i++) {
        uint16_t r = (uint16_t)((i * 31) & 0x1F);
        uint16_t g = (uint16_t)((i * 63) & 0x3F);
        uint16_t b = (uint16_t)((i * 17) & 0x1F);
        pal[i] = (uint16_t)((r << 11) | (g << 5) | b);
    }
}

static void bench_nes(const uint8_t *idx, const uint16_t *pal, int frames)
{
    display_reset_stats();
    for (int i = 0; i < frames; i++) display_show_nes_frame(idx, pal);
    display_stats_t st;
    display_get_stats(&st);
    int rows = s_band_v1 - s_band_v0;
    uint32_t bytes = (uint32_t)rows * PANEL_W * 2;
    /* nominal bits / MHz = us; the C6 SPI source clock is 80 MHz, so a 60 MHz
     * request silently falls back to 40 MHz (flagged by "SLOW"). */
    uint32_t ideal_us = bytes * 8 / (uint32_t)s_spi_mhz;
    const char *flag;
    if (s_tx_errors) flag = "TX-ERRORS";
    else if (st.us_spi > (uint32_t)((uint64_t)ideal_us * 112 / 100)) flag = "SLOW";
    else flag = "ok";
    ESP_LOGI(TAG,
             "bench NES rot=%d %-5s %2dMHz: total=%5luus convert=%4luus spi=%5luus "
             "last=%5luus max=%5luus bytes=%6lu ideal=%5luus %s",
             (int)s_rot, scale_name(s_scale), s_spi_mhz, (unsigned long)st.us_avg,
             (unsigned long)st.us_convert, (unsigned long)st.us_spi, (unsigned long)st.us_last,
             (unsigned long)st.us_max, (unsigned long)bytes, (unsigned long)ideal_us, flag);
}

static void bench_ui(int frames)
{
    if (!s_canvas) return;
    /* cheap synthetic UI content (high frequency, cache unfriendly) */
    for (int i = 0; i < s_canvas_w * s_canvas_h; i++) s_canvas[i] = (uint8_t)((i * 7) >> 5);
    display_reset_stats();
    for (int i = 0; i < frames; i++) display_ui_commit();
    display_stats_t st;
    display_get_stats(&st);
    ESP_LOGI(TAG, "bench UI  rot=%d          : total=%5luus convert=%4luus spi=%5luus max=%5luus",
             (int)s_rot, (unsigned long)st.us_avg, (unsigned long)st.us_convert,
             (unsigned long)st.us_spi, (unsigned long)st.us_max);
}

void display_benchmark(int frames)
{
    if (frames <= 0) frames = 20;

    display_rotation_t save_rot = s_rot;
    display_scale_t save_scale = s_scale;
    int save_mhz = s_spi_mhz;
    bool save_bgr = s_bgr;

    uint8_t *idx = heap_caps_malloc((size_t)NES_FRAME_W * NES_FRAME_H, MALLOC_CAP_8BIT);
    uint16_t *pal = heap_caps_malloc(256 * sizeof(uint16_t), MALLOC_CAP_8BIT);
    if (!idx || !pal) {
        ESP_LOGE(TAG, "benchmark: out of memory (idx=%p pal=%p)", idx, pal);
        if (idx) heap_caps_free(idx);
        if (pal) heap_caps_free(pal);
        return;
    }
    bench_fill_frame(idx, pal);

    bool had_canvas = (s_canvas != NULL);
    uint32_t heap0 = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "=== display benchmark: %d frames/config, heap=%lu, MADCTL=%s, canvas=%s ===",
             frames, (unsigned long)heap0, s_bgr ? "BGR" : "RGB",
             had_canvas ? "held" : "released");
    ESP_LOGI(TAG, "chunk: %d B, %d rows per band; C6 SPI source clock is 80MHz "
                  "(60MHz requests fall back to 40MHz)", CHUNK_PIXELS * 2, CHUNK_ROWS);

    static const int clocks[] = {80, 60, 40};
    static const display_rotation_t lands[] = {DISPLAY_ROT_90, DISPLAY_ROT_270};
    static const display_scale_t scales[] = {DISPLAY_SCALE_FIT, DISPLAY_SCALE_FILL,
                                             DISPLAY_SCALE_PIXEL};

    for (size_t ci = 0; ci < sizeof(clocks) / sizeof(clocks[0]); ci++) {
        if (display_set_spi_clock(clocks[ci]) != ESP_OK) {
            ESP_LOGE(TAG, "cannot set %d MHz, skipping", clocks[ci]);
            continue;
        }
        s_tx_errors = 0;
        for (size_t ri = 0; ri < sizeof(lands) / sizeof(lands[0]); ri++) {
            display_set_rotation(lands[ri]);
            for (size_t si = 0; si < sizeof(scales) / sizeof(scales[0]); si++) {
                display_set_scale(scales[si]);
                bench_nes(idx, pal, frames);
            }
        }
        ESP_LOGI(TAG, "bench %2dMHz: tx errors=%lu", clocks[ci],
                 (unsigned long)s_tx_errors);
    }

    /* portrait mount at 80 MHz */
    display_set_spi_clock(80);
    for (int r = 0; r <= 1; r++) {
        display_set_rotation(r == 0 ? DISPLAY_ROT_0 : DISPLAY_ROT_180);
        for (size_t si = 0; si < sizeof(scales) / sizeof(scales[0]); si++) {
            display_set_scale(scales[si]);
            bench_nes(idx, pal, frames);
        }
    }

    /* UI canvas commit benchmark */
    bool acquired_here = false;
    if (!s_canvas) {
        if (display_ui_acquire() == ESP_OK) acquired_here = true;
    }
    if (s_canvas) {
        static const display_rotation_t all_rots[] = {DISPLAY_ROT_90, DISPLAY_ROT_270,
                                                      DISPLAY_ROT_0, DISPLAY_ROT_180};
        for (size_t ri = 0; ri < sizeof(all_rots) / sizeof(all_rots[0]); ri++) {
            if (s_rot != all_rots[ri]) display_set_rotation(all_rots[ri]);
            bench_ui(frames);
        }
    }

    /* heap balance of acquire/release */
    display_ui_release();
    uint32_t h_before = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    int cycles = 100, fails = 0;
    for (int i = 0; i < cycles; i++) {
        if (display_ui_acquire() != ESP_OK) {
            fails++;
            break;
        }
        display_ui_release();
    }
    uint32_t h_after = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "canvas acquire/release x%d: heap %lu -> %lu (delta %+ld), fails=%d",
             cycles, (unsigned long)h_before, (unsigned long)h_after,
             (long)((int32_t)h_after - (int32_t)h_before), fails);

    if (had_canvas) display_ui_acquire();

    /* restore caller state */
    display_set_spi_clock(save_mhz);
    display_set_rotation(save_rot);
    display_set_scale(save_scale);
    display_set_bgr(save_bgr);
    if (s_inited) display_fill(0x0000); /* benchmark leaves the panel black */
    display_reset_stats();
    if (acquired_here || !had_canvas) display_ui_release();

    ESP_LOGI(TAG, "=== display benchmark done: heap=%lu, tx errors=%lu (last=%s) ===",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned long)s_tx_errors, esp_err_to_name((esp_err_t)s_last_tx_error));
    heap_caps_free(idx);
    heap_caps_free(pal);
}
