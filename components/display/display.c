/*
 * MiniMario - display driver for Waveshare ESP32-C6-LCD-1.47
 * ST7789 172x320 SPI panel, driven through esp_lcd, landscape 320x172.
 *
 * The panel is left in the exact portrait configuration that Waveshare's own
 * ESP-IDF demo uses (verified against their demo screenshots):
 *   MADCTL = 0x40 (mirror X, RGB order)
 *   COLMOD = 0x55 (RGB565)
 *   CASET = x + 34, RASET = y   (visible window is 172 columns wide, centred)
 *   inversion ON (IPS panel)
 * and landscape pictures are produced by transposing the frame in software.
 * That keeps the panel setup identical to a known-good configuration.
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"

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
#define LCD_SPI_FREQ (80 * 1000 * 1000)
#define LCD_GAP_X 34

#define PANEL_W 172
#define PANEL_H 320

#define CHUNK_ROWS 32
#define CHUNK_PIXELS (PANEL_W * CHUNK_ROWS)

static esp_lcd_panel_io_handle_t s_io;
static SemaphoreHandle_t s_done;
static uint16_t s_chunk[CHUNK_PIXELS] __attribute__((aligned(4)));

static display_rotation_t s_rot = DISPLAY_ROT_90;
static display_scale_t s_scale = DISPLAY_SCALE_FIT;
static int s_backlight = 80;

static uint16_t s_pal_ui[16];
static uint8_t *s_canvas;
static volatile bool s_presenting = false;

static int16_t s_srcx[320];
static int16_t s_srcy[172];
static int s_img_x0 = 0;
static int s_img_w = 320;

/* stats */
static display_stats_t s_stats;
static uint64_t s_stats_us_sum;

static bool on_color_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_done, &hp);
    return hp == pdTRUE;
}

static void lcd_cmd(uint8_t cmd)
{
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(s_io, cmd, NULL, 0));
}

static void lcd_cmd_data(uint8_t cmd, const uint8_t *data, size_t len)
{
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(s_io, cmd, data, len));
}

/* ---- backlight (PWM on GPIO22) ---- */
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

/* ---- low level panel helpers ---- */

/* Write a rectangle given in *physical portrait* coordinates (x = 0..171,
 * y = 0..319).  Data is row major, RGB565 little-endian words as expected by
 * the panel (we send native uint16_t values). */
static void panel_write(int x, int y, int w, int h, const uint16_t *data, size_t nbytes)
{
    uint16_t x0 = (uint16_t)(x + LCD_GAP_X);
    uint16_t x1 = (uint16_t)(x + w - 1 + LCD_GAP_X);
    uint16_t y0 = (uint16_t)y;
    uint16_t y1 = (uint16_t)(y + h - 1);
    uint8_t caset[4] = {x0 >> 8, x0 & 0xff, x1 >> 8, x1 & 0xff};
    uint8_t raset[4] = {y0 >> 8, y0 & 0xff, y1 >> 8, y1 & 0xff};
    lcd_cmd_data(0x2A, caset, 4);
    lcd_cmd_data(0x2B, raset, 4);
    if (s_presenting) xSemaphoreTake(s_done, portMAX_DELAY);
    esp_err_t err = esp_lcd_panel_io_tx_color(s_io, 0x2C, data, nbytes);
    if (err == ESP_OK) {
        s_presenting = true;
        xSemaphoreTake(s_done, portMAX_DELAY);
        s_presenting = false;
    } else {
        ESP_LOGE(TAG, "tx_color failed: %s", esp_err_to_name(err));
    }
}

static void panel_write_wait(void)
{
    if (s_presenting) {
        xSemaphoreTake(s_done, portMAX_DELAY);
        s_presenting = false;
    }
}

static void panel_clear(uint16_t color)
{
    for (int i = 0; i < CHUNK_PIXELS; i++) s_chunk[i] = color;
    for (int y = 0; y < PANEL_H; y += CHUNK_ROWS) {
        int rows = PANEL_H - y;
        if (rows > CHUNK_ROWS) rows = CHUNK_ROWS;
        panel_write(0, y, PANEL_W, rows, s_chunk, (size_t)PANEL_W * rows * 2);
    }
}

/* ---- init ---- */

esp_err_t display_init(void)
{
    s_done = xSemaphoreCreateBinary();

    gpio_config_t rst = {
        .pin_bit_mask = 1ULL << PIN_RST,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&rst));
    gpio_set_level(PIN_RST, 1);

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

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_LCD_CS,
        .dc_gpio_num = PIN_DC,
        .spi_mode = 0,
        .pclk_hz = LCD_SPI_FREQ,
        .trans_queue_depth = 2,
        .on_color_trans_done = on_color_done,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &s_io));

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

    lcd_cmd(0x01); /* SWRESET */
    vTaskDelay(pdMS_TO_TICKS(120));
    lcd_cmd(0x11); /* SLPOUT */
    vTaskDelay(pdMS_TO_TICKS(120));
    lcd_cmd_data(0x36, (const uint8_t[]){0x40}, 1); /* MADCTL: MX=1, RGB order */
    lcd_cmd_data(0x3A, (const uint8_t[]){0x55}, 1); /* COLMOD: RGB565 */
    lcd_cmd_data(0xB0, b0, sizeof(b0));
    lcd_cmd_data(0xB2, b2, sizeof(b2));
    lcd_cmd_data(0xB7, b7, sizeof(b7));
    lcd_cmd_data(0xBB, bb, sizeof(bb));
    lcd_cmd_data(0xC0, c0, sizeof(c0));
    lcd_cmd_data(0xC2, c2, sizeof(c2));
    lcd_cmd_data(0xC3, c3, sizeof(c3));
    lcd_cmd_data(0xC4, c4, sizeof(c4));
    lcd_cmd_data(0xC6, c6, sizeof(c6));
    lcd_cmd_data(0xD0, d0, sizeof(d0));
    lcd_cmd_data(0xE0, gamma_p, sizeof(gamma_p));
    lcd_cmd_data(0xE1, gamma_n, sizeof(gamma_n));
    lcd_cmd(0x21); /* INVON */
    lcd_cmd(0x29); /* DISPON */
    vTaskDelay(pdMS_TO_TICKS(20));

    lcd_cmd(0x2C); /* RAMWR, start streaming */
    panel_clear(0x0000);
    display_set_backlight(s_backlight);

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

    ESP_LOGI(TAG, "ST7789 up: %dx%d panel, %dMHz SPI, rot=%d scale=%d", PANEL_W, PANEL_H,
             LCD_SPI_FREQ / 1000000, s_rot, s_scale);
    return ESP_OK;
}

/* ---- rotation / scale ---- */

void display_set_rotation(display_rotation_t rot)
{
    s_rot = rot;
}

display_rotation_t display_get_rotation(void) { return s_rot; }

int display_logical_width(void) { return (s_rot == DISPLAY_ROT_90 || s_rot == DISPLAY_ROT_270) ? 320 : 172; }
int display_logical_height(void) { return (s_rot == DISPLAY_ROT_90 || s_rot == DISPLAY_ROT_270) ? 172 : 320; }

static void build_maps(display_scale_t scale)
{
    int x0, w;
    if (scale == DISPLAY_SCALE_FIT) {
        w = 210;
        x0 = (320 - w) / 2;
    } else if (scale == DISPLAY_SCALE_PIXEL) {
        w = 197;
        x0 = (320 - w) / 2;
    } else {
        w = 320;
        x0 = 0;
    }
    s_img_x0 = x0;
    s_img_w = w;
    for (int x = 0; x < 320; x++) {
        if (x < x0 || x >= x0 + w) {
            s_srcx[x] = -1;
        } else if (scale == DISPLAY_SCALE_FILL) {
            s_srcx[x] = (int16_t)((x * 256) / 320);
        } else if (scale == DISPLAY_SCALE_PIXEL) {
            s_srcx[x] = (int16_t)(((x - x0) * 224) / w);
        } else {
            s_srcx[x] = (int16_t)(((x - x0) * 256) / w);
        }
        if (s_srcx[x] > 255) s_srcx[x] = 255;
    }
    for (int y = 0; y < 172; y++) {
        s_srcy[y] = (int16_t)((y * 224) / 172);
        if (s_srcy[y] > 223) s_srcy[y] = 223;
    }
}

void display_set_scale(display_scale_t scale)
{
    if (scale >= DISPLAY_SCALE_COUNT) scale = DISPLAY_SCALE_FIT;
    s_scale = scale;
    build_maps(scale);
    /* clear out the letterbox bars once */
    if (scale != DISPLAY_SCALE_FILL) display_fill(0x0000);
}

display_scale_t display_get_scale(void) { return s_scale; }

/* ---- simple panel operations ---- */

void display_fill(uint16_t color)
{
    panel_clear(color);
}

void display_blit(int x, int y, int w, int h, const uint16_t *pixels)
{
    for (int r = 0; r < h; r += CHUNK_ROWS) {
        int rows = h - r;
        if (rows > CHUNK_ROWS) rows = CHUNK_ROWS;
        memcpy(s_chunk, pixels + (size_t)r * w, (size_t)w * rows * 2);
        panel_write(x, y + r, w, rows, s_chunk, (size_t)w * rows * 2);
    }
}

static void stats_add(uint32_t us)
{
    s_stats.frames++;
    s_stats.us_last = us;
    s_stats_us_sum += us;
    s_stats.us_avg = (uint32_t)(s_stats_us_sum / s_stats.frames);
    if (us > s_stats.us_max) s_stats.us_max = us;
}

void display_get_stats(display_stats_t *out) { *out = s_stats; }

void display_reset_stats(void)
{
    memset(&s_stats, 0, sizeof(s_stats));
    s_stats_us_sum = 0;
}

/* ---- NES frame presentation ---- */

void display_show_nes_frame(const uint8_t *indexed, const uint16_t *palette565)
{
    uint32_t t0 = (uint32_t)esp_timer_get_time();
    bool rot90 = (s_rot == DISPLAY_ROT_90);
    const uint16_t *pal = palette565;
    const int *srcx = s_srcx;
    const int *srcy = s_srcy;
    int v_start = 0, v_end = PANEL_H;
    if (s_scale != DISPLAY_SCALE_FILL) {
        /* letterboxed picture only covers the middle band */
        v_start = (PANEL_H - s_img_w) / 2;
        v_end = v_start + s_img_w;
    }

    for (int v0 = v_start; v0 < v_end; v0 += CHUNK_ROWS) {
        int rows = v_end - v0;
        if (rows > CHUNK_ROWS) rows = CHUNK_ROWS;
        uint16_t *dst = s_chunk;
        for (int r = 0; r < rows; r++) {
            int v = v0 + r;
            int xs = rot90 ? (319 - v) : v;
            int sx = (xs >= 0 && xs < 320) ? srcx[xs] : -1;
            if (sx < 0) {
                for (int u = 0; u < PANEL_W; u++) *dst++ = 0x0000;
                continue;
            }
            const uint8_t *col = indexed + sx;
            if (rot90) {
                for (int u = 0; u < PANEL_W; u++) {
                    *dst++ = pal[col[srcy[u] * 256]];
                }
            } else {
                for (int u = 0; u < PANEL_W; u++) {
                    *dst++ = pal[col[srcy[171 - u] * 256]];
                }
            }
        }
        panel_write(0, v0, PANEL_W, rows, s_chunk, (size_t)PANEL_W * rows * 2);
    }
    stats_add((uint32_t)esp_timer_get_time() - t0);
}

/* ---- UI canvas ---- */

esp_err_t display_ui_acquire(void)
{
    if (s_canvas) return ESP_OK;
    s_canvas = heap_caps_malloc((size_t)DISPLAY_W * DISPLAY_H, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!s_canvas) {
        ESP_LOGE(TAG, "not enough RAM for UI canvas (%d bytes)", DISPLAY_W * DISPLAY_H);
        return ESP_ERR_NO_MEM;
    }
    memset(s_canvas, 0, (size_t)DISPLAY_W * DISPLAY_H);
    return ESP_OK;
}

void display_ui_release(void)
{
    if (s_canvas) {
        heap_caps_free(s_canvas);
        s_canvas = NULL;
    }
}

void display_ui_clear(uint8_t color)
{
    if (s_canvas) memset(s_canvas, color, (size_t)DISPLAY_W * DISPLAY_H);
}

void display_ui_pixel(int x, int y, uint8_t color)
{
    if (!s_canvas) return;
    if ((unsigned)x >= DISPLAY_W || (unsigned)y >= DISPLAY_H) return;
    s_canvas[y * DISPLAY_W + x] = color;
}

void display_ui_rect(int x, int y, int w, int h, uint8_t color)
{
    if (!s_canvas) return;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w;
    int y1 = y + h;
    if (x1 > DISPLAY_W) x1 = DISPLAY_W;
    if (y1 > DISPLAY_H) y1 = DISPLAY_H;
    if (x0 >= x1 || y0 >= y1) return;
    for (int r = y0; r < y1; r++) memset(s_canvas + r * DISPLAY_W + x0, color, (size_t)(x1 - x0));
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
    return (int)strlen(text) * 8 * scale;
}

void display_ui_text(int x, int y, const char *text, uint8_t fg, uint8_t bg, int scale)
{
    if (!s_canvas || scale < 1) return;
    if (scale > 4) scale = 4;
    int cx = x;
    for (const char *p = text; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 32 || c > 126) c = '?';
        const char *glyph = font8x8_basic[c];
        for (int row = 0; row < 8; row++) {
            uint8_t bits = (uint8_t)glyph[row];
            for (int col = 0; col < 8; col++) {
                uint8_t color = (bits & (1u << col)) ? fg : bg;
                int px = cx + col * scale;
                int py = y + row * scale;
                if (scale == 1) {
                    display_ui_pixel(px, py, color);
                } else {
                    display_ui_rect(px, py, scale, scale, color);
                }
            }
        }
        cx += 8 * scale;
        if (cx >= DISPLAY_W) break;
    }
}

void display_ui_commit(void)
{
    if (!s_canvas) return;
    uint32_t t0 = (uint32_t)esp_timer_get_time();
    bool rot90 = (s_rot == DISPLAY_ROT_90);
    for (int v0 = 0; v0 < PANEL_H; v0 += CHUNK_ROWS) {
        int rows = PANEL_H - v0;
        if (rows > CHUNK_ROWS) rows = CHUNK_ROWS;
        uint16_t *dst = s_chunk;
        for (int r = 0; r < rows; r++) {
            int v = v0 + r;
            int xs = rot90 ? (319 - v) : v;
            const uint8_t *col = s_canvas + xs;
            if (rot90) {
                for (int u = 0; u < PANEL_W; u++) *dst++ = s_pal_ui[col[u * DISPLAY_W]];
            } else {
                for (int u = 0; u < PANEL_W; u++) *dst++ = s_pal_ui[col[(171 - u) * DISPLAY_W]];
            }
        }
        panel_write(0, v0, PANEL_W, rows, s_chunk, (size_t)PANEL_W * rows * 2);
    }
    stats_add((uint32_t)esp_timer_get_time() - t0);
}

/* ---- test pattern ---- */

void display_test_pattern(void)
{
    if (display_ui_acquire() != ESP_OK) return;
    display_ui_clear(UI_BLACK);
    /* coloured quadrants + orientation markers */
    display_ui_rect(0, 0, 160, 86, UI_RED);
    display_ui_rect(160, 0, 160, 86, UI_GREEN);
    display_ui_rect(0, 86, 160, 86, UI_BLUE);
    display_ui_rect(160, 86, 160, 86, UI_WHITE);
    display_ui_text(4, 4, "TOP-LEFT", UI_WHITE, UI_RED, 2);
    display_ui_text(324 - display_ui_text_width("TOP-RIGHT", 2) - 4, 4, "TOP-RIGHT", UI_BLACK, UI_GREEN, 2);
    display_ui_text(4, 172 - 4 - 16, "BOT-LEFT", UI_BLACK, UI_BLUE, 2);
    display_ui_text(324 - display_ui_text_width("BOT-RIGHT", 2) - 4, 172 - 4 - 16, "BOT-RIGHT", UI_BLACK, UI_WHITE, 2);
    display_ui_text(8, 60, "MiniMario", UI_YELLOW, UI_RED, 3);
    display_ui_text(8, 104, "ESP32-C6-LCD-1.47", UI_CYAN, UI_BLUE, 1);
    display_ui_commit();
}
