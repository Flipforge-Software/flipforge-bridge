#pragma once

#include "esp_err.h"

/*
 * Starts the TinyUSB CDC console used for logs and physical pairing.
 * Unlike the ESP32-S2 ROM USB console, TinyUSB supports a USB cable being
 * attached after the Devboard has already booted from Flipper power.
 */
esp_err_t usb_console_start(void);
