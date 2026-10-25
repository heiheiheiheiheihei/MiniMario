/*
 * MiniMario - bit values of Bluepad32's virtual gamepad.
 *
 * `gamepad_map.c` must stay free of ESP-IDF/Bluepad32 includes so that the
 * button mapping can be unit-tested on a normal PC (see /tmp mapping test).
 * These values must match `controller/uni_gamepad.h` from Bluepad32;
 * `gamepad.c` has compile-time asserts that check they still do.
 */
#pragma once

/* uni_gamepad.h: enum { BUTTON_A = BIT(0), BUTTON_B = BIT(1), ... } */
#define GP_BUTTON_A          (1u << 0)
#define GP_BUTTON_B          (1u << 1)
#define GP_BUTTON_X          (1u << 2)
#define GP_BUTTON_Y          (1u << 3)
#define GP_BUTTON_SHOULDER_L (1u << 4)
#define GP_BUTTON_SHOULDER_R (1u << 5)
#define GP_BUTTON_TRIGGER_L  (1u << 6)
#define GP_BUTTON_TRIGGER_R  (1u << 7)
#define GP_BUTTON_THUMB_L    (1u << 8)
#define GP_BUTTON_THUMB_R    (1u << 9)

#define GP_MISC_BUTTON_SYSTEM  (1u << 0)
#define GP_MISC_BUTTON_SELECT  (1u << 1)
#define GP_MISC_BUTTON_START   (1u << 2)
#define GP_MISC_BUTTON_CAPTURE (1u << 3)

#define GP_DPAD_UP    (1u << 0)
#define GP_DPAD_DOWN  (1u << 1)
#define GP_DPAD_RIGHT (1u << 2)
#define GP_DPAD_LEFT  (1u << 3)
