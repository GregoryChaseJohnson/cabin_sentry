// ota_diag.c — full updated file

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
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "camera_status.h"

#ifdef CONFIG_EXAMPLE_USE_CERT_BUNDLE
#include "esp_crt_bundle.h"
#endif
#if CONFIG_EXAMPLE_CONNECT_WIFI
#include "esp_wifi.h"
#endif

#include "cJSON.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "stream_ctrl.h"

static const char *TAG = "enhanced_ota";

volatile bool stream_paused = false;

extern const uint8_t server_cert_pem_start[] asm("_binary_ca_cert_pem_start");
extern const uint8_t server_cert_pem_end[]   asm("_binary_ca_cert_pem_end");

static char ota_status[32] = "idle";

// -------- OTA LED Logic --------------------------
#define OTA_LED_GPIO       GPIO_NUM_33
#define OTA_LED_ACTIVE_LOW 1

static TaskHandle_t  s_ota_led_task      = NULL;
static bool          s_ota_blink_started = false;

static inline void ota_led_set(int on) {
    gpio_set_level(OTA_LED_GPIO, OTA_LED_ACTIVE_LOW ? !on : on);
}

static void ota_led_init(void) {
    gpio_reset_pin(OTA_LED_GPIO);
    gpio_set_direction(OTA_LED_GPIO, GPIO_MODE_OUTPUT);
    ota_led_set(0);
}

static void ota_led_task(void *arg) {
    while (1) {
        ota_led_set(1);
        vTaskDelay(pdMS_TO_TICKS(50));
        ota_led_set(0);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void ota_led_start(void) {
    if (!s_ota_led_task) {
        xTaskCreate(ota_led_task, "ota_led_task", 2048, NULL, 2, &s_ota_led_task);
    }
}

static void ota_led_stop(void) {
    if (s_ota_led_task) {
        vTaskDelete(s_ota_led_task);
        s_ota_led_task = NULL;
    }
    ota_led_set(0);
}
// --------------------------------------------------

// Forward declarations
static esp_err_t handle_trigger_ota(httpd_req_t *req);
static esp_err_t handle_trigger_diag(httpd_req_t *req);

//---------------------------------------------------------------------
// Public API
//---------------------------------------------------------------------
esp_err_t ota_diag_register(httpd_handle_t server)
{
    if (!server) {
        ESP_LOGE(TAG, "ota_diag_register: server handle is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    ota_led_init();

    httpd_uri_t ota_uri = {
        .uri      = "/trigger_ota",
        .method   = HTTP_GET,
        .handler  = handle_trigger_ota,
        .user_ctx = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ota_uri));

    httpd_uri_t diag_uri = {
        .uri      = "/trigger_diag",
        .method   = HTTP_GET,
        .handler  = handle_trigger_diag,
        .user_ctx = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &diag_uri));

    ESP_LOGI(TAG, "OTA/diag endpoints registered on shared HTTP server");
    return ESP_OK;
}

//---------------------------------------------------------------------
// Diagnostics Functions
//---------------------------------------------------------------------
static char *collect_diagnostics(const char *ota_status_str)
{
    if (!ota_status_str) ota_status_str = "";
    uint64_t uptime    = esp_timer_get_time() / 1000000ULL;
    int      free_heap = esp_get_free_heap_size();

    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    cJSON_AddStringToObject(root, "device_id", "esp32_001");

    time_t now; time(&now);
    char time_str[32] = "unknown";
    struct tm ti;
    if (gmtime_r(&now, &ti)) {
        strftime(time_str, sizeof(time_str), "%Y-%m-%dT%H:%M:%SZ", &ti);
    }
    cJSON_AddStringToObject(root, "timestamp", time_str);
    cJSON_AddStringToObject(root, "ota_status", ota_status_str);

    cJSON *metrics = cJSON_CreateObject();
    cJSON_AddNumberToObject(metrics, "uptime", uptime);
    cJSON_AddNumberToObject(metrics, "free_heap", free_heap);
    cJSON_AddNumberToObject(metrics, "temperature", 0.0);
    cJSON_AddItemToObject(root, "metrics", metrics);

    cJSON_AddItemToObject(root, "errors", cJSON_CreateArray());
    cJSON_AddBoolToObject(root, "camera_init_ok",   camera_init_ok);
    cJSON_AddBoolToObject(root, "camera_server_ok", camera_server_ok);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json_str;
}

static void send_diagnostics(const char *json_payload)
{
    if (!json_payload) {
        ESP_LOGE(TAG, "send_diagnostics: NULL payload");
        return;
    }
    esp_http_client_config_t cfg = {
        .url        = "http://192.168.12.125:8071/diagnostics",
        .method     = HTTP_METHOD_POST,
        .timeout_ms = 5000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client for diagnostics");
        return;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json_payload, strlen(json_payload));
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Diagnostics sent successfully");
    } else {
        ESP_LOGE(TAG, "Failed to send diagnostics: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
}

static esp_err_t handle_trigger_diag(httpd_req_t *req)
{
    stream_paused = true; 
    ESP_LOGI(TAG, "Received /trigger_diag request");
    char *diag = collect_diagnostics("on_demand");
    if (!diag) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Diagnostics collection failed");
        stream_paused = false;   // Resume stream on error
        return ESP_FAIL;
    }
    send_diagnostics(diag);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, diag, HTTPD_RESP_USE_STRLEN);
    free(diag);
    stream_paused = false;       // Resume stream after normal completion
    return err;
}


//---------------------------------------------------------------------
// HTTP event handler for OTA progress
//---------------------------------------------------------------------
static esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (!s_ota_blink_started) {
                ota_led_start();
                s_ota_blink_started = true;
            }
            break;
        case HTTP_EVENT_ERROR:
            ota_led_stop();
            s_ota_blink_started = false;
            break;
        default:
            break;
    }
    return ESP_OK;
}

//---------------------------------------------------------------------
// Background OTA task
//---------------------------------------------------------------------
static void simple_ota_example_task(void *pvParameter)
{
    ESP_LOGI(TAG, "Starting OTA task...");
    strcpy(ota_status, "in_progress");
    ota_led_start();

#ifndef CONFIG_EXAMPLE_FIRMWARE_UPGRADE_URL
    ESP_LOGE(TAG, "CONFIG_EXAMPLE_FIRMWARE_UPGRADE_URL is not set");
    ota_led_stop();
    strcpy(ota_status, "failed");
    vTaskDelete(NULL);
    return;
#endif

    esp_http_client_config_t cfg = {
        .url               = CONFIG_EXAMPLE_FIRMWARE_UPGRADE_URL,
#ifdef CONFIG_EXAMPLE_USE_CERT_BUNDLE
        .crt_bundle_attach = esp_crt_bundle_attach,
#else
        .cert_pem          = (const char *)server_cert_pem_start,
#endif
        .event_handler     = _http_event_handler,
        .keep_alive_enable = true,
    };

    esp_https_ota_config_t ota_cfg = { .http_config = &cfg };
    esp_err_t ret = esp_https_ota(&ota_cfg);

    ota_led_stop();
    bool ok = (ret == ESP_OK);
    strcpy(ota_status, ok ? "success" : "failed");
    ESP_LOGI(TAG, "OTA result: %s", ok ? "success" : "failed");

    if (ok) {
        vTaskDelay(pdMS_TO_TICKS(700));
        esp_restart();
    }

    vTaskDelete(NULL);
}

//---------------------------------------------------------------------
// OTA handler (ASYNC)
//---------------------------------------------------------------------
//---------------------------------------------------------------------
// OTA handler (streamed): first sends {"status":"queued"}, then {"status":"success"/"failed"}
//---------------------------------------------------------------------
static esp_err_t handle_trigger_ota(httpd_req_t *req)
{
    stream_paused = true; 
    ESP_LOGI(TAG, "HTTP GET /trigger_ota (stream)");

    // 1) tell client we queued the job
    httpd_resp_set_type(req, "application/json");
    const char *queued = "{\"status\":\"queued\"}\n";
    httpd_resp_send_chunk(req, queued, strlen(queued));

    // 2) do the OTA
    strcpy(ota_status, "in_progress");
    ota_led_start();

#ifndef CONFIG_EXAMPLE_FIRMWARE_UPGRADE_URL
    ESP_LOGE(TAG, "CONFIG_EXAMPLE_FIRMWARE_UPGRADE_URL not set");
    ota_led_stop();
    strcpy(ota_status, "failed");
    const char *err0 = "{\"status\":\"failed\"}\n";
    httpd_resp_send_chunk(req, err0, strlen(err0));
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_FAIL;
#endif

    esp_http_client_config_t cfg = {
        .url               = CONFIG_EXAMPLE_FIRMWARE_UPGRADE_URL,
#ifdef CONFIG_EXAMPLE_USE_CERT_BUNDLE
        .crt_bundle_attach = esp_crt_bundle_attach,
#else
        .cert_pem          = (const char *)server_cert_pem_start,
#endif
        .event_handler     = _http_event_handler,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota_cfg = { .http_config = &cfg };
    esp_err_t ret = esp_https_ota(&ota_cfg);

    // 3) finish up and send final status
    ota_led_stop();
    bool ok = (ret == ESP_OK);
    strcpy(ota_status, ok ? "success" : "failed");

    char buf[64];
    int n = snprintf(buf, sizeof(buf), "{\"status\":\"%s\"}\n", ok ? "success" : "failed");
    httpd_resp_send_chunk(req, buf, n);

    // 4) end chunked response
    httpd_resp_send_chunk(req, NULL, 0);

    // 5) reboot if OK
    if (ok) {
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
    return ESP_OK;
}
