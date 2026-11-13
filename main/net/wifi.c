#include "wifi.h"

#include "diagnostics.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "http_api.h"
#include "lwip/ip_addr.h"
#include "ping/ping_sock.h"
#include "sdkconfig.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Reboot if Wi-Fi has had no IP for this long (grows with consecutive
 * watchdog reboots, capped, so a long router outage doesn't cause reboot spam). */
#define NOIP_REBOOT_MS (120 * 1000)
#define NOIP_REBOOT_MAX_MS (30 * 60 * 1000)
/* Gateway ping every PING_INTERVAL_S; reboot after this many failures in a row
 * (also grows with the streak, capped). */
#define PING_INTERVAL_S 30
#define PING_FAILS_BASE 4
#define PING_FAILS_MAX 60

static const char *TAG = "pcsw";

static int s_retry;
static esp_timer_handle_t s_noip_timer;
static esp_timer_handle_t s_reconnect_timer;
static esp_ping_handle_t s_ping;
static uint32_t s_ping_fails;
static bool s_ping_ever_ok;

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
    uint64_t ms = (uint64_t)NOIP_REBOOT_MS * (1 + diagnostics_streak());
    if (ms > NOIP_REBOOT_MAX_MS) {
        ms = NOIP_REBOOT_MAX_MS;
    }
    /* Fails harmlessly if already running, so flapping doesn't extend the deadline. */
    esp_timer_start_once(s_noip_timer, ms * 1000);
}

static void noip_timeout(void *argument)
{
    (void)argument;
    diagnostics_watchdog_restart(WD_NOIP);
}

static void publish_ping_status(void)
{
    diagnostics_set_ping_status(s_ping_ever_ok, s_ping_fails);
}

static void ping_success(esp_ping_handle_t handle, void *args)
{
    (void)handle;
    (void)args;
    s_ping_fails = 0;
    s_ping_ever_ok = true;
    publish_ping_status();
    diagnostics_reset_streak(); /* network is demonstrably working */
}

static void ping_timeout(esp_ping_handle_t handle, void *args)
{
    (void)handle;
    (void)args;
    if (!s_ping_ever_ok) {
        ESP_LOGW(TAG, "Gateway ping not answered yet (router may block ICMP); check inactive");
        return;
    }
    s_ping_fails++;
    publish_ping_status();
    uint32_t limit = PING_FAILS_BASE * (1 + diagnostics_streak());
    if (limit > PING_FAILS_MAX) {
        limit = PING_FAILS_MAX;
    }
    ESP_LOGW(TAG, "Gateway ping failed (%lu/%lu)", (unsigned long)s_ping_fails,
             (unsigned long)limit);
    if (s_ping_fails >= limit) {
        diagnostics_watchdog_restart(WD_PING);
    }
}

static void stop_gateway_ping(void)
{
    if (s_ping != NULL) {
        esp_ping_stop(s_ping);
        esp_ping_delete_session(s_ping);
        s_ping = NULL;
    }
}

static void start_gateway_ping(void)
{
    stop_gateway_ping();
    s_ping_fails = 0;
    publish_ping_status();

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info;
    if (netif == NULL || esp_netif_get_ip_info(netif, &ip_info) != ESP_OK ||
        ip_info.gw.addr == 0) {
        ESP_LOGW(TAG, "No gateway address; ping check not started");
        return;
    }

    esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
    config.count = ESP_PING_COUNT_INFINITE;
    config.interval_ms = PING_INTERVAL_S * 1000;
    config.timeout_ms = 3000;
    ip_addr_t target = {0};
    ip_addr_copy_from_ip4(target, ip_info.gw);
    config.target_addr = target;

    const esp_ping_callbacks_t callbacks = {
        .on_ping_success = ping_success,
        .on_ping_timeout = ping_timeout,
    };
    if (esp_ping_new_session(&config, &callbacks, &s_ping) != ESP_OK) {
        s_ping = NULL;
        ESP_LOGW(TAG, "Could not create ping session");
        return;
    }
    esp_ping_start(s_ping);
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
        diagnostics_note_disconnect(event->reason);
        http_api_stop();
        stop_gateway_ping();
        arm_noip_timer();
        ESP_LOGW(TAG, "Wi-Fi disconnected (reason %d, total %lu); retry in backoff",
                 event->reason, (unsigned long)diagnostics_disconnects());
        schedule_reconnect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP) {
        ESP_LOGW(TAG, "Lost IP address");
        http_api_stop();
        stop_gateway_ping();
        arm_noip_timer();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = event_data;
        s_retry = 0;
        esp_timer_stop(s_noip_timer);
        diagnostics_touch_uptime();
        ESP_LOGI(TAG, "Connected; open http://" IPSTR, IP2STR(&event->ip_info.ip));
        http_api_start();
        start_gateway_ping();
    }
}

void wifi_start(void)
{
    const size_t ssid_length = strlen(CONFIG_PCSW_WIFI_SSID);
    const size_t wifi_password_length = strlen(CONFIG_PCSW_WIFI_PASSWORD);
    if (ssid_length == 0 || ssid_length > sizeof(((wifi_config_t *)0)->sta.ssid) ||
        wifi_password_length == 0 ||
        wifi_password_length > sizeof(((wifi_config_t *)0)->sta.password)) {
        ESP_LOGE(TAG, "Set a valid Wi-Fi SSID and password in menuconfig");
        return;
    }

    const size_t control_password_length = strlen(CONFIG_PCSW_AUTH_TOKEN);
    if (control_password_length < 12 || control_password_length > 64) {
        ESP_LOGE(TAG, "Set an HTTP bearer token of 12 to 64 characters in menuconfig");
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
    memcpy(wifi_config.sta.ssid, CONFIG_PCSW_WIFI_SSID, ssid_length);
    memcpy(wifi_config.sta.password, CONFIG_PCSW_WIFI_PASSWORD, wifi_password_length);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* Modem power save can make an always-on server unreachable after hours. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
}
