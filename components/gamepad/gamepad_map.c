/*
 * MiniMario - Bluepad32 virtual-gamepad -> NES_PAD_* mapping.
 *
 * Pure C with no ESP-IDF or Bluepad32 dependencies, so the mapping can be
 * unit-tested on the host. `gamepad.c` calls it for every controller report.
 */
#include "gamepad_map.h"

#include <stddef.h>

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

/* USB HID keyboard usage IDs (HID usage page 0x07), as delivered by Bluepad32
 * in uni_keyboard_t.pressed_keys. */
#define KB_UP        0x52
#define KB_DOWN      0x51
#define KB_LEFT      0x50
#define KB_RIGHT     0x4F
#define KB_W         0x1A
#define KB_A_KEY     0x04
#define KB_S         0x16
#define KB_D         0x07
#define KB_Z         0x1D
#define KB_X         0x1B
#define KB_J         0x0D
#define KB_K         0x0E
#define KB_SPACE     0x2C
#define KB_ENTER     0x28
#define KB_BACKSPACE 0x2A
#define KB_ESCAPE    0x29

/* Map a BLE HID keyboard report to a NES_PAD_* mask, so a phone running a
 * "BLE HID keyboard" app (or a real BT keyboard) can drive the emulator:
 *
 *   arrows / WASD        d-pad
 *   Z / J / Space        A
 *   X / K                B
 *   Enter                Start
 *   Backspace / Escape   Select
 */
uint8_t gamepad_map_keyboard(uint8_t modifiers, const uint8_t *keys, int count)
{
    (void)modifiers;
    uint8_t mask = 0;
    if (keys == NULL)
        return 0;

    for (int i = 0; i < count; i++) {
        switch (keys[i]) {
        case KB_UP:
        case KB_W:
            mask |= PAD_UP;
            break;
        case KB_DOWN:
        case KB_S:
            mask |= PAD_DOWN;
            break;
        case KB_LEFT:
        case KB_A_KEY:
            mask |= PAD_LEFT;
            break;
        case KB_RIGHT:
        case KB_D:
            mask |= PAD_RIGHT;
            break;
        case KB_Z:
        case KB_J:
        case KB_SPACE:
            mask |= PAD_A;
            break;
        case KB_X:
        case KB_K:
            mask |= PAD_B;
            break;
        case KB_ENTER:
            mask |= PAD_START;
            break;
        case KB_BACKSPACE:
        case KB_ESCAPE:
            mask |= PAD_SELECT;
            break;
        default:
            break;
        }
    }
    return mask;
}
