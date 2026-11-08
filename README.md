# ESP32 Remote PC Switch

Minimal ESP-IDF starter for an active-high relay module connected to GPIO 2.

At startup, the firmware configures GPIO 2 as an output and sets it LOW, so the
active-high relay is OFF. It then joins Wi-Fi and starts a HTTP server. The
server accepts one authenticated pulse request. The relay turns ON for the
requested duration, then automatically returns OFF.

## Configure

Open `idf.py menuconfig` and set your Wi-Fi name, Wi-Fi password, relay password
(12 to 64 characters), and minimum/maximum pulse durations under **Relay Wi-Fi
Configuration**. The duration settings default to 100 and 2,000 milliseconds;
menuconfig permits values from 1 to 30,000 milliseconds. Ensure the minimum is
not greater than the maximum. These settings are stored in the ignored
`sdkconfig` file and compiled into the firmware. Do not commit `sdkconfig` or
share the built firmware.

Build and flash from an ESP-IDF terminal with:

```text
idf.py set-target esp32
idf.py menuconfig
idf.py build flash monitor
```

After connecting, the serial log prints the ESP32's IP address. From a device
on the same trusted network, send a pulse request with the configured relay
password. This example requests a 500 millisecond pulse:

```powershell
$headers = @{ "X-Relay-Password" = "your-long-relay-password" }
Invoke-RestMethod -Method Post -Uri "http://ESP32-IP/relay/pulse?duration=500" -Headers $headers
```

If the relay is already pulsing, another request receives `409 Conflict` and
does not extend the active pulse. Requests outside the configured duration
limits receive `400 Bad Request`.

## Reliability and diagnostics

- Wi-Fi power save is disabled so the always-on server stays reachable.
- On disconnect, the firmware reconnects with backoff (1s, 2s, 5s, then every
  10s) and logs the disconnect reason code.
- If there is no IP address for 120 seconds, the ESP32 reboots itself. The relay
  always starts OFF, so a reboot is safe.
- Run counters are kept in RTC memory (no flash wear; survives panics and
  watchdog resets, lost on power loss). Flash (NVS) gets one small write per boot.
- Every boot logs a line like
  `BOOT #3 reset=PANIC | previous run: uptime>=7200s wifi_disconnects=4 ...`.
  The reset reason is one of `power-on`, `software`, `PANIC`, `interrupt-wdt`,
  `task-wdt`, `BROWNOUT`, and so on, which tells you whether it crashed.

Read the current state with the same password header:

```powershell
Invoke-RestMethod -Uri "http://ESP32-IP/debug" -Headers $headers
```

It returns plain text: `uptime_s`, `boot_count`, `reset_reason`,
`wifi_disconnects`, `last_disconnect_reason` (Wi-Fi reason code),
`last_disconnect_at_s`, `rssi`, `free_heap`, `min_free_heap`, and a
`prev_*` summary of the run before the last reboot. `prev_uptime_s` is a lower
bound, and `prev_*` is zero after a power loss. A steadily shrinking
`min_free_heap` points to a memory leak, and `prev_noip_watchdog_restart=1`
means the no-IP watchdog rebooted the device.

This is plain HTTP, so the password is not encrypted in transit. Use it only on
a trusted local network; avoid exposing the ESP32's HTTP port to the internet.
