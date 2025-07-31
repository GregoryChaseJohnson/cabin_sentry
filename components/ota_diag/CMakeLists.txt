idf_component_register(SRCS "ota_diag.c"
                       INCLUDE_DIRS  "." "include" 
                       PRIV_REQUIRES esp_event app_update esp_http_client esp_https_server esp_netif nvs_flash esp_wifi json esp_timer esp_https_ota driver)
