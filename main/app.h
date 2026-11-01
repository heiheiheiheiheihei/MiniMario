/*
 * MiniMario - application shell: boot flow, menu <-> emulator switching,
 * settings (NVS) and the request interface used by the serial console.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    APP_MODE_BOOT = 0,
    APP_MODE_MENU = 1,
    APP_MODE_EMU = 2,
} app_mode_t;

/* Boot and then run the application forever (called from app_main()). */
void app_run(void) __attribute__((noreturn));

app_mode_t app_get_mode(void);
bool app_is_emulating(void);

/* Human readable reason for the last failed play request (static buffer). */
const char *app_last_error(void);

/* Requests from the console task.  Each returns false when not applicable
 * (e.g. `play` while the emulator is already running). */
bool app_request_play(int index);              /* -1 = current menu selection */
bool app_request_play_path(const char *path);
bool app_request_menu(void);                   /* stop emulator / redraw menu */
bool app_request_rescan(void);
bool app_request_test_pattern(void);
bool app_request_clear(void);
uint32_t app_rescan_seq(void);                 /* bumps after each rescan */

/* Display settings, persisted in NVS.  Safe to call from the console task. */
void app_set_rotation(int rot);   /* 0..3 */
void app_set_scale(int scale);    /* 0..2 */
void app_set_backlight(int pct);  /* 0..100 */
void app_set_bgr(int enable);     /* 0|1 colour order fallback */
int app_get_rotation(void);
int app_get_scale(void);
int app_get_bgr(void);
void app_set_wswap(int enable); /* RGB565 byte order on the wire */
int app_get_wswap(void);

/* Inject a BOOT-button event (serial console testing only): 1 = tap, 2 = hold. */
void app_boot_simulate(int kind);

/* Phone controller over Wi-Fi (SoftAP + web page).  Persisted in NVS. */
void app_set_wpad(int enable);    /* 0|1 */
int app_get_wpad(void);
void app_set_wpad_wpa2(int enable); /* 1 = WPA2-PSK, 0 = open network (default) */
int app_get_wpad_wpa2(void);
