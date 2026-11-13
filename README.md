# ESP32 Remote PC Switch

Turn a PC on or off over Wi-Fi. The ESP32 presses the motherboard power button
through a relay and reads the power LED through an optocoupler to know whether
the PC is on.

## Wiring

- **Button:** a GPIO drives a relay whose contacts connect the two `PWR_SW`
  motherboard header pins, like the front-panel button.
- **Sense:** the motherboard power LED (`PWR_LED`) feeds an optocoupler. The optocoupler output pulls a GPIO low (internal pull-up) when the
  LED is lit.

## Configure

Run `idf.py menuconfig` and open **PC Switch Configuration**:

| Option                                         | Default     |
| ---------------------------------------------- | ----------- |
| Wi-Fi SSID / password                          |             |
| `PCSW_AUTH_TOKEN` (12 to 64 characters)        |             |
| `PCSW_BUTTON_GPIO` / `PCSW_BUTTON_ACTIVE_HIGH` | 2 / high    |
| `PCSW_SENSE_GPIO` / `PCSW_SENSE_ACTIVE_HIGH`   | 27 / low    |
| `PCSW_PRESS_MS`                                | 500         |
| `PCSW_PULSE_MIN_MS` / `PCSW_PULSE_MAX_MS`      | 100 / 10000 |

Settings live in the git-ignored `sdkconfig`.

## Build and flash

```text
idf.py set-target esp32
idf.py menuconfig
idf.py build flash monitor
```

The serial log prints the IP address once connected.

## API

All routes need `Authorization: Bearer <token>` and return JSON (errors are
`{"error":"..."}`).

| Route                           | Description                                                         |
| ------------------------------- | ------------------------------------------------------------------- |
| `GET /status`                   | `{"power":"on","pressing":false}`                                   |
| `POST /power/on`                | Press if the PC is off, otherwise `already_on`                      |
| `POST /power/off`               | Press if the PC is on, otherwise `already_off`                      |
| `POST /power/press?duration=ms` | Raw press within the min/max limits (e.g. `6000` to hard power off) |
| `GET /debug`                    | Uptime, reboot reason, Wi-Fi and heap diagnostics                   |

A successful press returns `202 {"result":"pressed"}`. A press already in
progress returns `409`; bad input returns `400`; a bad token returns `401`.

Set `TOKEN` to the `PCSW_AUTH_TOKEN` you configured. In the terminal:

```bash
# bash / zsh / Git Bash
TOKEN=your-auth-token
```

```powershell
# PowerShell (also use curl.exe instead of curl below, since curl is an alias there)
$TOKEN = "your-auth-token"
```

Then:

```bash
curl -H "Authorization: Bearer $TOKEN" "http://ESP32-IP/status"
curl -X POST -H "Authorization: Bearer $TOKEN" "http://ESP32-IP/power/on"
curl -X POST -H "Authorization: Bearer $TOKEN" "http://ESP32-IP/power/off"
curl -X POST -H "Authorization: Bearer $TOKEN" "http://ESP32-IP/power/press?duration=6000"
curl -H "Authorization: Bearer $TOKEN" "http://ESP32-IP/debug"
```

## Security

This is plain HTTP with a shared token. Use it on a trusted local network only,
never exposed to the internet.
