#include <inttypes.h>

#include "activity_led.h"
#include "bridge_config.h"
#include "bridge_credentials.h"
#include "bridge_metrics.h"
#include "bridge_server.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "pairing_service.h"
#include "usb_console.h"
#include "wifi_manager.h"

static const char* TAG = "flipforge_bridge";

#if CONFIG_FLIPFORGE_DEBUG_METRICS
static void metrics_task(void* argument) {
    ExpansionTransport* transport = argument;
    BridgeMetricsSnapshot previous = bridge_metrics_snapshot();
    for(;;) {
        vTaskDelay(pdMS_TO_TICKS(5000U));
        const BridgeMetricsSnapshot current = bridge_metrics_snapshot();
        const uint64_t elapsed_us = current.started_us > 0U ?
                                        (uint64_t)esp_timer_get_time() - current.started_us : 0U;
        const uint64_t window_rpc =
            current.rpc_payload_rx_bytes + current.rpc_payload_tx_bytes -
            previous.rpc_payload_rx_bytes - previous.rpc_payload_tx_bytes;
        ESP_LOGI(
            TAG,
            "metrics uptime_ms=%" PRIu64 " rpc_Bps=%" PRIu64
            " tcp_rx=%" PRIu64 " tcp_tx=%" PRIu64
            " expansion_rx=%" PRIu64 " expansion_tx=%" PRIu64
            " frames=%" PRIu64 " ack_avg_us=%" PRIu64
            " queue_peak=%" PRIu32 " retries=%" PRIu32 " errors=%" PRIu32
            " baud=%" PRIu32,
            elapsed_us / 1000U,
            window_rpc / 5U,
            current.tcp_rx_bytes,
            current.tcp_tx_bytes,
            current.expansion_rx_bytes,
            current.expansion_tx_bytes,
            current.frame_count,
            current.ack_count > 0U ? current.ack_wait_us / current.ack_count : 0U,
            current.queue_peak_bytes,
            current.retries,
            current.protocol_errors,
            expansion_transport_negotiated_baud(transport));
        previous = current;
    }
}
#endif

void app_main(void) {
    ESP_ERROR_CHECK(usb_console_start());
    ESP_LOGI(
        TAG,
        "%s version=%s protocol=%u",
        FLIPFORGE_BRIDGE_PRODUCT,
        FLIPFORGE_BRIDGE_VERSION,
        FLIPFORGE_BRIDGE_PROTOCOL_VERSION);

    esp_err_t error = nvs_flash_init();
    if(error == ESP_ERR_NVS_NO_FREE_PAGES || error == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        error = nvs_flash_init();
    }
    ESP_ERROR_CHECK(error);
    bridge_metrics_init();

#if CONFIG_FLIPFORGE_ACTIVITY_LED
    error = activity_led_start();
    if(error != ESP_OK) {
        ESP_LOGW(TAG, "Activity LED unavailable: %s", esp_err_to_name(error));
    }
#endif

    BridgeCredentials credentials;
    ESP_ERROR_CHECK(bridge_credentials_load_or_create(&credentials));

    ExpansionTransport* transport = NULL;
    ESP_ERROR_CHECK(expansion_transport_create(&transport));
    ESP_ERROR_CHECK(expansion_transport_start(transport));
    ESP_ERROR_CHECK(wifi_manager_start(&credentials));
    ESP_ERROR_CHECK(pairing_service_start(&credentials, wifi_manager_ssid()));
    ESP_ERROR_CHECK(bridge_server_start(transport, &credentials));
    bridge_credentials_clear(&credentials);

#if CONFIG_FLIPFORGE_DEBUG_METRICS
    ESP_ERROR_CHECK(
        xTaskCreate(metrics_task, "bridge_metrics", 4096U, transport, 3U, NULL) == pdPASS ?
            ESP_OK : ESP_ERR_NO_MEM);
#endif
}
