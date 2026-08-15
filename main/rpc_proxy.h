#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "expansion_transport.h"

esp_err_t rpc_proxy_run(
    int socket_fd,
    ExpansionTransport* transport,
    const uint8_t* initial_bytes,
    size_t initial_size);
