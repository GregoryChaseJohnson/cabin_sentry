idf_component_register(SRCS "wifi_sta.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_wifi  wifi_sta nvs_flash)