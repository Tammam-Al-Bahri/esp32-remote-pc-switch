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

This is plain HTTP, so the password is not encrypted in transit. Use it only on
a trusted local network; avoid exposing the ESP32's HTTP port to the internet.
