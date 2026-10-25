/*
 * MiniMario - BLE gamepad input backend (Bluepad32 + BTstack).
 *
 * The ESP32-C6 has a Bluetooth LE 5 controller only (no BR/EDR), so we run
 * Bluepad32 on top of BTstack in "controller only" mode:
 *
 *   ESP-IDF BT controller (VHCI) -> BTstack -> Bluepad32 -> this file
 *
 * Bluepad32 is callback driven; it runs its own BTstack run loop in a
 * dedicated FreeRTOS task created by gamepad_init().  Everything else in the
 * emulator just calls gamepad_read(), which is cheap and task safe.
 */
#include "gamepad.h"

#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

/* Bluepad32 / BTstack public headers */
#include "btstack_port_esp32.h"
#include "btstack_run_loop.h"
#include "uni.h"

/* Pure mapping (no ESP-IDF/Bluepad32 deps, host unit-tested). */
#include "gamepad_map.h"
#include "gamepad_map_bits.h"

static const char *TAG = "gamepad";

/* ---- Fallback input ---------------------------------------------------- */

/* Waveshare ESP32-C6-LCD-1.47: the BOOT button is wired to GPIO9 (active
 * low, board has an external pull-up; the internal pull-up is enabled too). */
#ifndef GAMEPAD_BOOT_GPIO
#define GAMEPAD_BOOT_GPIO 9
#endif

/* ---- Bluetooth task ---------------------------------------------------- */

#define GAMEPAD_BT_TASK_STACK 8192
#define GAMEPAD_BT_TASK_PRIO  10
#define GAMEPAD_BT_READY_TIMEOUT_MS 8000

/* Number of controller slots Bluepad32 was built with.  Must be >= the
 * GAMEPAD_MAX_DEVICES exposed in gamepad.h (sdkconfig: CONFIG_BLUEPAD32_MAX_DEVICES=2). */
#define GP_SLOTS CONFIG_BLUEPAD32_MAX_DEVICES

#if GP_SLOTS < GAMEPAD_MAX_DEVICES
#error "CONFIG_BLUEPAD32_MAX_DEVICES must be >= GAMEPAD_MAX_DEVICES"
#endif

/* `gamepad_map.c` uses plain numbers so it can be host-tested: make sure they
 * still match Bluepad32's uni_gamepad.h bit layout. */
_Static_assert(BUTTON_A == GP_BUTTON_A, "Bluepad32 BUTTON_A changed");
_Static_assert(BUTTON_B == GP_BUTTON_B, "Bluepad32 BUTTON_B changed");
_Static_assert(BUTTON_SHOULDER_L == GP_BUTTON_SHOULDER_L, "Bluepad32 BUTTON_SHOULDER_L changed");
_Static_assert(BUTTON_SHOULDER_R == GP_BUTTON_SHOULDER_R, "Bluepad32 BUTTON_SHOULDER_R changed");
_Static_assert(BUTTON_TRIGGER_L == GP_BUTTON_TRIGGER_L, "Bluepad32 BUTTON_TRIGGER_L changed");
_Static_assert(BUTTON_TRIGGER_R == GP_BUTTON_TRIGGER_R, "Bluepad32 BUTTON_TRIGGER_R changed");
_Static_assert(MISC_BUTTON_SELECT == GP_MISC_BUTTON_SELECT, "Bluepad32 MISC_BUTTON_SELECT changed");
_Static_assert(MISC_BUTTON_START == GP_MISC_BUTTON_START, "Bluepad32 MISC_BUTTON_START changed");
_Static_assert(DPAD_UP == GP_DPAD_UP, "Bluepad32 DPAD_UP changed");
_Static_assert(DPAD_DOWN == GP_DPAD_DOWN, "Bluepad32 DPAD_DOWN changed");
_Static_assert(DPAD_LEFT == GP_DPAD_LEFT, "Bluepad32 DPAD_LEFT changed");
_Static_assert(DPAD_RIGHT == GP_DPAD_RIGHT, "Bluepad32 DPAD_RIGHT changed");

/* ---- State ------------------------------------------------------------- */

typedef enum {
    SLOT_KIND_NONE = 0,
    SLOT_KIND_GAMEPAD,
    SLOT_KIND_KEYBOARD,
} slot_kind_t;

typedef struct {
    bool connected;
    uint8_t kind;       /* slot_kind_t */
    uint8_t buttons;    /* last NES_PAD_* mask (gamepad or keyboard) */
    char name[32];
    uint16_t vendor_id;
    uint16_t product_id;
    int8_t battery;
} slot_state_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static slot_state_t s_slots[GP_SLOTS];       /* guarded by s_lock            */
static gamepad_info_t s_info[GAMEPAD_MAX_DEVICES]; /* public snapshot        */
static uint8_t s_ble_mask;                   /* guarded by s_lock            */

static volatile uint8_t s_inject_mask;
static volatile bool s_fallback_enabled = true;
static bool s_boot_gpio_ok;
static bool s_initialized;
static SemaphoreHandle_t s_bt_ready_sem;

/* Diagnostics */
static uint32_t s_ble_up_at_ms;

/* ---- Helpers ----------------------------------------------------------- */

static uint8_t slot_mask_unlocked(void)
{
    uint8_t mask = 0;
    for (int i = 0; i < GP_SLOTS; i++) {
        if (s_slots[i].connected)
            mask |= s_slots[i].buttons;
    }
    return mask;
}

static void slot_clear(int slot)
{
    if (slot < 0 || slot >= GP_SLOTS)
        return;
    portENTER_CRITICAL(&s_lock);
    memset(&s_slots[slot], 0, sizeof(s_slots[slot]));
    s_slots[slot].battery = -1;
    s_ble_mask = slot_mask_unlocked();
    if (slot < GAMEPAD_MAX_DEVICES) {
        memset(&s_info[slot], 0, sizeof(s_info[slot]));
        s_info[slot].battery = -1;
    }
    portEXIT_CRITICAL(&s_lock);
}

static int8_t battery_to_percent(uint8_t battery)
{
    /* Bluepad32: 0 = empty, 254 = full, 255 = not available. */
    if (battery == 255)
        return -1;
    if (battery >= 254)
        return 100;
    return (int8_t)((battery * 100 + 127) / 254);
}

/* Map a Bluepad32 virtual gamepad to an NES_PAD_* bitmask. */
static uint8_t map_gamepad(const uni_gamepad_t *gp)
{
    return gamepad_map_raw(gp->buttons, gp->misc_buttons, gp->dpad, gp->axis_x, gp->axis_y);
}

static void configure_boot_gpio(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << GAMEPAD_BOOT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&io) == ESP_OK) {
        s_boot_gpio_ok = true;
        ESP_LOGI(TAG, "fallback: BOOT button on GPIO%d -> NES A", GAMEPAD_BOOT_GPIO);
    } else {
        ESP_LOGW(TAG, "fallback: could not configure GPIO%d", GAMEPAD_BOOT_GPIO);
    }
}

static void log_heap(const char *where)
{
    ESP_LOGI(TAG, "heap %s: free=%u internal=%u dma=%u min=%u", where,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
}

/* ---- Bluepad32 platform callbacks -------------------------------------- */

static void platform_init(int argc, const char **argv)
{
    (void)argc;
    (void)argv;
    ESP_LOGI(TAG, "Bluepad32 custom platform 'MiniMario' init");
}

static void platform_on_init_complete(void)
{
    s_ble_up_at_ms = (uint32_t)(esp_log_timestamp());
    log_heap("after BT init");

    /* Safe to call BTstack functions here (we are on the BTstack thread). */
    uni_bt_start_scanning_and_autoconnect_unsafe();
    uni_bt_allow_incoming_connections(true);

    ESP_LOGI(TAG, "Bluetooth LE up at %u ms, scanning + autoconnect started",
             (unsigned)s_ble_up_at_ms);

    if (s_bt_ready_sem)
        xSemaphoreGive(s_bt_ready_sem);
}

static uni_error_t platform_on_device_discovered(bd_addr_t addr, const char *name, uint16_t cod, uint8_t rssi)
{
    (void)addr;
    (void)rssi;

    /* Mice are useless for a NES pad; keyboards are kept because a phone
     * running a "BLE HID keyboard" app can drive the emulator with one. */
    if (cod != 0 && (cod & UNI_BT_COD_MINOR_MASK & UNI_BT_COD_MINOR_MICE)) {
        ESP_LOGI(TAG, "ignoring mouse: %s", name ? name : "?");
        return UNI_ERROR_IGNORE_DEVICE;
    }

    ESP_LOGI(TAG, "HID device discovered: %s (%s), rssi=%d", name ? name : "?",
             bd_addr_to_str(addr), (int)rssi);
    return UNI_ERROR_SUCCESS;
}

static void platform_on_device_connected(uni_hid_device_t *d)
{
    if (d == NULL) {
        ESP_LOGW(TAG, "device connected: NULL");
        return;
    }
    ESP_LOGI(TAG, "controller connected (idx=%d), heap=%u", uni_hid_device_get_idx_for_instance(d),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
}

static void platform_on_device_disconnected(uni_hid_device_t *d)
{
    int slot = d ? uni_hid_device_get_idx_for_instance(d) : -1;
    ESP_LOGI(TAG, "controller disconnected (idx=%d), heap=%u", slot,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    slot_clear(slot);
}

static uni_error_t platform_on_device_ready(uni_hid_device_t *d)
{
    if (d == NULL)
        return UNI_ERROR_IGNORE_DEVICE;

    int slot = uni_hid_device_get_idx_for_instance(d);
    if (slot < 0) {
        ESP_LOGE(TAG, "device ready with invalid slot");
        return UNI_ERROR_IGNORE_DEVICE;
    }

    uint16_t vid = uni_hid_device_get_vendor_id(d);
    uint16_t pid = uni_hid_device_get_product_id(d);
    const char *name = uni_gamepad_get_model_name(d->controller_type);
    if (name == NULL || strcmp(name, "Unknown") == 0) {
        const char *guessed = uni_guess_controller_name(vid, pid);
        if (guessed)
            name = guessed;
    }
    if (name == NULL)
        name = "BLE controller";

    portENTER_CRITICAL(&s_lock);
    slot_state_t *st = &s_slots[slot];
    st->connected = true;
    st->kind = SLOT_KIND_NONE;
    st->buttons = 0;
    strncpy(st->name, name, sizeof(st->name) - 1);
    st->name[sizeof(st->name) - 1] = '\0';
    st->vendor_id = vid;
    st->product_id = pid;
    st->battery = -1;
    s_ble_mask = slot_mask_unlocked();
    if (slot < GAMEPAD_MAX_DEVICES) {
        gamepad_info_t *info = &s_info[slot];
        memset(info, 0, sizeof(*info));
        info->connected = true;
        strncpy(info->name, st->name, sizeof(info->name) - 1);
        info->vendor_id = vid;
        info->product_id = pid;
        info->battery = -1;
    }
    portEXIT_CRITICAL(&s_lock);

    /* Light the player LED if the protocol supports it (nice pairing cue). */
    if (d->report_parser.set_player_leds)
        d->report_parser.set_player_leds(d, GAMEPAD_SEAT_A);

    ESP_LOGI(TAG, "controller ready: slot=%d '%s' vid=0x%04x pid=0x%04x type=%d", slot, name, vid, pid,
             (int)d->controller_type);
    return UNI_ERROR_SUCCESS;
}

static void platform_on_controller_data(uni_hid_device_t *d, uni_controller_t *ctl)
{
    if (d == NULL || ctl == NULL)
        return;

    int slot = uni_hid_device_get_idx_for_instance(d);
    if (slot < 0 || slot >= GP_SLOTS)
        return;

    uint8_t mask = 0;
    uint8_t kind = SLOT_KIND_NONE;

    if (ctl->klass == UNI_CONTROLLER_CLASS_GAMEPAD) {
        mask = map_gamepad(&ctl->gamepad);
        kind = SLOT_KIND_GAMEPAD;
    } else if (ctl->klass == UNI_CONTROLLER_CLASS_KEYBOARD) {
        /* BLE HID keyboards: lets a phone running a "BLE HID keyboard" app (or
         * any Bluetooth keyboard) drive the emulator.  See gamepad_map.c for
         * the key mapping. */
        mask = gamepad_map_keyboard(ctl->keyboard.modifiers, ctl->keyboard.pressed_keys,
                                    UNI_KEYBOARD_PRESSED_KEYS_MAX);
        kind = SLOT_KIND_KEYBOARD;
    } else {
        /* Mice / balance boards: not used. */
        portENTER_CRITICAL(&s_lock);
        if (s_slots[slot].kind != SLOT_KIND_NONE) {
            s_slots[slot].kind = SLOT_KIND_NONE;
            s_slots[slot].buttons = 0;
            s_ble_mask = slot_mask_unlocked();
        }
        portEXIT_CRITICAL(&s_lock);
        return;
    }

    int8_t battery = battery_to_percent(ctl->battery);

    portENTER_CRITICAL(&s_lock);
    slot_state_t *st = &s_slots[slot];
    st->connected = true;
    st->kind = kind;
    st->buttons = mask;
    st->battery = battery;
    s_ble_mask = slot_mask_unlocked();
    if (slot < GAMEPAD_MAX_DEVICES) {
        s_info[slot].connected = true;
        s_info[slot].buttons = mask;
        s_info[slot].battery = battery;
    }
    portEXIT_CRITICAL(&s_lock);
}

static void platform_on_oob_event(uni_platform_oob_event_t event, void *data)
{
    switch (event) {
    case UNI_PLATFORM_OOB_GAMEPAD_SYSTEM_BUTTON:
        ESP_LOGI(TAG, "system button pressed");
        break;
    case UNI_PLATFORM_OOB_BLUETOOTH_ENABLED:
        ESP_LOGI(TAG, "Bluetooth discovery/scanning %s", data ? "enabled" : "disabled");
        break;
    default:
        break;
    }
}

static const uni_property_t *platform_get_property(uni_property_idx_t idx)
{
    (void)idx;
    return NULL;
}

static struct uni_platform s_platform = {
    .name = "MiniMario",
    .init = platform_init,
    .on_init_complete = platform_on_init_complete,
    .on_device_discovered = platform_on_device_discovered,
    .on_device_connected = platform_on_device_connected,
    .on_device_disconnected = platform_on_device_disconnected,
    .on_device_ready = platform_on_device_ready,
    .on_controller_data = platform_on_controller_data,
    .get_property = platform_get_property,
    .on_oob_event = platform_on_oob_event,
};

/* ---- BTstack thread ---------------------------------------------------- */

static void gamepad_bt_task(void *arg)
{
    (void)arg;

    /* Configure BTstack for the ESP32 VHCI controller. */
    btstack_init();

    /* Must be called before uni_init(). */
    uni_platform_set_custom(&s_platform);

    uni_init(0, NULL);

    /* Does not return: processes HCI events and Bluepad32 timers. */
    btstack_run_loop_execute();

    ESP_LOGE(TAG, "BTstack run loop returned, deleting task");
    vTaskDelete(NULL);
}

/* ---- Public API -------------------------------------------------------- */

esp_err_t gamepad_init(void)
{
    if (s_initialized)
        return ESP_OK;

    log_heap("before BT init");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
        ESP_LOGW(TAG, "nvs_flash_init: %s (BT pairing keys may not persist)", esp_err_to_name(err));

    configure_boot_gpio();

    s_bt_ready_sem = xSemaphoreCreateBinary();
    if (s_bt_ready_sem == NULL) {
        ESP_LOGE(TAG, "out of memory creating BT ready semaphore");
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(gamepad_bt_task, "bluepad32", GAMEPAD_BT_TASK_STACK, NULL, GAMEPAD_BT_TASK_PRIO, NULL) !=
        pdPASS) {
        ESP_LOGE(TAG, "out of memory creating BT task");
        vSemaphoreDelete(s_bt_ready_sem);
        s_bt_ready_sem = NULL;
        return ESP_ERR_NO_MEM;
    }

    if (xSemaphoreTake(s_bt_ready_sem, pdMS_TO_TICKS(GAMEPAD_BT_READY_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Bluetooth stack not ready after %d ms (continuing with fallback input)",
                 GAMEPAD_BT_READY_TIMEOUT_MS);
    } else {
        ESP_LOGI(TAG, "Bluetooth gamepad backend ready");
    }
    vSemaphoreDelete(s_bt_ready_sem);
    s_bt_ready_sem = NULL;

    log_heap("after BT ready");
    s_initialized = true;
    return ESP_OK;
}

uint8_t gamepad_read(void)
{
    uint8_t mask;

    portENTER_CRITICAL(&s_lock);
    mask = s_ble_mask;
    portEXIT_CRITICAL(&s_lock);

    mask |= s_inject_mask;

    /* Cheap BOOT-button fallback: one GPIO read per call (~60 Hz). */
    if (s_fallback_enabled && s_boot_gpio_ok && gpio_get_level(GAMEPAD_BOOT_GPIO) == 0)
        mask |= PAD_A;

    return mask;
}

int gamepad_count(void)
{
    int count = 0;
    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < GP_SLOTS; i++) {
        if (s_slots[i].connected)
            count++;
    }
    portEXIT_CRITICAL(&s_lock);
    return count;
}

const gamepad_info_t *gamepad_info(int index)
{
    if (index < 0 || index >= GAMEPAD_MAX_DEVICES)
        return NULL;
    return &s_info[index];
}

void gamepad_inject(uint8_t mask)
{
    s_inject_mask = mask;
}

void gamepad_pause_scan(bool pause)
{
    if (pause) {
        uni_bt_stop_scanning_safe();
        ESP_LOGI(TAG, "BLE scanning paused");
    } else {
        uni_bt_start_scanning_and_autoconnect_safe();
        ESP_LOGI(TAG, "BLE scanning resumed");
    }
}

void gamepad_set_fallback(bool enabled)
{
    s_fallback_enabled = enabled;
}
