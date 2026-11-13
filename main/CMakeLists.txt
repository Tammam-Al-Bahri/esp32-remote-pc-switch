idf_component_register(SRCS "main.c"
                            "net/wifi.c" "net/http_api.c"
                            "hw/power_button.c" "hw/power_sense.c"
                            "diag/diagnostics.c"
                       INCLUDE_DIRS "." "net" "hw" "diag"
                       REQUIRES esp_driver_gpio esp_event esp_http_server esp_netif esp_timer esp_wifi nvs_flash lwip)
