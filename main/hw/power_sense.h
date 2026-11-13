#pragma once

#include "esp_err.h"
#include <stdbool.h>

/* Configures the sense GPIO as an input with the internal pull-up. */
esp_err_t power_sense_init(void);

/* True if the PC reads as powered on. Samples for ~1 s and reports on if any sample is
 * active, so a blinking power LED (e.g. sleep) still counts as on. Blocks the caller. */
bool power_sense_is_on(void);
