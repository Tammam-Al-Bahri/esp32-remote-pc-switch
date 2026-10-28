#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define RELAY_GPIO GPIO_NUM_2
#define RELAY_PASSWORD_HEADER "X-Relay-Password"

static const char *TAG = "relay";
static httpd_handle_t s_http_server;

static bool request_is_authorized(httpd_req_t *request)
{
    char supplied_password[65];
    const size_t password_length = strlen(CONFIG_RELAY_CONTROL_PASSWORD);
    if (password_length < 12 || password_length > 64) {
        return false;
    }

    const size_t header_length = httpd_req_get_hdr_value_len(request, RELAY_PASSWORD_HEADER);
    if (header_length != password_length || header_length >= sizeof(supplied_password)) {
        return false;
    }

    if (httpd_req_get_hdr_value_str(request, RELAY_PASSWORD_HEADER,
                                    supplied_password, sizeof(supplied_password)) != ESP_OK) {
        return false;
    }

    return strcmp(supplied_password, CONFIG_RELAY_CONTROL_PASSWORD) == 0;
}

static esp_err_t send_unauthorized(httpd_req_t *request)
{
    httpd_resp_set_status(request, "401 Unauthorized");
    httpd_resp_set_type(request, "text/plain");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_sendstr(request, "Invalid relay password\n");
}

static esp_err_t set_relay(httpd_req_t *request, bool turn_on)
{
    if (!request_is_authorized(request)) {
        return send_unauthorized(request);
    }

    esp_err_t result = gpio_set_level(RELAY_GPIO, turn_on ? 1 : 0);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Could not set relay GPIO: %s", esp_err_to_name(result));
        httpd_resp_set_status(request, "500 Internal Server Error");
        return httpd_resp_sendstr(request, "Could not set relay\n");
    }

    ESP_LOGI(TAG, "Relay turned %s", turn_on ? "ON" : "OFF");
    httpd_resp_set_type(request, "text/plain");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_sendstr(request, turn_on ? "Relay is ON\n" : "Relay is OFF\n");
}

static esp_err_t relay_on_handler(httpd_req_t *request)
{
    return set_relay(request, true);
}

static esp_err_t relay_off_handler(httpd_req_t *request)
{
    return set_relay(request, false);
}

static void stop_http_server(void)
{
    if (s_http_server != NULL) {
        httpd_stop(s_http_server);
        s_http_server = NULL;
    }
}

static void start_http_server(void)
{
    if (s_http_server != NULL) {
        return;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    if (httpd_start(&s_http_server, &config) != ESP_OK) {
        s_http_server = NULL;
        ESP_LOGE(TAG, "Could not start HTTP server");
        return;
    }

    const httpd_uri_t relay_on_uri = {
        .uri = "/relay/on",
        .method = HTTP_POST,
        .handler = relay_on_handler,
    };
    const httpd_uri_t relay_off_uri = {
        .uri = "/relay/off",
        .method = HTTP_POST,
        .handler = relay_off_handler,
    };

    if (httpd_register_uri_handler(s_http_server, &relay_on_uri) != ESP_OK ||
        httpd_register_uri_handler(s_http_server, &relay_off_uri) != ESP_OK) {
        ESP_LOGE(TAG, "Could not register relay HTTP routes");
        stop_http_server();
        return;
    }

    ESP_LOGI(TAG, "HTTP relay control is ready: POST /relay/on or POST /relay/off");
}

static void wifi_event_handler(void *argument, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)argument;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_ERROR_CHECK(esp_wifi_connect());
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        stop_http_server();
        ESP_LOGW(TAG, "Wi-Fi disconnected; reconnecting");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = event_data;
        ESP_LOGI(TAG, "Connected; open http://" IPSTR, IP2STR(&event->ip_info.ip));
        start_http_server();
    }
}

static void initialise_wifi(void)
{
    const size_t ssid_length = strlen(CONFIG_RELAY_WIFI_SSID);
    const size_t wifi_password_length = strlen(CONFIG_RELAY_WIFI_PASSWORD);
    if (ssid_length == 0 || ssid_length > sizeof(((wifi_config_t *)0)->sta.ssid) ||
        wifi_password_length == 0 ||
        wifi_password_length > sizeof(((wifi_config_t *)0)->sta.password)) {
        ESP_LOGE(TAG, "Set a valid Wi-Fi SSID and password in menuconfig");
        return;
    }

    const size_t control_password_length = strlen(CONFIG_RELAY_CONTROL_PASSWORD);
    if (control_password_length < 12 || control_password_length > 64) {
        ESP_LOGE(TAG, "Set a relay password of 12 to 64 characters in menuconfig");
        return;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    if (esp_netif_create_default_wifi_sta() == NULL) {
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }

    wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_config));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               wifi_event_handler, NULL));

    wifi_config_t wifi_config = {0};
    memcpy(wifi_config.sta.ssid, CONFIG_RELAY_WIFI_SSID, ssid_length);
    memcpy(wifi_config.sta.password, CONFIG_RELAY_WIFI_PASSWORD, wifi_password_length);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

void app_main(void)
{
    ESP_ERROR_CHECK(gpio_reset_pin(RELAY_GPIO));
    ESP_ERROR_CHECK(gpio_set_direction(RELAY_GPIO, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_set_level(RELAY_GPIO, 0));
    ESP_LOGI(TAG, "Relay initialised OFF on GPIO %d", RELAY_GPIO);

    esp_err_t nvs_result = nvs_flash_init();
    if (nvs_result == ESP_ERR_NVS_NO_FREE_PAGES || nvs_result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_result);

    initialise_wifi();
}