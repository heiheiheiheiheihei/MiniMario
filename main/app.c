/*
 * MiniMario - application shell.
 *
 * Boot:
 *   NVS -> display init (rotation/scale/backlight/bgr from NVS) -> UI canvas
 *   -> BOOT button / gamepad -> ROM store (TF card + embedded partition)
 *   -> menu
 *
 * Starting a ROM:
 *   release the 55 KB UI canvas, load/map the ROM, nes_port_set_rom(),
 *   frame sink = display_show_nes_frame(), input source = gamepad + quit
 *   combos, then block in nes_port_run().  On return: unload the ROM,
 *   re-acquire the canvas and redraw the menu.
 */
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

#include "display.h"
#include "gamepad.h"
#include "nes_port.h"
#include "phone_pad.h"
#include "pad.h"
#include "minimario.h"
#include "app.h"
#include "console.h"
#include "rom_store.h"
#include "ui_menu.h"

static const char *TAG = "app";

/* BOOT button on the Waveshare board (active low). */
#define BOOT_BTN_GPIO GPIO_NUM_9

#define NVS_NAMESPACE "mm"

/* Requests from the console task. */
#define PLAY_NONE (-2)

typedef struct {
    int play_index; /* PLAY_NONE, -1 = current selection, or a ROM index */
    bool play_is_path;
    char play_path[128];
    bool rescan;
    bool test;
    bool clear;
} requests_t;

static SemaphoreHandle_t s_req_lock;
static requests_t s_req;

static volatile app_mode_t s_mode = APP_MODE_BOOT;
static volatile bool s_emulating;
static volatile uint32_t s_rescan_seq;
static char s_last_error[96];

/* NVS-persisted display settings. */
static int s_rot = DISPLAY_ROT_90;
static int s_scale = DISPLAY_SCALE_FIT;
static int s_bl = 80;
static int s_wpad;      /* phone controller over Wi-Fi */
static int s_wpad_wpa2; /* phone pad: 1 = WPA2-PSK, 0 = open network */
/* The 1.47" panel is wired BGR (Waveshare's own reference config uses
 * TFT_RGB_ORDER=TFT_BGR), so the ST7789 colour-order bit must be set by
 * default: with it cleared red and blue come out swapped. */
static int s_bgr = 1;
/* RGB565 byte order on the wire (0 = native word order, 1 = high byte first). */
static int s_wswap = 0;

static int64_t s_hold_ui_until; /* `test` keeps the test pattern on screen */
static int64_t s_menu_ignore_pad_until; /* ignore held buttons right after quitting a game */

/* ------------------------------------------------------------------ */
/* NVS settings                                                        */
/* ------------------------------------------------------------------ */

#define SETTINGS_VERSION 2

static void settings_load(void)
{
    nvs_handle_t h;
    int8_t v;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_get_i8(h, "rot", &v) == ESP_OK && v >= 0 && v <= 3) s_rot = v;
        if (nvs_get_i8(h, "scale", &v) == ESP_OK && v >= 0 && v <= 2) s_scale = v;
        if (nvs_get_i8(h, "bl", &v) == ESP_OK && v >= 0 && v <= 100) s_bl = v;
        if (nvs_get_i8(h, "bgr", &v) == ESP_OK && v >= 0 && v <= 1) s_bgr = v;
        if (nvs_get_i8(h, "wpad", &v) == ESP_OK && (v == 0 || v == 1)) s_wpad = v;
        if (nvs_get_i8(h, "wpauth", &v) == ESP_OK && (v == 0 || v == 1)) s_wpad_wpa2 = v;
        if (nvs_get_i8(h, "wswap", &v) == ESP_OK && (v == 0 || v == 1)) s_wswap = v;

        /* Version 1 firmware stored bgr=0, which produced swapped red/blue on
         * this board; migrate existing NVS contents to the new default. */
        int8_t ver = 0;
        if (nvs_get_i8(h, "cfgver", &ver) != ESP_OK || ver < SETTINGS_VERSION) {
            s_bgr = 1;
            nvs_set_i8(h, "bgr", 1);
            nvs_set_i8(h, "cfgver", SETTINGS_VERSION);
            nvs_commit(h);
            ESP_LOGI(TAG, "settings migration: colour order -> BGR");
        }
        nvs_close(h);
    }
    ESP_LOGI(TAG, "settings: rot=%d scale=%d bl=%d%% bgr=%d wpad=%d wpauth=%s", s_rot, s_scale,
             s_bl, s_bgr, s_wpad, s_wpad_wpa2 ? "WPA2" : "open");
}

static void settings_store(const char *key, int value)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs open failed, setting %s not persisted", key);
        return;
    }
    nvs_set_i8(h, key, (int8_t)value);
    nvs_commit(h);
    nvs_close(h);
}

/* ------------------------------------------------------------------ */
/* requests                                                            */
/* ------------------------------------------------------------------ */

static void requests_init(void)
{
    s_req_lock = xSemaphoreCreateMutex();
    memset(&s_req, 0, sizeof(s_req));
    s_req.play_index = PLAY_NONE;
}

static bool req_take(requests_t *out)
{
    bool any = false;
    xSemaphoreTake(s_req_lock, portMAX_DELAY);
    *out = s_req;
    if (out->play_index != PLAY_NONE || out->rescan || out->test || out->clear) any = true;
    memset(&s_req, 0, sizeof(s_req));
    s_req.play_index = PLAY_NONE;
    xSemaphoreGive(s_req_lock);
    return any;
}

static void set_last_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void set_last_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_last_error, sizeof(s_last_error), fmt, ap);
    va_end(ap);
}

const char *app_last_error(void)
{
    return s_last_error;
}

bool app_request_play(int index)
{
    if (s_mode == APP_MODE_EMU) return false;
    int count = rom_store_count();
    if (index < 0) index = ui_menu_selected();
    if (count <= 0 || index >= count) return false;
    xSemaphoreTake(s_req_lock, portMAX_DELAY);
    s_req.play_index = index;
    s_req.play_is_path = false;
    xSemaphoreGive(s_req_lock);
    return true;
}

bool app_request_play_path(const char *path)
{
    if (s_mode == APP_MODE_EMU || !path || !*path) return false;
    xSemaphoreTake(s_req_lock, portMAX_DELAY);
    s_req.play_index = 0;
    s_req.play_is_path = true;
    snprintf(s_req.play_path, sizeof(s_req.play_path), "%s", path);
    xSemaphoreGive(s_req_lock);
    return true;
}

bool app_request_menu(void)
{
    if (s_mode == APP_MODE_EMU) {
        nes_port_request_quit();
        return true;
    }
    ui_menu_refresh();
    return true;
}

bool app_request_rescan(void)
{
    if (s_mode == APP_MODE_EMU) return false;
    xSemaphoreTake(s_req_lock, portMAX_DELAY);
    s_req.rescan = true;
    xSemaphoreGive(s_req_lock);
    return true;
}

bool app_request_test_pattern(void)
{
    if (s_mode == APP_MODE_EMU) return false;
    xSemaphoreTake(s_req_lock, portMAX_DELAY);
    s_req.test = true;
    xSemaphoreGive(s_req_lock);
    return true;
}

bool app_request_clear(void)
{
    if (s_mode == APP_MODE_EMU) return false;
    xSemaphoreTake(s_req_lock, portMAX_DELAY);
    s_req.clear = true;
    xSemaphoreGive(s_req_lock);
    return true;
}

uint32_t app_rescan_seq(void) { return s_rescan_seq; }

app_mode_t app_get_mode(void) { return s_mode; }
bool app_is_emulating(void) { return s_emulating; }

/* ------------------------------------------------------------------ */
/* display settings                                                    */
/* ------------------------------------------------------------------ */

void app_set_rotation(int rot)
{
    if (rot < 0 || rot > 3) return;
    s_rot = rot;
    display_set_rotation((display_rotation_t)rot);
    settings_store("rot", rot);
    ui_menu_refresh();
}

void app_set_scale(int scale)
{
    if (scale < 0 || scale >= DISPLAY_SCALE_COUNT) return;
    s_scale = scale;
    display_set_scale((display_scale_t)scale);
    settings_store("scale", scale);
    ui_menu_refresh();
}

void app_set_backlight(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    s_bl = pct;
    display_set_backlight(pct);
    settings_store("bl", pct);
}

void app_set_bgr(int enable)
{
    s_bgr = enable ? 1 : 0;
    display_set_bgr(enable != 0);
    settings_store("bgr", s_bgr);
}

int app_get_rotation(void) { return s_rot; }
int app_get_scale(void) { return s_scale; }
void app_set_wpad(int enable)
{
    if (enable) {
        esp_err_t e = phone_pad_start_ex(s_wpad_wpa2 != 0);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "phone pad start failed: %s", esp_err_to_name(e));
            s_wpad = 0;
            settings_store("wpad", 0);
            return;
        }
        s_wpad = 1;
        ESP_LOGI(TAG, "phone pad on: " PHONE_PAD_URL " (SSID '%s', auth=%s)", PHONE_PAD_SSID,
                 s_wpad_wpa2 ? "WPA2" : "open");
    } else {
        phone_pad_stop();
        s_wpad = 0;
        ESP_LOGI(TAG, "phone pad off");
    }
    settings_store("wpad", s_wpad);
}

int app_get_wpad(void) { return s_wpad; }

void app_set_wpad_wpa2(int enable)
{
    s_wpad_wpa2 = enable ? 1 : 0;
    settings_store("wpauth", s_wpad_wpa2);
    if (s_wpad) {
        phone_pad_stop();
        esp_err_t e = phone_pad_start_ex(s_wpad_wpa2 != 0);
        ESP_LOGI(TAG, "phone pad restart with auth=%s: %s", s_wpad_wpa2 ? "WPA2" : "open",
                 esp_err_to_name(e));
    }
}

int app_get_wpad_wpa2(void) { return s_wpad_wpa2; }

int app_get_bgr(void) { return display_get_bgr() ? 1 : 0; }

void app_set_wswap(int enable)
{
    s_wswap = enable ? 1 : 0;
    display_set_wire_swap(s_wswap != 0);
    settings_store("wswap", s_wswap);
}

int app_get_wswap(void) { return s_wswap; }

/* ------------------------------------------------------------------ */
/* emulator input                                                      */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* single-button (BOOT) control                                        */
/* ------------------------------------------------------------------ */
/* The board only has one usable button when no gamepad/phone is around,
 * so the BOOT button drives the whole UI:
 *
 *   menu : tap  -> move to the next ROM
 *          hold -> start the highlighted ROM
 *   game : hold -> A button
 *          1.5 s hold -> quit back to the menu
 *          double tap -> START (to get past title screens)
 *
 * Events are edge based, so the same button can drive both modes without
 * the old "BOOT == A" fallback leaking A presses into the menu.
 */
#define BOOT_TAP_MAX_US 600000
#define BOOT_HOLD_MENU_US 800000
#define BOOT_HOLD_GAME_US 1500000
#define BOOT_DEBOUNCE_US 20000
#define BOOT_DOUBLE_US 400000

typedef enum { BOOT_EVT_NONE = 0, BOOT_EVT_SHORT = 1, BOOT_EVT_LONG = 2 } boot_evt_t;

static struct {
    bool pressed;
    bool consumed; /* long press already reported for this hold */
    int64_t t_down;
    int64_t t_change;
} s_boot;

static volatile int s_sim_short; /* console injection for testing */
static volatile int s_sim_long;

void app_boot_simulate(int kind)
{
    if (kind == BOOT_EVT_LONG) {
        s_sim_long++;
    } else {
        s_sim_short++; /* two calls in a row simulate a double tap */
    }
}

static bool boot_is_pressed(void) { return s_boot.pressed; }

/* Forget the current press: used when a mode change happens while the button
 * is still down, so the new mode does not see a phantom tap/hold. */
static void boot_events_reset(bool suppress_current)
{
    int64_t now = esp_timer_get_time();
    bool down = gpio_get_level(BOOT_BTN_GPIO) == 0;
    s_boot.pressed = down;
    s_boot.t_down = now;
    s_boot.t_change = now;
    s_boot.consumed = suppress_current && down;
}

static boot_evt_t boot_poll(void)
{
    if (s_sim_long > 0) {
        s_sim_long--;
        return BOOT_EVT_LONG;
    }
    if (s_sim_short > 0) {
        s_sim_short--;
        return BOOT_EVT_SHORT;
    }

    int64_t now = esp_timer_get_time();
    bool down = gpio_get_level(BOOT_BTN_GPIO) == 0;

    if (down != s_boot.pressed) {
        if (now - s_boot.t_change < BOOT_DEBOUNCE_US) return BOOT_EVT_NONE;
        s_boot.t_change = now;
        s_boot.pressed = down;
        if (down) {
            s_boot.t_down = now;
            s_boot.consumed = false;
            return BOOT_EVT_NONE;
        }
        bool tapped = !s_boot.consumed && (now - s_boot.t_down) < BOOT_TAP_MAX_US;
        s_boot.consumed = false;
        return tapped ? BOOT_EVT_SHORT : BOOT_EVT_NONE;
    }

    if (s_boot.pressed && !s_boot.consumed) {
        int64_t hold_us = (s_mode == APP_MODE_EMU) ? BOOT_HOLD_GAME_US : BOOT_HOLD_MENU_US;
        if (now - s_boot.t_down >= hold_us) {
            s_boot.consumed = true;
            return BOOT_EVT_LONG;
        }
    }
    return BOOT_EVT_NONE;
}

/* ------------------------------------------------------------------ */
/* live status in the letterbox bars                                    */
/* ------------------------------------------------------------------ */
/* With FIT/PIXEL scaling the panel keeps two empty bars beside the game
 * image; they used to still show the menu.  Now the app pushes live numbers
 * there twice a second and the display driver renders them into the bars. */

static char s_hud_rom[8] = "--";

static void hud_rom_name(const char *name)
{
    int n = 0;
    if (name != NULL) {
        while (name[n] && name[n] != '.' && name[n] != '_' && name[n] != ' ' && n < 6) n++;
    }
    for (int i = 0; i < 6; i++) {
        char c = (i < n) ? name[i] : ' ';
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        s_hud_rom[i] = c;
    }
    s_hud_rom[6] = 0;
    while (n > 0 && s_hud_rom[n - 1] == ' ') s_hud_rom[--n] = 0;
    if (n == 0) snprintf(s_hud_rom, sizeof(s_hud_rom), "--");
}

/* Draw the "label / value" block centred in one bar rectangle of the canvas. */
static void hud_draw_block(int bx, int by, int bw, int bh, const char *text)
{
    int lines = 1;
    for (const char *p = text; *p; p++) {
        if (*p == '\n') lines++;
    }
    int top = by + (bh - lines * 10) / 2;
    if (top < by) top = by;
    int lead = (bw - 6 * 8) / 2;
    if (lead < 0) lead = 0;
    const char *s = text;
    for (int i = 0; i < lines && s && *s; i++) {
        const char *e = strchr(s, '\n');
        int len = e ? (int)(e - s) : (int)strlen(s);
        if (len > 6) len = 6;
        char line[8];
        memcpy(line, s, (size_t)len);
        line[len] = 0;
        display_ui_text(bx + lead, top + i * 10, line, (i & 1) ? UI_WHITE : UI_CYAN,
                        UI_BLACK, 1);
        s = e ? e + 1 : NULL;
    }
}

static void hud_update(void)
{
    if (display_get_scale() == DISPLAY_SCALE_FILL) return; /* no bars at all */
    if (display_ui_acquire() != ESP_OK) return;

    nes_port_stats_t st;
    nes_port_get_stats_ex(&st);
    display_stats_t dst;
    display_get_stats(&dst);

    /* Averages, not the last frame: single frames are far too noisy for a
     * status readout that is refreshed twice a second. */
    unsigned core_us = (st.frames > 0) ? (unsigned)(st.total_core_us / st.frames)
                                       : (unsigned)st.last_core_us;
    unsigned disp_us = dst.us_avg;

    char fps_s[16], cpu_s[16], dsp_s[16], heap_s[16];
    snprintf(fps_s, sizeof(fps_s), "%u.%u", (unsigned)(st.fps_x100 / 100),
             (unsigned)((st.fps_x100 % 100) / 10));
    snprintf(cpu_s, sizeof(cpu_s), "%u.%ums", core_us / 1000, (core_us / 100) % 10);
    snprintf(dsp_s, sizeof(dsp_s), "%u.%ums", disp_us / 1000, (disp_us / 100) % 10);
    snprintf(heap_s, sizeof(heap_s), "%uK", (unsigned)(esp_get_free_heap_size() / 1024));

    const char *pad_s = gamepad_count() > 0 ? "BT" : (phone_pad_clients() > 0 ? "WIFI" : "--");

    char left[96], right[96];
    snprintf(left, sizeof(left), "FPS\n%s\nCPU\n%s\nDSP\n%s", fps_s, cpu_s, dsp_s);
    snprintf(right, sizeof(right), "HEAP\n%s\nROM\n%s\nPAD\n%s", heap_s, s_hud_rom, pad_s);

    const int lw = display_logical_width();
    const int lh = display_logical_height();
    int x0, y0, w, h;
    display_image_rect(&x0, &y0, &w, &h);

    if (display_get_rotation() == DISPLAY_ROT_0 || display_get_rotation() == DISPLAY_ROT_180) {
        /* portrait: bars are the strips above and below the image */
        if (y0 > 0) {
            display_ui_rect(0, 0, lw, y0, UI_BLACK);
            hud_draw_block(0, 0, lw, y0, left);
        }
        int by = y0 + h, bh = lh - by;
        if (bh > 0) {
            display_ui_rect(0, by, lw, bh, UI_BLACK);
            hud_draw_block(0, by, lw, bh, right);
        }
    } else {
        if (x0 > 0) {
            display_ui_rect(0, 0, x0, lh, UI_BLACK);
            hud_draw_block(0, 0, x0, lh, left);
        }
        int bx = x0 + w, bw = lw - bx;
        if (bw > 0) {
            display_ui_rect(bx, 0, bw, lh, UI_BLACK);
            hud_draw_block(bx, 0, bw, lh, right);
        }
    }
    display_ui_commit_bars();
}

/* Merge the gamepad, phone and single BOOT button into one NES pad mask.
 * START+SELECT held for 800 ms quits the game. */
static uint8_t input_source(void)
{
    esp_task_wdt_reset();
    static int64_t s_hud_next_us;
    {
        int64_t now_us = esp_timer_get_time();
        if (now_us >= s_hud_next_us) {
            s_hud_next_us = now_us + 500 * 1000;
            hud_update();
        }
    }
    uint8_t pad = gamepad_read();
    pad |= phone_pad_read();
    int64_t now = esp_timer_get_time();

    static int64_t combo_since;
    static bool combo_fired;
    if ((pad & (PAD_START | PAD_SELECT)) == (PAD_START | PAD_SELECT)) {
        if (!combo_since) {
            combo_since = now;
        } else if (!combo_fired && now - combo_since > 800000) {
            combo_fired = true;
            ESP_LOGI(TAG, "quit combo (START+SELECT)");
            nes_port_request_quit();
        }
    } else {
        combo_since = 0;
        combo_fired = false;
    }

    /* BOOT button: held = A, 1.5 s = quit, double tap = START. */
    static int64_t s_bt_start_until;
    static int64_t s_bt_last_tap;
    boot_evt_t ev = boot_poll();
    if (ev == BOOT_EVT_LONG) {
        ESP_LOGI(TAG, "quit (BOOT 1.5 s hold)");
        nes_port_request_quit();
    } else if (ev == BOOT_EVT_SHORT) {
        if (s_bt_last_tap && now - s_bt_last_tap < BOOT_DOUBLE_US) {
            s_bt_start_until = now + 120000;
            s_bt_last_tap = 0;
            ESP_LOGI(TAG, "BOOT double tap: START");
        } else {
            s_bt_last_tap = now;
        }
    }
    if (boot_is_pressed()) pad |= PAD_A;
    if (now < s_bt_start_until) pad |= PAD_START;

    return pad;
}

/* Configure the BOOT pin as an input with pull-up before the gamepad
 * component gets a chance to reconfigure it (its fallback also uses it). */
static void boot_button_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BOOT_BTN_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) ESP_LOGW(TAG, "BOOT pin config: %s", esp_err_to_name(err));
}


/* ------------------------------------------------------------------ */
/* menu / emulator                                                     */
/* ------------------------------------------------------------------ */

static void acquire_canvas(void)
{
    esp_err_t err = display_ui_acquire();
    if (err != ESP_OK) ESP_LOGE(TAG, "UI canvas acquire failed: %s", esp_err_to_name(err));
}

static void do_rescan(void)
{
    esp_err_t err = rom_store_rescan();
    if (err != ESP_OK) {
        ui_menu_show_message("rescan failed: %s", esp_err_to_name(err));
    } else {
        int count = rom_store_count();
        ui_menu_set_selected(0);
        if (count == 0) {
            ui_menu_show_message("rescan: still no ROMs found");
        } else {
            ui_menu_show_message("rescan: %d ROM(s)", count);
        }
        ESP_LOGI(TAG, "rescan done: %d ROM(s) heap=%u", count,
                 (unsigned)esp_get_free_heap_size());
    }
    s_rescan_seq++;
}

static void do_play(int index, const char *path)
{
    ui_menu_clear_message();
    s_last_error[0] = 0;

    display_ui_release(); /* free 55 KB before loading the ROM */

    rom_image_t img;
    memset(&img, 0, sizeof(img));
    esp_err_t err = path ? rom_store_load_path(path, &img) : rom_store_load(index, &img);
    if (err != ESP_OK) {
        set_last_error("%s", rom_store_error_str(err));
        ESP_LOGE(TAG, "ROM load failed: %s", s_last_error);
        acquire_canvas();
        ui_menu_show_message("load failed: %s", rom_store_error_str(err));
        ui_menu_refresh();
        return;
    }

    const char *rom_name = path ? path : rom_store_get(index)->name;
    hud_rom_name(rom_name);
    ESP_LOGI(TAG, "loading ROM: %u bytes (%s), heap=%u", (unsigned)img.len, rom_name,
             (unsigned)esp_get_free_heap_size());

    err = nes_port_init();
    if (err == ESP_OK) {
        nes_port_set_rom_name(rom_name);
        err = nes_port_set_rom(img.data, img.len);
    }
    if (err != ESP_OK) {
        set_last_error("emulator: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "%s", s_last_error);
        rom_store_unload();
        acquire_canvas();
        ui_menu_show_message("emulator init failed: %s", esp_err_to_name(err));
        ui_menu_refresh();
        return;
    }

    nes_port_set_frame_sink(display_show_nes_frame);
    nes_port_set_input_source(input_source);
    display_reset_stats();

    s_mode = APP_MODE_EMU;
    s_emulating = true;
    boot_events_reset(true);
    hud_update(); /* paint the bars before the first frame */
    ESP_LOGI(TAG, "emulator running (free heap=%u)", (unsigned)esp_get_free_heap_size());

    esp_err_t run_err = nes_port_run();

    s_emulating = false;
    s_mode = APP_MODE_MENU;
    boot_events_reset(true);
    nes_port_set_frame_sink(NULL);
    nes_port_set_input_source(NULL);

    uint32_t frames = 0, fps_x100 = 0, last_us = 0, machine = 0;
    nes_port_get_stats(&frames, &fps_x100, &last_us, &machine);
    int fps = (int)(fps_x100 / 100);
    ui_menu_set_fps(fps > 0 ? fps : -1);
    ESP_LOGI(TAG, "emulator stopped (%s): frames=%u fps=%u.%02u last=%uus heap=%u",
             esp_err_to_name(run_err), (unsigned)frames, (unsigned)(fps_x100 / 100),
             (unsigned)(fps_x100 % 100), (unsigned)last_us,
             (unsigned)esp_get_free_heap_size());

    rom_store_unload();
    acquire_canvas();
    /* The START+SELECT combo may still be held while we return to the menu:
     * swallow input for a moment so the menu does not immediately relaunch. */
    s_menu_ignore_pad_until = esp_timer_get_time() + 800 * 1000;
    ui_menu_refresh();
}

static void process_requests(void)
{
    requests_t r;
    if (!req_take(&r)) return;

    if (r.clear) {
        if (display_ui_acquire() == ESP_OK) {
            display_ui_clear(UI_BLACK);
            display_ui_commit();
        }
        ui_menu_refresh();
    }
    if (r.test) {
        display_test_pattern();
        s_hold_ui_until = esp_timer_get_time() + 4 * 1000 * 1000;
    }
    if (r.rescan) do_rescan();
    if (r.play_index != PLAY_NONE) {
        do_play(r.play_is_path ? 0 : r.play_index, r.play_is_path ? r.play_path : NULL);
    }
}

static void menu_loop(void)
{
    s_mode = APP_MODE_MENU;
    ui_menu_refresh();

    uint8_t prev_pad = 0;
    int64_t last_status = 0;
    int64_t next_repeat = 0;
    int64_t ignore_pad_until = 0;
    int64_t last_alive = esp_timer_get_time();

    boot_events_reset(true);

    while (1) {
        esp_task_wdt_reset();
        process_requests();

        int64_t now = esp_timer_get_time();
        uint8_t pad = gamepad_read() | phone_pad_read();

        /* BOOT button: tap = next ROM, hold = play.  Colour order / byte
         * order are host-only settings (`bgr`, `swap` over the console). */
        boot_evt_t bev = boot_poll();
        if (bev == BOOT_EVT_SHORT) {
            ui_menu_move(1);
        } else if (bev == BOOT_EVT_LONG) {
            if (app_request_play(-1)) continue;
        }

        if (now < ignore_pad_until || now < s_menu_ignore_pad_until) {
            prev_pad = pad;
        } else {
            uint8_t pressed = pad & (uint8_t)~prev_pad;
            if (pressed & (PAD_UP | PAD_DOWN)) {
                ui_menu_move((pressed & PAD_UP) ? -1 : 1);
                next_repeat = now + 450000;
            } else if (pad & (PAD_UP | PAD_DOWN)) {
                if (now >= next_repeat) {
                    ui_menu_move((pad & PAD_UP) ? -1 : 1);
                    next_repeat = now + 120000;
                }
            }
            if (pressed & (PAD_A | PAD_START)) {
                if (app_request_play(-1)) {
                    prev_pad = pad;
                    ignore_pad_until = 0;
                    continue; /* process the play request right away */
                }
            }
            if (pressed & PAD_B) {
                app_request_rescan();
            }
            prev_pad = pad;
        }

        if (now < s_hold_ui_until) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (ui_menu_dirty() || now - last_status > 500000) {
            last_status = now;
            ui_menu_refresh();
            ui_menu_draw();
        }

        if (now - last_alive > 10 * 1000 * 1000) {
            last_alive = now;
            ESP_LOGI(TAG, "alive: mode=menu heap=%u roms=%d", (unsigned)esp_get_free_heap_size(),
                     rom_store_count());
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* ------------------------------------------------------------------ */
/* boot                                                                */
/* ------------------------------------------------------------------ */

static void print_board_info(void)
{
    esp_chip_info_t info;
    esp_chip_info(&info);
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    ESP_LOGI(TAG, "chip=%s cores=%d rev=%d.%d flash=%luMB", CONFIG_IDF_TARGET, info.cores,
             info.revision / 100, info.revision % 100,
             (unsigned long)(flash_size / (1024 * 1024)));
    ESP_LOGI(TAG, "free heap=%u DMA=%u min=%u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
}

void app_run(void)
{
    esp_reset_reason_t rr = esp_reset_reason();
    ESP_LOGI(TAG, "=== MiniMario %s boot === reset_reason=%d (%s)", MINIMARIO_VERSION, (int)rr,
             rr == ESP_RST_POWERON ? "poweron" : rr == ESP_RST_SW ? "sw" : rr == ESP_RST_PANIC
                 ? "PANIC" : rr == ESP_RST_INT_WDT ? "INT_WDT" : rr == ESP_RST_TASK_WDT ? "TASK_WDT"
                 : rr == ESP_RST_BROWNOUT ? "BROWNOUT" : rr == ESP_RST_USB ? "usb" : "other");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    settings_load();
    requests_init();

    /* Subscribe the main task to the task watchdog and feed it from the app
     * loops.  Without this the emulator's 60 Hz busy loop starves the idle
     * task and the watchdog prints a backtrace every 10 s. */
    (void) esp_task_wdt_add(NULL);

    print_board_info();

    ESP_ERROR_CHECK(display_init());
    display_set_rotation((display_rotation_t)s_rot);
    display_set_scale((display_scale_t)s_scale); /* also builds the scale maps */
    display_set_backlight(s_bl);
    display_set_wire_swap(s_wswap != 0);
    if (s_bgr) display_set_bgr(true);
    acquire_canvas();

    boot_button_init();
    /* BOOT is handled by the single-button UI logic below, not as a fake A. */
    gamepad_set_fallback(false);
    err = gamepad_init();
    if (err != ESP_OK) ESP_LOGW(TAG, "gamepad init: %s (keyboard-only mode)", esp_err_to_name(err));

    esp_err_t rom_err = rom_store_init();
    if (rom_err != ESP_OK) ESP_LOGW(TAG, "rom store: %s", esp_err_to_name(rom_err));

    ui_menu_init();
    console_init();
    if (s_wpad) {
        esp_err_t e = phone_pad_start_ex(s_wpad_wpa2 != 0);
        ESP_LOGI(TAG, "phone pad (NVS enabled, auth=%s): %s", s_wpad_wpa2 ? "WPA2" : "open",
                 esp_err_to_name(e));
    }
    ui_menu_refresh();
    ui_menu_draw();

    ESP_LOGI(TAG, "READY: mode=menu roms=%d sd=%s heap=%u", rom_store_count(),
             rom_store_sd_mounted() ? "mounted" : "absent", (unsigned)esp_get_free_heap_size());
    printf("MMBOOT ready fw=%s roms=%d sd=%d heap=%u\n", MINIMARIO_VERSION, rom_store_count(),
           rom_store_sd_mounted() ? 1 : 0, (unsigned)esp_get_free_heap_size());

    menu_loop();
    __builtin_unreachable();
}
