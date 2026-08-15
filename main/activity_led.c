#include "activity_led.h"

#include <stdint.h>

#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define ACTIVITY_LED_RED_GPIO 6
#define ACTIVITY_LED_GREEN_GPIO 5
#define ACTIVITY_LED_BLUE_GPIO 4

#define ACTIVITY_LED_FREQUENCY_HZ 5000U
#define ACTIVITY_LED_MAX_DUTY 255U
#define ACTIVITY_LED_VISIBLE_DUTY 20U
#define ACTIVITY_LED_PULSE_MS 40U
#define ACTIVITY_LED_GAP_MS 60U
#define ACTIVITY_LED_NOTIFY_TRANSFER (1U << 0U)

static const char* TAG = "activity_led";
static TaskHandle_t s_activity_task;

static esp_err_t set_channel(ledc_channel_t channel, uint8_t value) {
    const uint32_t brightness = ((uint32_t)value * ACTIVITY_LED_VISIBLE_DUTY) / UINT8_MAX;
    const uint32_t active_low_duty = ACTIVITY_LED_MAX_DUTY - brightness;
    esp_err_t error = ledc_set_duty(LEDC_LOW_SPEED_MODE, channel, active_low_duty);
    if(error == ESP_OK) {
        error = ledc_update_duty(LEDC_LOW_SPEED_MODE, channel);
    }
    return error;
}

static esp_err_t set_rgb(uint8_t red, uint8_t green, uint8_t blue) {
    esp_err_t error = set_channel(LEDC_CHANNEL_0, red);
    if(error == ESP_OK) error = set_channel(LEDC_CHANNEL_1, green);
    if(error == ESP_OK) error = set_channel(LEDC_CHANNEL_2, blue);
    return error;
}

static void activity_task(void* argument) {
    (void)argument;
    for(;;) {
        uint32_t notification = 0U;
        xTaskNotifyWait(0U, UINT32_MAX, &notification, portMAX_DELAY);
        if((notification & ACTIVITY_LED_NOTIFY_TRANSFER) == 0U) continue;

        if(set_rgb(0U, 0U, UINT8_MAX) != ESP_OK) {
            ESP_LOGW(TAG, "Unable to turn on the activity LED");
        }
        vTaskDelay(pdMS_TO_TICKS(ACTIVITY_LED_PULSE_MS));
        if(set_rgb(0U, 0U, 0U) != ESP_OK) {
            ESP_LOGW(TAG, "Unable to turn off the activity LED");
        }
        vTaskDelay(pdMS_TO_TICKS(ACTIVITY_LED_GAP_MS));
    }
}

esp_err_t activity_led_start(void) {
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = ACTIVITY_LED_FREQUENCY_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t error = ledc_timer_config(&timer);
    if(error != ESP_OK) return error;

    const ledc_channel_config_t channels[] = {
        {
            .gpio_num = ACTIVITY_LED_RED_GPIO,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = LEDC_CHANNEL_0,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = LEDC_TIMER_0,
            .duty = ACTIVITY_LED_MAX_DUTY,
            .hpoint = 0,
        },
        {
            .gpio_num = ACTIVITY_LED_GREEN_GPIO,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = LEDC_CHANNEL_1,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = LEDC_TIMER_0,
            .duty = ACTIVITY_LED_MAX_DUTY,
            .hpoint = 0,
        },
        {
            .gpio_num = ACTIVITY_LED_BLUE_GPIO,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = LEDC_CHANNEL_2,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = LEDC_TIMER_0,
            .duty = ACTIVITY_LED_MAX_DUTY,
            .hpoint = 0,
        },
    };
    for(size_t index = 0U; index < sizeof(channels) / sizeof(channels[0]); ++index) {
        error = ledc_channel_config(&channels[index]);
        if(error != ESP_OK) return error;
    }

    error = set_rgb(0U, 0U, 0U);
    if(error != ESP_OK) return error;
    if(xTaskCreate(activity_task, "activity_led", 2048U, NULL, 2U, &s_activity_task) != pdPASS) {
        s_activity_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "RPC transfer activity LED ready");
    return ESP_OK;
}

void activity_led_note_transfer(size_t bytes) {
    if(bytes == 0U || !s_activity_task) return;
    xTaskNotify(s_activity_task, ACTIVITY_LED_NOTIFY_TRANSFER, eSetBits);
}
