/*
 * MiniMario - bitmask gamepad input abstraction.
 *
 * Backends:
 *  - Bluetooth LE gamepads through Bluepad32 (PS5 DualSense, Xbox Series,
 *    Switch Pro / Joy-Con, 8BitDo in BLE mode, ...).
 *  - Fallback: BOOT button (GPIO9) and serial console key injection, so the
 *    emulator is usable without a controller (and testable over USB).
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "pad.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GAMEPAD_MAX_DEVICES 2

typedef struct {
    bool connected;
    char name[32];
    uint16_t vendor_id;
    uint16_t product_id;
    int8_t battery; /* -1 = unknown, else 0..100 */
    uint8_t buttons; /* NES_PAD_* mask */
} gamepad_info_t;

/* Initialise the Bluetooth stack + polling task and the fallback inputs. */
esp_err_t gamepad_init(void);

/* Latest NES_PAD_* mask. Safe to call from any task. */
uint8_t gamepad_read(void);

/* Number of connected BLE controllers (not counting fallbacks). */
int gamepad_count(void);

/* Information about controller index (0..GAMEPAD_MAX_DEVICES-1). */
const gamepad_info_t *gamepad_info(int index);

/* Inject a synthetic button mask (serial console / tests).  The mask is OR-ed
 * into the returned state until it is changed again. */
void gamepad_inject(uint8_t mask);

/* Enable/disable the BOOT-button fallback (default: enabled). */
void gamepad_set_fallback(bool enabled);

#ifdef __cplusplus
}
#endif
