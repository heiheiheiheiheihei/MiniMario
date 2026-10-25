# MiniMario phone controller over Wi-Fi (SoftAP + web page + WebSocket).
idf_component_register(
    SRCS "phone_pad.c"
    INCLUDE_DIRS "include"
    EMBED_TXTFILES "www/index.html"
    REQUIRES common
    PRIV_REQUIRES esp_wifi esp_netif esp_http_server esp_timer lwip
)
