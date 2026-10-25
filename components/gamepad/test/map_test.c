/*
 * Host unit test for the MiniMario Bluepad32 -> NES_PAD_* mapping.
 *
 * Build (no ESP-IDF, no hardware):
 *   gcc -std=c11 -Wall -Wextra -Werror \
 *       -I <repo>/components/gamepad -I <repo>/components/common/include \
 *       map_test.c <repo>/components/gamepad/gamepad_map.c -o map_test
 *
 * The input bit values below are hard-coded from Bluepad32's
 * controller/uni_gamepad.h on purpose (independent of gamepad_map_bits.h);
 * gamepad.c has _Static_assert()s that keep the two in sync.
 */
#include <stdint.h>
#include <stdio.h>

#include "gamepad_map.h"
#include "pad.h"

/* uni_gamepad.h: BUTTON_* = BIT(0..9) in enum order. */
#define BP_BUTTON_A          0x0001
#define BP_BUTTON_B          0x0002
#define BP_BUTTON_X          0x0004
#define BP_BUTTON_Y          0x0008
#define BP_BUTTON_SHOULDER_L 0x0010
#define BP_BUTTON_SHOULDER_R 0x0020
#define BP_BUTTON_TRIGGER_L  0x0040
#define BP_BUTTON_TRIGGER_R  0x0080
#define BP_BUTTON_THUMB_L    0x0100
#define BP_BUTTON_THUMB_R    0x0200

/* MISC_BUTTON_* = BIT(0..3): SYSTEM, SELECT, START, CAPTURE */
#define BP_MISC_SYSTEM  0x01
#define BP_MISC_SELECT  0x02
#define BP_MISC_START   0x04
#define BP_MISC_CAPTURE 0x08

/* DPAD_* = BIT(0..3): UP, DOWN, RIGHT, LEFT */
#define BP_DPAD_UP    0x01
#define BP_DPAD_DOWN  0x02
#define BP_DPAD_RIGHT 0x04
#define BP_DPAD_LEFT  0x08

#define AXIS_NEUTRAL 0

static int failures;
static int checks;

#define CHECK_EQ(actual, expected)                                                            \
    do {                                                                                     \
        uint8_t a_ = (uint8_t)(actual);                                                      \
        uint8_t e_ = (uint8_t)(expected);                                                    \
        checks++;                                                                            \
        if (a_ != e_) {                                                                      \
            printf("FAIL %s:%d: got 0x%02x, expected 0x%02x\n", __FILE__, __LINE__, a_, e_); \
            failures++;                                                                      \
        }                                                                                    \
    } while (0)

static uint8_t map(uint16_t buttons, uint8_t misc, uint8_t dpad, int32_t x, int32_t y)
{
    return gamepad_map_raw(buttons, misc, dpad, x, y);
}

int main(void)
{
    /* No input -> no NES buttons. */
    CHECK_EQ(map(0, 0, 0, AXIS_NEUTRAL, AXIS_NEUTRAL), 0);

    /* Face buttons (Xbox layout: A south, B east). */
    CHECK_EQ(map(BP_BUTTON_A, 0, 0, 0, 0), PAD_A);
    CHECK_EQ(map(BP_BUTTON_B, 0, 0, 0, 0), PAD_B);
    CHECK_EQ(map(BP_BUTTON_A | BP_BUTTON_B, 0, 0, 0, 0), PAD_A | PAD_B);
    CHECK_EQ(map(BP_BUTTON_X, 0, 0, 0, 0), 0); /* X/Y unused on NES */
    CHECK_EQ(map(BP_BUTTON_Y, 0, 0, 0, 0), 0);
    CHECK_EQ(map(BP_BUTTON_THUMB_L | BP_BUTTON_THUMB_R, 0, 0, 0, 0), 0);

    /* Start / Select (Xbox Menu / View); SYSTEM (guide) and CAPTURE unused. */
    CHECK_EQ(map(0, BP_MISC_START, 0, 0, 0), PAD_START);
    CHECK_EQ(map(0, BP_MISC_SELECT, 0, 0, 0), PAD_SELECT);
    CHECK_EQ(map(0, BP_MISC_START | BP_MISC_SELECT, 0, 0, 0), PAD_START | PAD_SELECT);
    CHECK_EQ(map(0, BP_MISC_SYSTEM, 0, 0, 0), 0);
    CHECK_EQ(map(0, BP_MISC_CAPTURE, 0, 0, 0), 0);

    /* D-pad. */
    CHECK_EQ(map(0, 0, BP_DPAD_UP, 0, 0), PAD_UP);
    CHECK_EQ(map(0, 0, BP_DPAD_DOWN, 0, 0), PAD_DOWN);
    CHECK_EQ(map(0, 0, BP_DPAD_LEFT, 0, 0), PAD_LEFT);
    CHECK_EQ(map(0, 0, BP_DPAD_RIGHT, 0, 0), PAD_RIGHT);
    CHECK_EQ(map(0, 0, BP_DPAD_UP | BP_DPAD_LEFT, 0, 0), PAD_UP | PAD_LEFT);
    CHECK_EQ(map(0, 0, BP_DPAD_DOWN | BP_DPAD_RIGHT, 0, 0), PAD_DOWN | PAD_RIGHT);

    /* Shoulders / triggers as NES A/B. */
    CHECK_EQ(map(BP_BUTTON_SHOULDER_R, 0, 0, 0, 0), PAD_A);
    CHECK_EQ(map(BP_BUTTON_TRIGGER_R, 0, 0, 0, 0), PAD_A);
    CHECK_EQ(map(BP_BUTTON_SHOULDER_L, 0, 0, 0, 0), PAD_B);
    CHECK_EQ(map(BP_BUTTON_TRIGGER_L, 0, 0, 0, 0), PAD_B);

    /* Left analogue stick: deadzone 256, +Y is down, +X is right. */
    CHECK_EQ(map(0, 0, 0, 0, -512), PAD_UP);
    CHECK_EQ(map(0, 0, 0, 0, 512), PAD_DOWN);
    CHECK_EQ(map(0, 0, 0, -512, 0), PAD_LEFT);
    CHECK_EQ(map(0, 0, 0, 512, 0), PAD_RIGHT);
    CHECK_EQ(map(0, 0, 0, 0, -256), PAD_UP);   /* exactly at deadzone -> active */
    CHECK_EQ(map(0, 0, 0, 0, -255), 0);        /* just inside deadzone -> inactive */
    CHECK_EQ(map(0, 0, 0, 256, 0), PAD_RIGHT);
    CHECK_EQ(map(0, 0, 0, 255, 0), 0);
    CHECK_EQ(map(0, 0, 0, -300, -300), PAD_UP | PAD_LEFT); /* diagonal */

    /* Combined "jump + run + start". */
    CHECK_EQ(map(BP_BUTTON_A | BP_BUTTON_B, BP_MISC_START, BP_DPAD_RIGHT, 0, 0),
             PAD_A | PAD_B | PAD_START | PAD_RIGHT);

    /* High button bits (thumbs) must not leak into the NES mask, but triggers
     * in the same 16-bit field still map to A/B. */
    CHECK_EQ(map(BP_BUTTON_THUMB_L | BP_BUTTON_THUMB_R, 0, 0, 0, 0), 0);
    CHECK_EQ(map(0xffc0, 0, 0, 0, 0), PAD_A | PAD_B);

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
