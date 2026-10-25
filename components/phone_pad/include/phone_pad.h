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

#define PHONE_PAD_SSID "MiniMario-Pad"
#define PHONE_PAD_PASSWORD "minimario" /* only used when WPA2 mode is enabled */
#define PHONE_PAD_IP "192.168.4.1"
#define PHONE_PAD_URL "http://192.168.4.1/"

/* Start SoftAP + HTTP/WebSocket server.  Idempotent.
 * `use_password == false` starts an OPEN network (default: easiest to join,
 * no WPA2 handshake), `true` uses PHONE_PAD_PASSWORD (WPA2-PSK). */
esp_err_t phone_pad_start_ex(bool use_password);
esp_err_t phone_pad_start(void); /* = phone_pad_start_ex(false) */

/* Stop the server and the Wi-Fi AP.  Idempotent. */
void phone_pad_stop(void);

bool phone_pad_is_running(void);

/* Current NES_PAD_* mask from the phone (0 when no fresh input). */
uint8_t phone_pad_read(void);

/* Number of connected WebSocket clients (0 or 1). */
int phone_pad_clients(void);

/* Connected Wi-Fi stations (phones joined to the AP). */
int phone_pad_stations(void);

/* Human readable status line, e.g. "AP=MiniMario ip=192.168.4.1 client=1 mask=0x00" */
void phone_pad_status(char *buf, size_t len);

/* Self test using the lwIP loopback interface: HTTP GET of the page and a full
 * WebSocket round trip (handshake + one text frame) while verifying that the
 * button mask is applied.  Useful to validate the server without a phone. */
esp_err_t phone_pad_selftest(char *report, size_t len);

/* Runtime Wi-Fi driver debug logs (association diagnostics). */
void phone_pad_set_debug(bool on);

/* Scan from the board itself and describe all visible APs (diagnostics). */
esp_err_t phone_pad_scan(char *report, size_t len);

/* Simulate the phone holding an NES_PAD_* mask for `ms` milliseconds, through
 * the real WebSocket input path (loopback).  For automated tests. */
esp_err_t phone_pad_press(uint8_t mask, int ms);

#ifdef __cplusplus
}
#endif
