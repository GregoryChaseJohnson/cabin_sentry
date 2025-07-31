#include <string.h>
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "wifi_sta.h"

static const char *TAG = "wifi_sta";

//------------------------------------------------------------------------------
// Static IP Configuration
//------------------------------------------------------------------------------
#define STATIC_IP      "192.168.12.50"   // Adjust this to a free IP in your 192.168.12.x range
#define GATEWAY        "192.168.12.1"    // Your router's IP
#define SUBNET_MASK    "255.255.255.0"
#define DNS_SERVER     "8.8.8.8"         // Use a public DNS (or your router’s IP if preferred)

static void configure_static_ip(esp_netif_t *netif)
{
    esp_netif_ip_info_t ip_info;
    
    ip_info.ip.addr      = esp_ip4addr_aton(STATIC_IP);
    ip_info.gw.addr      = esp_ip4addr_aton(GATEWAY);
    ip_info.netmask.addr = esp_ip4addr_aton(SUBNET_MASK);

    ESP_LOGI(TAG, "Attempting to set Static IP: %s", STATIC_IP);

    if (esp_netif_dhcpc_stop(netif) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to stop DHCP client. Falling back to DHCP.");
    } else if (esp_netif_set_ip_info(netif, &ip_info) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set static IP. Re-enabling DHCP.");
        esp_netif_dhcpc_start(netif);
    } else {
        ESP_LOGI(TAG, "Static IP configured successfully.");
    }

    // Set DNS
    esp_netif_dns_info_t dns;
    dns.ip.u_addr.ip4.addr = esp_ip4addr_aton(DNS_SERVER);
    dns.ip.type = ESP_IPADDR_TYPE_V4;  // Use ESP_IPADDR_TYPE_V4
    esp_err_t err = esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set DNS. SNTP may fail. (err=0x%x)", err);
    } else {
        ESP_LOGI(TAG, "DNS set to %s", DNS_SERVER);
    }
}

//------------------------------------------------------------------------------
// Event Handler (Wi-Fi & IP Events)
//------------------------------------------------------------------------------
static void wifi_event_handler(void* arg, esp_event_base_t base, int32_t id, void* data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Wi-Fi started, attempting to connect...");
        esp_wifi_connect();
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Wi-Fi disconnected, reconnecting...");
        esp_wifi_connect();
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Connected! IP address assigned: " IPSTR,
                 IP2STR(&event->ip_info.ip));
    }
}

//------------------------------------------------------------------------------
// Wi-Fi Initialization with Static IP + DNS
//------------------------------------------------------------------------------
esp_err_t wifi_init_sta(const char *ssid, const char *pass)
{
    esp_err_t ret;

    // Initialize NVS
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize network interface and event loop
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Create default Wi-Fi STA interface
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();

    // Apply static IP configuration (with DNS)
    configure_static_ip(netif);

    // Initialize Wi-Fi with default configuration
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    ESP_ERROR_CHECK(ret);

    // Register event handlers for Wi-Fi and IP events
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    // Set Wi-Fi credentials
    wifi_config_t wifi_config = { 0 };
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password));

    // Start Wi-Fi station
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi initialization completed.");
    return ESP_OK;
}
