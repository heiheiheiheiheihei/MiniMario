/*
 * MiniMario - Bluepad32 virtual-gamepad -> NES_PAD_* mapping.
 *
 * Pure C with no ESP-IDF or Bluepad32 dependencies, so the mapping can be
 * unit-tested on the host. `gamepad.c` calls it for every controller report.
 */
#include "gamepad_map.h"

#include "gamepad_map_bits.h"
#include "pad.h"

/* Left analogue stick -> d-pad conversion (Bluepad32 axis range -512..511). */
#define GAMEPAD_STICK_DEADZONE 256

uint8_t gamepad_map_raw(uint16_t buttons, uint8_t misc_buttons, uint8_t dpad, int32_t axis_x, int32_t axis_y)
{
    uint8_t mask = 0;

    if (buttons & GP_BUTTON_A)
        mask |= PAD_A;
    if (buttons & GP_BUTTON_B)
        mask |= PAD_B;

    /* Shoulders/triggers are convenient NES A/B for platformers. */
    if (buttons & (GP_BUTTON_TRIGGER_R | GP_BUTTON_SHOULDER_R))
        mask |= PAD_A;
    if (buttons & (GP_BUTTON_TRIGGER_L | GP_BUTTON_SHOULDER_L))
        mask |= PAD_B;

    if (misc_buttons & GP_MISC_BUTTON_START)
        mask |= PAD_START;
    if (misc_buttons & GP_MISC_BUTTON_SELECT)
        mask |= PAD_SELECT;

    if (dpad & GP_DPAD_UP)
        mask |= PAD_UP;
    if (dpad & GP_DPAD_DOWN)
        mask |= PAD_DOWN;
    if (dpad & GP_DPAD_LEFT)
        mask |= PAD_LEFT;
    if (dpad & GP_DPAD_RIGHT)
        mask |= PAD_RIGHT;

    /* Analogue stick as a d-pad (HID convention: +Y is down). */
    if (axis_y <= -GAMEPAD_STICK_DEADZONE)
        mask |= PAD_UP;
    if (axis_y >= GAMEPAD_STICK_DEADZONE)
        mask |= PAD_DOWN;
    if (axis_x <= -GAMEPAD_STICK_DEADZONE)
        mask |= PAD_LEFT;
    if (axis_x >= GAMEPAD_STICK_DEADZONE)
        mask |= PAD_RIGHT;

    return mask;
}
