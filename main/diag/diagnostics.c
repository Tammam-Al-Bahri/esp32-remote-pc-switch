#include "diagnostics.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"
#include <stdio.h>
#include <string.h>

#define DIAG_MAGIC 0xD1A60003u

static const char *TAG = "pcsw";

/* Per-run counters. Kept in RTC memory: survives panic/WDT/software resets
 * with zero flash wear (lost only on power loss). */
typedef struct {
    uint32_t magic;
    uint32_t disconnects;
    uint32_t last_reason;       /* last Wi-Fi disconnect reason code */
    uint32_t last_disconnect_s; /* uptime when it happened */
    uint32_t wd_cause;          /* WD_NOIP / WD_PING if a watchdog caused the restart */
    uint32_t streak;            /* consecutive watchdog restarts without recovery */
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
static volatile uint32_t s_ping_fails;
static volatile bool s_ping_ever_ok;

static const char *wd_name(uint32_t cause)
{
    return cause == WD_NOIP ? "no-ip" : cause == WD_PING ? "gateway-ping" : "none";
}

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

void diagnostics_init(void)
{
    const esp_reset_reason_t reason = esp_reset_reason();
    diag_t prev = {0};
    if (s_diag.magic == DIAG_MAGIC && reason != ESP_RST_POWERON) {
        prev = s_diag;
    }
    memset(&s_diag, 0, sizeof(s_diag));
    s_diag.magic = DIAG_MAGIC;
    s_diag.streak = prev.wd_cause != WD_NONE ? prev.streak + 1 : 0;

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
             "last_reason=%lu watchdog=%s streak=%lu",
             (unsigned long)s_rec.boot_count, reset_reason_name(s_rec.reset_reason),
             (unsigned long)prev.uptime_s, (unsigned long)prev.disconnects,
             (unsigned long)prev.last_reason, wd_name(prev.wd_cause),
             (unsigned long)s_diag.streak);
}

uint32_t diagnostics_streak(void)
{
    return s_diag.streak;
}

void diagnostics_reset_streak(void)
{
    s_diag.streak = 0;
}

uint32_t diagnostics_disconnects(void)
{
    return s_diag.disconnects;
}

void diagnostics_note_disconnect(uint32_t reason)
{
    s_diag.disconnects++;
    s_diag.last_reason = reason;
    s_diag.last_disconnect_s = uptime_s();
    s_diag.uptime_s = s_diag.last_disconnect_s;
}

void diagnostics_touch_uptime(void)
{
    s_diag.uptime_s = uptime_s();
}

void diagnostics_set_ping_status(bool ever_ok, uint32_t consecutive_fails)
{
    s_ping_ever_ok = ever_ok;
    s_ping_fails = consecutive_fails;
}

void diagnostics_watchdog_restart(uint32_t cause)
{
    s_diag.wd_cause = cause;
    s_diag.uptime_s = uptime_s();
    ESP_LOGE(TAG, "Watchdog (%s) restarting; streak=%lu", wd_name(cause),
             (unsigned long)s_diag.streak);
    esp_restart();
}

void diagnostics_format_json(char *buf, size_t size)
{
    int rssi = 0;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        rssi = ap.rssi;
    }
    s_diag.uptime_s = uptime_s();

    snprintf(buf, size,
             "{\"uptime_s\":%lu,"
             "\"boot_count\":%lu,"
             "\"reset_reason\":\"%s\","
             "\"wifi_disconnects\":%lu,"
             "\"last_disconnect_reason\":%lu,"
             "\"last_disconnect_at_s\":%lu,"
             "\"rssi\":%d,"
             "\"free_heap\":%lu,"
             "\"min_free_heap\":%lu,"
             "\"prev_uptime_s\":%lu,"
             "\"prev_wifi_disconnects\":%lu,"
             "\"prev_last_disconnect_reason\":%lu,"
             "\"prev_watchdog_restart\":\"%s\","
             "\"watchdog_streak\":%lu,"
             "\"ping_ok_this_boot\":%s,"
             "\"ping_consecutive_fails\":%lu}\n",
             (unsigned long)s_diag.uptime_s, (unsigned long)s_rec.boot_count,
             reset_reason_name(s_rec.reset_reason),
             (unsigned long)s_diag.disconnects, (unsigned long)s_diag.last_reason,
             (unsigned long)s_diag.last_disconnect_s, rssi,
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)esp_get_minimum_free_heap_size(),
             (unsigned long)s_rec.prev.uptime_s, (unsigned long)s_rec.prev.disconnects,
             (unsigned long)s_rec.prev.last_reason, wd_name(s_rec.prev.wd_cause),
             (unsigned long)s_diag.streak, s_ping_ever_ok ? "true" : "false",
             (unsigned long)s_ping_fails);
}
