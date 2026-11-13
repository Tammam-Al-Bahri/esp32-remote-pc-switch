#include "http_api.h"

#include "diagnostics.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "power_button.h"
#include "power_sense.h"
#include "sdkconfig.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define PCSW_AUTH_HEADER "Authorization"
#define PCSW_AUTH_SCHEME "Bearer "
#define PCSW_TOKEN_MIN 12
#define PCSW_TOKEN_MAX 64
#define PCSW_QUERY_MAX_LENGTH 64

static const char *TAG = "pcsw";
static httpd_handle_t s_http_server;

/* ---- JSON responses ---------------------------------------------------- */

static esp_err_t send_json(httpd_req_t *request, const char *status, const char *body)
{
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_sendstr(request, body);
}

static esp_err_t send_error(httpd_req_t *request, const char *status, const char *message)
{
    char body[80];
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}\n", message);
    return send_json(request, status, body);
}

static esp_err_t send_result(httpd_req_t *request, const char *status, const char *result)
{
    char body[48];
    snprintf(body, sizeof(body), "{\"result\":\"%s\"}\n", result);
    return send_json(request, status, body);
}

/* ---- Auth -------------------------------------------------------------- */

/* Compares in time independent of where the first mismatch is. */
static bool token_equals(const char *supplied, size_t supplied_length,
                         const char *expected, size_t expected_length)
{
    unsigned diff = (unsigned)(supplied_length ^ expected_length);
    for (size_t i = 0; i < expected_length; i++) {
        const char c = i < supplied_length ? supplied[i] : 0;
        diff |= (unsigned)((unsigned char)c ^ (unsigned char)expected[i]);
    }
    return diff == 0;
}

static bool request_is_authorized(httpd_req_t *request)
{
    char header[sizeof(PCSW_AUTH_SCHEME) - 1 + PCSW_TOKEN_MAX + 1];
    const size_t token_length = strlen(CONFIG_PCSW_AUTH_TOKEN);
    if (token_length < PCSW_TOKEN_MIN || token_length > PCSW_TOKEN_MAX) {
        return false;
    }

    const size_t header_length = httpd_req_get_hdr_value_len(request, PCSW_AUTH_HEADER);
    const size_t scheme_length = sizeof(PCSW_AUTH_SCHEME) - 1;
    if (header_length <= scheme_length || header_length >= sizeof(header)) {
        return false;
    }
    if (httpd_req_get_hdr_value_str(request, PCSW_AUTH_HEADER, header,
                                    sizeof(header)) != ESP_OK) {
        return false;
    }
    if (strncasecmp(header, PCSW_AUTH_SCHEME, scheme_length) != 0) {
        return false;
    }

    return token_equals(header + scheme_length, header_length - scheme_length,
                        CONFIG_PCSW_AUTH_TOKEN, token_length);
}

/* Sends 401 and returns false if the request is not authorised. */
static bool require_auth(httpd_req_t *request)
{
    if (request_is_authorized(request)) {
        return true;
    }
    httpd_resp_set_hdr(request, "WWW-Authenticate", "Bearer");
    send_error(request, "401 Unauthorized", "unauthorized");
    return false;
}

/* ---- Query helpers ----------------------------------------------------- */

/* Copies the value of `key` into out. Returns 1 if found, 0 if absent, -1 if the query
 * string is too long or the value does not fit. */
static int query_value(httpd_req_t *request, const char *key, char *out, size_t out_size)
{
    char query[PCSW_QUERY_MAX_LENGTH];
    const size_t query_length = httpd_req_get_url_query_len(request);
    if (query_length == 0) {
        return 0;
    }
    if (query_length >= sizeof(query) ||
        httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK) {
        return -1;
    }
    const esp_err_t result = httpd_query_key_value(query, key, out, out_size);
    if (result == ESP_ERR_NOT_FOUND) {
        return 0;
    }
    return result == ESP_OK ? 1 : -1;
}

/* ---- Press helper ------------------------------------------------------ */

/* Starts a press and sends 202 {"result":"pressed"}, or 409/500 on failure. */
static esp_err_t press_and_reply(httpd_req_t *request, uint32_t ms)
{
    const esp_err_t result = power_button_press(ms);
    if (result == ESP_ERR_INVALID_STATE) {
        return send_error(request, "409 Conflict", "press_in_progress");
    }
    if (result != ESP_OK) {
        return send_error(request, "500 Internal Server Error", "press_failed");
    }
    return send_result(request, "202 Accepted", "pressed");
}

/* ---- Handlers ---------------------------------------------------------- */

static esp_err_t status_handler(httpd_req_t *request)
{
    if (!require_auth(request)) {
        return ESP_OK;
    }

    const bool on = power_sense_is_on();
    char body[64];
    snprintf(body, sizeof(body), "{\"power\":\"%s\",\"pressing\":%s}\n", on ? "on" : "off",
             power_button_is_pressing() ? "true" : "false");
    return send_json(request, "200 OK", body);
}

static esp_err_t power_on_handler(httpd_req_t *request)
{
    if (!require_auth(request)) {
        return ESP_OK;
    }
    if (power_button_is_pressing()) {
        return send_error(request, "409 Conflict", "press_in_progress");
    }
    if (power_sense_is_on()) {
        return send_result(request, "200 OK", "already_on");
    }
    return press_and_reply(request, CONFIG_PCSW_PRESS_MS);
}

static esp_err_t power_off_handler(httpd_req_t *request)
{
    if (!require_auth(request)) {
        return ESP_OK;
    }

    if (power_button_is_pressing()) {
        return send_error(request, "409 Conflict", "press_in_progress");
    }
    if (!power_sense_is_on()) {
        return send_result(request, "200 OK", "already_off");
    }
    return press_and_reply(request, CONFIG_PCSW_PRESS_MS);
}

static esp_err_t power_press_handler(httpd_req_t *request)
{
    if (!require_auth(request)) {
        return ESP_OK;
    }

    char duration_text[16];
    if (query_value(request, "duration", duration_text, sizeof(duration_text)) != 1 ||
        duration_text[0] == '\0') {
        return send_error(request, "400 Bad Request", "invalid_duration");
    }
    for (const char *character = duration_text; *character != '\0'; character++) {
        if (*character < '0' || *character > '9') {
            return send_error(request, "400 Bad Request", "invalid_duration");
        }
    }
    const unsigned long duration = strtoul(duration_text, NULL, 10);
    if (duration < CONFIG_PCSW_PULSE_MIN_MS || duration > CONFIG_PCSW_PULSE_MAX_MS) {
        return send_error(request, "400 Bad Request", "duration_out_of_range");
    }

    return press_and_reply(request, (uint32_t)duration);
}

static esp_err_t debug_handler(httpd_req_t *request)
{
    if (!require_auth(request)) {
        return ESP_OK;
    }

    char body[1024];
    diagnostics_format_json(body, sizeof(body));
    return send_json(request, "200 OK", body);
}

/* ---- Server lifecycle -------------------------------------------------- */

void http_api_stop(void)
{
    if (s_http_server != NULL) {
        httpd_stop(s_http_server);
        s_http_server = NULL;
    }
}

void http_api_start(void)
{
    if (s_http_server != NULL) {
        return;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true; /* drop stale client sockets instead of running out */
    config.max_uri_handlers = 8;
    config.stack_size = 6144; /* /debug formats a 1 KiB body on the stack */
    if (httpd_start(&s_http_server, &config) != ESP_OK) {
        s_http_server = NULL;
        ESP_LOGE(TAG, "Could not start HTTP server");
        return;
    }

    const httpd_uri_t routes[] = {
        {.uri = "/status", .method = HTTP_GET, .handler = status_handler},
        {.uri = "/power/on", .method = HTTP_POST, .handler = power_on_handler},
        {.uri = "/power/off", .method = HTTP_POST, .handler = power_off_handler},
        {.uri = "/power/press", .method = HTTP_POST, .handler = power_press_handler},
        {.uri = "/debug", .method = HTTP_GET, .handler = debug_handler},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        if (httpd_register_uri_handler(s_http_server, &routes[i]) != ESP_OK) {
            ESP_LOGE(TAG, "Could not register HTTP route %s", routes[i].uri);
            http_api_stop();
            return;
        }
    }

    ESP_LOGI(TAG, "HTTP control is ready: GET /status, POST /power/{on,off,press}, GET /debug");
}
