#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include <ota_diag.h>
#include "wifi_sta.h"

#include <esp_system.h>
#include "esp_camera.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "camera_pins.h"

#ifdef CAM_PIN_PWDN
#undef CAM_PIN_PWDN
#endif
#define CAM_PIN_PWDN 32   // AI‑Thinker ESP32‑CAM PWDN pin

#include "esp_task_wdt.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "camera_status.h"
#include "esp_heap_caps.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_ota_ops.h"
#include "stream_ctrl.h"

bool camera_init_ok = false;
bool camera_server_ok = false;

#define LED_GPIO GPIO_NUM_33
static const char *TAG = "main_app";

/* Camera server tag and configuration */
static const char *CAM_TAG = "esp32-cam Webserver";
#define PART_BOUNDARY "123456789000000000000987654321"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY     = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART         = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

#define CONFIG_XCLK_FREQ 10000000
#define DESIRED_FPS 5
#define DESIRED_FRAME_TIME_MS (1000 / DESIRED_FPS)
#define FILE_SIZE_BUFFER_LENGTH 14
#define FILE_SIZE_CHANGE_THRESHOLD 800

#define TWDT_TIMEOUT_S 5

#define CAM_PIN_RESET   -1
#define CAM_PIN_XCLK    0
#define CAM_PIN_SIOD    26
#define CAM_PIN_SIOC    27
#define CAM_PIN_D7      35
#define CAM_PIN_D6      34
#define CAM_PIN_D5      39
#define CAM_PIN_D4      36
#define CAM_PIN_D3      21
#define CAM_PIN_D2      19
#define CAM_PIN_D1      18
#define CAM_PIN_D0      5
#define CAM_PIN_VSYNC   25
#define CAM_PIN_HREF    23
#define CAM_PIN_PCLK    22

#define DISABLE_ROLLBACK 1

static size_t file_size_buffer[FILE_SIZE_BUFFER_LENGTH] = {0};
static int buffer_index = 1;

/* Camera server helper functions */
static inline void send_detection_notification(void) {
    ESP_LOGI(CAM_TAG, "Motion detected!");
}

static esp_err_t init_camera(void) {
    camera_config_t camera_config = {
        .pin_pwdn      = CAM_PIN_PWDN,
        .pin_reset     = CAM_PIN_RESET,
        .pin_xclk      = CAM_PIN_XCLK,
        .pin_sccb_sda  = CAM_PIN_SIOD,
        .pin_sccb_scl  = CAM_PIN_SIOC,
        .pin_d7        = CAM_PIN_D7,
        .pin_d6        = CAM_PIN_D6,
        .pin_d5        = CAM_PIN_D5,
        .pin_d4        = CAM_PIN_D4,
        .pin_d3        = CAM_PIN_D3,
        .pin_d2        = CAM_PIN_D2,
        .pin_d1        = CAM_PIN_D1,
        .pin_d0        = CAM_PIN_D0,
        .pin_vsync     = CAM_PIN_VSYNC,
        .pin_href      = CAM_PIN_HREF,
        .pin_pclk      = CAM_PIN_PCLK,
        .xclk_freq_hz  = CONFIG_XCLK_FREQ,
        .ledc_timer    = LEDC_TIMER_0,
        .ledc_channel  = LEDC_CHANNEL_0,
        .pixel_format  = PIXFORMAT_JPEG,
        .frame_size    = FRAMESIZE_VGA,
        .jpeg_quality  = 10,
        .fb_count      = 1,
        .fb_location   = CAMERA_FB_IN_PSRAM,
        .grab_mode     = CAMERA_GRAB_WHEN_EMPTY
    };

    esp_err_t err = esp_camera_init(&camera_config);
    if (err == ESP_ERR_NO_MEM || err == ESP_FAIL) {
        camera_config.frame_size = FRAMESIZE_QVGA;
        err = esp_camera_init(&camera_config);
    }
    return err;
}

static inline void update_file_size_buffer(size_t new_size) {
    file_size_buffer[buffer_index] = new_size;
    buffer_index = (buffer_index + 1) % FILE_SIZE_BUFFER_LENGTH;
}

static inline size_t calculate_average_file_size(void) {
    size_t sum = 0;
    for (int i = 0; i < FILE_SIZE_BUFFER_LENGTH; i++) sum += file_size_buffer[i];
    return sum / FILE_SIZE_BUFFER_LENGTH;
}

/* ---- MJPEG handler with real FPS pacing and lower sensor load ---- */
static esp_err_t jpg_stream_httpd_handler(httpd_req_t *req) {
    esp_err_t res = ESP_OK;
    camera_fb_t *fb = NULL;
    size_t _jpg_buf_len = 0;
    uint8_t *_jpg_buf = NULL;

    // Join TWDT safely
    esp_err_t tw = esp_task_wdt_add(NULL);
    if (tw != ESP_OK && tw != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(CAM_TAG, "TWDT add failed: %s", esp_err_to_name(tw));
        return ESP_FAIL;
    }

    // If paused, 503 before any body
    if (stream_paused) {
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send_err(req, 503, "Stream paused for OTA/diag");
        (void)esp_task_wdt_delete(NULL);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);

    const int64_t frame_period_us = (int64_t)DESIRED_FRAME_TIME_MS * 1000;
    int64_t next_deadline_us = esp_timer_get_time() + frame_period_us;
    int64_t last_send_us = 0;

    while (true) {
        // WDT reset (no aborts)
        esp_err_t tr = esp_task_wdt_reset();
        if (tr != ESP_OK && tr != ESP_ERR_INVALID_STATE && tr != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(CAM_TAG, "TWDT reset issue: %s", esp_err_to_name(tr));
        }

        if (stream_paused) {
            ESP_LOGW(CAM_TAG, "Stream paused mid-connection; closing socket.");
            res = ESP_FAIL;
            break;
        }

        // Get a frame (blocks until frame available or already prepared)
        fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGE(CAM_TAG, "Camera capture failed");
            res = ESP_FAIL;
            break;
        }

        // Ensure JPEG buffer
        if (fb->format != PIXFORMAT_JPEG) {
            bool ok = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
            if (!ok) {
                ESP_LOGE(CAM_TAG, "JPEG compression failed");
                esp_camera_fb_return(fb);
                res = ESP_FAIL;
                break;
            }
        } else {
            _jpg_buf_len = fb->len;
            _jpg_buf = fb->buf;
        }

        // Motion detection (unchanged)
        size_t current_jpg_size = _jpg_buf_len;
        update_file_size_buffer(current_jpg_size);
        size_t average_jpg_size = calculate_average_file_size();
        if (abs((int)current_jpg_size - (int)average_jpg_size) > FILE_SIZE_CHANGE_THRESHOLD) {
            ESP_LOGI(CAM_TAG, "Significant change in image size; possible motion");
            send_detection_notification();
        }

        // Multipart send with retries (unchanged)
        char part_buf[64];
        int max_retries = 5;
        int retry_delay = 100; // ms
        for (int retry_count = 0; retry_count < max_retries; ++retry_count) {
            res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
            if (res != ESP_OK) goto handle_send_result;

            int hlen = snprintf(part_buf, sizeof(part_buf), _STREAM_PART, (unsigned)_jpg_buf_len);
            res = httpd_resp_send_chunk(req, part_buf, hlen);
            if (res != ESP_OK) goto handle_send_result;

            res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);

        handle_send_result:
            if (res == ESP_OK) break;

            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                ESP_LOGW(CAM_TAG, "Send EAGAIN/EWOULDBLOCK, retry %d in %d ms", retry_count + 1, retry_delay);
                vTaskDelay(pdMS_TO_TICKS(retry_delay));
                retry_delay <<= 1;
                continue;
            } else {
                ESP_LOGE(CAM_TAG, "Send failed, errno=%d", errno);
                break;
            }
        }

        if (res != ESP_OK) {
            // Clean up frame buffers before leaving the loop
            if (fb) {
                if (fb->format != PIXFORMAT_JPEG) free(_jpg_buf);
                esp_camera_fb_return(fb);
                _jpg_buf = NULL; fb = NULL;
            }
            break;
        }

        // ====== PACE HERE (sleep BEFORE returning fb to halt new capture) ======
        int64_t now_us = esp_timer_get_time();
        int64_t wait_us = next_deadline_us - now_us;
        if (wait_us < 0) wait_us = 0;
        if (wait_us > 0) vTaskDelay(pdMS_TO_TICKS((wait_us + 999) / 1000));

        // After pacing, log effective FPS (full period since last send)
        int64_t after_sleep_us = esp_timer_get_time();
        if (last_send_us != 0) {
            int64_t period_ms = (after_sleep_us - last_send_us) / 1000;
            if (period_ms <= 0) period_ms = 1;
            ESP_LOGI(CAM_TAG, "MJPG: %luKB %lums (%.1ffps)",
                     (unsigned long)(_jpg_buf_len / 1024),
                     (unsigned long)period_ms,
                     1000.0f / (float)period_ms);
        }
        last_send_us = after_sleep_us;

        // ====== NOW release buffers so the next capture starts only after pacing ======
        if (fb->format != PIXFORMAT_JPEG) free(_jpg_buf);
        esp_camera_fb_return(fb);
        _jpg_buf = NULL; fb = NULL;

        // Schedule the next frame time
        next_deadline_us = after_sleep_us + frame_period_us;
    }

    // Leave TWDT safely
    tw = esp_task_wdt_delete(NULL);
    if (tw != ESP_OK && tw != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(CAM_TAG, "TWDT delete failed: %s", esp_err_to_name(tw));
    }
    return res;
}


static httpd_uri_t uri_get = {
    .uri      = "/",
    .method   = HTTP_GET,
    .handler  = jpg_stream_httpd_handler,
    .user_ctx = NULL
};

static httpd_handle_t start_camera_server(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 81;           // camera on 81
    config.ctrl_port   = 32769;        // unique control port
    httpd_handle_t stream_httpd = NULL;
    if (httpd_start(&stream_httpd, &config) == ESP_OK) {
        httpd_register_uri_handler(stream_httpd, &uri_get);
        ESP_LOGI(CAM_TAG, "Camera server started on port %d", config.server_port);
    } else {
        ESP_LOGE(CAM_TAG, "Error starting camera server on port %d", config.server_port);
    }
    return stream_httpd;
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
    if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t* disconn = (wifi_event_sta_disconnected_t*) event_data;
        ESP_LOGE("wifi_debug", "Wi-Fi disconnected. Reason: %d", disconn->reason);
    }
}

static void register_wifi_disconnect_logger(void) {
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &wifi_event_handler, NULL, NULL));
}

void app_main(void) {
    ESP_LOGI(TAG, "App starting. Initializing NVS...");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* Task WDT for long-running HTTP handlers */
    esp_task_wdt_config_t twdt_cfg = {
        .timeout_ms     = TWDT_TIMEOUT_S * 1000,
        .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
        .trigger_panic  = false
    };
    esp_err_t tw = esp_task_wdt_init(&twdt_cfg);
    if (tw != ESP_OK && tw != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(tw);

    if (esp_netif_init() != ESP_OK) ESP_LOGW(TAG, "esp_netif_init failed (continuing)");
    esp_err_t ev = esp_event_loop_create_default();
    if (ev != ESP_OK && ev != ESP_ERR_INVALID_STATE) ESP_LOGW(TAG, "event loop: %s", esp_err_to_name(ev));
    esp_netif_create_default_wifi_sta();
    register_wifi_disconnect_logger();

    wifi_init_config_t wic = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&wic) != ESP_OK) ESP_LOGW(TAG, "wifi_init failed");
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_ps(WIFI_PS_NONE);

    wifi_config_t wcfg = {0};
    strlcpy((char*)wcfg.sta.ssid,     "TMOBILE-2577", sizeof(wcfg.sta.ssid));
    strlcpy((char*)wcfg.sta.password, "65a23267e9",   sizeof(wcfg.sta.password));
    wcfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wcfg.sta.pmf_cfg.capable    = true;
    wcfg.sta.pmf_cfg.required   = false;
    esp_wifi_set_config(WIFI_IF_STA, &wcfg);

    esp_wifi_start();
    esp_wifi_connect();

    /* OTA/Diag server on 80 with unique ctrl_port */
    {
        httpd_handle_t ota_server = NULL;
        httpd_config_t cfg80      = HTTPD_DEFAULT_CONFIG();
        cfg80.server_port         = 80;
        cfg80.ctrl_port           = 32768;
        cfg80.max_open_sockets    = 4;
        if (httpd_start(&ota_server, &cfg80) == ESP_OK) {
            ESP_LOGI(TAG, "OTA/diag server started on port %d", cfg80.server_port);
            ota_diag_register(ota_server);
        } else {
            ESP_LOGE(TAG, "Failed to start OTA/diag server");
        }
    }

    /* Camera server on 81 */
    httpd_handle_t cam_server = start_camera_server();
    camera_server_ok = (cam_server != NULL);

    ESP_LOGI(CAM_TAG, "Initializing camera...");
    ESP_LOGI(TAG, "Heap(8bit)=%u  PSRAM=%u",
             heap_caps_get_free_size(MALLOC_CAP_8BIT),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    camera_init_ok = (init_camera() == ESP_OK);

#if !DISABLE_ROLLBACK
    /* Only validate or rollback if the running image is in PENDING_VERIFY */
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (running
        && esp_ota_get_state_partition(running, &ota_state) == ESP_OK
        && ota_state == ESP_OTA_IMG_PENDING_VERIFY) {

        if (camera_init_ok && camera_server_ok) {
            ESP_LOGI(TAG, "Diagnostics PASSED. Marking app valid.");
            ESP_ERROR_CHECK(esp_ota_mark_app_valid_cancel_rollback());
        } else if (esp_ota_check_rollback_is_possible()) {
            ESP_LOGE(TAG, "Diagnostics FAILED. Rolling back app.");
            ESP_ERROR_CHECK(esp_ota_mark_app_invalid_rollback_and_reboot());
        } else {
            ESP_LOGE(TAG, "Rollback not possible—keeping current app.");
        }
    }
#endif

    ESP_LOGI(TAG, "App running. camera_init_ok=%d, camera_server_ok=%d",
             camera_init_ok, camera_server_ok);
}
