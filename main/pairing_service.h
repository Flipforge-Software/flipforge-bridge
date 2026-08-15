#pragma once

#include "bridge_credentials.h"
#include "esp_err.h"

esp_err_t pairing_service_start(const BridgeCredentials* credentials, const char* ssid);
