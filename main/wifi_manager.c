#include "wifi_manager.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"

static const char* TAG = "bridge_wifi";
static char bridge_ssid[20];
static bool started;

esp_err_t wifi_manager_start(const BridgeCredentials* credentials) {
    if(!credentials || started) return credentials ? ESP_ERR_INVALID_STATE : ESP_ERR_INVALID_ARG;
    uint8_t mac[6];
    esp_err_t error = esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    if(error != ESP_OK) return error;
    snprintf(bridge_ssid, sizeof(bridge_ssid), "Flipforge-%02X%02X", mac[4], mac[5]);

    error = esp_netif_init();
    if(error != ESP_OK && error != ESP_ERR_INVALID_STATE) return error;
    error = esp_event_loop_create_default();
    if(error != ESP_OK && error != ESP_ERR_INVALID_STATE) return error;
    if(!esp_netif_create_default_wifi_ap()) return ESP_FAIL;

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    error = esp_wifi_init(&init);
    if(error != ESP_OK) return error;

    wifi_config_t config = {0};
    memcpy(config.ap.ssid, bridge_ssid, strlen(bridge_ssid));
    config.ap.ssid_len = strlen(bridge_ssid);
    memcpy(config.ap.password, credentials->ap_password, strlen(credentials->ap_password));
    config.ap.channel = 6U;
    config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    config.ap.max_connection = 2U;
    config.ap.pmf_cfg.capable = true;
    config.ap.pmf_cfg.required = false;

    error = esp_wifi_set_mode(WIFI_MODE_AP);
    if(error == ESP_OK) error = esp_wifi_set_config(WIFI_IF_AP, &config);
    if(error == ESP_OK) error = esp_wifi_start();
    memset(&config, 0, sizeof(config));
    if(error != ESP_OK) return error;
    started = true;
    ESP_LOGI(TAG, "SoftAP started ssid=%s security=WPA2 client_limit=2", bridge_ssid);
    return ESP_OK;
}

const char* wifi_manager_ssid(void) { return bridge_ssid; }
bool wifi_manager_is_started(void) { return started; }
