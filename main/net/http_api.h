#pragma once

/* Start/stop the HTTP server (idempotent). Routes: GET /status, POST /power/{on,off,press}, GET /debug. */
/* All responses are JSON and require the bearer token. */
void http_api_start(void);
void http_api_stop(void);
