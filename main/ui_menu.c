/*
 * MiniMario - landscape menu implementation.  See ui_menu.h.
 *
 * Layout is computed from display_logical_width()/display_logical_height():
 *   - landscape (320x172, rotations 90/270): header, 7 ROM rows, status,
 *     details/message, 2 footer lines,
 *   - portrait (172x320, rotations 0/180): header, 16 ROM rows, 3 short
 *     footer lines (the menu is designed for landscape, this keeps it usable).
 */
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_system.h"

#include "display.h"
#include "gamepad.h"
#include "minimario.h"
#include "rom_store.h"
#include "ui_menu.h"
#include "phone_pad.h"

static const char *TAG = "ui_menu";

#define ROW_H 13
#define HEADER_H 20
#define COL_BAR UI_NAVY

static SemaphoreHandle_t s_lock;
static int s_sel;
static int s_top;
static bool s_dirty = true;
static bool s_msg_set;
static char s_msg[96];
static int s_fps = -1;

/* ------------------------------------------------------------------ */
/* state                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    int w, h;
    int list_y;
    int rows;
    int status_y;
    int details_y;
    int footer_y;
    int footer_lines;
} layout_t;

static void compute_layout(layout_t *L)
{
    L->w = display_logical_width();
    L->h = display_logical_height();
    L->list_y = HEADER_H + 4;
    if (L->w >= 300) {
        L->rows = 7;
        L->status_y = 119;
        L->details_y = 129;
        L->footer_y = 142;
        L->footer_lines = 2;
    } else {
        int rows = (L->h - L->list_y - 80) / ROW_H;
        if (rows < 3) rows = 3;
        if (rows > 16) rows = 16;
        L->rows = rows;
        L->status_y = L->list_y + rows * ROW_H + 4;
        L->details_y = L->status_y + 10;
        L->footer_y = L->h - 40;
        L->footer_lines = 3;
    }
}

static int visible_rows(void)
{
    layout_t L;
    compute_layout(&L);
    return L.rows;
}

static void lock(void)
{
    if (s_lock) xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
}

static void unlock(void)
{
    if (s_lock) xSemaphoreGiveRecursive(s_lock);
}

void ui_menu_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateRecursiveMutex();
    s_sel = 0;
    s_top = 0;
    s_dirty = true;
    s_msg_set = false;
    s_msg[0] = 0;
    s_fps = -1;
}

void ui_menu_lock(void) { lock(); }
void ui_menu_unlock(void) { unlock(); }

void ui_menu_refresh(void)
{
    lock();
    s_dirty = true;
    unlock();
}

bool ui_menu_dirty(void)
{
    lock();
    bool d = s_dirty;
    unlock();
    return d;
}

void ui_menu_set_selected(int index)
{
    lock();
    int count = rom_store_count();
    int rows = visible_rows();
    if (count <= 0) {
        s_sel = 0;
        s_top = 0;
    } else {
        if (index < 0) index = 0;
        if (index >= count) index = count - 1;
        s_sel = index;
        if (s_sel < s_top) s_top = s_sel;
        if (s_sel >= s_top + rows) s_top = s_sel - rows + 1;
    }
    s_dirty = true;
    unlock();
}

void ui_menu_move(int delta)
{
    lock();
    int count = rom_store_count();
    int rows = visible_rows();
    if (count > 0) {
        /* Wrap around: with a single BOOT button there is no "up", so the
         * cursor must cycle through the list instead of stopping at the ends. */
        int sel = (s_sel + delta) % count;
        if (sel < 0) sel += count;
        s_sel = sel;
        if (s_sel < s_top) s_top = s_sel;
        if (s_sel >= s_top + rows) s_top = s_sel - rows + 1;
        if (s_top > count - rows) s_top = count > rows ? count - rows : 0;
        s_dirty = true;
    }
    unlock();
}

int ui_menu_selected(void)
{
    lock();
    int sel = s_sel;
    unlock();
    return sel;
}

void ui_menu_show_message(const char *fmt, ...)
{
    lock();
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_msg, sizeof(s_msg), fmt, ap);
    va_end(ap);
    s_msg_set = true;
    s_dirty = true;
    unlock();
    ESP_LOGI(TAG, "message: %s", s_msg);
}

void ui_menu_clear_message(void)
{
    lock();
    s_msg_set = false;
    s_msg[0] = 0;
    s_dirty = true;
    unlock();
}

void ui_menu_set_fps(int fps)
{
    lock();
    if (s_fps != fps) {
        s_fps = fps;
        s_dirty = true;
    }
    unlock();
}

/* ------------------------------------------------------------------ */
/* drawing helpers                                                     */
/* ------------------------------------------------------------------ */

static void text(int x, int y, const char *s, uint8_t fg, uint8_t bg, int scale)
{
    display_ui_text(x, y, s, fg, bg, scale);
}

static void center_text(int w, int y, const char *s, uint8_t fg, uint8_t bg, int scale)
{
    int tw = display_ui_text_width(s, scale);
    int x = (w - tw) / 2;
    if (x < 0) x = 0;
    display_ui_text(x, y, s, fg, bg, scale);
}

static void fmt_size(uint32_t bytes, char *out, size_t n)
{
    if (bytes >= 1024u * 1024u) {
        snprintf(out, n, "%u.%uM", (unsigned)(bytes / (1024u * 1024u)),
                 (unsigned)((bytes / (1024u * 102u)) % 10u));
    } else if (bytes >= 10u * 1024u) {
        snprintf(out, n, "%uK", (unsigned)(bytes / 1024u));
    } else {
        snprintf(out, n, "%uK", (unsigned)((bytes + 1023u) / 1024u));
    }
}

static void draw_header(const layout_t *L)
{
    display_ui_rect(0, 0, L->w, HEADER_H, COL_BAR);
    if (L->w >= 200) {
        text(4, 2, "MiniMario", UI_WHITE, COL_BAR, 2);
    } else {
        text(4, 6, "MiniMario", UI_WHITE, COL_BAR, 1);
    }
    if (L->w >= 300) {
        const char *ver = "v" MINIMARIO_VERSION;
        text(L->w - 4 - display_ui_text_width(ver, 1), 8, ver, UI_LGRAY, COL_BAR, 1);
    }
    display_ui_rect(0, HEADER_H, L->w, 1, UI_DGRAY);
}

static void draw_row(const layout_t *L, int row, int index, const rom_entry_t *e, bool selected)
{
    int y = L->list_y + row * ROW_H;
    uint8_t bg = selected ? UI_LBLUE : UI_BLACK;
    uint8_t fg = selected ? UI_BLACK : UI_WHITE;

    display_ui_rect(0, y, L->w, ROW_H, bg);

    char name[28];
    int name_chars = (L->w - 4 - 24) / 8; /* leave room for "NN " prefix */
    if (name_chars > 24) name_chars = 24;
    if (name_chars < 6) name_chars = 6;
    snprintf(name, sizeof(name), "%.*s", name_chars, e->name);

    char left[40];
    snprintf(left, sizeof(left), "%2d %s", index + 1, name);
    text(4, y + 3, left, fg, bg, 1);

    if (L->w < 300) return;

    char size[12];
    fmt_size(e->size, size, sizeof(size));
    char right[24];
    if (e->too_large) {
        snprintf(right, sizeof(right), "%s !big", size);
    } else {
        snprintf(right, sizeof(right), "%s %s", size,
                 e->source == ROM_SRC_FLASH ? "[flash]" : "[sd]");
    }
    int rx = L->w - 4 - display_ui_text_width(right, 1);
    uint8_t rfg;
    if (selected) {
        rfg = UI_BLACK;
    } else if (e->source == ROM_SRC_FLASH) {
        rfg = UI_LGREEN;
    } else if (e->too_large) {
        rfg = UI_ORANGE;
    } else {
        rfg = UI_LGRAY;
    }
    text(rx, y + 3, right, rfg, bg, 1);
}

static void draw_no_rom_screen(const layout_t *L)
{
    int y = L->list_y + 4;
    center_text(L->w, y, "NO ROM FOUND", UI_RED, UI_BLACK, 2);
    y += 22;
    if (L->w >= 300) {
        center_text(L->w, y, "Copy .nes files to the", UI_WHITE, UI_BLACK, 1);
        y += 10;
        center_text(L->w, y, "root of the TF card, or flash them", UI_WHITE, UI_BLACK, 1);
        y += 10;
        center_text(L->w, y, "into the roms partition with:", UI_WHITE, UI_BLACK, 1);
        y += 14;
        center_text(L->w, y, "tools/flash_rom.py game.nes", UI_YELLOW, UI_BLACK, 1);
        y += 14;
        center_text(L->w, y, "B / 'rescan' to rescan", UI_CYAN, UI_BLACK, 1);
    } else {
        center_text(L->w, y, "Copy .nes to TF card", UI_WHITE, UI_BLACK, 1);
        y += 10;
        center_text(L->w, y, "root, or flash ROMs", UI_WHITE, UI_BLACK, 1);
        y += 10;
        center_text(L->w, y, "tools/flash_rom.py", UI_YELLOW, UI_BLACK, 1);
        y += 14;
        center_text(L->w, y, "B: rescan", UI_CYAN, UI_BLACK, 1);
    }
}

static void draw_status(const layout_t *L, int count)
{
    int heap_kb = (int)(esp_get_free_heap_size() / 1024);
    char pad[16];
    int pads = gamepad_count();
    if (phone_pad_clients() > 0) {
        snprintf(pad, sizeof(pad), "phone");
    } else if (pads > 0) {
        const gamepad_info_t *gi = gamepad_info(0);
        if (gi && gi->connected) {
            snprintf(pad, sizeof(pad), "%.10s", gi->name);
        } else {
            snprintf(pad, sizeof(pad), "1");
        }
    } else if (phone_pad_is_running()) {
        snprintf(pad, sizeof(pad), "wifi");
    } else {
        snprintf(pad, sizeof(pad), "none");
    }
    char fps[12];
    if (s_fps >= 0) {
        snprintf(fps, sizeof(fps), "%d", s_fps);
    } else {
        snprintf(fps, sizeof(fps), "--");
    }

    char line[72];
    if (L->w >= 300) {
        /* Keep "FPS:" right-aligned and clip the left status to the space that
         * is actually left: with "ROM:16/16" the old "HEAP:" line collided with
         * it.  "H:" keeps the heap value and leaves room for PAD + FPS. */
        char left[72];
        snprintf(left, sizeof(left), "SD:%s ROM:%d/%d H:%dK PAD:%s",
                 rom_store_sd_mounted() ? "OK" : "--", count > 0 ? s_sel + 1 : 0, count, heap_kb,
                 pad);
        int fx = L->w - 4 - display_ui_text_width("FPS:", 1) - display_ui_text_width(fps, 1);
        int max_chars = (fx - 8) / 8;
        if (max_chars < 0) max_chars = 0;
        snprintf(line, sizeof(line), "%.*s", max_chars, left);
        text(4, L->status_y, line, UI_LGRAY, UI_BLACK, 1);
        text(fx, L->status_y, "FPS:", UI_DGRAY, UI_BLACK, 1);
        text(fx + display_ui_text_width("FPS:", 1), L->status_y, fps, UI_LGRAY, UI_BLACK, 1);
    } else {
        snprintf(line, sizeof(line), "SD:%s R:%d/%d H:%dK%s", rom_store_sd_mounted() ? "OK" : "--",
                 count > 0 ? s_sel + 1 : 0, count, heap_kb, phone_pad_is_running() ? " W" : "");
        text(4, L->status_y, line, UI_LGRAY, UI_BLACK, 1);
    }
}

static void draw_details(const layout_t *L, int count)
{
    int y = L->details_y;
    int max_chars = (L->w - 8) / 8;
    char line[96];

    if (s_msg_set) {
        snprintf(line, sizeof(line), "%.*s", max_chars, s_msg);
        text(4, y, line, UI_ORANGE, UI_BLACK, 1);
        return;
    }
    if (!rom_store_sd_mounted()) {
        snprintf(line, sizeof(line), "%.*s", max_chars, "SD: no TF card - flash ROMs only");
        text(4, y, line, UI_ORANGE, UI_BLACK, 1);
        return;
    }
    if (count > 0) {
        const rom_entry_t *e = rom_store_get(s_sel);
        if (e) {
            snprintf(line, sizeof(line), "%s%s", e->name,
                     e->source == ROM_SRC_FLASH ? "  [embedded]" : "  [TF card]");
            line[max_chars] = 0;
            text(4, y, line, UI_LGRAY, UI_BLACK, 1);
        }
    }
}

static void draw_footer(const layout_t *L)
{
    int y = L->footer_y;
    if (L->footer_lines >= 3) {
        text(4, y, "A play  B rescan", UI_CYAN, UI_BLACK, 1);
        text(4, y + 10, "BOOT tap=next hold=play", UI_YELLOW, UI_BLACK, 1);
        text(4, y + 20, "SEL+STA quit", UI_CYAN, UI_BLACK, 1);
        return;
    }
    display_ui_rect(0, y - 3, L->w, 1, UI_DGRAY);
    text(4, y, "A/START play   B rescan   SEL+STA quit", UI_CYAN, UI_BLACK, 1);
    text(4, y + 10, "BOOT tap=next hold=play", UI_YELLOW, UI_BLACK, 1);
}

/* ------------------------------------------------------------------ */

void ui_menu_draw(void)
{
    if (!s_lock) return;

    lock();
    layout_t L;
    compute_layout(&L);

    /* Keep the cursor valid in case the ROM list changed. */
    int count = rom_store_count();
    if (count <= 0) {
        s_sel = 0;
        s_top = 0;
    } else {
        if (s_sel >= count) s_sel = count - 1;
        if (s_sel < 0) s_sel = 0;
        if (s_top > s_sel) s_top = s_sel;
        if (s_sel >= s_top + L.rows) s_top = s_sel - L.rows + 1;
        if (s_top > count - L.rows) s_top = count > L.rows ? count - L.rows : 0;
    }

    display_ui_clear(UI_BLACK);
    draw_header(&L);

    if (count == 0) {
        draw_no_rom_screen(&L);
    } else {
        for (int r = 0; r < L.rows; r++) {
            int idx = s_top + r;
            if (idx >= count) break;
            const rom_entry_t *e = rom_store_get(idx);
            if (!e) break;
            draw_row(&L, r, idx, e, idx == s_sel);
        }
    }

    draw_status(&L, count);
    draw_details(&L, count);
    draw_footer(&L);
    display_ui_commit();
    s_dirty = false;
    unlock();
}
