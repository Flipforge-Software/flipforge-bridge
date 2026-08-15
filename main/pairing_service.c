#include "pairing_service.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bridge_config.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define PAIRING_BUTTON_GPIO GPIO_NUM_0

static const char* TAG = "bridge_pairing";

typedef struct {
    BridgeCredentials credentials;
    char ssid[20];
    uint64_t deadline_us;
} PairingContext;

static void encode_hex(const uint8_t* bytes, size_t size, char* output) {
    static const char digits[] = "0123456789abcdef";
    for(size_t index = 0; index < size; ++index) {
        output[index * 2U] = digits[bytes[index] >> 4U];
        output[index * 2U + 1U] = digits[bytes[index] & 0x0FU];
    }
    output[size * 2U] = '\0';
}

static void pairing_task(void* argument) {
    PairingContext* context = argument;
    fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);
    char command[16] = {0};
    size_t command_size = 0U;
    uint32_t held_ms = 0U;

    for(;;) {
        const bool button_held = gpio_get_level(PAIRING_BUTTON_GPIO) == 0;
        held_ms = button_held ? held_ms + 50U : 0U;
        if(held_ms == 2000U) {
            context->deadline_us = (uint64_t)esp_timer_get_time() +
                                   (uint64_t)BRIDGE_PAIRING_WINDOW_MS * 1000ULL;
            ESP_LOGI(TAG, "Physical pairing window opened for %u seconds", BRIDGE_PAIRING_WINDOW_MS / 1000U);
        }

        char byte;
        while(read(STDIN_FILENO, &byte, 1U) == 1) {
            if(byte == '\n' || byte == '\r') {
                command[command_size] = '\0';
                const bool window_open =
                    (uint64_t)esp_timer_get_time() < context->deadline_us;
                if(strcmp(command, "PAIR") == 0 && window_open && button_held) {
                    char secret_hex[BRIDGE_PAIRING_SECRET_BYTES * 2U + 1U];
                    encode_hex(
                        context->credentials.pairing_secret,
                        sizeof(context->credentials.pairing_secret),
                        secret_hex);
                    /* Direct physical provisioning response, deliberately not ESP_LOG. */
                    fprintf(
                        stdout,
                        "FFPAIR1 %s %s %s\n",
                        context->ssid,
                        context->credentials.ap_password,
                        secret_hex);
                    fflush(stdout);
                    memset(secret_hex, 0, sizeof(secret_hex));
                    context->deadline_us = 0U;
                }
                command_size = 0U;
            } else if(command_size + 1U < sizeof(command)) {
                command[command_size++] = byte;
            } else {
                command_size = 0U;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50U));
    }

    bridge_credentials_clear(&context->credentials);
    free(context);
    vTaskDelete(NULL);
}

esp_err_t pairing_service_start(const BridgeCredentials* credentials, const char* ssid) {
    if(!credentials || !ssid) return ESP_ERR_INVALID_ARG;
    PairingContext* context = calloc(1U, sizeof(*context));
    if(!context) return ESP_ERR_NO_MEM;
    context->credentials = *credentials;
    strncpy(context->ssid, ssid, sizeof(context->ssid) - 1U);
    if(credentials->newly_created) {
        context->deadline_us = (uint64_t)esp_timer_get_time() +
                               (uint64_t)BRIDGE_PAIRING_WINDOW_MS * 1000ULL;
    }

    gpio_config_t config = {
        .pin_bit_mask = 1ULL << PAIRING_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t error = gpio_config(&config);
    if(error != ESP_OK) {
        bridge_credentials_clear(&context->credentials);
        free(context);
        return error;
    }
    if(xTaskCreate(pairing_task, "pairing", 4096U, context, 4U, NULL) != pdPASS) {
        bridge_credentials_clear(&context->credentials);
        free(context);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "USB pairing service ready; physical confirmation required");
    return ESP_OK;
}
