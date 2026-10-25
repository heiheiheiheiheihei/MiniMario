/*
 * MiniMario - private header for the host-testable gamepad mapping.
 *
 * Kept separate from the public `include/gamepad.h` (which needs esp_err.h)
 * so a plain `gcc gamepad_map.c` can unit-test it.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Map one Bluepad32 `uni_gamepad_t` report (field values, not the struct) to a
 * NES_PAD_* mask.  -512..511 analogue axes, bitfields documented in
 * `gamepad_map_bits.h`. */
uint8_t gamepad_map_raw(uint16_t buttons, uint8_t misc_buttons, uint8_t dpad, int32_t axis_x, int32_t axis_y);

#ifdef __cplusplus
}
#endif
