#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "protocol_examples_common.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_https_server.h" // For HTTPS server

#ifdef CONFIG_EXAMPLE_USE_CERT_BUNDLE
#include "esp_crt_bundle.h"
#endif

#if CONFIG_EXAMPLE_CONNECT_WIFI
#include "esp_wifi.h"
#endif

#define HASH_LEN 32
#define OTA_URL_SIZE 256
static const char *TAG = "enhanced_ota";

extern const uint8_t server_cert_pem_start[] asm("_binary_ca_cert_pem_start");
extern const uint8_t server_cert_pem_end[]   asm("_binary_ca_cert_pem_end");

static void initialize_sntp(void)
{
    ESP_LOGI(TAG, "Initializing SNTP");
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org"); // or your favorite NTP server
    sntp_init();
}

static void wait_for_time_sync(void)
{
    time_t now = 0;
    struct tm timeinfo = { 0 };
    int retry = 0;
    const int retry_max = 20;
    while (timeinfo.tm_year < (2021 - 1900) && ++retry < retry_max) {
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_max);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        time(&now);
        localtime_r(&now, &timeinfo);
    }
    if (retry == retry_max) {
        ESP_LOGW(TAG, "Failed to synchronize time via SNTP");
    } else {
        ESP_LOGI(TAG, "Time is set now");
    }
}

static esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ERROR:
        ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
        break;
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
        break;
    case HTTP_EVENT_HEADER_SENT:
    case HTTP_EVENT_ON_HEADER:
    case HTTP_EVENT_ON_DATA:
    case HTTP_EVENT_ON_FINISH:
    case HTTP_EVENT_DISCONNECTED:
    case HTTP_EVENT_REDIRECT:
        // For brevity, omit the logs or replicate them as needed
        break;
    }
    return ESP_OK;
}

static void simple_ota_example_task(void *pvParameter)
{
    ESP_LOGI(TAG, "Starting OTA example task");

    esp_http_client_config_t config = {
        .url = CONFIG_EXAMPLE_FIRMWARE_UPGRADE_URL,
#ifdef CONFIG_EXAMPLE_USE_CERT_BUNDLE
        .crt_bundle_attach = esp_crt_bundle_attach,
#else
        .cert_pem = (char *)server_cert_pem_start,
#endif
        .event_handler = _http_event_handler,
        .keep_alive_enable = true,
    };

    ESP_LOGI(TAG, "Attempting to download update from %s", config.url);
    esp_https_ota_config_t ota_config = {
        .http_config = &config,
    };

    esp_err_t ret = esp_https_ota(&ota_config);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA Succeeded, Rebooting...");
        esp_restart();
    } else {
        ESP_LOGE(TAG, "Firmware upgrade failed");
    }

    vTaskDelete(NULL); // End this task on failure
}

static bool is_midnight(void)
{
    // A simple function to check if current local time is ~00:00
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    // For example, check if hour == 0 and minute == 0
    // Maybe allow a small window, e.g. minute <= 1
    if (timeinfo.tm_hour == 0 && timeinfo.tm_min == 0) {
        return true;
    }
    return false;
}

static void midnight_update_task(void *pvParam)
{
    while (1) {
        if (is_midnight()) {
            ESP_LOGI(TAG, "It's midnight! Starting OTA check...");
            xTaskCreate(&simple_ota_example_task, "ota_update_task", 8192, NULL, 5, NULL);

            // Wait 1 minute to avoid triggering multiple times
            vTaskDelay(60000 / portTICK_PERIOD_MS);
        }
        // Check time every 30 seconds, for example
        vTaskDelay(30000 / portTICK_PERIOD_MS);
    }
}

static esp_err_t handle_trigger_ota(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Manual OTA trigger via HTTP endpoint");
    // Start the OTA task (non-blocking)
    xTaskCreate(&simple_ota_example_task, "manual_ota_task", 8192, NULL, 5, NULL);

    const char *resp_str = "OTA Triggered. Device will update if new firmware is found.\n";
    httpd_resp_send(req, resp_str, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static httpd_handle_t start_https_server(void)
{
    httpd_ssl_config_t conf = HTTPD_SSL_CONFIG_DEFAULT();
    // Optionally set conf.cacert_buf = ... if you want client verification
    // Or conf.servercert, conf.serverkey if you have them in embeded
    // For brevity, we'll do an unsecure HTTP or use internal default self-signed

    // If you prefer plain HTTP server:
    // httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    // httpd_handle_t server = NULL;
    // if (httpd_start(&server, &config) == ESP_OK) { ... }

    // For HTTPS example, check if you have certs or define them.
    // We'll do a fallback to HTTP if you don't have actual certs.

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t trigger_uri = {
            .uri       = "/trigger_ota",
            .method    = HTTP_GET,
            .handler   = handle_trigger_ota,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &trigger_uri);
        ESP_LOGI(TAG, "HTTP server started. Access /trigger_ota for manual update");
    }
    return server;
}

void app_main(void)
{
    ESP_LOGI(TAG, "Enhanced OTA example app_main start");

    // 1) Initialize NVS
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // 2) Print partition info if desired
    // get_sha256_of_partitions(); // from your old code

    // 3) Initialize TCP/IP, events, connect to Wi-Fi
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(example_connect());

#if CONFIG_EXAMPLE_CONNECT_WIFI
    // Force Wi-Fi high performance
    esp_wifi_set_ps(WIFI_PS_NONE);
#endif

    // 4) SNTP init to get correct time
    initialize_sntp();
    wait_for_time_sync();

    // 5) Start local HTTPS (or HTTP) server for manual triggers
    start_https_server();

    // 6) Create a task to check for OTA updates at midnight
    xTaskCreate(&midnight_update_task, "midnight_update_task", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Setup done. Now waiting for midnight or manual trigger for OTA update.");
}
