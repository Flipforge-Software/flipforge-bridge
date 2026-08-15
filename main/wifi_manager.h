#pragma once

#include <stdbool.h>

#include "bridge_credentials.h"
#include "esp_err.h"

esp_err_t wifi_manager_start(const BridgeCredentials* credentials);
const char* wifi_manager_ssid(void);
bool wifi_manager_is_started(void);
