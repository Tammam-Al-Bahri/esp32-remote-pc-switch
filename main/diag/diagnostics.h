#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#define WD_NONE 0
#define WD_NOIP 1
#define WD_PING 2

/* Reads the previous run's RTC/NVS record, bumps the boot count and logs the BOOT line.
 * NVS must already be initialised. */
void diagnostics_init(void);

/* Consecutive watchdog restarts without recovery. */
uint32_t diagnostics_streak(void);
/* The network is demonstrably working: clear the streak. */
void diagnostics_reset_streak(void);

/* Wi-Fi disconnects seen this run. */
uint32_t diagnostics_disconnects(void);

/* Record a Wi-Fi disconnect (count, reason, timestamp). */
void diagnostics_note_disconnect(uint32_t reason);
/* Refresh the "last known uptime" kept across restarts. */
void diagnostics_touch_uptime(void);
/* Publish gateway-ping state for the debug report. */
void diagnostics_set_ping_status(bool ever_ok, uint32_t consecutive_fails);

/* Record the cause, log it and restart. Does not return. */
void diagnostics_watchdog_restart(uint32_t cause);

/* Fill buf with the JSON object served by /debug. */
void diagnostics_format_json(char *buf, size_t size);
