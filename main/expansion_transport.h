#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "expansion_state_machine.h"

typedef struct ExpansionTransport ExpansionTransport;

esp_err_t expansion_transport_create(ExpansionTransport** output);
esp_err_t expansion_transport_start(ExpansionTransport* transport);
void expansion_transport_destroy(ExpansionTransport* transport);
void expansion_transport_request_reconnect(ExpansionTransport* transport);

ExpansionState expansion_transport_state(ExpansionTransport* transport);
const char* expansion_transport_state_name(ExpansionState state);
uint32_t expansion_transport_negotiated_baud(ExpansionTransport* transport);

esp_err_t expansion_transport_begin_rpc(ExpansionTransport* transport);
esp_err_t expansion_transport_end_rpc(ExpansionTransport* transport);
esp_err_t expansion_transport_send_rpc(
    ExpansionTransport* transport,
    const uint8_t* bytes,
    size_t size);
esp_err_t expansion_transport_receive_rpc(
    ExpansionTransport* transport,
    uint8_t* output,
    size_t capacity,
    size_t* output_size,
    uint32_t timeout_ms);
