#pragma once

#include "bridge_credentials.h"
#include "esp_err.h"
#include "expansion_transport.h"

esp_err_t bridge_server_start(
    ExpansionTransport* transport,
    const BridgeCredentials* credentials);
