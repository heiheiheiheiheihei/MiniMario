/*
 * MiniMario - landscape menu (320x172) drawn on the display UI canvas.
 *
 * The menu is intentionally dependency free (no LVGL): a header, a ROM list,
 * a status line, a message line and two footer help lines.  The cursor state
 * can be moved from the app task (gamepad) or from the serial console task
 * (up/down/select commands), so all state transitions take s_lock.  Drawing
 * only ever happens in the app task which owns the display UI canvas.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

/* Create the state mutex.  Call once, before ui_menu_move()/ui_menu_draw(). */
void ui_menu_init(void);

/* Navigation - thread safe (console task + app task). */
void ui_menu_move(int delta);
void ui_menu_set_selected(int index);
int ui_menu_selected(void);
void ui_menu_refresh(void); /* mark dirty so the app task redraws */
bool ui_menu_dirty(void);

/* Status/message shown by the next redraw - thread safe. */
void ui_menu_show_message(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void ui_menu_clear_message(void);
void ui_menu_set_fps(int fps); /* last emulator fps, -1 = unknown */

/* Redraw + display_ui_commit().  App task only (uses the UI canvas). */
void ui_menu_draw(void);

/* Recursive lock, used by the console `shot` command to take a consistent
 * snapshot of the display UI canvas while the app task is drawing. */
void ui_menu_lock(void);
void ui_menu_unlock(void);
