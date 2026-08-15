#include "usb_console.h"

#include <stdio.h>

#include "sdkconfig.h"
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_console.h"
#include "tinyusb_default_config.h"

#if !CONFIG_TINYUSB_CDC_ENABLED
#error "Flipforge Bridge requires TinyUSB CDC"
#endif

#if CONFIG_ESP_CONSOLE_USB_CDC
#error "The ESP32-S2 ROM USB console must be disabled when TinyUSB is enabled"
#endif

esp_err_t usb_console_start(void) {
    const tinyusb_config_t usb_config = TINYUSB_DEFAULT_CONFIG();
    esp_err_t error = tinyusb_driver_install(&usb_config);
    if(error != ESP_OK) return error;

    const tinyusb_config_cdcacm_t cdc_config = {0};
    error = tinyusb_cdcacm_init(&cdc_config);
    if(error != ESP_OK) {
        tinyusb_driver_uninstall();
        return error;
    }

    error = tinyusb_console_init(TINYUSB_CDC_ACM_0);
    if(error != ESP_OK) {
        tinyusb_cdcacm_deinit(TINYUSB_CDC_ACM_0);
        tinyusb_driver_uninstall();
        return error;
    }

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    return ESP_OK;
}
