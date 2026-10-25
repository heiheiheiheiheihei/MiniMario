/*
 * MiniMario - phone controller over Wi-Fi: SoftAP + HTTP page + WebSocket.
 *
 * Phone connects to the "MiniMario" access point (password "minimario") and
 * opens http://192.168.4.1/ .  The page shows a touch gamepad; every change is
 * sent as a two-character hex NES_PAD_* mask over a WebSocket.  A keepalive is
 * sent every 100 ms, so a phone that goes away cannot leave a key stuck.
 */
#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "lwip/ip4_addr.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <sys/time.h>
#include <unistd.h>

#include "phone_pad.h"
#include "pad.h"

static const char *TAG = "phone_pad";

/* Embedded page (EMBED_TXTFILES www/index.html). */
extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

#define RX_TIMEOUT_US (1200 * 1000) /* no packet for this long -> treat as released */

static httpd_handle_t s_server;
static esp_netif_t *s_ap_netif;
static bool s_running;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_mask;
static volatile int64_t s_last_rx_us;

/* ------------------------------------------------------------------------- */
/* WebSocket protocol: text frame with two hex digits, e.g. "10" = PAD_UP    */
/* ------------------------------------------------------------------------- */

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "phone connected (ws handshake)");
        s_last_rx_us = esp_timer_get_time();
        return ESP_OK;
    }

    httpd_ws_frame_t pkt = {0};
    uint8_t buf[32];
    pkt.type = HTTPD_WS_TYPE_TEXT;
    pkt.payload = buf;
    esp_err_t err = httpd_ws_recv_frame(req, &pkt, sizeof(buf) - 1);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ws recv: %s", esp_err_to_name(err));
        return err;
    }
    if (pkt.len == 0) {
        s_last_rx_us = esp_timer_get_time();
        return ESP_OK;
    }
    if (pkt.len >= sizeof(buf)) pkt.len = sizeof(buf) - 1;
    buf[pkt.len] = '\0';

    unsigned value = 0;
    if (sscanf((const char *)buf, "%x", &value) == 1) {
        portENTER_CRITICAL(&s_lock);
        s_mask = (uint8_t)value;
        portEXIT_CRITICAL(&s_lock);
        s_last_rx_us = esp_timer_get_time();
    }
    return ESP_OK;
}

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, index_html_start, (ssize_t)(index_html_end - index_html_start));
}

static esp_err_t favicon_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t redirect_handler(httpd_req_t *req)
{
    /* Captive-portal probes (Apple/Android) land here: send them to the pad. */
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", PHONE_PAD_URL);
    return httpd_resp_send(req, NULL, 0);
}

static httpd_uri_t s_uri_index = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = index_handler,
    .user_ctx = NULL,
};
static httpd_uri_t s_uri_ws = {
    .uri = "/ws",
    .method = HTTP_GET,
    .handler = ws_handler,
    .user_ctx = NULL,
    .is_websocket = true,
};
static httpd_uri_t s_uri_favicon = {
    .uri = "/favicon.ico",
    .method = HTTP_GET,
    .handler = favicon_handler,
};
static httpd_uri_t s_uri_hotspot = {
    .uri = "/hotspot-detect.html",
    .method = HTTP_GET,
    .handler = redirect_handler,
};
static httpd_uri_t s_uri_generate = {
    .uri = "/generate_204",
    .method = HTTP_GET,
    .handler = redirect_handler,
};
static httpd_uri_t s_uri_connecttest = {
    .uri = "/connecttest.txt",
    .method = HTTP_GET,
    .handler = redirect_handler,
};

/* ------------------------------------------------------------------------- */
/* start / stop                                                              */
/* ------------------------------------------------------------------------- */

static esp_err_t start_http_server(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 8;
    cfg.max_open_sockets = 3;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 6144;
    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        s_server = NULL;
        return err;
    }
    httpd_register_uri_handler(s_server, &s_uri_index);
    httpd_register_uri_handler(s_server, &s_uri_ws);
    httpd_register_uri_handler(s_server, &s_uri_favicon);
    httpd_register_uri_handler(s_server, &s_uri_hotspot);
    httpd_register_uri_handler(s_server, &s_uri_generate);
    httpd_register_uri_handler(s_server, &s_uri_connecttest);
    return ESP_OK;
}

esp_err_t phone_pad_start(void)
{
    if (s_running) return ESP_OK;

    ESP_LOGI(TAG, "starting Wi-Fi AP '%s' (heap=%u)", PHONE_PAD_SSID,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (s_ap_netif == NULL) return ESP_FAIL;

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wcfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(err));
        goto fail;
    }

    wifi_config_t ap = {0};
    strncpy((char *)ap.ap.ssid, PHONE_PAD_SSID, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = (uint8_t)strlen(PHONE_PAD_SSID);
    strncpy((char *)ap.ap.password, PHONE_PAD_PASSWORD, sizeof(ap.ap.password));
    ap.ap.channel = 6;
    ap.ap.max_connection = 2;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.pmf_cfg.required = false;

    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (err == ESP_OK) err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi start: %s", esp_err_to_name(err));
        goto fail;
    }

    esp_netif_ip_info_t ip = {0};
    if (esp_netif_get_ip_info(s_ap_netif, &ip) == ESP_OK) {
        ESP_LOGI(TAG, "AP up: SSID='%s' pass='%s' ip=" IPSTR " access point ready",
                 PHONE_PAD_SSID, PHONE_PAD_PASSWORD, IP2STR(&ip.ip));
    }

    err = start_http_server();
    if (err != ESP_OK) goto fail;

    portENTER_CRITICAL(&s_lock);
    s_mask = 0;
    portEXIT_CRITICAL(&s_lock);
    s_last_rx_us = 0;
    s_running = true;

    ESP_LOGI(TAG, "phone pad ready: %s (heap=%u)", PHONE_PAD_URL,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    return ESP_OK;

fail:
    if (s_ap_netif) {
        esp_netif_destroy(s_ap_netif);
        s_ap_netif = NULL;
    }
    esp_wifi_stop();
    esp_wifi_deinit();
    return err;
}

void phone_pad_stop(void)
{
    if (!s_running) return;
    s_running = false;
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    esp_wifi_stop();
    esp_wifi_deinit();
    if (s_ap_netif) {
        esp_netif_destroy(s_ap_netif);
        s_ap_netif = NULL;
    }
    portENTER_CRITICAL(&s_lock);
    s_mask = 0;
    portEXIT_CRITICAL(&s_lock);
    s_last_rx_us = 0;
    ESP_LOGI(TAG, "phone pad stopped (heap=%u)", (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
}

bool phone_pad_is_running(void) { return s_running; }

uint8_t phone_pad_read(void)
{
    if (!s_running) return 0;

    int64_t last = s_last_rx_us;
    if (last == 0 || esp_timer_get_time() - last > RX_TIMEOUT_US) return 0;

    portENTER_CRITICAL(&s_lock);
    uint8_t mask = s_mask;
    portEXIT_CRITICAL(&s_lock);
    return mask;
}

int phone_pad_clients(void)
{
    if (!s_running) return 0;
    int64_t last = s_last_rx_us;
    return (last != 0 && esp_timer_get_time() - last <= RX_TIMEOUT_US) ? 1 : 0;
}


/* ------------------------------------------------------------------------- */
/* self test (no phone needed):                       */
/*   - HTTP GET / over the lwIP loopback interface -> page is served          */
/*   - WebSocket handshake on /ws + one masked text frame -> mask is applied  */
/* This verifies the whole server/WS input path from inside the firmware.     */
/* ------------------------------------------------------------------------- */

static int http_get_self(int *status_out, int *len_out, char *body, size_t body_len)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct timeval tv = {.tv_sec = 2, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(80);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }

    const char *req = "GET / HTTP/1.1\r\nHost: 192.168.4.1\r\nConnection: close\r\n\r\n";
    if (send(fd, req, strlen(req), 0) < 0) {
        close(fd);
        return -1;
    }

    /* Read until the end of the HTTP headers, then the body. */
    char buf[1024];
    int have = 0;
    int hdr_end = -1;
    int status = 0;
    while (have < (int)sizeof(buf) - 1) {
        int n = recv(fd, buf + have, sizeof(buf) - 1 - have, 0);
        if (n <= 0) break;
        have += n;
        buf[have] = 0;
        char *p = strstr(buf, "\r\n\r\n");
        if (p) {
            hdr_end = (int)(p - buf) + 4;
            break;
        }
    }
    if (hdr_end < 0) {
        close(fd);
        return -1;
    }
    sscanf(buf, "HTTP/%*d.%*d %d", &status);
    *status_out = status;

    int total = have - hdr_end;
    if (body && total > 0) {
        int cp = total;
        if (cp > (int)body_len - 1) cp = (int)body_len - 1;
        memcpy(body, buf + hdr_end, cp);
        body[cp] = 0;
    } else if (body) {
        body[0] = 0;
    }
    for (;;) {
        int n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        if (body && total + n < (int)body_len) {
            memcpy(body + total, buf, n);
            body[total + n] = 0;
        } else if (body && total < (int)body_len - 1) {
            memcpy(body + total, buf, (int)body_len - 1 - total);
            body[body_len - 1] = 0;
        }
        total += n;
    }
    *len_out = total;
    close(fd);
    return 0;
}

static int ws_frame_self(uint8_t mask_value)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct timeval tv = {.tv_sec = 2, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(80);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }

    const char *req =
        "GET /ws HTTP/1.1\r\n"
        "Host: 192.168.4.1\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";
    if (send(fd, req, strlen(req), 0) < 0) {
        close(fd);
        return -1;
    }

    char resp[512] = {0};
    int got = 0;
    while (got < (int)sizeof(resp) - 1) {
        int n = recv(fd, resp + got, sizeof(resp) - 1 - got, 0);
        if (n <= 0) break;
        got += n;
        if (strstr(resp, "\r\n\r\n")) break;
    }
    if (!strstr(resp, "101")) {
        close(fd);
        return -1;
    }

    /* Client-to-server frame must be masked: FIN|TEXT, MASK|len=2, key, data */
    static const char hex[] = "0123456789abcdef";
    char p0 = hex[(mask_value >> 4) & 0x0f];
    char p1 = hex[mask_value & 0x0f];
    uint8_t frame[8] = {0x81, 0x82, 0x11, 0x22, 0x33, 0x44, 0, 0};
    frame[6] = (uint8_t)p0 ^ 0x11;
    frame[7] = (uint8_t)p1 ^ 0x22;
    if (send(fd, frame, sizeof(frame), 0) < 0) {
        close(fd);
        return -1;
    }

    vTaskDelay(pdMS_TO_TICKS(200));
    uint8_t applied = phone_pad_read();
    close(fd);
    return (applied == mask_value) ? 0 : -1;
}

esp_err_t phone_pad_selftest(char *report, size_t len)
{
    if (!s_running) {
        if (report) snprintf(report, len, "wpad is off");
        return ESP_ERR_INVALID_STATE;
    }

    int status = 0, body_len = 0;
    static char body[2048];

    if (http_get_self(&status, &body_len, body, sizeof(body)) != 0) {
        if (report) snprintf(report, len, "FAIL: http connect on loopback (is 127.0.0.1 loopback enabled?)");
        return ESP_FAIL;
    }
    bool page_ok = (status == 200) && (strstr(body, "MiniMario Pad") != NULL);
    int page_size = (int)(index_html_end - index_html_start);

    bool ws_ok = false;
    if (ws_frame_self(0xF0) == 0) {
        vTaskDelay(pdMS_TO_TICKS(1400)); /* let the keepalive timeout expire */
        ws_ok = (phone_pad_read() == 0);
    }

    if (report) {
        snprintf(report, len,
                 "http status=%d page=%s (%d/%d bytes) websocket=%s keepalive=%s",
                 status, page_ok ? "ok" : "BAD", body_len, page_size,
                 ws_ok ? "ok" : "BAD", ws_ok ? "ok" : "BAD");
    }
    return (page_ok && ws_ok) ? ESP_OK : ESP_FAIL;
}

void phone_pad_status(char *buf, size_t len)
{
    if (buf == NULL || len == 0) return;
    snprintf(buf, len, "wpad=%s ssid=%s pass=%s url=%s client=%d mask=0x%02x heap=%u",
             s_running ? "on" : "off", PHONE_PAD_SSID, PHONE_PAD_PASSWORD, PHONE_PAD_URL,
             phone_pad_clients(), phone_pad_read(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
}
