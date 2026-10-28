# ESP32 Remote PC Switch

Minimal ESP-IDF starter for an active-high relay module connected to GPIO 2.

At startup, the firmware configures GPIO 2 as an output and sets it LOW, so the
active-high relay is OFF. It then joins Wi-Fi and starts a HTTP server. The
server accepts authenticated `POST` requests at `/relay/on` and `/relay/off`.

## Configure

Open `idf.py menuconfig`, then enter your Wi-Fi name, Wi-Fi password, and a
relay password of 12 to 64 characters under **Relay Wi-Fi Configuration**.
These values are stored in the ignored `sdkconfig` file and compiled into the
firmware. Do not commit `sdkconfig` or share the built firmware.

Build and flash from an ESP-IDF terminal with:

```text
idf.py set-target esp32
idf.py menuconfig
idf.py build flash monitor
```

After connecting, the serial log prints the ESP32's IP address. From a device
on the same trusted network, send either command with the configured relay
password:

```powershell
$headers = @{ "X-Relay-Password" = "your-long-relay-password" }
Invoke-RestMethod -Method Post -Uri "http://ESP32-IP/relay/on" -Headers $headers
Invoke-RestMethod -Method Post -Uri "http://ESP32-IP/relay/off" -Headers $headers
```

This is plain HTTP, so the password is not encrypted in transit. Use it only on
a trusted local network; do not expose the ESP32's HTTP port to the internet.
