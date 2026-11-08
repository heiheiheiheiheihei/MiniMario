/*
 * MiniMario - serial debug console.
 *
 * Command protocol for host automation:
 *   - every response line starts with OK / ERR / INFO / ROM / HINT / NOTE,
 *   - `shot` (emulator running)  : "SHOT 256 224 57344 512\n" + 57344 bytes
 *                                  indexed framebuffer + 512 bytes RGB565
 *                                  palette (LE) + "SHOTEND\n"
 *   - `shot` (menu)             : "SHOTUI 320 172 55040 32\n" + 55040 bytes
 *                                  indexed UI canvas + 32 bytes RGB565 UI
 *                                  palette + "SHOTEND\n"
 *   - `shotraw`                 : "SHOTRAW <w> <h> <n>\n" + n bytes + "SHOTEND\n"
 * tools/shot.py parses all three.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdarg.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"

#include "display.h"
#include "gamepad.h"
#include "nes_port.h"
#include "pad.h"
#include "minimario.h"
#include "app.h"
#include "console.h"
#include "rom_store.h"
#include "ui_menu.h"
#include "phone_pad.h"

static const char *TAG = "console";

#define CONSOLE_TASK_STACK 6144
#define CONSOLE_TASK_PRIO 3
#define CONSOLE_LINE_MAX 160

static char s_line[CONSOLE_LINE_MAX];
static int s_line_len;

/* ------------------------------------------------------------------ */
/* output helpers                                                      */
/* ------------------------------------------------------------------ */

static void out(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void out(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
}

static bool raw_write(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    while (len) {
        size_t chunk = len > 2048 ? 2048 : len;
        int w = usb_serial_jtag_write_bytes(p, chunk, pdMS_TO_TICKS(2000));
        if (w <= 0) return false;
        p += w;
        len -= (size_t)w;
    }
    return true;
}

static void logs_quiet(bool quiet)
{
    esp_log_level_set("*", quiet ? ESP_LOG_NONE : ESP_LOG_INFO);
}

static const char *mode_name(void)
{
    switch (app_get_mode()) {
    case APP_MODE_MENU:
        return "menu";
    case APP_MODE_EMU:
        return "emu";
    default:
        return "boot";
    }
}

static const char *button_names(uint8_t mask, char *buf, size_t n)
{
    static const struct {
        uint8_t bit;
        const char *name;
    } names[] = {
        {PAD_A, "A"},       {PAD_B, "B"},         {PAD_SELECT, "SELECT"},
        {PAD_START, "START"}, {PAD_UP, "UP"},     {PAD_DOWN, "DOWN"},
        {PAD_LEFT, "LEFT"}, {PAD_RIGHT, "RIGHT"},
    };
    buf[0] = 0;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (mask & names[i].bit) {
            if (buf[0]) strncat(buf, "+", n - strlen(buf) - 1);
            strncat(buf, names[i].name, n - strlen(buf) - 1);
        }
    }
    if (!buf[0]) snprintf(buf, n, "none");
    return buf;
}

/* ------------------------------------------------------------------ */
/* commands                                                            */
/* ------------------------------------------------------------------ */

static void cmd_help(void)
{
    out("commands:\n");
    out("  help                     this help\n");
    out("  info                     system / emulator information\n");
    out("  roms                     list ROMs (flash partition + TF card)\n");
    out("  play <n|flash|path>      start a ROM (menu entry index, 'flash'\n");
    out("                           or a .nes path on the TF card)\n");
    out("  menu                     stop the game and return to the menu\n");
    out("  quit                     same as menu\n");
    out("  up | down                move the menu cursor (also: next/prev)\n");
    out("  select                   start the highlighted ROM\n");
    out("  rescan                   rescan TF card + flash partition\n");
    out("  sdtest [clean]           TF self-test: write/read a temp file, copy an\n");
    out("                           embedded ROM to the card and rescan it\n");
    out("                           ('clean' removes the test ROM)\n");
    out("  key <a|b|select|start|up|down|left|right|none> [+ ...]\n");
    out("                           inject pad buttons (key none releases)\n");
    out("  pad                      show the current pad state\n");
    out("  rot <0-3>                display rotation (0=portrait, 1=landscape)\n");
    out("  scale <0-2>              0=FIT 1=FILL 2=PIXEL\n");
    out("  bl <0-100>               backlight percentage\n");
    out("  bgr <0|1>                colour order fallback (persisted)\n");
    out("  boot tap|hold|double     simulate a BOOT-button event (testing)\n");
    out("  wpad on|off|status|test  phone controller over Wi-Fi: board starts the\n");
    out("  wpad auth open|wpa2      AP security (default: open, no password)\n");
    out("  wpad press <hex> [ms]    simulate phone holding buttons (e.g. 01=A)\n");
    out("  wpad scan                scan Wi-Fi from the board (diagnostics)\n");
    out("  wpad debug on|off        Wi-Fi driver debug logs (diagnostics)\n");
    out("  bt scan on|off           pause/resume BLE scanning (diagnostics)\n");
    out("                           'MiniMario' AP, phone opens http://192.168.4.1/\n");
    out("  test                     show the colour test pattern\n");
    out("  swap <0|1>               RGB565 byte order sent to the panel\n");
    out("  pal                      colour order + palette dump (debug)\n");
    out("  clear                    clear the screen\n");
    out("  shot                     dump frame/canvas (indexed + palette)\n");
    out("  shotui                   dump the UI canvas even while a game runs\n");
    out("  shotraw                  dump the raw indexed buffer only\n");
    out("  ping                     'OK pong' liveness check\n");
    out("  reboot                   restart the device\n");
}

static void cmd_info(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);

    out("INFO mode=%s\n", mode_name());
    out("INFO fw=%s chip=%s cores=%d flash=%luMB\n", MINIMARIO_VERSION, CONFIG_IDF_TARGET,
        chip.cores, (unsigned long)(flash_size / (1024 * 1024)));
    out("INFO heap=%u min=%u dma=%u\n", (unsigned)esp_get_free_heap_size(),
        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA));
    out("INFO roms=%d flash_roms=%d sd=%s\n", rom_store_count(),
        rom_store_count_source(ROM_SRC_FLASH), rom_store_sd_mounted() ? "mounted" : "absent");
    if (!rom_store_sd_mounted()) out("INFO sd_error=%s\n", rom_store_sd_error());
    out("INFO flash_error=%s\n", rom_store_flash_error()[0] ? rom_store_flash_error() : "none");

    int pads = gamepad_count();
    const gamepad_info_t *gi = pads > 0 ? gamepad_info(0) : NULL;
    out("INFO gamepad=%d name=%s\n", pads, (gi && gi->connected) ? gi->name : "none");

    display_stats_t st;
    display_get_stats(&st);
    out("INFO display=%dx%d rot=%d scale=%d bl=%d bgr=%d frames=%lu avg=%luus max=%luus "
        "convert=%luus spi=%luus\n",
        display_logical_width(), display_logical_height(), (int)display_get_rotation(),
        (int)display_get_scale(), display_get_backlight(), app_get_bgr(),
        (unsigned long)st.frames, (unsigned long)st.us_avg, (unsigned long)st.us_max,
        (unsigned long)st.us_convert, (unsigned long)st.us_spi);

    if (app_is_emulating()) {
        uint32_t frames = 0, fps_x100 = 0, last_us = 0, machine = 0;
        nes_port_get_stats(&frames, &fps_x100, &last_us, &machine);
        out("INFO emu=frames=%u fps=%u.%02u last=%uus machine=%u\n", (unsigned)frames,
            (unsigned)(fps_x100 / 100), (unsigned)(fps_x100 % 100), (unsigned)last_us,
            (unsigned)machine);
    }
}

static void cmd_roms(void)
{
    int n = rom_store_count();
    out("INFO roms=%d sd=%s\n", n, rom_store_sd_mounted() ? "mounted" : "absent");
    if (!rom_store_sd_mounted()) out("NOTE sd: %s\n", rom_store_sd_error());
    for (int i = 0; i < n; i++) {
        const rom_entry_t *e = rom_store_get(i);
        if (!e) continue;
        out("ROM %d %s %u %s\n", i, e->source == ROM_SRC_FLASH ? "flash" : "sd", (unsigned)e->size,
            e->name);
    }
    if (n == 0) {
        out("HINT copy .nes files (max 256K) to the TF card root, or flash the\n");
        out("HINT embedded container: tools/flash_rom.py\n");
    }
}

/* Wait until the app task has started the emulator (or failed). */
static void wait_for_start(int index, const char *path)
{
    for (int i = 0; i < 300; i++) { /* 6 s max */
        if (app_is_emulating()) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (app_is_emulating()) {
        const rom_entry_t *e = path ? NULL : rom_store_get(index);
        out("OK play %d %s\n", index, path ? path : (e ? e->name : "?"));
        out("OK running\n");
    } else {
        const char *err = app_last_error();
        out("ERR load: %s\n", (err && *err) ? err : "timeout");
    }
}

static void cmd_play(const char *arg)
{
    if (app_is_emulating()) {
        out("ERR busy: emulator running (send 'quit' first)\n");
        return;
    }
    if (!arg[0]) {
        if (rom_store_count() <= 0) {
            out("ERR no ROMs available\n");
            return;
        }
        int idx = ui_menu_selected();
        if (!app_request_play(idx)) {
            out("ERR play request rejected\n");
            return;
        }
        wait_for_start(idx, NULL);
        return;
    }
    if (strcasecmp(arg, "flash") == 0) {
        int count = rom_store_count();
        int idx = -1;
        for (int i = 0; i < count; i++) {
            const rom_entry_t *e = rom_store_get(i);
            if (e && e->source == ROM_SRC_FLASH) {
                idx = i;
                break;
            }
        }
        if (idx < 0) {
            out("ERR no embedded ROM (flash one with tools/flash_rom.py)\n");
            return;
        }
        if (!app_request_play(idx)) {
            out("ERR play request rejected\n");
            return;
        }
        wait_for_start(idx, NULL);
        return;
    }
    bool numeric = true;
    for (const char *p = arg; *p; p++) {
        if (!isdigit((unsigned char)*p)) {
            numeric = false;
            break;
        }
    }
    if (numeric) {
        int idx = atoi(arg);
        if (idx < 0 || idx >= rom_store_count()) {
            out("ERR index %d out of range (0..%d)\n", idx, rom_store_count() - 1);
            return;
        }
        if (!app_request_play(idx)) {
            out("ERR play request rejected\n");
            return;
        }
        wait_for_start(idx, NULL);
        return;
    }
    if (!app_request_play_path(arg)) {
        out("ERR play request rejected\n");
        return;
    }
    wait_for_start(-1, arg);
}

static void cmd_menu(void)
{
    if (!app_is_emulating()) {
        app_request_menu();
        out("OK already in menu\n");
        return;
    }
    app_request_menu();
    for (int i = 0; i < 300; i++) {
        if (!app_is_emulating()) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (app_is_emulating()) {
        out("ERR emulator did not stop\n");
    } else {
        out("OK menu\n");
    }
}

static void cmd_move(int delta)
{
    if (app_is_emulating()) {
        out("ERR busy: emulator running\n");
        return;
    }
    ui_menu_move(delta);
    out("OK selected %d\n", ui_menu_selected());
    vTaskDelay(pdMS_TO_TICKS(80)); /* let the app task redraw before a shot */
}

static void cmd_select(void)
{
    if (app_is_emulating()) {
        out("ERR busy: emulator running\n");
        return;
    }
    if (rom_store_count() <= 0) {
        out("ERR no ROMs available\n");
        return;
    }
    int idx = ui_menu_selected();
    if (!app_request_play(idx)) {
        out("ERR play request rejected\n");
        return;
    }
    wait_for_start(idx, NULL);
}

static void cmd_rescan(void)
{
    if (app_is_emulating()) {
        out("ERR busy: emulator running\n");
        return;
    }
    uint32_t seq = app_rescan_seq();
    if (!app_request_rescan()) {
        out("ERR rescan request rejected\n");
        return;
    }
    for (int i = 0; i < 500; i++) { /* 10 s max */
        if (app_rescan_seq() != seq) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    out("OK rescan roms=%d sd=%s\n", rom_store_count(),
        rom_store_sd_mounted() ? "mounted" : "absent");
    vTaskDelay(pdMS_TO_TICKS(80));
}

/* ------------------------------------------------------------------ */
/* TF card self-test                                                   */
/* ------------------------------------------------------------------ */

/* Ask the app task to rescan (it owns the menu/UI state) and wait for it. */
static bool request_rescan_and_wait(void)
{
    uint32_t seq = app_rescan_seq();
    if (!app_request_rescan()) return false;
    for (int i = 0; i < 500; i++) { /* 10 s max */
        if (app_rescan_seq() != seq) return true;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return false;
}

static int sdtest_find_sd(const char *name, uint32_t *size)
{
    int n = rom_store_count();
    for (int i = 0; i < n; i++) {
        const rom_entry_t *e = rom_store_get(i);
        if (!e || e->source != ROM_SRC_SD) continue;
        if (name && strcmp(e->name, name) != 0) continue;
        if (size) *size = e->size;
        return i;
    }
    return -1;
}

static uint8_t sdtest_pattern(size_t off)
{
    return (uint8_t)(off * 7u + 3u);
}

/* Write a deterministic 32 KB file, read it back and verify every byte. */
static bool sdtest_rw(void)
{
    enum { CHUNK = 8192, CHUNKS = 4 };
    const char *path = "/sdcard/mm_sdtest.tmp";
    uint8_t *buf = malloc(CHUNK);
    if (!buf) {
        out("ERR sdtest: out of memory\n");
        return false;
    }
    bool ok = true;
    int64_t t0 = 0, t1 = 0, t2 = 0, t3 = 0;

    FILE *f = fopen(path, "wb");
    if (!f) {
        free(buf);
        out("ERR sdtest: cannot create %s\n", path);
        return false;
    }
    t0 = esp_timer_get_time();
    for (int c = 0; c < CHUNKS && ok; c++) {
        for (int i = 0; i < CHUNK; i++)
            buf[i] = sdtest_pattern((size_t)c * CHUNK + (size_t)i);
        if (fwrite(buf, 1, CHUNK, f) != CHUNK) ok = false;
    }
    if (fclose(f) != 0) ok = false;
    t1 = esp_timer_get_time();
    if (!ok) {
        remove(path);
        free(buf);
        out("ERR sdtest: write failed\n");
        return false;
    }

    f = fopen(path, "rb");
    if (!f) {
        remove(path);
        free(buf);
        out("ERR sdtest: reopen failed\n");
        return false;
    }
    t2 = esp_timer_get_time();
    for (int c = 0; c < CHUNKS && ok; c++) {
        if (fread(buf, 1, CHUNK, f) != CHUNK) {
            ok = false;
            break;
        }
        for (int i = 0; i < CHUNK; i++) {
            if (buf[i] != sdtest_pattern((size_t)c * CHUNK + (size_t)i)) {
                ok = false;
                break;
            }
        }
    }
    t3 = esp_timer_get_time();
    fclose(f);
    remove(path);
    free(buf);
    if (!ok) {
        out("ERR sdtest: readback verify failed\n");
        return false;
    }
    out("INFO sdtest: rw %uB write=%lldms read=%lldms\n",
        (unsigned)(CHUNK * CHUNKS), (long long)(t1 - t0) / 1000,
        (long long)(t3 - t2) / 1000);
    return true;
}

/* Copy the smallest embedded ROM to the card, read it back, rescan and
 * verify it is listed as an SD ROM.  Leaves /sdcard/mm_sdtest.nes. */
static bool sdtest_copy(int *out_index)
{
    int src = -1;
    uint32_t best = 0xFFFFFFFFu;
    for (int i = 0; i < rom_store_count(); i++) {
        const rom_entry_t *e = rom_store_get(i);
        if (!e || e->source != ROM_SRC_FLASH) continue;
        if (e->size < best) {
            best = e->size;
            src = i;
        }
    }
    if (src < 0) {
        out("ERR sdtest: no embedded ROM to copy\n");
        return false;
    }

    char src_name[40];
    snprintf(src_name, sizeof(src_name), "%s", rom_store_get(src)->name);

    rom_image_t img;
    esp_err_t err = rom_store_load(src, &img);
    if (err != ESP_OK) {
        out("ERR sdtest: load '%s': %s\n", src_name, esp_err_to_name(err));
        return false;
    }
    const size_t img_len = img.len;
    const char *name = "mm_sdtest.nes";
    char path[64];
    snprintf(path, sizeof(path), "/sdcard/%s", name);

    bool ok = false;
    FILE *f = fopen(path, "wb");
    if (f) {
        ok = fwrite(img.data, 1, img_len, f) == img_len;
        if (fclose(f) != 0) ok = false;
    }
    if (!ok) {
        remove(path);
        rom_store_unload();
        out("ERR sdtest: copy to %s failed\n", path);
        return false;
    }

    /* Read it back through the VFS (same path the ROM loader uses). */
    uint8_t *buf = malloc(4096);
    if (!buf) {
        remove(path);
        rom_store_unload();
        out("ERR sdtest: out of memory\n");
        return false;
    }
    f = fopen(path, "rb");
    ok = (f != NULL);
    size_t off = 0;
    while (ok && off < img_len) {
        size_t want = img_len - off;
        if (want > 4096) want = 4096;
        ok = fread(buf, 1, want, f) == want &&
             memcmp(buf, img.data + off, want) == 0;
        off += want;
    }
    if (f) fclose(f);
    free(buf);
    if (!ok) {
        remove(path);
        rom_store_unload();
        out("ERR sdtest: readback of %s failed\n", path);
        return false;
    }
    out("INFO sdtest: copied '%s' %uB -> %s\n", src_name, (unsigned)img_len, path);
    rom_store_unload();

    if (!request_rescan_and_wait()) {
        out("ERR sdtest: rescan failed\n");
        return false;
    }
    uint32_t size = 0;
    int idx = sdtest_find_sd(name, &size);
    if (idx < 0) {
        out("ERR sdtest: %s not listed after rescan\n", name);
        return false;
    }
    if (size != img_len) {
        out("ERR sdtest: size mismatch (%u != %u)\n", (unsigned)size, (unsigned)img_len);
        return false;
    }
    out("INFO sdtest: listed as ROM %d '%s' %uB\n", idx, name, (unsigned)size);
    *out_index = idx;
    return true;
}

static void cmd_sdtest(const char *arg)
{
    if (app_is_emulating()) {
        out("ERR busy: emulator running (send 'quit' first)\n");
        return;
    }
    if (!rom_store_sd_mounted()) {
        out("ERR sdtest: TF card not mounted (%s)\n", rom_store_sd_error());
        return;
    }
    if (strcasecmp(arg, "clean") == 0) {
        remove("/sdcard/mm_sdtest.nes");
        remove("/sdcard/mm_sdtest.tmp");
        if (!request_rescan_and_wait()) {
            out("ERR sdtest: rescan failed\n");
            return;
        }
        out("OK sdtest clean roms=%d sd_roms=%d\n", rom_store_count(),
            rom_store_count_source(ROM_SRC_SD));
        return;
    }
    if (arg[0]) {
        out("ERR usage: sdtest [clean]\n");
        return;
    }

    out("INFO sdtest: start (sd mounted)\n");
    if (!sdtest_rw()) return;
    int idx = -1;
    if (!sdtest_copy(&idx)) return;
    out("OK sdtest roms=%d sd_roms=%d index=%d\n", rom_store_count(),
        rom_store_count_source(ROM_SRC_SD), idx);
}

static int parse_key_mask(const char *arg, bool *recognised)
{
    *recognised = true;
    if (!arg[0] || strcasecmp(arg, "none") == 0 || strcasecmp(arg, "release") == 0 ||
        strcmp(arg, "0") == 0) {
        return 0;
    }
    int mask = 0;
    char buf[96];
    snprintf(buf, sizeof(buf), "%.*s", (int)sizeof(buf) - 1, arg);
    char *save = NULL;
    for (char *tok = strtok_r(buf, "+ ", &save); tok; tok = strtok_r(NULL, "+ ", &save)) {
        if (strcasecmp(tok, "a") == 0) mask |= PAD_A;
        else if (strcasecmp(tok, "b") == 0) mask |= PAD_B;
        else if (strcasecmp(tok, "select") == 0 || strcasecmp(tok, "sel") == 0) mask |= PAD_SELECT;
        else if (strcasecmp(tok, "start") == 0) mask |= PAD_START;
        else if (strcasecmp(tok, "up") == 0) mask |= PAD_UP;
        else if (strcasecmp(tok, "down") == 0) mask |= PAD_DOWN;
        else if (strcasecmp(tok, "left") == 0) mask |= PAD_LEFT;
        else if (strcasecmp(tok, "right") == 0) mask |= PAD_RIGHT;
        else {
            *recognised = false;
            return 0;
        }
    }
    return mask;
}

static void cmd_key(const char *arg)
{
    bool ok = false;
    int mask = parse_key_mask(arg, &ok);
    if (!ok) {
        out("ERR key: unknown button '%s'\n", arg);
        return;
    }
    gamepad_inject((uint8_t)mask);
    char names[48];
    out("OK key=0x%02x (%s)\n", mask, button_names((uint8_t)mask, names, sizeof(names)));
}

static void cmd_pad(void)
{
    uint8_t mask = gamepad_read();
    char names[48];
    out("PAD 0x%02x (%s) gamepads=%d\n", mask, button_names(mask, names, sizeof(names)),
        gamepad_count());
    const gamepad_info_t *gi = gamepad_count() > 0 ? gamepad_info(0) : NULL;
    if (gi && gi->connected) {
        out("PAD name=%s vid=0x%04x pid=0x%04x battery=%d\n", gi->name, gi->vendor_id,
            gi->product_id, gi->battery);
    }
}

static bool dump_shot(bool raw, bool force_ui)
{
    bool emu = app_is_emulating() && !force_ui;
    bool ok = true;

    logs_quiet(true);

    if (emu) {
        /* Freeze the emulation while the ~57 KB blob is streamed so animated
         * screens give a tear-free screenshot (the loop keeps its cadence and
         * stays responsive to console commands while paused). */
        nes_port_set_paused(true);
        const uint8_t *fb = nes_port_framebuffer();
        const uint16_t *pal = nes_port_palette();
        if (!fb) {
            nes_port_set_paused(false);
            logs_quiet(false);
            out("ERR shot: no framebuffer yet\n");
            return false;
        }
        char hdr[48];
        if (raw) {
            snprintf(hdr, sizeof(hdr), "SHOTRAW %d %d %d\n", NES_FRAME_W, NES_FRAME_H,
                     NES_FRAME_W * NES_FRAME_H);
            ok = raw_write(hdr, strlen(hdr)) &&
                 raw_write(fb, (size_t)NES_FRAME_W * NES_FRAME_H);
        } else {
            if (!pal) {
                nes_port_set_paused(false);
                logs_quiet(false);
                out("ERR shot: no palette\n");
                return false;
            }
            snprintf(hdr, sizeof(hdr), "SHOT %d %d %d %d\n", NES_FRAME_W, NES_FRAME_H,
                     NES_FRAME_W * NES_FRAME_H, 256 * 2);
            ok = raw_write(hdr, strlen(hdr)) &&
                 raw_write(fb, (size_t)NES_FRAME_W * NES_FRAME_H) && raw_write(pal, 256 * 2);
        }
        nes_port_set_paused(false);
    } else {
        ui_menu_lock();
        const uint8_t *canvas = display_ui_canvas();
        const uint16_t *pal = display_ui_palette();
        if (!canvas || !pal) {
            ui_menu_unlock();
            logs_quiet(false);
            out("ERR shot: UI canvas unavailable\n");
            return false;
        }
        int w = display_logical_width();
        int h = display_logical_height();
        size_t nbytes = (size_t)w * h;
        char hdr[56];
        if (raw) {
            snprintf(hdr, sizeof(hdr), "SHOTRAW %d %d %d\n", w, h, (int)nbytes);
            ok = raw_write(hdr, strlen(hdr)) && raw_write(canvas, nbytes);
        } else {
            snprintf(hdr, sizeof(hdr), "SHOTUI %d %d %d %d\n", w, h, (int)nbytes, 16 * 2);
            ok = raw_write(hdr, strlen(hdr)) && raw_write(canvas, nbytes) &&
                 raw_write(pal, 16 * 2);
        }
        ui_menu_unlock();
    }

    if (ok) ok = raw_write("SHOTEND\n", 8);
    logs_quiet(false);
    if (!ok) out("ERR shot: write failed (host not reading?)\n");
    return ok;
}

static void cmd_shot(bool raw, bool force_ui)
{
    /* Small settle delay: the app task may be in the middle of a redraw. */
    vTaskDelay(pdMS_TO_TICKS(30));
    dump_shot(raw, force_ui);
}

/* ------------------------------------------------------------------ */
/* command dispatch                                                    */
/* ------------------------------------------------------------------ */

static void handle_line(char *line)
{
    while (*line == ' ') line++;
    if (!*line) return;

    char *arg = line;
    while (*arg && *arg != ' ') arg++;
    if (*arg) {
        *arg++ = 0;
        while (*arg == ' ') arg++;
    }

    char cmd[16];
    snprintf(cmd, sizeof(cmd), "%.*s", (int)sizeof(cmd) - 1, line);
    for (char *p = cmd; *p; p++) *p = (char)tolower((unsigned char)*p);

    if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) {
        cmd_help();
    } else if (!strcmp(cmd, "info")) {
        cmd_info();
    } else if (!strcmp(cmd, "roms") || !strcmp(cmd, "ls")) {
        cmd_roms();
    } else if (!strcmp(cmd, "play") || !strcmp(cmd, "run")) {
        cmd_play(arg);
    } else if (!strcmp(cmd, "menu") || !strcmp(cmd, "quit") || !strcmp(cmd, "stop")) {
        cmd_menu();
    } else if (!strcmp(cmd, "up") || !strcmp(cmd, "prev")) {
        cmd_move(-1);
    } else if (!strcmp(cmd, "down") || !strcmp(cmd, "next")) {
        cmd_move(1);
    } else if (!strcmp(cmd, "select") || !strcmp(cmd, "start")) {
        cmd_select();
    } else if (!strcmp(cmd, "rescan")) {
        cmd_rescan();
    } else if (!strcmp(cmd, "sdtest")) {
        cmd_sdtest(arg);
    } else if (!strcmp(cmd, "key")) {
        cmd_key(arg);
    } else if (!strcmp(cmd, "pad")) {
        cmd_pad();
    } else if (!strcmp(cmd, "rot")) {
        int v = atoi(arg);
        if (v < 0 || v > 3) {
            out("ERR usage: rot <0-3>\n");
        } else {
            app_set_rotation(v);
            out("OK rot=%d\n", app_get_rotation());
        }
    } else if (!strcmp(cmd, "scale")) {
        int v = atoi(arg);
        if (v < 0 || v > 2) {
            out("ERR usage: scale <0-2> (0 FIT, 1 FILL, 2 PIXEL)\n");
        } else {
            app_set_scale(v);
            out("OK scale=%d\n", app_get_scale());
        }
    } else if (!strcmp(cmd, "bl") || !strcmp(cmd, "backlight")) {
        int v = atoi(arg);
        if (!*arg || v < 0 || v > 100) {
            out("ERR usage: bl <0-100>\n");
        } else {
            app_set_backlight(v);
            out("OK bl=%d\n", display_get_backlight());
        }
    } else if (!strcmp(cmd, "boot")) {
        if (!strcmp(arg, "tap") || !strcmp(arg, "short")) {
            app_boot_simulate(1);
            out("OK boot tap injected\n");
        } else if (!strcmp(arg, "hold") || !strcmp(arg, "long")) {
            app_boot_simulate(2);
            out("OK boot hold injected\n");
        } else if (!strcmp(arg, "double") || !strcmp(arg, "dbl")) {
            app_boot_simulate(1);
            app_boot_simulate(1);
            out("OK boot double tap injected\n");
        } else {
            out("ERR usage: boot tap|hold\n");
        }
    } else if (!strcmp(cmd, "wpad")) {
        if (!strcmp(arg, "test")) {
            char rep[200];
            esp_err_t e = phone_pad_selftest(rep, sizeof(rep));
            out("%s wpad selftest: %s\n", e == ESP_OK ? "OK" : "ERR", rep);
        } else if (!strcmp(arg, "on") || !strcmp(arg, "1")) {
            app_set_wpad(1);
            char st[160];
            phone_pad_status(st, sizeof(st));
            out("OK %s\n", st);
        } else if (!strcmp(arg, "off") || !strcmp(arg, "0")) {
            app_set_wpad(0);
            out("OK wpad=off\n");
        } else if (!strncmp(arg, "auth", 4)) {
            const char *v = arg + 4;
            while (*v == ' ') v++;
            if (!strcmp(v, "wpa2")) {
                app_set_wpad_wpa2(1);
                out("OK wpad auth=wpa2 (password: %s)\n", PHONE_PAD_PASSWORD);
            } else if (!strcmp(v, "open")) {
                app_set_wpad_wpa2(0);
                out("OK wpad auth=open (no password)\n");
            } else {
                out("ERR usage: wpad auth open|wpa2\n");
            }
        } else if (!strncmp(arg, "press", 5)) {
            char *rest = arg + 5;
            while (*rest == ' ') rest++;
            unsigned v = (unsigned)strtoul(rest, &rest, 16);
            while (*rest == ' ') rest++;
            int ms = *rest ? atoi(rest) : 300;
            if (ms < 50) ms = 50;
            if (ms > 5000) ms = 5000;
            esp_err_t e = phone_pad_press((uint8_t)v, ms);
            out("%s wpad press mask=0x%02x for %d ms\n", e == ESP_OK ? "OK" : "ERR", v, ms);
        } else if (!strncmp(arg, "scan", 4)) {
            static char rep[480];
            esp_err_t e = phone_pad_scan(rep, sizeof(rep));
            out("%s %s\n", e == ESP_OK ? "OK" : "ERR", rep);
        } else if (!strncmp(arg, "debug", 5)) {
            const char *v = arg + 5;
            while (*v == ' ') v++;
            phone_pad_set_debug(strcmp(v, "off") != 0);
            out("OK wpad debug %s\n", strcmp(v, "off") != 0 ? "on" : "off");
        } else {
            char st[160];
            phone_pad_status(st, sizeof(st));
            out("INFO %s\n", st);
        }
    } else if (!strcmp(cmd, "bt")) {
        if (!strcmp(arg, "scan off")) {
            gamepad_pause_scan(true);
            out("OK bt scan=off\n");
        } else if (!strcmp(arg, "scan on")) {
            gamepad_pause_scan(false);
            out("OK bt scan=on\n");
        } else {
            out("ERR usage: bt scan on|off\n");
        }
    } else if (!strcmp(cmd, "swap")) {
        int v = atoi(arg);
        if (v < 0 || v > 1) {
            out("ERR usage: swap <0|1> (RGB565 byte order: 0 native, 1 high byte first)\n");
        } else {
            app_set_wswap(v);
            out("OK swap=%d\n", app_get_wswap());
        }
    } else if (!strcmp(cmd, "pal")) {
        char info[240];
        display_debug_palettes(info, sizeof(info));
        out("OK %s\n", info);
    } else if (!strcmp(cmd, "bgr")) {
        int v = atoi(arg);
        if (!*arg || (v != 0 && v != 1)) {
            out("ERR usage: bgr <0|1>\n");
        } else {
            app_set_bgr(v);
            out("OK bgr=%d\n", app_get_bgr());
        }
    } else if (!strcmp(cmd, "test")) {
        if (app_request_test_pattern()) {
            out("OK test pattern\n");
        } else {
            out("ERR busy: emulator running\n");
        }
    } else if (!strcmp(cmd, "clear")) {
        if (app_request_clear()) {
            out("OK clear\n");
        } else {
            out("ERR busy: emulator running\n");
        }
    } else if (!strcmp(cmd, "shot")) {
        cmd_shot(false, false);
    } else if (!strcmp(cmd, "shotraw")) {
        cmd_shot(true, false);
    } else if (!strcmp(cmd, "shotui")) {
        cmd_shot(false, true);
    } else if (!strcmp(cmd, "ping")) {
        out("OK pong\n");
    } else if (!strcmp(cmd, "reboot") || !strcmp(cmd, "reset")) {
        out("OK reboot\n");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    } else {
        out("ERR unknown command '%s' (try 'help')\n", cmd);
    }
}

/* ------------------------------------------------------------------ */
/* task                                                                */
/* ------------------------------------------------------------------ */

static void console_task(void *arg)
{
    (void)arg;
    uint8_t buf[64];
    out("\nconsole ready - type 'help'\n");

    while (1) {
        int n = usb_serial_jtag_read_bytes(buf, sizeof(buf), pdMS_TO_TICKS(50));
        if (n <= 0) continue;
        for (int i = 0; i < n; i++) {
            char c = (char)buf[i];
            if (c == '\r') continue;
            if (c == '\n') {
                out("\n");
                s_line[s_line_len] = 0;
                handle_line(s_line);
                s_line_len = 0;
            } else if (c == 0x7f || c == 0x08) {
                if (s_line_len > 0) {
                    s_line_len--;
                    out("\b \b");
                }
            } else if (c == 3) { /* Ctrl-C: drop the line */
                s_line_len = 0;
                out("^C\n");
            } else if (s_line_len < CONSOLE_LINE_MAX - 1 && (unsigned char)c >= 32) {
                s_line[s_line_len++] = c;
                out("%c", c);
            }
        }
    }
}

void console_init(void)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.tx_buffer_size = 4096;
    cfg.rx_buffer_size = 1024;
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_serial_jtag driver install failed: %s", esp_err_to_name(err));
        return;
    }
    BaseType_t ok = xTaskCreate(console_task, "console", CONSOLE_TASK_STACK, NULL,
                                CONSOLE_TASK_PRIO, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "console task create failed");
        return;
    }
    ESP_LOGI(TAG, "console started (usb-serial-jtag)");
}
