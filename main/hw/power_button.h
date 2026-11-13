#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/* Configures the button GPIO (released), validates the
 * configured pulse limits and creates the press timer. Returns an error if the configuration is invalid. */
esp_err_t power_button_init(void);

/* Hold the button for ms milliseconds, then release it.
 * ESP_OK: pulse started. ESP_ERR_INVALID_STATE: a pulse is already active.
 * Any other value: hardware/timer failure (button left released). */
esp_err_t power_button_press(uint32_t ms);

bool power_button_is_pressing(void);
