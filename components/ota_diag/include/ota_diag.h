#pragma once

#include "esp_err.h"
#include "esp_http_server.h"   // for httpd_handle_t

#ifdef __cplusplus
extern "C" {
#endif

// Register OTA + diagnostics endpoints on an existing httpd server.
// This function does NOT start/stop the http server.
esp_err_t ota_diag_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
