#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "bridge_config.h"
#include "esp_err.h"

typedef struct {
    char ap_password[BRIDGE_AP_PASSWORD_BYTES + 1U];
    uint8_t pairing_secret[BRIDGE_PAIRING_SECRET_BYTES];
    bool newly_created;
} BridgeCredentials;

esp_err_t bridge_credentials_load_or_create(BridgeCredentials* credentials);
void bridge_credentials_clear(BridgeCredentials* credentials);
