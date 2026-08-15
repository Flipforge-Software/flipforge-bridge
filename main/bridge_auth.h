#pragma once

#include <stddef.h>
#include <stdint.h>

#include "bridge_config.h"
#ifdef BRIDGE_HOST_TEST
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG -2
#else
#include "esp_err.h"
#endif

esp_err_t bridge_auth_hmac_sha256(
    const uint8_t secret[BRIDGE_PAIRING_SECRET_BYTES],
    const uint8_t* message,
    size_t message_size,
    uint8_t output[BRIDGE_HMAC_BYTES]);
