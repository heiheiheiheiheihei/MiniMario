# gamepad - wireless input for MiniMario

NES joypad input for the Waveshare ESP32-C6-LCD-1.47, with wireless controllers
as the primary source and two fallbacks so the firmware is always playable.

Public API: [`include/gamepad.h`](include/gamepad.h) (frozen; `NES_PAD_*` bits
from `components/common/include/pad.h`).

| Backend | Status | Notes |
|---|---|---|
| Bluetooth LE gamepads (Bluepad32 + BTstack) | primary | **BLE only**, see compatibility below |
| BOOT button (GPIO9) | fallback | active low, mapped to NES `A` |
| `gamepad_inject(mask)` | fallback | used by the serial console / automation |

`gamepad_read()` returns the OR of all connected BLE gamepads, the injected
mask and (unless disabled with `gamepad_set_fallback(false)`) the BOOT button.
It is safe to call from any task and costs one critical section plus one GPIO
read, so it can be called every frame.

---

## What actually works on the ESP32-C6 (important!)

The ESP32-C6 has **Bluetooth LE 5 only - no Bluetooth Classic (BR/EDR)**.
Most gamepads on the market are BR/EDR and therefore **cannot ever work** on
this board, no matter what firmware is used:

| Controller | Protocol | Works on C6? |
|---|---|---|
| Xbox Wireless (Xbox One S / Series X\|S, firmware **5.x or newer**) | BLE | **yes** |
| Xbox Adaptive | BLE | **yes** |
| Steam Controller (with Bluetooth LE firmware update) | BLE | **yes** |
| Stadia Controller (with Bluetooth LE firmware update) | BLE | **yes** |
| Generic BLE HID (HOGP) gamepads, some Android pads | BLE | **usually** |
| PS5 DualSense | BR/EDR | no |
| PS4 DualShock 4 / PS3 DualShock 3 | BR/EDR | no |
| Nintendo Switch Pro / Joy-Con | BR/EDR | no |
| Nintendo Wii Remote / Wii U Pro / Balance Board | BR/EDR | no |
| 8BitDo (all classic modes: Switch/XInput/DInput/macOS) | BR/EDR | no |
| Older Xbox controllers (firmware 3.x/4.x) | BR/EDR | no |

**Recommended controller: an Xbox Series X|S or Xbox One S controller updated
to firmware 5.x+.** Everything else in the "not supported" list is a hardware
protocol limitation of the C6, not a software limitation.

More details in Bluepad32's docs: `docs/FAQ.md` and
`docs/supported_gamepads.md` in the Bluepad32 repository (the WiFi/BT
compatibility table is authoritative).

**Workaround for BR/EDR pads:** the serial console can inject any input with
`gamepad_inject()`; a PC-side bridge (USB/Bluetooth pad on the host ->
`key ...` commands over `/dev/ttyACM0`) can be used to play with a controller
the C6 cannot talk to directly (see `tools/` and the project README).

## Pairing

There is no pairing menu: Bluepad32 scans on boot and connects automatically
to supported controllers that are in pairing / advertising mode, and remembers
the link keys in NVS so the same controller reconnects after a reboot.

| Controller | How to put it in pairing mode |
|---|---|
| Xbox Series X\|S / One S (fw 5.x+) | Press and hold the **Pair** button (top edge, next to USB) until the Xbox logo flashes quickly. Make sure it is not connected to a console/PC (turn that device off or forget the controller). |
| Steam Controller (BLE fw) | Hold **Y** + Steam button until the LED blinks (see Steam's BLE update FAQ). |
| Stadia Controller (BLE fw) | Hold **Stadia** + **Y** until the LED blinks. |
| Generic BLE HID pad | Usual pairing combo (often **Home/PS/Xbox** + a face button, or a dedicated pair button). |

Put the controller in pairing mode *after* the firmware has booted (or before -
both work, the device keeps scanning until the first gamepad connects).

## Button mapping

Bluepad32 exposes a virtual gamepad with the standard Xbox button positions
(`BUTTON_A` = south, `BUTTON_B` = east, ...). The mapping to the NES pad is
implemented once in [`gamepad_map.c`](gamepad_map.c) (pure C, host tested):

| Bluepad32 input | NES button |
|---|---|
| `BUTTON_A` (south: Xbox **A**) | `PAD_A` |
| `BUTTON_B` (east: Xbox **B**) | `PAD_B` |
| `MISC_BUTTON_START` (Xbox **Menu**) | `PAD_START` |
| `MISC_BUTTON_SELECT` (Xbox **View/Back**) | `PAD_SELECT` |
| `DPAD_UP/DOWN/LEFT/RIGHT` | `PAD_UP/DOWN/LEFT/RIGHT` |
| Left stick (deadzone 256/512) | `PAD_UP/DOWN/LEFT/RIGHT` |
| `BUTTON_SHOULDER_R` / `BUTTON_TRIGGER_R` (RB/RT) | `PAD_A` (convenience) |
| `BUTTON_SHOULDER_L` / `BUTTON_TRIGGER_L` (LB/LT) | `PAD_B` (convenience) |
| BOOT button (GPIO9) | `PAD_A` (fallback) |
| `gamepad_inject(mask)` | any `NES_PAD_*` bits (fallback) |

Multi-controller: up to `GAMEPAD_MAX_DEVICES` (2) controllers are merged with a
logical OR; the first one controls the game. `gamepad_info(0..1)` reports
name / VID / PID / battery per slot. The mapping works for any controller that
Bluepad32's generic HID/HOGP parser can read, not just the named families.

### Host unit test (no hardware)

The mapping is deliberately free of ESP-IDF/Bluepad32 dependencies, so it can
be validated on a PC:

```sh
cd components/gamepad/test
./run_host_test.sh          # 34 checks: buttons, start/select, dpad, stick, triggers
```

`gamepad.c` additionally `_Static_assert()`s that the bit values used by the
host-tested mapping still match Bluepad32's `uni_gamepad.h`, so a Bluepad32
upgrade cannot silently break it.

## Memory

Bluepad32 + BTstack + the ESP-IDF BLE controller run in one extra FreeRTOS
task ("bluepad32", 8 KiB stack, priority 10). Measured on the board with the
trimmed configuration below:

| Point | Free heap |
|---|---|
| after `display_init()` + UI canvas (55 KB) | 311,936 B |
| after `gamepad_init()` returns (BT up, BLE scanning) | **275,376 B** (-36,560 B) |
| emulator phase (UI canvas released) | **330,420 B** |
| BT task stack high-water-mark | 5,036 B free of 8,192 B |

Static RAM: `libbluepad32` .bss 13.0 KB + `libbtstack` .bss 8.9 KB in the full
firmware (Bluepad32 and BTstack use static pools on purpose; the dynamic part is
the 36.5 KB above, which includes the 8 KB task stack and the ESP-IDF BLE
controller allocations).  Total Bluetooth footprint in the integrated firmware
is therefore ~58.5 KB (21.9 KB static + 36.6 KB dynamic).  The 330 KB
emulator-phase heap above was measured without the emulator linked; the full
firmware also has the 57 KB NES framebuffer and nofrendo tables in .bss, which
are static and not part of the heap.  In the integrated MiniMario firmware the
Lead measured ~183 KB free heap right after `gamepad_init()` (menu phase, UI
canvas alive), with the input path (`key ...` console injection, `pad`, BLE
stack logs) verified end-to-end.

Scanning stops automatically once a controller connects and resumes when it
disconnects, so the radio does not burn CPU during play. A fixed integer-math
benchmark on the application core measured 603,343 cycles before BT init and
603,342 cycles while BLE scanning: the idle/scanning stack steals no measurable
CPU. A connected controller sends ~4-10 HID reports/s of a few dozen bytes.

RAM tuning applied (on top of Bluepad32 defaults):
* `CONFIG_BT_LE_*` buffer counts reduced (see below) - saves ~6 KB of heap.
* BTstack's `HCI_HOST_ACL_PACKET_NUM` 20 -> 8 in the vendored
  `third_party/bluepad32/src/components/btstack/include/btstack_config.h` -
  saves ~6 KB of static RAM (BLE HID never needs 20 ACL buffers).

If even more RAM is ever needed, `GAMEPAD_BT_TASK_STACK` can be lowered to 5120
(measured peak use is ~3.2 KB), and `Gamepad` logging can be disabled - but
the current numbers leave ~330 KB for the emulator, above the 318 KB target.

## Integration (what the firmware build needs)

The Bluepad32/BTstack sources are vendored under
`third_party/bluepad32/src/components/` (Bluepad32 v4.2.0 + BTstack
`5d4d8cc`, BTstack patched with Bluepad32's `external/patches`, integrated as
an ESP-IDF component with `integrate_btstack.py`). No network access is needed
at build time.

1. Top-level `CMakeLists.txt`, before `include($ENV{IDF_PATH}/tools/cmake/project.cmake)`:

   ```cmake
   set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/bluepad32/src/components)
   ```

2. `main/CMakeLists.txt`: add `gamepad` to `REQUIRES`. The `gamepad` component
   itself requires `bluepad32` + `btstack` (and `common`, `driver`, `nvs_flash`).

3. `sdkconfig.defaults` (required - BTstack replaces the ESP-IDF host):

   ```
   # Bluetooth: Bluepad32 v4.2.0 (BTstack is the host; ESP32-C6 is BLE-only)
   CONFIG_BT_ENABLED=y
   CONFIG_BT_CONTROLLER_ONLY=y
   CONFIG_BT_LE_MAX_CONNECTIONS=3
   CONFIG_BLUEPAD32_PLATFORM_CUSTOM=y
   CONFIG_BLUEPAD32_MAX_DEVICES=2
   # CONFIG_BLUEPAD32_USB_CONSOLE_ENABLE is not set
   CONFIG_BTSTACK_AUDIO=n

   # ESP32-C6 BLE controller RAM trimming (512 KB SRAM, no PSRAM, HID traffic only)
   CONFIG_BT_LE_ACL_BUF_COUNT=6
   CONFIG_BT_LE_HCI_EVT_HI_BUF_COUNT=12
   CONFIG_BT_LE_HCI_EVT_LO_BUF_COUNT=4
   CONFIG_BT_LE_MSYS_1_BLOCK_COUNT=8
   CONFIG_BT_LE_MSYS_2_BLOCK_COUNT=12
   ```

   Do **not** enable `CONFIG_BLUEPAD32_USB_CONSOLE_ENABLE`: it starts its own
   UART REPL, which conflicts with the project serial console (it also fails to
   compile for the USB-Serial-JTAG-only console). Audio must stay disabled on
   the C6 (BTstack issue bluekitchen/btstack#599).

4. `gamepad_init()` should be called once after `nvs_flash_init()` (it can be
   called before or after `display_init()`). It returns after the BT stack is
   up, or after an 8 s timeout, so a Bluetooth failure never blocks boot.

## Troubleshooting

* **No controller connects / no "HID device discovered" in the log**:
  the controller is almost certainly BR/EDR-only (see compatibility table), or
  it is still connected to a console/phone, or it is not in pairing mode.
* **Xbox controller with firmware 3.x/4.x**: update it with the Xbox
  Accessories app on a Windows PC/console; BLE needs firmware 5.x+.
* **Expected boot log lines**:
  ```
  Bluepad32 (C) 2016-2025 ... Version: v4.2.0
  BR/EDR support: disabled
  BLE support: enabled
  BLE_INIT: ble controller commit:...
  BTstack up and running at xx:xx:xx:xx:xx:xx
  gamepad: heap after BT init: free=...
  gamepad: Bluetooth LE up at ... ms, scanning + autoconnect started
  gamepad: Bluetooth discovery/scanning enabled
  ```
  and when a controller is found:
  ```
  gamepad: HID device discovered: ...
  gamepad: controller connected ...
  gamepad: controller ready: slot=0 'Xbox Wireless Controller' vid=0x045e pid=0x0b13 ...
  ```
  `Failed command: HCI_EVENT_COMMAND_COMPLETE: opcode = 0x0c05 - status=1` is
  expected and harmless: it is the BR/EDR-only `Write_Simple_Pairing_Mode`
  command that the BLE-only C6 controller rejects (Bluepad32 sends it for
  compatibility and continues).
* **Controller no longer reconnects**: link keys live in NVS. `idf.py
  erase-flash` clears them (also clears all settings).
* **A previously configured controller connects to another host**: turn that
  host off or remove the controller from it; BLE controllers only accept one
  host at a time.
* **Nothing happens at all**: the BOOT button (GPIO9) is NES `A`, and the
  serial console can inject any mask with the `key` command
  (`gamepad_inject()`), so the emulator is testable without any controller.

## References

* Bluepad32 v4.2.0 <https://github.com/ricardoquesada/bluepad32> (Apache-2.0)
* BTstack <https://github.com/bluekitchen/btstack> (see its LICENSE)
* Bluepad32 ESP32 integration notes: `docs/plat_esp32.md` upstream

## BLE HID keyboard support (phone as a controller, Android)

`gamepad.c` also accepts `UNI_CONTROLLER_CLASS_KEYBOARD` devices.  This lets an
Android phone running a "BLE HID keyboard" app (the phone advertises itself as a
Bluetooth keyboard) drive the NES -- no custom app needed on the board side.

| Key | NES |
|-----|-----|
| Arrow keys / W A S D | d-pad |
| Z / J / Space | A |
| X / K | B |
| Enter | Start |
| Backspace / Esc | Select |

The mapping lives in `gamepad_map.c:gamepad_map_keyboard()` and is covered by
the host test (`test/run_host_test.sh`, 44 checks).  Keyboards and gamepads can
be connected at the same time; their masks are OR-ed together.  iOS cannot act
as a Bluetooth HID peripheral for third-party apps, so this path is Android-only.
