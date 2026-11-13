#pragma once

/* Validates the Wi-Fi credentials and HTTP token, brings up Wi-Fi STA and starts the reconnect,
 * no-IP and gateway-ping watchdogs. The HTTP server follows the IP lifecycle. */
void wifi_start(void);
