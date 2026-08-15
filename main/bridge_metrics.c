#include "bridge_metrics.h"

#include <string.h>

#include "sdkconfig.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#if CONFIG_FLIPFORGE_DEBUG_METRICS
static BridgeMetricsSnapshot metrics;
static portMUX_TYPE metrics_lock = portMUX_INITIALIZER_UNLOCKED;

#define METRIC_UPDATE(expression)              \
    do {                                       \
        taskENTER_CRITICAL(&metrics_lock);     \
        expression;                            \
        taskEXIT_CRITICAL(&metrics_lock);      \
    } while(0)

void bridge_metrics_init(void) {
    taskENTER_CRITICAL(&metrics_lock);
    memset(&metrics, 0, sizeof(metrics));
    metrics.started_us = (uint64_t)esp_timer_get_time();
    taskEXIT_CRITICAL(&metrics_lock);
}

void bridge_metrics_add_tcp_rx(size_t bytes) { METRIC_UPDATE(metrics.tcp_rx_bytes += bytes); }
void bridge_metrics_add_tcp_tx(size_t bytes) { METRIC_UPDATE(metrics.tcp_tx_bytes += bytes); }
void bridge_metrics_add_expansion_rx(size_t bytes) {
    METRIC_UPDATE(metrics.expansion_rx_bytes += bytes);
}
void bridge_metrics_add_expansion_tx(size_t bytes) {
    METRIC_UPDATE(metrics.expansion_tx_bytes += bytes);
}
void bridge_metrics_add_rpc_rx(size_t bytes) { METRIC_UPDATE(metrics.rpc_payload_rx_bytes += bytes); }
void bridge_metrics_add_rpc_tx(size_t bytes) { METRIC_UPDATE(metrics.rpc_payload_tx_bytes += bytes); }
void bridge_metrics_frame(void) { METRIC_UPDATE(++metrics.frame_count); }
void bridge_metrics_ack_wait(uint64_t microseconds) {
    METRIC_UPDATE(metrics.ack_wait_us += microseconds; ++metrics.ack_count);
}
void bridge_metrics_queue_used(size_t bytes) {
    METRIC_UPDATE(if(bytes > metrics.queue_peak_bytes) metrics.queue_peak_bytes = (uint32_t)bytes);
}
void bridge_metrics_retry(void) { METRIC_UPDATE(++metrics.retries); }
void bridge_metrics_protocol_error(void) { METRIC_UPDATE(++metrics.protocol_errors); }

BridgeMetricsSnapshot bridge_metrics_snapshot(void) {
    BridgeMetricsSnapshot snapshot;
    taskENTER_CRITICAL(&metrics_lock);
    snapshot = metrics;
    taskEXIT_CRITICAL(&metrics_lock);
    return snapshot;
}
#else
void bridge_metrics_init(void) {}
void bridge_metrics_add_tcp_rx(size_t bytes) { (void)bytes; }
void bridge_metrics_add_tcp_tx(size_t bytes) { (void)bytes; }
void bridge_metrics_add_expansion_rx(size_t bytes) { (void)bytes; }
void bridge_metrics_add_expansion_tx(size_t bytes) { (void)bytes; }
void bridge_metrics_add_rpc_rx(size_t bytes) { (void)bytes; }
void bridge_metrics_add_rpc_tx(size_t bytes) { (void)bytes; }
void bridge_metrics_frame(void) {}
void bridge_metrics_ack_wait(uint64_t microseconds) { (void)microseconds; }
void bridge_metrics_queue_used(size_t bytes) { (void)bytes; }
void bridge_metrics_retry(void) {}
void bridge_metrics_protocol_error(void) {}
BridgeMetricsSnapshot bridge_metrics_snapshot(void) {
    return (BridgeMetricsSnapshot){0};
}
#endif
