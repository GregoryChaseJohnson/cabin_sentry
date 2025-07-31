idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES ota_diag
    PRIV_REQUIRES  esp32-camera esp_timer wifi_sta esp_wifi esp_https_server esp_driver_gpio nvs_flash app_update esp_http_client esp_https_ota
)
