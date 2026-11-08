#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RELAY_GPIO GPIO_NUM_2
#define RELAY_PASSWORD_HEADER "X-Relay-Password"
#define RELAY_PULSE_QUERY_KEY "duration"
#define RELAY_QUERY_MAX_LENGTH 64

static const char *TAG = "relay";
static httpd_handle_t s_http_server;
static esp_timer_handle_t s_relay_timer;
static portMUX_TYPE s_relay_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_relay_pulsing;

/* Reboot if Wi-Fi has had no IP for this long. */
#define NOIP_REBOOT_MS (120 * 1000)
#define DIAG_MAGIC 0xD1A60002u

/* Per-run counters. Kept in RTC memory: survives panic/WDT/software resets
 * with zero flash wear (lost only on power loss). */
typedef struct {
    uint32_t magic;
    uint32_t disconnects;
    uint32_t last_reason;       /* last Wi-Fi disconnect reason code */
    uint32_t last_disconnect_s; /* uptime when it happened */
    uint32_t noip_restarts;     /* 1 if the no-IP watchdog caused the restart */
    uint32_t uptime_s;          /* last uptime we knew about (updated on events) */
} diag_t;

/* Persisted in NVS: one write per boot. */
typedef struct {
    uint32_t boot_count;
    uint32_t reset_reason;
    diag_t prev; /* summary of the run before this boot (zeroed if unknown) */
} boot_rec_t;

static RTC_NOINIT_ATTR diag_t s_diag;
static boot_rec_t s_rec;
static int s_retry;
static esp_timer_handle_t s_noip_timer;
static esp_timer_handle_t s_reconnect_timer;

static uint32_t uptime_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

static const char *reset_reason_name(uint32_t reason)
{
    switch ((esp_reset_reason_t)reason) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "interrupt-wdt";
    case ESP_RST_TASK_WDT:  return "task-wdt";
    case ESP_RST_WDT:       return "other-wdt";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_EXT:       return "external-pin";
    default:                return "unknown";
    }
}

static void init_diagnostics(void)
{
    const esp_reset_reason_t reason = esp_reset_reason();
    diag_t prev = {0};
    if (s_diag.magic == DIAG_MAGIC && reason != ESP_RST_POWERON) {
        prev = s_diag;
    }
    memset(&s_diag, 0, sizeof(s_diag));
    s_diag.magic = DIAG_MAGIC;

    nvs_handle_t handle;
    size_t size = sizeof(s_rec);
    memset(&s_rec, 0, sizeof(s_rec));
    if (nvs_open("diag", NVS_READWRITE, &handle) == ESP_OK) {
        if (nvs_get_blob(handle, "rec", &s_rec, &size) != ESP_OK || size != sizeof(s_rec)) {
            memset(&s_rec, 0, sizeof(s_rec));
        }
        s_rec.boot_count++;
        s_rec.reset_reason = (uint32_t)reason;
        s_rec.prev = prev;
        if (nvs_set_blob(handle, "rec", &s_rec, sizeof(s_rec)) == ESP_OK) {
            nvs_commit(handle);
        }
        nvs_close(handle);
    } else {
        s_rec.boot_count = 1;
        s_rec.reset_reason = (uint32_t)reason;
        s_rec.prev = prev;
    }

    ESP_LOGW(TAG, "BOOT #%lu reset=%s | previous run: uptime>=%lus wifi_disconnects=%lu "
             "last_reason=%lu noip_watchdog=%lu",
             (unsigned long)s_rec.boot_count, reset_reason_name(s_rec.reset_reason),
             (unsigned long)prev.uptime_s, (unsigned long)prev.disconnects,
             (unsigned long)prev.last_reason, (unsigned long)prev.noip_restarts);
}

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

static esp_err_t send_bad_request(httpd_req_t *request)
{
    httpd_resp_set_status(request, "400 Bad Request");
    httpd_resp_set_type(request, "text/plain");
    return httpd_resp_sendstr(request, "Invalid duration\n");
}

static esp_err_t relay_pulse_handler(httpd_req_t *request)
{
    if (!request_is_authorized(request)) {
        return send_unauthorized(request);
    }

    char query[RELAY_QUERY_MAX_LENGTH];
    const size_t query_length = httpd_req_get_url_query_len(request);
    if (query_length == 0 || query_length >= sizeof(query) ||
        httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK) {
        return send_bad_request(request);
    }

    char duration_text[16];
    if (httpd_query_key_value(query, RELAY_PULSE_QUERY_KEY, duration_text,
                              sizeof(duration_text)) != ESP_OK || duration_text[0] == '\0') {
        return send_bad_request(request);
    }

    for (const char *character = duration_text; *character != '\0'; character++) {
        if (*character < '0' || *character > '9') {
            return send_bad_request(request);
        }
    }

    char *end = NULL;
    const unsigned long duration = strtoul(duration_text, &end, 10);
    if (end == duration_text || *end != '\0' ||
        duration < CONFIG_RELAY_PULSE_MIN_MS ||
        duration > CONFIG_RELAY_PULSE_MAX_MS) {
        return send_bad_request(request);
    }

    portENTER_CRITICAL(&s_relay_lock);
    const bool already_pulsing = s_relay_pulsing;
    if (!already_pulsing) {
        s_relay_pulsing = true;
    }
    portEXIT_CRITICAL(&s_relay_lock);

    if (already_pulsing) {
        httpd_resp_set_status(request, "409 Conflict");
        httpd_resp_set_type(request, "text/plain");
        return httpd_resp_sendstr(request, "Relay pulse already active\n");
    }

    esp_err_t result = gpio_set_level(RELAY_GPIO, 1);
    if (result == ESP_OK) {
        result = esp_timer_start_once(s_relay_timer, (uint64_t)duration * 1000);
    }
    if (result != ESP_OK) {
        gpio_set_level(RELAY_GPIO, 0);
        portENTER_CRITICAL(&s_relay_lock);
        s_relay_pulsing = false;
        portEXIT_CRITICAL(&s_relay_lock);
        ESP_LOGE(TAG, "Could not start relay pulse: %s", esp_err_to_name(result));
        httpd_resp_set_status(request, "500 Internal Server Error");
        return httpd_resp_sendstr(request, "Could not start relay pulse\n");
    }

    ESP_LOGI(TAG, "Relay pulse started for %lu ms", duration);
    httpd_resp_set_type(request, "text/plain");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_sendstr(request, "Relay pulse started\n");
}

static esp_err_t debug_handler(httpd_req_t *request)
{
    if (!request_is_authorized(request)) {
        return send_unauthorized(request);
    }

    int rssi = 0;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        rssi = ap.rssi;
    }
    s_diag.uptime_s = uptime_s();

    char body[512];
    snprintf(body, sizeof(body),
             "uptime_s=%lu\n"
             "boot_count=%lu\n"
             "reset_reason=%s\n"
             "wifi_disconnects=%lu\n"
             "last_disconnect_reason=%lu\n"
             "last_disconnect_at_s=%lu\n"
             "rssi=%d\n"
             "free_heap=%lu\n"
             "min_free_heap=%lu\n"
             "prev_uptime_s=%lu\n"
             "prev_wifi_disconnects=%lu\n"
             "prev_last_disconnect_reason=%lu\n"
             "prev_noip_watchdog_restart=%lu\n",
             (unsigned long)s_diag.uptime_s, (unsigned long)s_rec.boot_count,
             reset_reason_name(s_rec.reset_reason),
             (unsigned long)s_diag.disconnects, (unsigned long)s_diag.last_reason,
             (unsigned long)s_diag.last_disconnect_s, rssi,
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)esp_get_minimum_free_heap_size(),
             (unsigned long)s_rec.prev.uptime_s, (unsigned long)s_rec.prev.disconnects,
             (unsigned long)s_rec.prev.last_reason, (unsigned long)s_rec.prev.noip_restarts);

    httpd_resp_set_type(request, "text/plain");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_sendstr(request, body);
}

static void relay_pulse_timeout(void *argument)
{
    (void)argument;

    gpio_set_level(RELAY_GPIO, 0);
    portENTER_CRITICAL(&s_relay_lock);
    s_relay_pulsing = false;
    portEXIT_CRITICAL(&s_relay_lock);
    ESP_LOGI(TAG, "Relay pulse finished; relay is OFF");
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
    config.lru_purge_enable = true; /* drop stale client sockets instead of running out */
    if (httpd_start(&s_http_server, &config) != ESP_OK) {
        s_http_server = NULL;
        ESP_LOGE(TAG, "Could not start HTTP server");
        return;
    }

    const httpd_uri_t relay_pulse_uri = {
        .uri = "/relay/pulse",
        .method = HTTP_POST,
        .handler = relay_pulse_handler,
    };

    const httpd_uri_t debug_uri = {
        .uri = "/debug",
        .method = HTTP_GET,
        .handler = debug_handler,
    };

    if (httpd_register_uri_handler(s_http_server, &relay_pulse_uri) != ESP_OK ||
        httpd_register_uri_handler(s_http_server, &debug_uri) != ESP_OK) {
        ESP_LOGE(TAG, "Could not register relay HTTP routes");
        stop_http_server();
        return;
    }

    ESP_LOGI(TAG, "HTTP relay control is ready: POST /relay/pulse?duration=...");
}

static void schedule_reconnect(void)
{
    static const uint32_t delays_ms[] = {1000, 2000, 5000, 10000};
    const uint32_t delay = delays_ms[s_retry < 3 ? s_retry : 3];
    s_retry++;
    esp_timer_stop(s_reconnect_timer);
    esp_timer_start_once(s_reconnect_timer, (uint64_t)delay * 1000);
}

static void reconnect_timeout(void *argument)
{
    (void)argument;
    const esp_err_t result = esp_wifi_connect();
    if (result != ESP_OK && result != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(result));
        schedule_reconnect();
    }
}

static void arm_noip_timer(void)
{
    /* Fails harmlessly if already running, so flapping doesn't extend the deadline. */
    esp_timer_start_once(s_noip_timer, (uint64_t)NOIP_REBOOT_MS * 1000);
}

static void noip_timeout(void *argument)
{
    (void)argument;
    s_diag.noip_restarts = 1;
    s_diag.uptime_s = uptime_s();
    ESP_LOGE(TAG, "No IP for %d s; restarting", NOIP_REBOOT_MS / 1000);
    esp_restart();
}

static void wifi_event_handler(void *argument, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)argument;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        arm_noip_timer();
        const esp_err_t result = esp_wifi_connect();
        if (result != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(result));
            schedule_reconnect();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = event_data;
        s_diag.disconnects++;
        s_diag.last_reason = event->reason;
        s_diag.last_disconnect_s = uptime_s();
        s_diag.uptime_s = s_diag.last_disconnect_s;
        stop_http_server();
        arm_noip_timer();
        ESP_LOGW(TAG, "Wi-Fi disconnected (reason %d, total %lu); retry in backoff",
                 event->reason, (unsigned long)s_diag.disconnects);
        schedule_reconnect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP) {
        ESP_LOGW(TAG, "Lost IP address");
        stop_http_server();
        arm_noip_timer();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = event_data;
        s_retry = 0;
        esp_timer_stop(s_noip_timer);
        s_diag.uptime_s = uptime_s();
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
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP,
                                               wifi_event_handler, NULL));

    const esp_timer_create_args_t noip_args = {.callback = noip_timeout, .name = "noip"};
    const esp_timer_create_args_t reconnect_args = {.callback = reconnect_timeout,
                                                    .name = "wifi_reconnect"};
    ESP_ERROR_CHECK(esp_timer_create(&noip_args, &s_noip_timer));
    ESP_ERROR_CHECK(esp_timer_create(&reconnect_args, &s_reconnect_timer));

    wifi_config_t wifi_config = {0};
    memcpy(wifi_config.sta.ssid, CONFIG_RELAY_WIFI_SSID, ssid_length);
    memcpy(wifi_config.sta.password, CONFIG_RELAY_WIFI_PASSWORD, wifi_password_length);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* Modem power save can make an always-on server unreachable after hours. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
}

void app_main(void)
{
    ESP_ERROR_CHECK(gpio_reset_pin(RELAY_GPIO));
    ESP_ERROR_CHECK(gpio_set_direction(RELAY_GPIO, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_set_level(RELAY_GPIO, 0));
    ESP_LOGI(TAG, "Relay initialised OFF on GPIO %d", RELAY_GPIO);

    if (CONFIG_RELAY_PULSE_MIN_MS < 1 ||
        CONFIG_RELAY_PULSE_MIN_MS > CONFIG_RELAY_PULSE_MAX_MS) {
        ESP_LOGE(TAG, "Configure valid relay pulse duration limits in menuconfig");
        return;
    }

    const esp_timer_create_args_t relay_timer_args = {
        .callback = relay_pulse_timeout,
        .name = "relay_pulse",
    };
    ESP_ERROR_CHECK(esp_timer_create(&relay_timer_args, &s_relay_timer));

    esp_err_t nvs_result = nvs_flash_init();
    if (nvs_result == ESP_ERR_NVS_NO_FREE_PAGES || nvs_result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_result);

    init_diagnostics();
    initialise_wifi();
}