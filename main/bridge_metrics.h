#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t started_us;
    uint64_t tcp_rx_bytes;
    uint64_t tcp_tx_bytes;
    uint64_t expansion_rx_bytes;
    uint64_t expansion_tx_bytes;
    uint64_t rpc_payload_rx_bytes;
    uint64_t rpc_payload_tx_bytes;
    uint64_t frame_count;
    uint64_t ack_wait_us;
    uint32_t ack_count;
    uint32_t queue_peak_bytes;
    uint32_t retries;
    uint32_t protocol_errors;
} BridgeMetricsSnapshot;

void bridge_metrics_init(void);
void bridge_metrics_add_tcp_rx(size_t bytes);
void bridge_metrics_add_tcp_tx(size_t bytes);
void bridge_metrics_add_expansion_rx(size_t bytes);
void bridge_metrics_add_expansion_tx(size_t bytes);
void bridge_metrics_add_rpc_rx(size_t bytes);
void bridge_metrics_add_rpc_tx(size_t bytes);
void bridge_metrics_frame(void);
void bridge_metrics_ack_wait(uint64_t microseconds);
void bridge_metrics_queue_used(size_t bytes);
void bridge_metrics_retry(void);
void bridge_metrics_protocol_error(void);
BridgeMetricsSnapshot bridge_metrics_snapshot(void);
