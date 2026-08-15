#pragma once

#include <stddef.h>

#include "esp_err.h"

/** Initializes the official Wi-Fi Devboard RGB LED in its inactive state. */
esp_err_t activity_led_start(void);

/** Schedules a non-blocking activity pulse after RPC payload bytes move. */
void activity_led_note_transfer(size_t bytes);
