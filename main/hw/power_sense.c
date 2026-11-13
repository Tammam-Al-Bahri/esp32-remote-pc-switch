#include "power_sense.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define SENSE_GPIO ((gpio_num_t)CONFIG_PCSW_SENSE_GPIO)
#define SAMPLE_COUNT 20
#define SAMPLE_INTERVAL_MS 50 /* 20 x 50 ms = ~1 s window */

#ifdef CONFIG_PCSW_SENSE_ACTIVE_HIGH
#define SENSE_ACTIVE_LEVEL 1
#else
#define SENSE_ACTIVE_LEVEL 0
#endif

_Static_assert(CONFIG_PCSW_SENSE_GPIO != CONFIG_PCSW_BUTTON_GPIO,
               "Button and sense GPIO must differ");

static const char *TAG = "pcsw";

esp_err_t power_sense_init(void)
{
    ESP_ERROR_CHECK(gpio_reset_pin(SENSE_GPIO));
    ESP_ERROR_CHECK(gpio_set_direction(SENSE_GPIO, GPIO_MODE_INPUT));
    ESP_ERROR_CHECK(gpio_set_pull_mode(SENSE_GPIO, GPIO_PULLUP_ONLY));
    ESP_LOGI(TAG, "Power sense on GPIO %d (active %s, internal pull-up)", (int)SENSE_GPIO,
             SENSE_ACTIVE_LEVEL ? "high" : "low");
    return ESP_OK;
}

bool power_sense_is_on(void)
{
    for (int i = 0; i < SAMPLE_COUNT; i++) {
        if (gpio_get_level(SENSE_GPIO) == SENSE_ACTIVE_LEVEL) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_INTERVAL_MS));
    }
    return false;
}
