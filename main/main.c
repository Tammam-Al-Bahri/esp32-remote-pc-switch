#include "diagnostics.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "power_button.h"
#include "power_sense.h"
#include "wifi.h"

void app_main(void)
{
    if (power_button_init() != ESP_OK || power_sense_init() != ESP_OK) {
        return;
    }

    esp_err_t nvs_result = nvs_flash_init();
    if (nvs_result == ESP_ERR_NVS_NO_FREE_PAGES || nvs_result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_result);

    diagnostics_init();
    wifi_start();
}
