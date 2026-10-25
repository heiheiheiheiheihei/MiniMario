/*
 * MiniMario - phone controller over Wi-Fi ("phone pad").
 *
 * The board starts a Wi-Fi access point and serves a small touch gamepad page.
 * Phone connects to the AP, opens http://192.168.4.1/ and controls the game
 * (no app installation, works on iPhone and Android).  Input is sent over a
 * WebSocket connection as a one-byte NES_PAD_* mask.
 *
 * Off by default: the Wi-Fi stack costs RAM/CPU, so it is started on demand
 * from the serial console (`wpad on`) and optionally remembered in NVS.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PHONE_PAD_SSID "MiniMario"
#define PHONE_PAD_PASSWORD "minimario"
#define PHONE_PAD_IP "192.168.4.1"
#define PHONE_PAD_URL "http://192.168.4.1/"

/* Start SoftAP + HTTP/WebSocket server.  Idempotent. */
esp_err_t phone_pad_start(void);

/* Stop the server and the Wi-Fi AP.  Idempotent. */
void phone_pad_stop(void);

bool phone_pad_is_running(void);

/* Current NES_PAD_* mask from the phone (0 when no fresh input). */
uint8_t phone_pad_read(void);

/* Number of connected WebSocket clients (0 or 1). */
int phone_pad_clients(void);

/* Human readable status line, e.g. "AP=MiniMario ip=192.168.4.1 client=1 mask=0x00" */
void phone_pad_status(char *buf, size_t len);

/* Self test using the lwIP loopback interface: HTTP GET of the page and a full
 * WebSocket round trip (handshake + one text frame) while verifying that the
 * button mask is applied.  Useful to validate the server without a phone. */
esp_err_t phone_pad_selftest(char *report, size_t len);

#ifdef __cplusplus
}
#endif
