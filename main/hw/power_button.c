#include "power_button.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

#define BUTTON_GPIO ((gpio_num_t)CONFIG_PCSW_BUTTON_GPIO)

#ifdef CONFIG_PCSW_BUTTON_ACTIVE_HIGH
#define BUTTON_PRESSED_LEVEL 1
#else
#define BUTTON_PRESSED_LEVEL 0
#endif
#define BUTTON_RELEASED_LEVEL (!BUTTON_PRESSED_LEVEL)

static const char *TAG = "pcsw";
static esp_timer_handle_t s_press_timer;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_pressing;

static void press_timeout(void *argument)
{
    (void)argument;

    gpio_set_level(BUTTON_GPIO, BUTTON_RELEASED_LEVEL);
    portENTER_CRITICAL(&s_lock);
    s_pressing = false;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "Button press finished; button is released");
}

esp_err_t power_button_init(void)
{
    /* Set the released level before enabling the output so the PC never sees a glitch. */
    ESP_ERROR_CHECK(gpio_reset_pin(BUTTON_GPIO));
    ESP_ERROR_CHECK(gpio_set_level(BUTTON_GPIO, BUTTON_RELEASED_LEVEL));
    ESP_ERROR_CHECK(gpio_set_direction(BUTTON_GPIO, GPIO_MODE_OUTPUT));
    ESP_LOGI(TAG, "Button initialised released on GPIO %d (active %s)", (int)BUTTON_GPIO,
             BUTTON_PRESSED_LEVEL ? "high" : "low");

    if (CONFIG_PCSW_PULSE_MIN_MS < 1 ||
        CONFIG_PCSW_PULSE_MIN_MS > CONFIG_PCSW_PULSE_MAX_MS) {
        ESP_LOGE(TAG, "Configure valid pulse duration limits in menuconfig");
        return ESP_ERR_INVALID_ARG;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = press_timeout,
        .name = "button_press",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_press_timer));
    return ESP_OK;
}

esp_err_t power_button_press(uint32_t ms)
{
    portENTER_CRITICAL(&s_lock);
    const bool already_pressing = s_pressing;
    if (!already_pressing) {
        s_pressing = true;
    }
    portEXIT_CRITICAL(&s_lock);

    if (already_pressing) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result = gpio_set_level(BUTTON_GPIO, BUTTON_PRESSED_LEVEL);
    if (result == ESP_OK) {
        result = esp_timer_start_once(s_press_timer, (uint64_t)ms * 1000);
    }
    if (result != ESP_OK) {
        gpio_set_level(BUTTON_GPIO, BUTTON_RELEASED_LEVEL);
        portENTER_CRITICAL(&s_lock);
        s_pressing = false;
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGE(TAG, "Could not start button press: %s", esp_err_to_name(result));
        return result;
    }

    ESP_LOGI(TAG, "Button press started for %lu ms", (unsigned long)ms);
    return ESP_OK;
}

bool power_button_is_pressing(void)
{
    portENTER_CRITICAL(&s_lock);
    const bool pressing = s_pressing;
    portEXIT_CRITICAL(&s_lock);
    return pressing;
}
