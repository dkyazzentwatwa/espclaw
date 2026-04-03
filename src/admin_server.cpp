#include "espclaw/admin_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <strings.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_http_server.h"
#include "esp_log.h"

#include "espclaw/admin_api.h"
#include "espclaw/agent_loop.h"
#include "espclaw/console_chat.h"
#include "espclaw/admin_ops.h"
#include "espclaw/admin_ui.h"
#include "espclaw/app_patterns.h"
#include "espclaw/app_runtime.h"
#include "espclaw/auth_store.h"
#include "espclaw/behavior_runtime.h"
#include "espclaw/board_config.h"
#include "espclaw/component_runtime.h"
#include "espclaw/control_loop.h"
#include "espclaw/context_runtime.h"
#include "espclaw/hardware.h"
#include "espclaw/lua_api_registry.h"
#include "espclaw/log_buffer.h"
#include "espclaw/ota_manager.h"
#include "espclaw/ota_state.h"
#include "espclaw/runtime.h"
#include "espclaw/system_monitor.h"
#include "espclaw/task_policy.h"
#include "espclaw/task_runtime.h"
#include "espclaw/workspace.h"

static const char *TAG = "espclaw_admin";
static httpd_handle_t s_admin_server;
/*
 * Long interactive chat runs execute inside the httpd worker task, so the
 * default ESP-IDF stack is too small once the handler, JSON rendering, and
 * agent loop all overlap. The ESP32-CAM now has PSRAM available, so we can
 * afford a larger admin stack to keep real model runs stable.
 */
static const size_t ESPCLAW_ADMIN_HTTPD_STACK_SIZE = 32768;
static const size_t ESPCLAW_ADMIN_HTTPD_ROUTE_HEADROOM = 16;

#ifndef CONFIG_ESPCLAW_ADMIN_PORT
#define CONFIG_ESPCLAW_ADMIN_PORT 8080
#endif

static esp_err_t send_body(httpd_req_t *req, const char *content_type, const char *body)
{
    httpd_resp_set_type(req, content_type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, body != NULL ? body : "");
}

static esp_err_t send_json(httpd_req_t *req, const char *body)
{
    return send_body(req, "application/json", body);
}

static esp_err_t send_json_status(httpd_req_t *req, const char *body, int status_code, const char *status_text)
{
    char status[32];

    snprintf(status, sizeof(status), "%d %s", status_code, status_text);
    httpd_resp_set_status(req, status);
    return send_json(req, body);
}

static const char *media_content_type(const char *relative_path)
{
    const char *extension;

    if (relative_path == NULL) {
        return "application/octet-stream";
    }

    extension = strrchr(relative_path, '.');
    if (extension == NULL) {
        return "application/octet-stream";
    }
    if (strcasecmp(extension, ".jpg") == 0 || strcasecmp(extension, ".jpeg") == 0) {
        return "image/jpeg";
    }
    if (strcasecmp(extension, ".png") == 0) {
        return "image/png";
    }
    if (strcasecmp(extension, ".gif") == 0) {
        return "image/gif";
    }
    if (strcasecmp(extension, ".webp") == 0) {
        return "image/webp";
    }
    if (strcasecmp(extension, ".txt") == 0 || strcasecmp(extension, ".log") == 0) {
        return "text/plain; charset=utf-8";
    }
    if (strcasecmp(extension, ".json") == 0) {
        return "application/json";
    }

    return "application/octet-stream";
}

static esp_err_t send_file_response(httpd_req_t *req, const char *absolute_path, const char *content_type)
{
    FILE *file = NULL;
    char chunk[2048];
    size_t read_size;

    if (req == NULL || absolute_path == NULL || content_type == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    file = fopen(absolute_path, "rb");
    if (file == NULL) {
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, content_type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    while ((read_size = fread(chunk, 1, sizeof(chunk), file)) > 0) {
        if (httpd_resp_send_chunk(req, chunk, read_size) != ESP_OK) {
            fclose(file);
            httpd_resp_send_chunk(req, NULL, 0);
            return ESP_FAIL;
        }
    }

    fclose(file);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static bool load_query_value(httpd_req_t *req, const char *key, char *buffer, size_t buffer_size)
{
    size_t query_length;
    char query[256];

    if (req == NULL || key == NULL || buffer == NULL || buffer_size == 0) {
        return false;
    }

    query_length = httpd_req_get_url_query_len(req);
    if (query_length == 0 || query_length >= sizeof(query)) {
        buffer[0] = '\0';
        return false;
    }

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        buffer[0] = '\0';
        return false;
    }

    return espclaw_admin_query_value(query, key, buffer, buffer_size);
}

static uint32_t load_query_u32(httpd_req_t *req, const char *key, uint32_t fallback)
{
    char value[32];

    if (load_query_value(req, key, value, sizeof(value))) {
        return (uint32_t)strtoul(value, NULL, 10);
    }

    return fallback;
}

static const char *blob_stage_name(espclaw_workspace_blob_stage_t stage)
{
    switch (stage) {
    case ESPCLAW_WORKSPACE_BLOB_STAGE_OPEN:
        return "open";
    case ESPCLAW_WORKSPACE_BLOB_STAGE_COMMITTED:
        return "committed";
    case ESPCLAW_WORKSPACE_BLOB_STAGE_NONE:
    default:
        return "missing";
    }
}

static bool json_bool_value(const char *json, const char *key, bool *value_out)
{
    char pattern[64];
    const char *cursor;

    if (json == NULL || key == NULL || value_out == NULL) {
        return false;
    }
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    cursor = strstr(json, pattern);
    if (cursor == NULL || (cursor = strchr(cursor, ':')) == NULL) {
        return false;
    }
    cursor++;
    while (*cursor != '\0' && (*cursor == ' ' || *cursor == '\n' || *cursor == '\r' || *cursor == '\t')) {
        cursor++;
    }
    if (strncmp(cursor, "true", 4) == 0) {
        *value_out = true;
        return true;
    }
    if (strncmp(cursor, "false", 5) == 0) {
        *value_out = false;
        return true;
    }
    return false;
}

static bool json_u32_value(const char *json, const char *key, uint32_t *value_out)
{
    char pattern[64];
    const char *cursor;
    char *end = NULL;
    unsigned long value;

    if (json == NULL || key == NULL || value_out == NULL) {
        return false;
    }
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    cursor = strstr(json, pattern);
    if (cursor == NULL || (cursor = strchr(cursor, ':')) == NULL) {
        return false;
    }
    cursor++;
    while (*cursor != '\0' && (*cursor == ' ' || *cursor == '\n' || *cursor == '\r' || *cursor == '\t')) {
        cursor++;
    }
    value = strtoul(cursor, &end, 10);
    if (end == cursor) {
        return false;
    }
    *value_out = (uint32_t)value;
    return true;
}

static bool json_string_value(const char *json, const char *key, char *buffer, size_t buffer_size)
{
    char pattern[64];
    const char *cursor;
    size_t used = 0;

    if (json == NULL || key == NULL || buffer == NULL || buffer_size == 0) {
        return false;
    }
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    cursor = strstr(json, pattern);
    if (cursor == NULL || (cursor = strchr(cursor, ':')) == NULL) {
        buffer[0] = '\0';
        return false;
    }
    cursor++;
    while (*cursor != '\0' && (*cursor == ' ' || *cursor == '\n' || *cursor == '\r' || *cursor == '\t')) {
        cursor++;
    }
    if (*cursor != '"') {
        buffer[0] = '\0';
        return false;
    }
    cursor++;
    while (*cursor != '\0' && *cursor != '"' && used + 1 < buffer_size) {
        if (*cursor == '\\' && cursor[1] != '\0') {
            cursor++;
            switch (*cursor) {
            case 'n':
                buffer[used++] = '\n';
                break;
            case 'r':
                buffer[used++] = '\r';
                break;
            case 't':
                buffer[used++] = '\t';
                break;
            default:
                buffer[used++] = *cursor;
                break;
            }
        } else {
            buffer[used++] = *cursor;
        }
        cursor++;
    }
    buffer[used] = '\0';
    return *cursor == '"';
}

static void copy_text(char *buffer, size_t buffer_size, const char *value)
{
    if (buffer == NULL || buffer_size == 0) {
        return;
    }
    snprintf(buffer, buffer_size, "%s", value != NULL ? value : "");
}

static size_t append_escaped_json(char *buffer, size_t buffer_size, size_t used, const char *value)
{
    const char *cursor = value != NULL ? value : "";

    if (buffer == NULL || buffer_size == 0 || used >= buffer_size) {
        return used;
    }

    used += (size_t)snprintf(buffer + used, buffer_size - used, "\"");
    while (*cursor != '\0' && used + 2 < buffer_size) {
        switch (*cursor) {
        case '\\':
        case '"':
            used += (size_t)snprintf(buffer + used, buffer_size - used, "\\%c", *cursor);
            break;
        case '\n':
            used += (size_t)snprintf(buffer + used, buffer_size - used, "\\n");
            break;
        case '\r':
            used += (size_t)snprintf(buffer + used, buffer_size - used, "\\r");
            break;
        case '\t':
            used += (size_t)snprintf(buffer + used, buffer_size - used, "\\t");
            break;
        default:
            buffer[used++] = *cursor;
            buffer[used] = '\0';
            break;
        }
        cursor++;
    }
    used += (size_t)snprintf(buffer + used, buffer_size - used, "\"");
    return used >= buffer_size ? buffer_size - 1 : used;
}

static void append_text(char *buffer, size_t buffer_size, const char *value)
{
    size_t used = strlen(buffer);

    if (used >= buffer_size - 1) {
        return;
    }
    snprintf(buffer + used, buffer_size - used, "%s", value != NULL ? value : "");
}

static esp_err_t read_request_body(httpd_req_t *req, char *buffer, size_t buffer_size)
{
    int total = 0;

    if (req == NULL || buffer == NULL || buffer_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    while (total < req->content_len && total < (int)buffer_size - 1) {
        int received = httpd_req_recv(req, buffer + total, (int)buffer_size - 1 - total);
        if (received <= 0) {
            return ESP_FAIL;
        }
        total += received;
    }

    buffer[total] = '\0';
    return total == req->content_len ? ESP_OK : ESP_FAIL;
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    return send_body(req, "text/html; charset=utf-8", espclaw_admin_ui_html());
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    char buffer[512];
    espclaw_ota_snapshot_t ota_snapshot;
    const espclaw_runtime_status_t *status = espclaw_runtime_status();
    espclaw_auth_profile_t *profile = NULL;

    profile = calloc(1, sizeof(*profile));
    if (profile == NULL) {
        return send_json_status(req, "{\"ok\":false,\"message\":\"out of memory\"}", 500, "Internal Server Error");
    }

    espclaw_auth_profile_default(profile);
    espclaw_auth_store_load(profile);
    espclaw_ota_manager_snapshot(&ota_snapshot);

    espclaw_render_admin_status_json(
        status != NULL ? &status->profile : NULL,
        status != NULL ? status->storage_backend : ESPCLAW_STORAGE_BACKEND_SD_CARD,
        profile->provider_id,
        "telegram",
        status != NULL && status->storage_ready,
        espclaw_runtime_get_yolo_mode(),
        &ota_snapshot.state,
        buffer,
        sizeof(buffer)
    );
    free(profile);
    return send_json(req, buffer);
}

static esp_err_t logs_get_handler(httpd_req_t *req)
{
    char response[6144];
    uint32_t tail_bytes = load_query_u32(req, "bytes", 4096);

    if (espclaw_log_buffer_render_json((size_t)tail_bytes, response, sizeof(response)) != 0) {
        espclaw_admin_render_result_json(false, "failed to render log buffer", response, sizeof(response));
        return send_json_status(req, response, 500, "Internal Server Error");
    }
    return send_json(req, response);
}

static esp_err_t auth_status_get_handler(httpd_req_t *req)
{
    char buffer[768];
    espclaw_auth_profile_t *profile = calloc(1, sizeof(*profile));

    if (profile == NULL) {
        return send_json_status(req, "{\"ok\":false,\"message\":\"out of memory\"}", 500, "Internal Server Error");
    }

    espclaw_auth_profile_default(profile);
    if (espclaw_auth_store_load(profile) != 0) {
        profile->configured = false;
    }
    espclaw_render_auth_profile_json(profile, buffer, sizeof(buffer));
    free(profile);
    return send_json(req, buffer);
}

static esp_err_t telegram_config_get_handler(httpd_req_t *req)
{
    char buffer[512];
    espclaw_telegram_config_t config;

    memset(&config, 0, sizeof(config));
    if (espclaw_runtime_get_telegram_config(&config) != ESP_OK) {
        return send_json_status(req, "{\"ok\":false,\"message\":\"telegram config unavailable\"}", 503, "Service Unavailable");
    }

    snprintf(
        buffer,
        sizeof(buffer),
        "{\"enabled\":%s,\"configured\":%s,\"ready\":%s,\"poll_interval_seconds\":%lu,\"token_hint\":",
        config.enabled ? "true" : "false",
        config.configured ? "true" : "false",
        config.ready ? "true" : "false",
        (unsigned long)config.poll_interval_seconds
    );
    append_escaped_json(buffer, sizeof(buffer), strlen(buffer), config.token_hint);
    append_text(buffer, sizeof(buffer), "}");
    return send_json(req, buffer);
}

static esp_err_t telegram_config_post_handler(httpd_req_t *req)
{
    char body[1024];
    char response[512];
    char message[256];
    espclaw_telegram_config_t config;
    bool enabled = false;
    uint32_t poll_interval_seconds = 0;
    char token[sizeof(config.bot_token)];

    memset(&config, 0, sizeof(config));
    memset(token, 0, sizeof(token));
    if (read_request_body(req, body, sizeof(body)) != ESP_OK) {
        espclaw_admin_render_result_json(false, "failed to read telegram config body", response, sizeof(response));
        return send_json_status(req, response, 400, "Bad Request");
    }
    if (espclaw_runtime_get_telegram_config(&config) != ESP_OK) {
        espclaw_admin_render_result_json(false, "telegram config unavailable", response, sizeof(response));
        return send_json_status(req, response, 503, "Service Unavailable");
    }
    if (json_bool_value(body, "enabled", &enabled)) {
        config.enabled = enabled;
    }
    if (json_u32_value(body, "poll_interval_seconds", &poll_interval_seconds)) {
        config.poll_interval_seconds = poll_interval_seconds;
    }
    if (json_string_value(body, "bot_token", token, sizeof(token))) {
        copy_text(config.bot_token, sizeof(config.bot_token), token);
    }
    if (espclaw_runtime_set_telegram_config(&config, message, sizeof(message)) != ESP_OK) {
        espclaw_admin_render_result_json(false, message[0] != '\0' ? message : "failed to save telegram config", response, sizeof(response));
        return send_json_status(req, response, 400, "Bad Request");
    }
    espclaw_admin_render_result_json(true, message, response, sizeof(response));
    return send_json(req, response);
}

static esp_err_t tools_get_handler(httpd_req_t *req)
{
    char buffer[65536];

    espclaw_render_tools_json(buffer, sizeof(buffer));
    return send_json(req, buffer);
}

static esp_err_t lua_api_get_handler(httpd_req_t *req)
{
    char buffer[12288];

    espclaw_render_lua_api_json(buffer, sizeof(buffer));
    return send_json(req, buffer);
}

static esp_err_t lua_api_markdown_get_handler(httpd_req_t *req)
{
    char buffer[16384];

    espclaw_render_lua_api_markdown(buffer, sizeof(buffer));
    return send_body(req, "text/markdown; charset=utf-8", buffer);
}

static esp_err_t app_patterns_get_handler(httpd_req_t *req)
{
    char buffer[4096];

    (void)req;
    espclaw_render_app_patterns_json(buffer, sizeof(buffer));
    return send_json(req, buffer);
}

static esp_err_t app_patterns_markdown_get_handler(httpd_req_t *req)
{
    char buffer[4096];

    espclaw_render_app_patterns_markdown(buffer, sizeof(buffer));
    return send_body(req, "text/markdown; charset=utf-8", buffer);
}

static esp_err_t ota_status_get_handler(httpd_req_t *req)
{
    char buffer[512];
    espclaw_ota_snapshot_t snapshot;

    espclaw_ota_manager_snapshot(&snapshot);
    espclaw_render_ota_status_json(&snapshot, buffer, sizeof(buffer));
    return send_json(req, buffer);
}

static esp_err_t ota_upload_post_handler(httpd_req_t *req)
{
    char message[256];
    char response[512];
    unsigned char *chunk = NULL;
    int total_received = 0;

    if (req == NULL || req->content_len <= 0) {
        espclaw_admin_render_result_json(false, "missing firmware body", response, sizeof(response));
        return send_json_status(req, response, 400, "Bad Request");
    }

    chunk = calloc(1, 4096);
    if (chunk == NULL) {
        espclaw_admin_render_result_json(false, "out of memory", response, sizeof(response));
        return send_json_status(req, response, 500, "Internal Server Error");
    }

    if (espclaw_ota_manager_begin((size_t)req->content_len, message, sizeof(message)) != ESP_OK) {
        free(chunk);
        espclaw_admin_render_result_json(false, message, response, sizeof(response));
        return send_json_status(req, response, 503, "Service Unavailable");
    }

    while (total_received < req->content_len) {
        int to_read = req->content_len - total_received;
        int received;

        if (to_read > 4096) {
            to_read = 4096;
        }
        received = httpd_req_recv(req, (char *)chunk, to_read);
        if (received <= 0) {
            espclaw_ota_manager_abort(message, sizeof(message));
            free(chunk);
            espclaw_admin_render_result_json(false, "failed to read firmware upload", response, sizeof(response));
            return send_json_status(req, response, 400, "Bad Request");
        }
        if (espclaw_ota_manager_write(chunk, (size_t)received, message, sizeof(message)) != ESP_OK) {
            espclaw_ota_manager_abort(message, sizeof(message));
            free(chunk);
            espclaw_admin_render_result_json(false, message, response, sizeof(response));
            return send_json_status(req, response, 500, "Internal Server Error");
        }
        total_received += received;
    }

    free(chunk);
    if (espclaw_ota_manager_finish(true, message, sizeof(message)) != ESP_OK) {
        espclaw_admin_render_result_json(false, message, response, sizeof(response));
        return send_json_status(req, response, 500, "Internal Server Error");
    }

    espclaw_admin_render_result_json(true, message, response, sizeof(response));
    return send_json(req, response);
}

static esp_err_t monitor_get_handler(httpd_req_t *req)
{
    char buffer[2048];
    espclaw_system_monitor_snapshot_t snapshot;
    const espclaw_runtime_status_t *status = espclaw_runtime_status();

    if (status == NULL) {
        espclaw_admin_render_result_json(false, "runtime status is unavailable", buffer, sizeof(buffer));
        return send_json_status(req, buffer, 503, "Service Unavailable");
    }
    if (espclaw_system_monitor_snapshot(
            &status->profile,
            status->storage_total_bytes,
            status->storage_used_bytes,
            &snapshot) != 0) {
        espclaw_admin_render_result_json(false, "failed to capture monitor snapshot", buffer, sizeof(buffer));
        return send_json_status(req, buffer, 500, "Internal Server Error");
    }

    espclaw_render_system_monitor_json(&snapshot, buffer, sizeof(buffer));
    return send_json(req, buffer);
}

static esp_err_t camera_get_handler(httpd_req_t *req)
{
    char buffer[1024];
    espclaw_hw_camera_status_t status;

    if (espclaw_hw_camera_status(&status) != 0) {
        espclaw_admin_render_result_json(false, "failed to capture camera diagnostics", buffer, sizeof(buffer));
        return send_json_status(req, buffer, 500, "Internal Server Error");
    }

    espclaw_render_camera_status_json(&status, buffer, sizeof(buffer));
    return send_json(req, buffer);
}

static esp_err_t camera_capture_post_handler(httpd_req_t *req)
{
    char buffer[1024];
    char filename[64];
    espclaw_hw_camera_capture_t capture;
    const espclaw_runtime_status_t *status = espclaw_runtime_status();
    const char *workspace_root = "/workspace";

    if (status != NULL && status->workspace_root[0] != '\0') {
        workspace_root = status->workspace_root;
    }
    if (!load_query_value(req, "filename", filename, sizeof(filename))) {
        filename[0] = '\0';
    }
    if (espclaw_hw_camera_capture(
            workspace_root,
            filename[0] != '\0' ? filename : NULL,
            &capture) != 0) {
        snprintf(buffer, sizeof(buffer), "{\"ok\":false,\"error\":");
        append_escaped_json(buffer, sizeof(buffer), strlen(buffer), capture.error[0] != '\0' ? capture.error : "camera capture failed");
        append_text(buffer, sizeof(buffer), "}");
        return send_json_status(req, buffer, 500, "Internal Server Error");
    }

    snprintf(
        buffer,
        sizeof(buffer),
        "{\"ok\":true,\"path\":\"%s\",\"mime_type\":\"%s\",\"bytes\":%u,\"width\":%u,\"height\":%u,\"simulated\":%s}",
        capture.relative_path,
        capture.mime_type,
        (unsigned)capture.bytes_written,
        (unsigned)capture.width,
        (unsigned)capture.height,
        capture.simulated ? "true" : "false"
    );
    return send_json(req, buffer);
}

static esp_err_t media_get_handler(httpd_req_t *req)
{
    char relative_path[256];
    char absolute_path[512];
    const espclaw_runtime_status_t *status = espclaw_runtime_status();
    const char *uri = req != NULL ? req->uri : NULL;
    struct stat file_stat;

    if (status == NULL || !status->storage_ready || status->workspace_root[0] == '\0') {
        return send_json_status(req, "{\"ok\":false,\"message\":\"workspace storage is not available\"}", 503, "Service Unavailable");
    }
    if (uri == NULL || strncmp(uri, "/media/", 7) != 0 || uri[7] == '\0') {
        return send_json_status(req, "{\"ok\":false,\"message\":\"missing media path\"}", 400, "Bad Request");
    }

    snprintf(relative_path, sizeof(relative_path), "media/%s", uri + 7);
    if (espclaw_workspace_resolve_path(status->workspace_root, relative_path, absolute_path, sizeof(absolute_path)) != 0 ||
        stat(absolute_path, &file_stat) != 0 || !S_ISREG(file_stat.st_mode)) {
        return send_json_status(req, "{\"ok\":false,\"message\":\"media file not found\"}", 404, "Not Found");
    }

    return send_file_response(req, absolute_path, media_content_type(relative_path));
}

static esp_err_t board_get_handler(httpd_req_t *req)
{
    char buffer[4096];

    espclaw_render_board_json(espclaw_board_current(), buffer, sizeof(buffer));
    return send_json(req, buffer);
}

static esp_err_t board_presets_get_handler(httpd_req_t *req)
{
    char buffer[4096];
    const espclaw_runtime_status_t *status = espclaw_runtime_status();

    espclaw_render_board_presets_json(status != NULL ? &status->profile : NULL, buffer, sizeof(buffer));
    return send_json(req, buffer);
}

static esp_err_t board_config_get_handler(httpd_req_t *req)
{
    char buffer[4096];
    const espclaw_runtime_status_t *status = espclaw_runtime_status();

    espclaw_render_board_config_json(
        status != NULL ? status->workspace_root : NULL,
        status != NULL ? &status->profile : NULL,
        espclaw_board_current(),
        buffer,
        sizeof(buffer)
    );
    return send_json(req, buffer);
}

static esp_err_t board_config_put_handler(httpd_req_t *req)
{
    char body[4096];
    char response[4096];
    const espclaw_runtime_status_t *status = espclaw_runtime_status();

    if (status == NULL || !status->storage_ready || status->workspace_root[0] == '\0') {
        espclaw_admin_render_result_json(false, "workspace storage is not ready", response, sizeof(response));
        return send_json_status(req, response, 503, "Service Unavailable");
    }
    if (read_request_body(req, body, sizeof(body)) != ESP_OK || body[0] == '\0') {
        espclaw_admin_render_result_json(false, "missing board config body", response, sizeof(response));
        return send_json_status(req, response, 400, "Bad Request");
    }
    if (espclaw_workspace_write_file(status->workspace_root, "config/board.json", body) != 0 ||
        espclaw_board_configure_current(status->workspace_root, &status->profile) != 0) {
        espclaw_admin_render_result_json(false, "failed to save board config", response, sizeof(response));
        return send_json_status(req, response, 500, "Internal Server Error");
    }

    espclaw_render_board_config_json(
        status->workspace_root,
        &status->profile,
        espclaw_board_current(),
        response,
        sizeof(response)
    );
    return send_json(req, response);
}

static esp_err_t board_apply_post_handler(httpd_req_t *req)
{
    char variant_id[ESPCLAW_BOARD_VARIANT_MAX + 1];
    char response[4096];
    const espclaw_runtime_status_t *status = espclaw_runtime_status();

    if (status == NULL || !status->storage_ready || status->workspace_root[0] == '\0') {
        espclaw_admin_render_result_json(false, "workspace storage is not ready", response, sizeof(response));
        return send_json_status(req, response, 503, "Service Unavailable");
    }
    if (!load_query_value(req, "variant_id", variant_id, sizeof(variant_id))) {
        espclaw_admin_render_result_json(false, "missing variant_id query parameter", response, sizeof(response));
        return send_json_status(req, response, 400, "Bad Request");
    }
    if (espclaw_board_write_variant_config(status->workspace_root, variant_id) != 0 ||
        espclaw_board_configure_current(status->workspace_root, &status->profile) != 0) {
        espclaw_admin_render_result_json(false, "failed to apply board preset", response, sizeof(response));
        return send_json_status(req, response, 500, "Internal Server Error");
    }

    espclaw_render_board_json(espclaw_board_current(), response, sizeof(response));
    return send_json(req, response);
}

static size_t render_wifi_status_json(char *buffer, size_t buffer_size)
{
    const espclaw_runtime_status_t *status = espclaw_runtime_status();

    if (buffer == NULL || buffer_size == 0) {
        return 0;
    }

    return (size_t)snprintf(
        buffer,
        buffer_size,
        "{\"ok\":true,\"wifi_ready\":%s,\"wifi_boot_deferred\":%s,\"provisioning_active\":%s,\"storage_ready\":%s,\"provisioning_transport\":",
        status != NULL && status->wifi_ready ? "true" : "false",
        status != NULL && status->wifi_boot_deferred ? "true" : "false",
        status != NULL && status->provisioning_active ? "true" : "false",
        status != NULL && status->storage_ready ? "true" : "false"
    );
}

static size_t render_wifi_status_prefix(char *buffer, size_t buffer_size)
{
    size_t used;
    espclaw_provisioning_descriptor_t provisioning;
    const espclaw_runtime_status_t *status = espclaw_runtime_status();

    used = render_wifi_status_json(buffer, buffer_size);
    espclaw_provisioning_descriptor_init(&provisioning);
    if (espclaw_runtime_get_provisioning_descriptor(&provisioning) == ESP_OK) {
        used = append_escaped_json(buffer, buffer_size, used, provisioning.transport);
        append_text(buffer, buffer_size, ",\"ssid\":");
        used = strlen(buffer);
        used = append_escaped_json(
            buffer,
            buffer_size,
            used,
            status != NULL ? status->wifi_ssid : ""
        );
        append_text(buffer, buffer_size, ",\"onboarding_ssid\":");
        used = strlen(buffer);
        used = append_escaped_json(
            buffer,
            buffer_size,
            used,
            status != NULL ? status->onboarding_ssid : ""
        );
        append_text(buffer, buffer_size, ",\"admin_url\":");
        used = strlen(buffer);
        used = append_escaped_json(buffer, buffer_size, used, provisioning.admin_url);
    } else {
        used = append_escaped_json(buffer, buffer_size, used, "");
        append_text(buffer, buffer_size, ",\"ssid\":\"\",\"onboarding_ssid\":\"\",\"admin_url\":\"\"");
    }
    return used;
}

static esp_err_t network_provisioning_get_handler(httpd_req_t *req)
{
    char buffer[1024];
    espclaw_provisioning_descriptor_t descriptor;

    espclaw_provisioning_descriptor_init(&descriptor);
    if (espclaw_runtime_get_provisioning_descriptor(&descriptor) != ESP_OK) {
        espclaw_admin_render_result_json(false, "failed to describe provisioning", buffer, sizeof(buffer));
        return send_json_status(req, buffer, 500, "Internal Server Error");
    }

    espclaw_provisioning_render_json(&descriptor, buffer, sizeof(buffer));
    return send_json(req, buffer);
}

static size_t render_wifi_scan_json(
    const espclaw_wifi_network_t *networks,
    size_t count,
    char *buffer,
    size_t buffer_size
)
{
    size_t used = 0;
    size_t index;

    if (buffer == NULL || buffer_size == 0) {
        return 0;
    }

    used += (size_t)snprintf(buffer, buffer_size, "{\"ok\":true,\"networks\":[");
    for (index = 0; index < count && used + 64 < buffer_size; ++index) {
        if (index > 0) {
            used += (size_t)snprintf(buffer + used, buffer_size - used, ",");
        }
        used += (size_t)snprintf(buffer + used, buffer_size - used, "{\"ssid\":");
        used = append_escaped_json(buffer, buffer_size, used, networks[index].ssid);
        used += (size_t)snprintf(
            buffer + used,
            buffer_size - used,
            ",\"rssi\":%d,\"channel\":%d,\"secure\":%s}",
            networks[index].rssi,
            networks[index].channel,
            networks[index].secure ? "true" : "false"
        );
    }
    used += (size_t)snprintf(buffer + used, buffer_size - used, "]}");
    return used >= buffer_size ? buffer_size - 1 : used;
}

static esp_err_t network_status_get_handler(httpd_req_t *req)
{
    char buffer[256];
    render_wifi_status_prefix(buffer, sizeof(buffer));
    append_text(buffer, sizeof(buffer), "}");
    return send_json(req, buffer);
}

static esp_err_t network_scan_get_handler(httpd_req_t *req)
{
    char buffer[4096];
    espclaw_wifi_network_t networks[12];
    size_t count = 0;

    if (espclaw_runtime_wifi_scan(networks, sizeof(networks) / sizeof(networks[0]), &count) != ESP_OK) {
        espclaw_admin_render_result_json(false, "failed to scan for Wi-Fi networks", buffer, sizeof(buffer));
        return send_json_status(req, buffer, 500, "Internal Server Error");
    }
    render_wifi_scan_json(networks, count, buffer, sizeof(buffer));
    return send_json(req, buffer);
}

static esp_err_t network_join_post_handler(httpd_req_t *req)
{
    char body[1024];
    char ssid[64];
    char password[128];
    char message[256];
    char response[512];
    size_t used;

    if (read_request_body(req, body, sizeof(body)) != ESP_OK) {
        espclaw_admin_render_result_json(false, "failed to read join request", response, sizeof(response));
        return send_json_status(req, response, 400, "Bad Request");
    }
    if (!espclaw_admin_json_string_value(body, "ssid", ssid, sizeof(ssid))) {
        espclaw_admin_render_result_json(false, "missing ssid", response, sizeof(response));
        return send_json_status(req, response, 400, "Bad Request");
    }
    if (!espclaw_admin_json_string_value(body, "password", password, sizeof(password))) {
        password[0] = '\0';
    }
    if (espclaw_runtime_wifi_join(ssid, password, message, sizeof(message)) != ESP_OK) {
        espclaw_admin_render_result_json(false, message[0] != '\0' ? message : "failed to join network", response, sizeof(response));
        return send_json_status(req, response, 500, "Internal Server Error");
    }

    used = render_wifi_status_prefix(response, sizeof(response));
    append_text(response, sizeof(response), ",\"message\":");
    used = strlen(response);
    used = append_escaped_json(response, sizeof(response), used, message);
    (void)used;
    append_text(response, sizeof(response), "}");
    return send_json(req, response);
}

static void merge_auth_profile_from_body(espclaw_auth_profile_t *profile, const char *body)
{
    int64_t expires_at = 0;

    if (profile == NULL || body == NULL) {
        return;
    }

    (void)espclaw_admin_json_string_value(body, "provider_id", profile->provider_id, sizeof(profile->provider_id));
    (void)espclaw_admin_json_string_value(body, "model", profile->model, sizeof(profile->model));
    (void)espclaw_admin_json_string_value(body, "base_url", profile->base_url, sizeof(profile->base_url));
    (void)espclaw_admin_json_string_value(body, "access_token", profile->access_token, sizeof(profile->access_token));
    (void)espclaw_admin_json_string_value(body, "refresh_token", profile->refresh_token, sizeof(profile->refresh_token));
    (void)espclaw_admin_json_string_value(body, "account_id", profile->account_id, sizeof(profile->account_id));
    (void)espclaw_admin_json_string_value(body, "source", profile->source, sizeof(profile->source));
    if (espclaw_admin_json_i64_value(body, "expires_at", &expires_at)) {
        profile->expires_at = expires_at;
    }
    profile->configured = profile->access_token[0] != '\0';
    if (profile->source[0] == '\0') {
        copy_text(profile->source, sizeof(profile->source), "admin");
    }
}

static esp_err_t auth_put_handler(httpd_req_t *req)
{
    char *body = NULL;
    char response[768];
    espclaw_auth_profile_t *profile = NULL;

    if (req->content_len <= 0 || req->content_len > 8191) {
        espclaw_admin_render_result_json(false, "missing auth body", response, sizeof(response));
        return send_json_status(req, response, 400, "Bad Request");
    }
    body = calloc(1, (size_t)req->content_len + 1);
    profile = calloc(1, sizeof(*profile));
    if (body == NULL || profile == NULL) {
        free(body);
        free(profile);
        return send_json_status(req, "{\"ok\":false,\"message\":\"out of memory\"}", 500, "Internal Server Error");
    }

    espclaw_auth_profile_default(profile);
    espclaw_auth_store_load(profile);
    if (read_request_body(req, body, (size_t)req->content_len + 1) != ESP_OK) {
        free(body);
        free(profile);
        espclaw_admin_render_result_json(false, "failed to read request body", response, sizeof(response));
        return send_json_status(req, response, 400, "Bad Request");
    }

    merge_auth_profile_from_body(profile, body);
    free(body);
    if (!espclaw_auth_profile_is_ready(profile)) {
        free(profile);
        espclaw_admin_render_result_json(false, "provider credentials are incomplete for the selected provider", response, sizeof(response));
        return send_json_status(req, response, 400, "Bad Request");
    }
    if (espclaw_auth_store_save(profile) != 0) {
        free(profile);
        espclaw_admin_render_result_json(false, "failed to save credentials", response, sizeof(response));
        return send_json_status(req, response, 500, "Internal Server Error");
    }

    espclaw_render_auth_profile_json(profile, response, sizeof(response));
    free(profile);
    return send_json(req, response);
}

static esp_err_t auth_delete_handler(httpd_req_t *req)
{
    char response[256];

    if (espclaw_auth_store_clear() != 0) {
        espclaw_admin_render_result_json(false, "failed to clear credentials", response, sizeof(response));
        return send_json_status(req, response, 500, "Internal Server Error");
    }

    espclaw_admin_render_result_json(true, "credentials cleared", response, sizeof(response));
    return send_json(req, response);
}

static esp_err_t auth_import_codex_cli_post_handler(httpd_req_t *req)
{
    char response[768];
    espclaw_auth_profile_t *profile = calloc(1, sizeof(*profile));

    if (profile == NULL) {
        return send_json_status(req, "{\"ok\":false,\"message\":\"out of memory\"}", 500, "Internal Server Error");
    }

    if (espclaw_auth_store_import_codex_cli(NULL, profile, response, sizeof(response)) != 0) {
        free(profile);
        espclaw_admin_render_result_json(false, response, response, sizeof(response));
        return send_json_status(req, response, 400, "Bad Request");
    }

    espclaw_render_auth_profile_json(profile, response, sizeof(response));
    free(profile);
    return send_json(req, response);
}

static esp_err_t auth_import_json_post_handler(httpd_req_t *req)
{
    char *body = NULL;
    char response[768];
    espclaw_auth_profile_t *profile = NULL;
    esp_err_t read_status;

    if (req->content_len <= 0 || req->content_len > 8191) {
        espclaw_admin_render_result_json(false, "missing auth json body", response, sizeof(response));
        return send_json_status(req, response, 400, "Bad Request");
    }
    body = (char *)calloc(1, (size_t)req->content_len + 1);
    profile = calloc(1, sizeof(*profile));
    if (body == NULL || profile == NULL) {
        free(profile);
        free(body);
        espclaw_admin_render_result_json(false, "out of memory while reading auth json", response, sizeof(response));
        return send_json_status(req, response, 500, "Internal Server Error");
    }
    read_status = read_request_body(req, body, (size_t)req->content_len + 1);
    if (read_status != ESP_OK) {
        free(body);
        free(profile);
        espclaw_admin_render_result_json(false, "missing auth json body", response, sizeof(response));
        return send_json_status(req, response, 400, "Bad Request");
    }
    if (espclaw_auth_store_import_json(body, profile, response, sizeof(response)) != 0) {
        free(body);
        free(profile);
        espclaw_admin_render_result_json(false, response, response, sizeof(response));
        return send_json_status(req, response, 400, "Bad Request");
    }

    free(body);
    espclaw_render_auth_profile_json(profile, response, sizeof(response));
    free(profile);
    return send_json(req, response);
}

static esp_err_t chat_run_post_handler(httpd_req_t *req)
{
    char session_id[ESPCLAW_AGENT_SESSION_ID_MAX + 1];
    char yolo_value[8];
    char *response = NULL;
    char *body = NULL;
    espclaw_agent_run_result_t *result = NULL;
    const espclaw_runtime_status_t *status = espclaw_runtime_status();
    bool yolo_mode = false;

    response = (char *)calloc(1, 12288);
    if (response == NULL) {
        return send_json_status(req, "{\"ok\":false,\"message\":\"out of memory\"}", 500, "Internal Server Error");
    }
    body = (char *)calloc(1, 2048);
    result = (espclaw_agent_run_result_t *)calloc(1, sizeof(*result));
    if (body == NULL || result == NULL) {
        free(result);
        free(body);
        free(response);
        return send_json_status(req, "{\"ok\":false,\"message\":\"out of memory\"}", 500, "Internal Server Error");
    }
    if (status == NULL) {
        espclaw_admin_render_result_json(false, "runtime status is not available", response, 12288);
        send_json_status(req, response, 503, "Service Unavailable");
        free(result);
        free(body);
        free(response);
        return ESP_OK;
    }
    if (!load_query_value(req, "session_id", session_id, sizeof(session_id))) {
        copy_text(session_id, sizeof(session_id), "admin");
    }
    if (load_query_value(req, "yolo", yolo_value, sizeof(yolo_value))) {
        yolo_mode = strtoul(yolo_value, NULL, 10) != 0U;
    } else {
        yolo_mode = espclaw_runtime_get_yolo_mode();
    }
    if (req->content_len <= 0 || read_request_body(req, body, 2048) != ESP_OK) {
        espclaw_admin_render_result_json(false, "missing chat body", response, 12288);
        send_json_status(req, response, 400, "Bad Request");
        free(result);
        free(body);
        free(response);
        return ESP_OK;
    }
    if (espclaw_console_run(
            status->storage_ready ? status->workspace_root : NULL,
            session_id,
            body,
            true,
            yolo_mode,
            result) != 0 &&
        !result->ok) {
        espclaw_admin_render_result_json(false, result->final_text, response, 12288);
        send_json_status(req, response, 500, "Internal Server Error");
        free(result);
        free(body);
        free(response);
        return ESP_OK;
    }

    snprintf(
        response,
        12288,
        "{\"ok\":true,\"session_id\":\"%s\",\"iterations\":%u,\"used_tools\":%s,\"response_id\":\"%s\",\"final_text\":",
        session_id,
        result->iterations,
        result->used_tools ? "true" : "false",
        result->response_id
    );
    append_escaped_json(response, 12288, strlen(response), result->final_text);
    append_text(response, 12288, "}");
    send_json(req, response);
    free(result);
    free(body);
    free(response);
    return ESP_OK;
}

static bool parse_operator_surface(const char *value, espclaw_operator_surface_t *surface_out)
{
    if (value == NULL || surface_out == NULL) {
        return false;
    }
    if (strcmp(value, "web") == 0) {
        *surface_out = ESPCLAW_OPERATOR_SURFACE_WEB;
        return true;
    }
    if (strcmp(value, "uart") == 0) {
        *surface_out = ESPCLAW_OPERATOR_SURFACE_UART;
        return true;
    }
    if (strcmp(value, "telegram") == 0) {
        *surface_out = ESPCLAW_OPERATOR_SURFACE_TELEGRAM;
        return true;
    }
    return false;
}

static esp_err_t bench_operator_turn_post_handler(httpd_req_t *req)
{
    char session_id[ESPCLAW_AGENT_SESSION_ID_MAX + 1];
    char surface_value[16];
    char yolo_value[8];
    char *response = NULL;
    char *body = NULL;
    espclaw_agent_run_result_t *result = NULL;
    espclaw_operator_bench_metrics_t metrics;
    espclaw_operator_surface_t surface;
    bool yolo_mode = false;
    esp_err_t err;

    response = (char *)calloc(1, 12288);
    body = (char *)calloc(1, 2048);
    result = (espclaw_agent_run_result_t *)calloc(1, sizeof(*result));
    if (response == NULL || body == NULL || result == NULL) {
        free(result);
        free(body);
        free(response);
        return send_json_status(req, "{\"ok\":false,\"message\":\"out of memory\"}", 500, "Internal Server Error");
    }
    if (!load_query_value(req, "surface", surface_value, sizeof(surface_value)) ||
        !parse_operator_surface(surface_value, &surface)) {
        free(result);
        free(body);
        free(response);
        return send_json_status(req, "{\"ok\":false,\"message\":\"missing or invalid surface\"}", 400, "Bad Request");
    }
    if (!load_query_value(req, "session_id", session_id, sizeof(session_id))) {
        copy_text(session_id, sizeof(session_id), "bench");
    }
    if (load_query_value(req, "yolo", yolo_value, sizeof(yolo_value))) {
        yolo_mode = strtoul(yolo_value, NULL, 10) != 0U;
    } else {
        yolo_mode = espclaw_runtime_get_yolo_mode();
    }
    if (req->content_len <= 0 || read_request_body(req, body, 2048) != ESP_OK) {
        free(result);
        free(body);
        free(response);
        return send_json_status(req, "{\"ok\":false,\"message\":\"missing bench body\"}", 400, "Bad Request");
    }

    memset(&metrics, 0, sizeof(metrics));
    err = espclaw_runtime_bench_operator_turn(surface, session_id, body, true, yolo_mode, result, &metrics);
    snprintf(
        response,
        12288,
        "{\"ok\":%s,\"surface\":\"%s\",\"session_id\":\"%s\",\"iterations\":%u,\"used_tools\":%s,\"response_id\":\"%s\",\"final_text\":",
        (err == ESP_OK || result->ok) ? "true" : "false",
        surface_value,
        session_id,
        result->iterations,
        result->used_tools ? "true" : "false",
        result->response_id
    );
    append_escaped_json(response, 12288, strlen(response), result->final_text);
    snprintf(
        response + strlen(response),
        12288 - strlen(response),
        ",\"memory\":{\"free_internal_before\":%lu,\"largest_internal_before\":%lu,"
        "\"free_internal_after\":%lu,\"largest_internal_after\":%lu}}",
        (unsigned long)metrics.free_internal_before,
        (unsigned long)metrics.largest_internal_before,
        (unsigned long)metrics.free_internal_after,
        (unsigned long)metrics.largest_internal_after
    );

    send_json_status(req, response, (err == ESP_OK || result->ok) ? 200 : 500, (err == ESP_OK || result->ok) ? "OK" : "Internal Server Error");
    free(result);
    free(body);
    free(response);
    return ESP_OK;
}

static esp_err_t chat_session_get_handler(httpd_req_t *req)
{
    char session_id[ESPCLAW_AGENT_SESSION_ID_MAX + 1];
    char *response = NULL;
    const espclaw_runtime_status_t *status = espclaw_runtime_status();

    response = (char *)calloc(1, 9216);
    if (response == NULL) {
        return send_json_status(req, "{\"ok\":false,\"message\":\"out of memory\"}", 500, "Internal Server Error");
    }
    if (status == NULL || !status->storage_ready) {
        espclaw_admin_render_result_json(false, "workspace storage is not available", response, 9216);
        send_json_status(req, response, 503, "Service Unavailable");
        free(response);
        return ESP_OK;
    }
    if (!load_query_value(req, "session_id", session_id, sizeof(session_id))) {
        copy_text(session_id, sizeof(session_id), "admin");
    }

    espclaw_render_session_transcript_json(status->workspace_root, session_id, response, 9216);
    send_json(req, response);
    free(response);
    return ESP_OK;
}

static esp_err_t workspace_get_handler(httpd_req_t *req)
{
    char buffer[1024];
    const espclaw_runtime_status_t *status = espclaw_runtime_status();

    espclaw_render_workspace_files_json(
        status != NULL && status->storage_ready ? status->workspace_root : NULL,
        buffer,
        sizeof(buffer)
    );
    return send_json(req, buffer);
}

static esp_err_t blob_status_get_handler(httpd_req_t *req)
{
    char blob_id[65];
    char response[768];
    espclaw_workspace_blob_status_t status;
    const espclaw_runtime_status_t *runtime = espclaw_runtime_status();

    if (runtime == NULL || !runtime->storage_ready) {
        espclaw_admin_render_result_json(false, "workspace storage is not available", response, sizeof(response));
        return send_json_status(req, response, 503, "Service Unavailable");
    }
    if (!load_query_value(req, "blob_id", blob_id, sizeof(blob_id))) {
        espclaw_admin_render_result_json(false, "missing blob_id query parameter", response, sizeof(response));
        return send_json_status(req, response, 400, "Bad Request");
    }
    if (espclaw_workspace_blob_status(runtime->workspace_root, blob_id, &status) != 0 &&
        status.stage == ESPCLAW_WORKSPACE_BLOB_STAGE_NONE) {
        espclaw_admin_render_result_json(false, "blob not found", response, sizeof(response));
        return send_json_status(req, response, 404, "Not Found");
    }

    snprintf(
        response,
        sizeof(response),
        "{\"ok\":true,\"blob_id\":\"%s\",\"stage\":\"%s\",\"bytes_written\":%lu,\"target_path\":",
        status.blob_id,
        blob_stage_name(status.stage),
        (unsigned long)status.bytes_written
    );
    append_escaped_json(response, sizeof(response), strlen(response), status.target_path);
    append_text(response, sizeof(response), ",\"content_type\":");
    append_escaped_json(response, sizeof(response), strlen(response), status.content_type);
    append_text(response, sizeof(response), "}");
    return send_json(req, response);
}

static esp_err_t context_chunks_get_handler(httpd_req_t *req)
{
    char path[ESPCLAW_CONTEXT_PATH_MAX + 1];
    char response[1024];
    const espclaw_runtime_status_t *runtime = espclaw_runtime_status();
    uint32_t chunk_bytes;

    if (runtime == NULL || !runtime->storage_ready) {
        espclaw_admin_render_result_json(false, "workspace storage is not available", response, sizeof(response));
        return send_json_status(req, response, 503, "Service Unavailable");
    }
    if (!load_query_value(req, "path", path, sizeof(path))) {
        espclaw_admin_render_result_json(false, "missing path query parameter", response, sizeof(response));
        return send_json_status(req, response, 400, "Bad Request");
    }
    chunk_bytes = load_query_u32(req, "chunk_bytes", 0U);
    if (espclaw_context_render_chunks_json(runtime->workspace_root, path, (size_t)chunk_bytes, response, sizeof(response)) != 0) {
        return send_json_status(req, response, 404, "Not Found");
    }
    return send_json(req, response);
}

static esp_err_t context_load_get_handler(httpd_req_t *req)
{
    char path[ESPCLAW_CONTEXT_PATH_MAX + 1];
    char response[4096];
    const espclaw_runtime_status_t *runtime = espclaw_runtime_status();
    uint32_t chunk_index;
    uint32_t chunk_bytes;

    if (runtime == NULL || !runtime->storage_ready) {
        espclaw_admin_render_result_json(false, "workspace storage is not available", response, sizeof(response));
        return send_json_status(req, response, 503, "Service Unavailable");
    }
    if (!load_query_value(req, "path", path, sizeof(path))) {
        espclaw_admin_render_result_json(false, "missing path query parameter", response, sizeof(response));
        return send_json_status(req, response, 400, "Bad Request");
    }
    chunk_index = load_query_u32(req, "chunk_index", UINT32_MAX);
    if (chunk_index == UINT32_MAX) {
        espclaw_admin_render_result_json(false, "missing chunk_index query parameter", response, sizeof(response));
        return send_json_status(req, response, 400, "Bad Request");
    }
    chunk_bytes = load_query_u32(req, "chunk_bytes", 0U);
    if (espclaw_context_render_chunk_json(runtime->workspace_root, path, (size_t)chunk_index, (size_t)chunk_bytes, response, sizeof(response)) != 0) {
        return send_json_status(req, response, 404, "Not Found");
    }
    return send_json(req, response);
}

static esp_err_t context_search_get_handler(httpd_req_t *req)
{
    char path[ESPCLAW_CONTEXT_PATH_MAX + 1];
    char query[ESPCLAW_CONTEXT_QUERY_MAX + 1];
    char response[6144];
    const espclaw_runtime_status_t *runtime = espclaw_runtime_status();
    uint32_t chunk_bytes;
    uint32_t limit;

    if (runtime == NULL || !runtime->storage_ready) {
        espclaw_admin_render_result_json(false, "workspace storage is not available", response, sizeof(response));
        return send_json_status(req, response, 503, "Service Unavailable");
    }
    if (!load_query_value(req, "path", path, sizeof(path)) ||
        !load_query_value(req, "query", query, sizeof(query))) {
        espclaw_admin_render_result_json(false, "missing path or query parameter", response, sizeof(response));
        return send_json_status(req, response, 400, "Bad Request");
    }
    chunk_bytes = load_query_u32(req, "chunk_bytes", 0U);
    limit = load_query_u32(req, "limit", 0U);
    if (espclaw_context_search_json(runtime->workspace_root, path, query, (size_t)chunk_bytes, (size_t)limit, response, sizeof(response)) != 0) {
        return send_json_status(req, response, 404, "Not Found");
    }
    return send_json(req, response);
}

static esp_err_t context_select_get_handler(httpd_req_t *req)
{
    char path[ESPCLAW_CONTEXT_PATH_MAX + 1];
    char query[ESPCLAW_CONTEXT_QUERY_MAX + 1];
    char response[8192];
    const espclaw_runtime_status_t *runtime = espclaw_runtime_status();
    uint32_t chunk_bytes;
    uint32_t limit;
    uint32_t output_bytes;

    if (runtime == NULL || !runtime->storage_ready) {
        espclaw_admin_render_result_json(false, "workspace storage is not available", response, sizeof(response));
        return send_json_status(req, response, 503, "Service Unavailable");
    }
    if (!load_query_value(req, "path", path, sizeof(path)) ||
        !load_query_value(req, "query", query, sizeof(query))) {
        espclaw_admin_render_result_json(false, "missing path or query parameter", response, sizeof(response));
        return send_json_status(req, response, 400, "Bad Request");
    }
    chunk_bytes = load_query_u32(req, "chunk_bytes", 0U);
    limit = load_query_u32(req, "limit", 0U);
    output_bytes = load_query_u32(req, "output_bytes", 0U);
    if (espclaw_context_select_json(
            runtime->workspace_root,
            path,
            query,
            (size_t)chunk_bytes,
            (size_t)limit,
            (size_t)output_bytes,
            response,
            sizeof(response)) != 0) {
        return send_json_status(req, response, 404, "Not Found");
    }
    return send_json(req, response);
}

static esp_err_t context_summarize_get_handler(httpd_req_t *req)
{
    char path[ESPCLAW_CONTEXT_PATH_MAX + 1];
    char query[ESPCLAW_CONTEXT_QUERY_MAX + 1];
    char response[6144];
    const espclaw_runtime_status_t *runtime = espclaw_runtime_status();
    uint32_t chunk_bytes;
    uint32_t limit;
    uint32_t summary_bytes;

    if (runtime == NULL || !runtime->storage_ready) {
        espclaw_admin_render_result_json(false, "workspace storage is not available", response, sizeof(response));
        return send_json_status(req, response, 503, "Service Unavailable");
    }
    if (!load_query_value(req, "path", path, sizeof(path)) ||
        !load_query_value(req, "query", query, sizeof(query))) {
        espclaw_admin_render_result_json(false, "missing path or query parameter", response, sizeof(response));
        return send_json_status(req, response, 400, "Bad Request");
    }
    chunk_bytes = load_query_u32(req, "chunk_bytes", 0U);
    limit = load_query_u32(req, "limit", 0U);
    summary_bytes = load_query_u32(req, "summary_bytes", 0U);
    if (espclaw_context_summarize_json(
            runtime->workspace_root,
            path,
            query,
            (size_t)chunk_bytes,
            (size_t)limit,
            (size_t)summary_bytes,
            response,
            sizeof(response)) != 0) {
        return send_json_status(req, response, 404, "Not Found");
    }
    return send_json(req, response);
}

static esp_err_t apps_get_handler(httpd_req_t *req)
{
    char buffer[2048];
    const espclaw_runtime_status_t *status = espclaw_runtime_status();

    espclaw_render_apps_json(
        status != NULL && status->storage_ready ? status->workspace_root : NULL,
        buffer,
        sizeof(buffer)
    );
    return send_json(req, buffer);
}

static esp_err_t components_get_handler(httpd_req_t *req)
{
    char buffer[4096];
    const espclaw_runtime_status_t *status = espclaw_runtime_status();

    (void)req;
    espclaw_render_components_json(
        status != NULL && status->storage_ready ? status->workspace_root : NULL,
        buffer,
        sizeof(buffer)
    );
    return send_json(req, buffer);
}

static esp_err_t hardware_get_handler(httpd_req_t *req)
{
    char buffer[4096];

    (void)req;
    espclaw_render_hardware_json(espclaw_board_current(), buffer, sizeof(buffer));
    return send_json(req, buffer);
}

static esp_err_t loops_get_handler(httpd_req_t *req)
{
    char buffer[4096];

    espclaw_control_loop_render_json(buffer, sizeof(buffer));
    return send_json(req, buffer);
}

static esp_err_t tasks_get_handler(httpd_req_t *req)
{
    char buffer[6144];

    (void)req;
    espclaw_task_render_json(buffer, sizeof(buffer));
    return send_json(req, buffer);
}

static esp_err_t behaviors_get_handler(httpd_req_t *req)
{
    char buffer[6144];
    const espclaw_runtime_status_t *status = espclaw_runtime_status();

    if (status == NULL || !status->storage_ready) {
        espclaw_admin_render_result_json(false, "workspace storage is not available", buffer, sizeof(buffer));
        return send_json_status(req, buffer, 503, "Service Unavailable");
    }

    espclaw_render_behaviors_json(status->workspace_root, buffer, sizeof(buffer));
    return send_json(req, buffer);
}

static esp_err_t app_detail_get_handler(httpd_req_t *req)
{
    char app_id[ESPCLAW_APP_ID_MAX + 1];
    char buffer[4096];
    const espclaw_runtime_status_t *status = espclaw_runtime_status();

    if (status == NULL || !status->storage_ready) {
        espclaw_admin_render_result_json(false, "workspace storage is not available", buffer, sizeof(buffer));
        return send_json_status(req, buffer, 503, "Service Unavailable");
    }
    if (!load_query_value(req, "app_id", app_id, sizeof(app_id))) {
        espclaw_admin_render_result_json(false, "missing app_id query parameter", buffer, sizeof(buffer));
        return send_json_status(req, buffer, 400, "Bad Request");
    }

    espclaw_render_app_detail_json(status->workspace_root, app_id, buffer, sizeof(buffer));
    return send_json(req, buffer);
}

static esp_err_t component_detail_get_handler(httpd_req_t *req)
{
    char component_id[ESPCLAW_COMPONENT_ID_MAX + 1];
    char buffer[8192];
    const espclaw_runtime_status_t *status = espclaw_runtime_status();

    if (status == NULL || !status->storage_ready) {
        espclaw_admin_render_result_json(false, "workspace storage is not available", buffer, sizeof(buffer));
        return send_json_status(req, buffer, 503, "Service Unavailable");
    }
    if (!load_query_value(req, "component_id", component_id, sizeof(component_id))) {
        espclaw_admin_render_result_json(false, "missing component_id query parameter", buffer, sizeof(buffer));
        return send_json_status(req, buffer, 400, "Bad Request");
    }
    if (espclaw_render_component_detail_json(status->workspace_root, component_id, buffer, sizeof(buffer)) != 0) {
        espclaw_admin_render_result_json(false, "component not found", buffer, sizeof(buffer));
        return send_json_status(req, buffer, 404, "Not Found");
    }
    return send_json(req, buffer);
}

static esp_err_t app_scaffold_post_handler(httpd_req_t *req)
{
    char app_id[ESPCLAW_APP_ID_MAX + 1];
    char title[ESPCLAW_APP_TITLE_MAX + 1];
    char permissions[256];
    char triggers[256];
    char buffer[256];
    const espclaw_runtime_status_t *status = espclaw_runtime_status();

    if (status == NULL || !status->storage_ready) {
        espclaw_admin_render_result_json(false, "workspace storage is not available", buffer, sizeof(buffer));
        return send_json_status(req, buffer, 503, "Service Unavailable");
    }
    if (!load_query_value(req, "app_id", app_id, sizeof(app_id))) {
        espclaw_admin_render_result_json(false, "missing app_id query parameter", buffer, sizeof(buffer));
        return send_json_status(req, buffer, 400, "Bad Request");
    }
    title[0] = '\0';
    permissions[0] = '\0';
    triggers[0] = '\0';
    load_query_value(req, "title", title, sizeof(title));
    load_query_value(req, "permissions", permissions, sizeof(permissions));
    load_query_value(req, "triggers", triggers, sizeof(triggers));

    if (espclaw_admin_scaffold_app(status->workspace_root, app_id, title, permissions, triggers) == 0) {
        espclaw_admin_render_result_json(true, "app scaffolded", buffer, sizeof(buffer));
        return send_json(req, buffer);
    }

    espclaw_admin_render_result_json(false, "failed to scaffold app", buffer, sizeof(buffer));
    return send_json_status(req, buffer, 500, "Internal Server Error");
}

static esp_err_t component_install_post_handler(httpd_req_t *req)
{
    char component_id[ESPCLAW_COMPONENT_ID_MAX + 1];
    char title[ESPCLAW_COMPONENT_TITLE_MAX + 1];
    char module[ESPCLAW_COMPONENT_MODULE_MAX + 1];
    char summary[ESPCLAW_COMPONENT_SUMMARY_MAX + 1];
    char version[ESPCLAW_COMPONENT_VERSION_MAX + 1];
    char source[4096];
    char buffer[256];
    const espclaw_runtime_status_t *status = espclaw_runtime_status();

    if (status == NULL || !status->storage_ready) {
        espclaw_admin_render_result_json(false, "workspace storage is not available", buffer, sizeof(buffer));
        return send_json_status(req, buffer, 503, "Service Unavailable");
    }
    if (!load_query_value(req, "component_id", component_id, sizeof(component_id)) ||
        !load_query_value(req, "module", module, sizeof(module))) {
        espclaw_admin_render_result_json(false, "missing component_id or module query parameter", buffer, sizeof(buffer));
        return send_json_status(req, buffer, 400, "Bad Request");
    }
    title[0] = '\0';
    summary[0] = '\0';
    version[0] = '\0';
    load_query_value(req, "title", title, sizeof(title));
    load_query_value(req, "summary", summary, sizeof(summary));
    load_query_value(req, "version", version, sizeof(version));
    if (read_request_body(req, source, sizeof(source)) != ESP_OK) {
        espclaw_admin_render_result_json(false, "failed to read request body", buffer, sizeof(buffer));
        return send_json_status(req, buffer, 400, "Bad Request");
    }
    if (espclaw_component_install(status->workspace_root, component_id, title, module, summary, version, source) != 0) {
        espclaw_admin_render_result_json(false, "failed to install component", buffer, sizeof(buffer));
        return send_json_status(req, buffer, 500, "Internal Server Error");
    }
    espclaw_admin_render_result_json(true, "component installed", buffer, sizeof(buffer));
    return send_json(req, buffer);
}

static esp_err_t component_install_from_file_post_handler(httpd_req_t *req)
{
    char component_id[ESPCLAW_COMPONENT_ID_MAX + 1];
    char title[ESPCLAW_COMPONENT_TITLE_MAX + 1];
    char module[ESPCLAW_COMPONENT_MODULE_MAX + 1];
    char summary[ESPCLAW_COMPONENT_SUMMARY_MAX + 1];
    char version[ESPCLAW_COMPONENT_VERSION_MAX + 1];
    char source_path[256];
    char buffer[256];
    const espclaw_runtime_status_t *status = espclaw_runtime_status();

    if (status == NULL || !status->storage_ready) {
        espclaw_admin_render_result_json(false, "workspace storage is not available", buffer, sizeof(buffer));
        return send_json_status(req, buffer, 503, "Service Unavailable");
    }
    if (!load_query_value(req, "component_id", component_id, sizeof(component_id)) ||
        !load_query_value(req, "module", module, sizeof(module)) ||
        !load_query_value(req, "source_path", source_path, sizeof(source_path))) {
        espclaw_admin_render_result_json(false, "missing component_id, module, or source_path query parameter", buffer, sizeof(buffer));
        return send_json_status(req, buffer, 400, "Bad Request");
    }
    title[0] = '\0';
    summary[0] = '\0';
    version[0] = '\0';
    load_query_value(req, "title", title, sizeof(title));
    load_query_value(req, "summary", summary, sizeof(summary));
    load_query_value(req, "version", version, sizeof(version));
    if (espclaw_component_install_from_file(status->workspace_root, component_id, title, module, summary, version, source_path) != 0) {
        espclaw_admin_render_result_json(false, "failed to install component from file", buffer, sizeof(buffer));
        return send_json_status(req, buffer, 500, "Internal Server Error");
    }
    espclaw_admin_render_result_json(true, "component installed from file", buffer, sizeof(buffer));
    return send_json(req, buffer);
}

static esp_err_t component_install_from_blob_post_handler(httpd_req_t *req)
{
    char component_id[ESPCLAW_COMPONENT_ID_MAX + 1];
    char title[ESPCLAW_COMPONENT_TITLE_MAX + 1];
    char module[ESPCLAW_COMPONENT_MODULE_MAX + 1];
    char summary[ESPCLAW_COMPONENT_SUMMARY_MAX + 1];
    char version[ESPCLAW_COMPONENT_VERSION_MAX + 1];
    char blob_id[65];
    char buffer[256];
    const espclaw_runtime_status_t *status = espclaw_runtime_status();

    if (status == NULL || !status->storage_ready) {
        espclaw_admin_render_result_json(false, "workspace storage is not available", buffer, sizeof(buffer));
        return send_json_status(req, buffer, 503, "Service Unavailable");
    }
    if (!load_query_value(req, "component_id", component_id, sizeof(component_id)) ||
        !load_query_value(req, "module", module, sizeof(module)) ||
        !load_query_value(req, "blob_id", blob_id, sizeof(blob_id))) {
        espclaw_admin_render_result_json(false, "missing component_id, module, or blob_id query parameter", buffer, sizeof(buffer));
        return send_json_status(req, buffer, 400, "Bad Request");
    }
    title[0] = '\0';
    summary[0] = '\0';
    version[0] = '\0';
    load_query_value(req, "title", title, sizeof(title));
    load_query_value(req, "summary", summary, sizeof(summary));
    load_query_value(req, "version", version, sizeof(version));
    if (espclaw_component_install_from_blob(status->workspace_root, component_id, title, module, summary, version, blob_id) != 0) {
        espclaw_admin_render_result_json(false, "failed to install component from blob", buffer, sizeof(buffer));
        return send_json_status(req, buffer, 500, "Internal Server Error");
    }
    espclaw_admin_render_result_json(true, "component installed from blob", buffer, sizeof(buffer));
    return send_json(req, buffer);
}

static esp_err_t component_install_from_url_post_handler(httpd_req_t *req)
{
    char component_id[ESPCLAW_COMPONENT_ID_MAX + 1];
    char title[ESPCLAW_COMPONENT_TITLE_MAX + 1];
    char module[ESPCLAW_COMPONENT_MODULE_MAX + 1];
    char summary[ESPCLAW_COMPONENT_SUMMARY_MAX + 1];
    char version[ESPCLAW_COMPONENT_VERSION_MAX + 1];
    char source_url[512];
    char buffer[256];
    const espclaw_runtime_status_t *status = espclaw_runtime_status();

    if (status == NULL || !status->storage_ready) {
        espclaw_admin_render_result_json(false, "workspace storage is not available", buffer, sizeof(buffer));
        return send_json_status(req, buffer, 503, "Service Unavailable");
    }
    if (!load_query_value(req, "component_id", component_id, sizeof(component_id)) ||
        !load_query_value(req, "module", module, sizeof(module)) ||
        !load_query_value(req, "source_url", source_url, sizeof(source_url))) {
        espclaw_admin_render_result_json(false, "missing component_id, module, or source_url query parameter", buffer, sizeof(buffer));
        return send_json_status(req, buffer, 400, "Bad Request");
    }
    title[0] = '\0';
    summary[0] = '\0';
    version[0] = '\0';
    load_query_value(req, "title", title, sizeof(title));
    load_query_value(req, "summary", summary, sizeof(summary));
    load_query_value(req, "version", version, sizeof(version));
    if (espclaw_component_install_from_url(status->workspace_root, component_id, title, module, summary, version, source_url) != 0) {
        espclaw_admin_render_result_json(false, "failed to install component from url", buffer, sizeof(buffer));
        return send_json_status(req, buffer, 500, "Internal Server Error");
    }
    espclaw_admin_render_result_json(true, "component installed from url", buffer, sizeof(buffer));
    return send_json(req, buffer);
}

static esp_err_t component_install_from_manifest_post_handler(httpd_req_t *req)
{
    char manifest_url[ESPCLAW_COMPONENT_URL_MAX + 1];
    char buffer[256];
    const espclaw_runtime_status_t *status = espclaw_runtime_status();

    if (status == NULL || !status->storage_ready) {
        espclaw_admin_render_result_json(false, "workspace storage is not available", buffer, sizeof(buffer));
        return send_json_status(req, buffer, 503, "Service Unavailable");
    }
    if (!load_query_value(req, "manifest_url", manifest_url, sizeof(manifest_url))) {
        espclaw_admin_render_result_json(false, "missing manifest_url query parameter", buffer, sizeof(buffer));
        return send_json_status(req, buffer, 400, "Bad Request");
    }
    if (espclaw_component_install_from_manifest(status->workspace_root, manifest_url) != 0) {
        espclaw_admin_render_result_json(false, "failed to install component from manifest", buffer, sizeof(buffer));
        return send_json_status(req, buffer, 500, "Internal Server Error");
    }
    espclaw_admin_render_result_json(true, "component installed from manifest", buffer, sizeof(buffer));
    return send_json(req, buffer);
}

static esp_err_t blob_begin_post_handler(httpd_req_t *req)
{
    char blob_id[65];
    char target_path[256];
    char content_type[64];
    char response[768];
    espclaw_workspace_blob_status_t status;
    const espclaw_runtime_status_t *runtime = espclaw_runtime_status();

    if (runtime == NULL || !runtime->storage_ready) {
        espclaw_admin_render_result_json(false, "workspace storage is not available", response, sizeof(response));
        return send_json_status(req, response, 503, "Service Unavailable");
    }
    if (!load_query_value(req, "blob_id", blob_id, sizeof(blob_id))) {
        espclaw_admin_render_result_json(false, "missing blob_id query parameter", response, sizeof(response));
        return send_json_status(req, response, 400, "Bad Request");
    }
    target_path[0] = '\0';
    content_type[0] = '\0';
    load_query_value(req, "target_path", target_path, sizeof(target_path));
    load_query_value(req, "content_type", content_type, sizeof(content_type));
    if (espclaw_workspace_blob_begin(runtime->workspace_root, blob_id, target_path, content_type) != 0 ||
        espclaw_workspace_blob_status(runtime->workspace_root, blob_id, &status) != 0) {
        espclaw_admin_render_result_json(false, "failed to begin blob upload", response, sizeof(response));
        return send_json_status(req, response, 500, "Internal Server Error");
    }

    snprintf(
        response,
        sizeof(response),
        "{\"ok\":true,\"blob_id\":\"%s\",\"stage\":\"%s\",\"bytes_written\":%lu,\"target_path\":",
        status.blob_id,
        blob_stage_name(status.stage),
        (unsigned long)status.bytes_written
    );
    append_escaped_json(response, sizeof(response), strlen(response), status.target_path);
    append_text(response, sizeof(response), ",\"content_type\":");
    append_escaped_json(response, sizeof(response), strlen(response), status.content_type);
    append_text(response, sizeof(response), "}");
    return send_json(req, response);
}

static esp_err_t blob_append_post_handler(httpd_req_t *req)
{
    char blob_id[65];
    char response[512];
    unsigned char chunk[2048];
    size_t bytes_written = 0;
    const espclaw_runtime_status_t *runtime = espclaw_runtime_status();

    if (runtime == NULL || !runtime->storage_ready) {
        espclaw_admin_render_result_json(false, "workspace storage is not available", response, sizeof(response));
        return send_json_status(req, response, 503, "Service Unavailable");
    }
    if (!load_query_value(req, "blob_id", blob_id, sizeof(blob_id))) {
        espclaw_admin_render_result_json(false, "missing blob_id query parameter", response, sizeof(response));
        return send_json_status(req, response, 400, "Bad Request");
    }
    if (req->content_len <= 0) {
        espclaw_admin_render_result_json(false, "missing blob body", response, sizeof(response));
        return send_json_status(req, response, 400, "Bad Request");
    }

    while (bytes_written < (size_t)req->content_len) {
        int to_read = req->content_len - (int)bytes_written;
        int received;
        size_t total_after_write = 0;

        if (to_read > (int)sizeof(chunk)) {
            to_read = (int)sizeof(chunk);
        }
        received = httpd_req_recv(req, (char *)chunk, to_read);
        if (received <= 0 ||
            espclaw_workspace_blob_append(runtime->workspace_root, blob_id, chunk, (size_t)received, &total_after_write) != 0) {
            espclaw_admin_render_result_json(false, "failed to append blob chunk", response, sizeof(response));
            return send_json_status(req, response, 500, "Internal Server Error");
        }
        bytes_written = total_after_write;
    }

    snprintf(
        response,
        sizeof(response),
        "{\"ok\":true,\"blob_id\":\"%s\",\"stage\":\"open\",\"bytes_written\":%lu}",
        blob_id,
        (unsigned long)bytes_written
    );
    return send_json(req, response);
}

static esp_err_t blob_commit_post_handler(httpd_req_t *req)
{
    char blob_id[65];
    char response[768];
    const espclaw_runtime_status_t *runtime = espclaw_runtime_status();
    char committed_path[256];
    size_t bytes_written = 0;

    if (runtime == NULL || !runtime->storage_ready) {
        espclaw_admin_render_result_json(false, "workspace storage is not available", response, sizeof(response));
        return send_json_status(req, response, 503, "Service Unavailable");
    }
    if (!load_query_value(req, "blob_id", blob_id, sizeof(blob_id))) {
        espclaw_admin_render_result_json(false, "missing blob_id query parameter", response, sizeof(response));
        return send_json_status(req, response, 400, "Bad Request");
    }
    if (espclaw_workspace_blob_commit(runtime->workspace_root, blob_id, committed_path, sizeof(committed_path), &bytes_written) != 0) {
        espclaw_admin_render_result_json(false, "failed to commit blob", response, sizeof(response));
        return send_json_status(req, response, 500, "Internal Server Error");
    }

    snprintf(
        response,
        sizeof(response),
        "{\"ok\":true,\"blob_id\":\"%s\",\"stage\":\"committed\",\"bytes_written\":%lu,\"target_path\":",
        blob_id,
        (unsigned long)bytes_written
    );
    append_escaped_json(response, sizeof(response), strlen(response), committed_path);
    append_text(response, sizeof(response), "}");
    return send_json(req, response);
}

static esp_err_t component_delete_handler(httpd_req_t *req)
{
    char component_id[ESPCLAW_COMPONENT_ID_MAX + 1];
    char buffer[256];
    const espclaw_runtime_status_t *status = espclaw_runtime_status();

    if (status == NULL || !status->storage_ready) {
        espclaw_admin_render_result_json(false, "workspace storage is not available", buffer, sizeof(buffer));
        return send_json_status(req, buffer, 503, "Service Unavailable");
    }
    if (!load_query_value(req, "component_id", component_id, sizeof(component_id))) {
        espclaw_admin_render_result_json(false, "missing component_id query parameter", buffer, sizeof(buffer));
        return send_json_status(req, buffer, 400, "Bad Request");
    }
    if (espclaw_component_remove(status->workspace_root, component_id) != 0) {
        espclaw_admin_render_result_json(false, "failed to remove component", buffer, sizeof(buffer));
        return send_json_status(req, buffer, 500, "Internal Server Error");
    }
    espclaw_admin_render_result_json(true, "component removed", buffer, sizeof(buffer));
    return send_json(req, buffer);
}

static esp_err_t app_source_put_handler(httpd_req_t *req)
{
    char app_id[ESPCLAW_APP_ID_MAX + 1];
    char source[4096];
    char buffer[256];
    const espclaw_runtime_status_t *status = espclaw_runtime_status();

    if (status == NULL || !status->storage_ready) {
        espclaw_admin_render_result_json(false, "workspace storage is not available", buffer, sizeof(buffer));
        return send_json_status(req, buffer, 503, "Service Unavailable");
    }
    if (!load_query_value(req, "app_id", app_id, sizeof(app_id))) {
        espclaw_admin_render_result_json(false, "missing app_id query parameter", buffer, sizeof(buffer));
        return send_json_status(req, buffer, 400, "Bad Request");
    }
    if (read_request_body(req, source, sizeof(source)) != ESP_OK) {
        espclaw_admin_render_result_json(false, "failed to read request body", buffer, sizeof(buffer));
        return send_json_status(req, buffer, 400, "Bad Request");
    }

    if (espclaw_app_update_source(status->workspace_root, app_id, source) == 0) {
        espclaw_admin_render_result_json(true, "app source updated", buffer, sizeof(buffer));
        return send_json(req, buffer);
    }

    espclaw_admin_render_result_json(false, "failed to update app source", buffer, sizeof(buffer));
    return send_json_status(req, buffer, 500, "Internal Server Error");
}

static esp_err_t app_install_from_file_post_handler(httpd_req_t *req)
{
    char app_id[ESPCLAW_APP_ID_MAX + 1];
    char title[ESPCLAW_APP_TITLE_MAX + 1];
    char permissions[256];
    char triggers[256];
    char source_path[256];
    char buffer[256];
    const espclaw_runtime_status_t *status = espclaw_runtime_status();

    if (status == NULL || !status->storage_ready) {
        espclaw_admin_render_result_json(false, "workspace storage is not available", buffer, sizeof(buffer));
        return send_json_status(req, buffer, 503, "Service Unavailable");
    }
    if (!load_query_value(req, "app_id", app_id, sizeof(app_id)) ||
        !load_query_value(req, "source_path", source_path, sizeof(source_path))) {
        espclaw_admin_render_result_json(false, "missing app_id or source_path query parameter", buffer, sizeof(buffer));
        return send_json_status(req, buffer, 400, "Bad Request");
    }
    title[0] = '\0';
    permissions[0] = '\0';
    triggers[0] = '\0';
    load_query_value(req, "title", title, sizeof(title));
    load_query_value(req, "permissions", permissions, sizeof(permissions));
    load_query_value(req, "triggers", triggers, sizeof(triggers));
    if (espclaw_app_install_from_file(status->workspace_root, app_id, title, permissions, triggers, source_path) != 0) {
        espclaw_admin_render_result_json(false, "failed to install app from file", buffer, sizeof(buffer));
        return send_json_status(req, buffer, 500, "Internal Server Error");
    }
    espclaw_admin_render_result_json(true, "app installed from file", buffer, sizeof(buffer));
    return send_json(req, buffer);
}

static esp_err_t app_install_from_blob_post_handler(httpd_req_t *req)
{
    char app_id[ESPCLAW_APP_ID_MAX + 1];
    char title[ESPCLAW_APP_TITLE_MAX + 1];
    char permissions[256];
    char triggers[256];
    char blob_id[65];
    char buffer[256];
    const espclaw_runtime_status_t *status = espclaw_runtime_status();

    if (status == NULL || !status->storage_ready) {
        espclaw_admin_render_result_json(false, "workspace storage is not available", buffer, sizeof(buffer));
        return send_json_status(req, buffer, 503, "Service Unavailable");
    }
    if (!load_query_value(req, "app_id", app_id, sizeof(app_id)) ||
        !load_query_value(req, "blob_id", blob_id, sizeof(blob_id))) {
        espclaw_admin_render_result_json(false, "missing app_id or blob_id query parameter", buffer, sizeof(buffer));
        return send_json_status(req, buffer, 400, "Bad Request");
    }
    title[0] = '\0';
    permissions[0] = '\0';
    triggers[0] = '\0';
    load_query_value(req, "title", title, sizeof(title));
    load_query_value(req, "permissions", permissions, sizeof(permissions));
    load_query_value(req, "triggers", triggers, sizeof(triggers));
    if (espclaw_app_install_from_blob(status->workspace_root, app_id, title, permissions, triggers, blob_id) != 0) {
        espclaw_admin_render_result_json(false, "failed to install app from blob", buffer, sizeof(buffer));
        return send_json_status(req, buffer, 500, "Internal Server Error");
    }
    espclaw_admin_render_result_json(true, "app installed from blob", buffer, sizeof(buffer));
    return send_json(req, buffer);
}

static esp_err_t app_install_from_url_post_handler(httpd_req_t *req)
{
    char app_id[ESPCLAW_APP_ID_MAX + 1];
    char title[ESPCLAW_APP_TITLE_MAX + 1];
    char permissions[256];
    char triggers[256];
    char source_url[512];
    char buffer[256];
    const espclaw_runtime_status_t *status = espclaw_runtime_status();

    if (status == NULL || !status->storage_ready) {
        espclaw_admin_render_result_json(false, "workspace storage is not available", buffer, sizeof(buffer));
        return send_json_status(req, buffer, 503, "Service Unavailable");
    }
    if (!load_query_value(req, "app_id", app_id, sizeof(app_id)) ||
        !load_query_value(req, "source_url", source_url, sizeof(source_url))) {
        espclaw_admin_render_result_json(false, "missing app_id or source_url query parameter", buffer, sizeof(buffer));
        return send_json_status(req, buffer, 400, "Bad Request");
    }
    title[0] = '\0';
    permissions[0] = '\0';
    triggers[0] = '\0';
    load_query_value(req, "title", title, sizeof(title));
    load_query_value(req, "permissions", permissions, sizeof(permissions));
    load_query_value(req, "triggers", triggers, sizeof(triggers));
    if (espclaw_app_install_from_url(status->workspace_root, app_id, title, permissions, triggers, source_url) != 0) {
        espclaw_admin_render_result_json(false, "failed to install app from url", buffer, sizeof(buffer));
        return send_json_status(req, buffer, 500, "Internal Server Error");
    }
    espclaw_admin_render_result_json(true, "app installed from url", buffer, sizeof(buffer));
    return send_json(req, buffer);
}

static esp_err_t app_run_post_handler(httpd_req_t *req)
{
    char app_id[ESPCLAW_APP_ID_MAX + 1];
    char trigger[ESPCLAW_APP_TRIGGER_NAME_MAX + 1];
    char payload[1024];
    char result[1024];
    char response[1400];
    const espclaw_runtime_status_t *status = espclaw_runtime_status();

    if (status == NULL || !status->storage_ready) {
        espclaw_admin_render_result_json(false, "workspace storage is not available", response, sizeof(response));
        return send_json_status(req, response, 503, "Service Unavailable");
    }
    if (!load_query_value(req, "app_id", app_id, sizeof(app_id))) {
        espclaw_admin_render_result_json(false, "missing app_id query parameter", response, sizeof(response));
        return send_json_status(req, response, 400, "Bad Request");
    }
    if (!load_query_value(req, "trigger", trigger, sizeof(trigger))) {
        snprintf(trigger, sizeof(trigger), "manual");
    }
    payload[0] = '\0';
    if (req->content_len > 0 && read_request_body(req, payload, sizeof(payload)) != ESP_OK) {
        espclaw_admin_render_result_json(false, "failed to read request body", response, sizeof(response));
        return send_json_status(req, response, 400, "Bad Request");
    }

    if (espclaw_app_run(status->workspace_root, app_id, trigger, payload, result, sizeof(result)) == 0) {
        espclaw_admin_render_run_result_json(app_id, trigger, true, result, response, sizeof(response));
        return send_json(req, response);
    }

    espclaw_admin_render_run_result_json(app_id, trigger, false, result, response, sizeof(response));
    return send_json_status(req, response, 500, "Internal Server Error");
}

static esp_err_t apps_delete_handler(httpd_req_t *req)
{
    char app_id[ESPCLAW_APP_ID_MAX + 1];
    char buffer[256];
    const espclaw_runtime_status_t *status = espclaw_runtime_status();

    if (status == NULL || !status->storage_ready) {
        espclaw_admin_render_result_json(false, "workspace storage is not available", buffer, sizeof(buffer));
        return send_json_status(req, buffer, 503, "Service Unavailable");
    }
    if (!load_query_value(req, "app_id", app_id, sizeof(app_id))) {
        espclaw_admin_render_result_json(false, "missing app_id query parameter", buffer, sizeof(buffer));
        return send_json_status(req, buffer, 400, "Bad Request");
    }

    if (espclaw_app_remove(status->workspace_root, app_id) == 0) {
        espclaw_admin_render_result_json(true, "app removed", buffer, sizeof(buffer));
        return send_json(req, buffer);
    }

    espclaw_admin_render_result_json(false, "failed to remove app", buffer, sizeof(buffer));
    return send_json_status(req, buffer, 500, "Internal Server Error");
}

static esp_err_t loop_start_post_handler(httpd_req_t *req)
{
    char loop_id[ESPCLAW_CONTROL_LOOP_ID_MAX + 1];
    char app_id[ESPCLAW_APP_ID_MAX + 1];
    char trigger[ESPCLAW_APP_TRIGGER_NAME_MAX + 1];
    char payload[1024];
    char message[256];
    char response[4096];
    const espclaw_runtime_status_t *status = espclaw_runtime_status();

    if (status == NULL || !status->storage_ready) {
        espclaw_admin_render_result_json(false, "workspace storage is not available", response, sizeof(response));
        return send_json_status(req, response, 503, "Service Unavailable");
    }
    if (!load_query_value(req, "loop_id", loop_id, sizeof(loop_id)) ||
        !load_query_value(req, "app_id", app_id, sizeof(app_id))) {
        espclaw_admin_render_result_json(false, "missing loop_id or app_id query parameter", response, sizeof(response));
        return send_json_status(req, response, 400, "Bad Request");
    }
    if (!load_query_value(req, "trigger", trigger, sizeof(trigger))) {
        snprintf(trigger, sizeof(trigger), "manual");
    }
    payload[0] = '\0';
    if (req->content_len > 0 && read_request_body(req, payload, sizeof(payload)) != ESP_OK) {
        espclaw_admin_render_result_json(false, "failed to read request body", response, sizeof(response));
        return send_json_status(req, response, 400, "Bad Request");
    }

    if (espclaw_control_loop_start(
            loop_id,
            status->workspace_root,
            app_id,
            trigger,
            payload,
            load_query_u32(req, "period_ms", 20),
            load_query_u32(req, "iterations", 0),
            message,
            sizeof(message)) == 0) {
        espclaw_control_loop_render_json(response, sizeof(response));
        return send_json(req, response);
    }

    espclaw_admin_render_result_json(false, message, response, sizeof(response));
    return send_json_status(req, response, 500, "Internal Server Error");
}

static esp_err_t loop_stop_post_handler(httpd_req_t *req)
{
    char loop_id[ESPCLAW_CONTROL_LOOP_ID_MAX + 1];
    char message[256];
    char response[512];

    if (!load_query_value(req, "loop_id", loop_id, sizeof(loop_id))) {
        espclaw_admin_render_result_json(false, "missing loop_id query parameter", response, sizeof(response));
        return send_json_status(req, response, 400, "Bad Request");
    }

    if (espclaw_control_loop_stop(loop_id, message, sizeof(message)) == 0) {
        espclaw_admin_render_result_json(true, message, response, sizeof(response));
        return send_json(req, response);
    }

    espclaw_admin_render_result_json(false, message, response, sizeof(response));
    return send_json_status(req, response, 404, "Not Found");
}

static esp_err_t task_start_post_handler(httpd_req_t *req)
{
    char task_id[ESPCLAW_TASK_ID_MAX + 1];
    char app_id[ESPCLAW_APP_ID_MAX + 1];
    char schedule[ESPCLAW_TASK_SCHEDULE_MAX + 1];
    char trigger[ESPCLAW_APP_TRIGGER_NAME_MAX + 1];
    char payload[1024];
    char message[256];
    char response[6144];
    const espclaw_runtime_status_t *status = espclaw_runtime_status();

    if (status == NULL || !status->storage_ready) {
        espclaw_admin_render_result_json(false, "workspace storage is not available", response, sizeof(response));
        return send_json_status(req, response, 503, "Service Unavailable");
    }
    if (!load_query_value(req, "task_id", task_id, sizeof(task_id)) ||
        !load_query_value(req, "app_id", app_id, sizeof(app_id))) {
        espclaw_admin_render_result_json(false, "missing task_id or app_id query parameter", response, sizeof(response));
        return send_json_status(req, response, 400, "Bad Request");
    }
    if (!load_query_value(req, "schedule", schedule, sizeof(schedule))) {
        snprintf(schedule, sizeof(schedule), "periodic");
    }
    if (!load_query_value(req, "trigger", trigger, sizeof(trigger))) {
        snprintf(trigger, sizeof(trigger), "%s", strcmp(schedule, "event") == 0 ? "sensor" : "timer");
    }
    payload[0] = '\0';
    if (req->content_len > 0 && read_request_body(req, payload, sizeof(payload)) != ESP_OK) {
        espclaw_admin_render_result_json(false, "failed to read request body", response, sizeof(response));
        return send_json_status(req, response, 400, "Bad Request");
    }

    if (espclaw_task_start_with_schedule(
            task_id,
            status->workspace_root,
            app_id,
            schedule,
            trigger,
            payload,
            load_query_u32(req, "period_ms", 20),
            load_query_u32(req, "iterations", 0),
            message,
            sizeof(message)) == 0) {
        espclaw_task_render_json(response, sizeof(response));
        return send_json(req, response);
    }

    espclaw_admin_render_result_json(false, message, response, sizeof(response));
    return send_json_status(req, response, 500, "Internal Server Error");
}

static esp_err_t task_stop_post_handler(httpd_req_t *req)
{
    char task_id[ESPCLAW_TASK_ID_MAX + 1];
    char message[256];
    char response[512];

    if (!load_query_value(req, "task_id", task_id, sizeof(task_id))) {
        espclaw_admin_render_result_json(false, "missing task_id query parameter", response, sizeof(response));
        return send_json_status(req, response, 400, "Bad Request");
    }

    if (espclaw_task_stop(task_id, message, sizeof(message)) == 0) {
        espclaw_admin_render_result_json(true, message, response, sizeof(response));
        return send_json(req, response);
    }

    espclaw_admin_render_result_json(false, message, response, sizeof(response));
    return send_json_status(req, response, 404, "Not Found");
}

static esp_err_t behavior_register_post_handler(httpd_req_t *req)
{
    char behavior_id[ESPCLAW_BEHAVIOR_ID_MAX + 1];
    char app_id[ESPCLAW_APP_ID_MAX + 1];
    char schedule[ESPCLAW_TASK_SCHEDULE_MAX + 1];
    char trigger[ESPCLAW_TASK_TRIGGER_MAX + 1];
    char payload[ESPCLAW_TASK_PAYLOAD_MAX];
    char message[256];
    char response[6144];
    espclaw_behavior_spec_t spec;
    const espclaw_runtime_status_t *status = espclaw_runtime_status();

    if (status == NULL || !status->storage_ready) {
        espclaw_admin_render_result_json(false, "workspace storage is not available", response, sizeof(response));
        return send_json_status(req, response, 503, "Service Unavailable");
    }
    if (!load_query_value(req, "behavior_id", behavior_id, sizeof(behavior_id)) ||
        !load_query_value(req, "app_id", app_id, sizeof(app_id))) {
        espclaw_admin_render_result_json(false, "missing behavior_id or app_id query parameter", response, sizeof(response));
        return send_json_status(req, response, 400, "Bad Request");
    }
    if (!load_query_value(req, "schedule", schedule, sizeof(schedule))) {
        snprintf(schedule, sizeof(schedule), "periodic");
    }
    if (!load_query_value(req, "trigger", trigger, sizeof(trigger))) {
        snprintf(trigger, sizeof(trigger), "%s", strcmp(schedule, "event") == 0 ? "sensor" : "timer");
    }
    payload[0] = '\0';
    if (req->content_len > 0 && read_request_body(req, payload, sizeof(payload)) != ESP_OK) {
        espclaw_admin_render_result_json(false, "failed to read request body", response, sizeof(response));
        return send_json_status(req, response, 400, "Bad Request");
    }

    memset(&spec, 0, sizeof(spec));
    snprintf(spec.behavior_id, sizeof(spec.behavior_id), "%s", behavior_id);
    snprintf(spec.title, sizeof(spec.title), "%s", behavior_id);
    snprintf(spec.app_id, sizeof(spec.app_id), "%s", app_id);
    snprintf(spec.schedule, sizeof(spec.schedule), "%s", schedule);
    snprintf(spec.trigger, sizeof(spec.trigger), "%s", trigger);
    snprintf(spec.payload, sizeof(spec.payload), "%s", payload);
    spec.period_ms = load_query_u32(req, "period_ms", 20);
    spec.max_iterations = load_query_u32(req, "iterations", 0);
    spec.autostart = load_query_u32(req, "autostart", 0) != 0;

    if (espclaw_behavior_register(status->workspace_root, &spec, message, sizeof(message)) == 0) {
        espclaw_render_behaviors_json(status->workspace_root, response, sizeof(response));
        return send_json(req, response);
    }

    espclaw_admin_render_result_json(false, message, response, sizeof(response));
    return send_json_status(req, response, 500, "Internal Server Error");
}

static esp_err_t behavior_start_post_handler(httpd_req_t *req)
{
    char behavior_id[ESPCLAW_BEHAVIOR_ID_MAX + 1];
    char message[256];
    char response[6144];
    const espclaw_runtime_status_t *status = espclaw_runtime_status();

    if (status == NULL || !status->storage_ready) {
        espclaw_admin_render_result_json(false, "workspace storage is not available", response, sizeof(response));
        return send_json_status(req, response, 503, "Service Unavailable");
    }
    if (!load_query_value(req, "behavior_id", behavior_id, sizeof(behavior_id))) {
        espclaw_admin_render_result_json(false, "missing behavior_id query parameter", response, sizeof(response));
        return send_json_status(req, response, 400, "Bad Request");
    }
    if (espclaw_behavior_start(status->workspace_root, behavior_id, message, sizeof(message)) == 0) {
        espclaw_render_behaviors_json(status->workspace_root, response, sizeof(response));
        return send_json(req, response);
    }

    espclaw_admin_render_result_json(false, message, response, sizeof(response));
    return send_json_status(req, response, 404, "Not Found");
}

static esp_err_t behavior_stop_post_handler(httpd_req_t *req)
{
    char behavior_id[ESPCLAW_BEHAVIOR_ID_MAX + 1];
    char message[256];
    char response[6144];
    const espclaw_runtime_status_t *status = espclaw_runtime_status();

    if (status == NULL || !status->storage_ready) {
        espclaw_admin_render_result_json(false, "workspace storage is not available", response, sizeof(response));
        return send_json_status(req, response, 503, "Service Unavailable");
    }
    if (!load_query_value(req, "behavior_id", behavior_id, sizeof(behavior_id))) {
        espclaw_admin_render_result_json(false, "missing behavior_id query parameter", response, sizeof(response));
        return send_json_status(req, response, 400, "Bad Request");
    }
    if (espclaw_behavior_stop(behavior_id, message, sizeof(message)) == 0) {
        espclaw_render_behaviors_json(status->workspace_root, response, sizeof(response));
        return send_json(req, response);
    }

    espclaw_admin_render_result_json(false, message, response, sizeof(response));
    return send_json_status(req, response, 404, "Not Found");
}

static esp_err_t behavior_delete_handler(httpd_req_t *req)
{
    char behavior_id[ESPCLAW_BEHAVIOR_ID_MAX + 1];
    char message[256];
    char response[6144];
    const espclaw_runtime_status_t *status = espclaw_runtime_status();

    if (status == NULL || !status->storage_ready) {
        espclaw_admin_render_result_json(false, "workspace storage is not available", response, sizeof(response));
        return send_json_status(req, response, 503, "Service Unavailable");
    }
    if (!load_query_value(req, "behavior_id", behavior_id, sizeof(behavior_id))) {
        espclaw_admin_render_result_json(false, "missing behavior_id query parameter", response, sizeof(response));
        return send_json_status(req, response, 400, "Bad Request");
    }
    if (espclaw_behavior_remove(status->workspace_root, behavior_id, message, sizeof(message)) == 0) {
        espclaw_render_behaviors_json(status->workspace_root, response, sizeof(response));
        return send_json(req, response);
    }

    espclaw_admin_render_result_json(false, message, response, sizeof(response));
    return send_json_status(req, response, 404, "Not Found");
}

static esp_err_t event_emit_post_handler(httpd_req_t *req)
{
    char event_name[ESPCLAW_TASK_TRIGGER_MAX + 1];
    char payload[ESPCLAW_TASK_PAYLOAD_MAX];
    char message[256];
    char response[512];

    if (!load_query_value(req, "name", event_name, sizeof(event_name))) {
        espclaw_admin_render_result_json(false, "missing name query parameter", response, sizeof(response));
        return send_json_status(req, response, 400, "Bad Request");
    }
    payload[0] = '\0';
    if (req->content_len > 0 && read_request_body(req, payload, sizeof(payload)) != ESP_OK) {
        espclaw_admin_render_result_json(false, "failed to read request body", response, sizeof(response));
        return send_json_status(req, response, 400, "Bad Request");
    }
    if (espclaw_task_emit_event(event_name, payload, message, sizeof(message)) == 0) {
        espclaw_admin_render_result_json(true, message, response, sizeof(response));
        return send_json(req, response);
    }

    espclaw_admin_render_result_json(false, message, response, sizeof(response));
    return send_json_status(req, response, 404, "Not Found");
}

esp_err_t espclaw_admin_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    int admin_core = espclaw_task_policy_core_for(ESPCLAW_TASK_KIND_ADMIN);
    httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = root_get_handler, .user_ctx = NULL},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_get_handler, .user_ctx = NULL},
        {.uri = "/api/logs", .method = HTTP_GET, .handler = logs_get_handler, .user_ctx = NULL},
        {.uri = "/api/board", .method = HTTP_GET, .handler = board_get_handler, .user_ctx = NULL},
        {.uri = "/api/board/presets", .method = HTTP_GET, .handler = board_presets_get_handler, .user_ctx = NULL},
        {.uri = "/api/board/config", .method = HTTP_GET, .handler = board_config_get_handler, .user_ctx = NULL},
        {.uri = "/api/board/config", .method = HTTP_PUT, .handler = board_config_put_handler, .user_ctx = NULL},
        {.uri = "/api/board/apply", .method = HTTP_POST, .handler = board_apply_post_handler, .user_ctx = NULL},
        {.uri = "/api/network/status", .method = HTTP_GET, .handler = network_status_get_handler, .user_ctx = NULL},
        {.uri = "/api/network/provisioning", .method = HTTP_GET, .handler = network_provisioning_get_handler, .user_ctx = NULL},
        {.uri = "/api/network/scan", .method = HTTP_GET, .handler = network_scan_get_handler, .user_ctx = NULL},
        {.uri = "/api/network/join", .method = HTTP_POST, .handler = network_join_post_handler, .user_ctx = NULL},
        {.uri = "/api/auth/status", .method = HTTP_GET, .handler = auth_status_get_handler, .user_ctx = NULL},
        {.uri = "/api/telegram/config", .method = HTTP_GET, .handler = telegram_config_get_handler, .user_ctx = NULL},
        {.uri = "/api/telegram/config", .method = HTTP_POST, .handler = telegram_config_post_handler, .user_ctx = NULL},
        {.uri = "/api/auth/codex", .method = HTTP_PUT, .handler = auth_put_handler, .user_ctx = NULL},
        {.uri = "/api/auth/codex", .method = HTTP_DELETE, .handler = auth_delete_handler, .user_ctx = NULL},
        {.uri = "/api/auth/import-json", .method = HTTP_POST, .handler = auth_import_json_post_handler, .user_ctx = NULL},
        {.uri = "/api/auth/import-codex-cli", .method = HTTP_POST, .handler = auth_import_codex_cli_post_handler, .user_ctx = NULL},
        {.uri = "/api/ota/status", .method = HTTP_GET, .handler = ota_status_get_handler, .user_ctx = NULL},
        {.uri = "/api/ota/upload", .method = HTTP_POST, .handler = ota_upload_post_handler, .user_ctx = NULL},
        {.uri = "/api/workspace/files", .method = HTTP_GET, .handler = workspace_get_handler, .user_ctx = NULL},
        {.uri = "/api/blobs/status", .method = HTTP_GET, .handler = blob_status_get_handler, .user_ctx = NULL},
        {.uri = "/api/blobs/begin", .method = HTTP_POST, .handler = blob_begin_post_handler, .user_ctx = NULL},
        {.uri = "/api/blobs/append", .method = HTTP_POST, .handler = blob_append_post_handler, .user_ctx = NULL},
        {.uri = "/api/blobs/commit", .method = HTTP_POST, .handler = blob_commit_post_handler, .user_ctx = NULL},
        {.uri = "/api/context/chunks", .method = HTTP_GET, .handler = context_chunks_get_handler, .user_ctx = NULL},
        {.uri = "/api/context/load", .method = HTTP_GET, .handler = context_load_get_handler, .user_ctx = NULL},
        {.uri = "/api/context/search", .method = HTTP_GET, .handler = context_search_get_handler, .user_ctx = NULL},
        {.uri = "/api/context/select", .method = HTTP_GET, .handler = context_select_get_handler, .user_ctx = NULL},
        {.uri = "/api/context/summarize", .method = HTTP_GET, .handler = context_summarize_get_handler, .user_ctx = NULL},
        {.uri = "/api/monitor", .method = HTTP_GET, .handler = monitor_get_handler, .user_ctx = NULL},
        {.uri = "/api/camera", .method = HTTP_GET, .handler = camera_get_handler, .user_ctx = NULL},
        {.uri = "/api/camera/capture", .method = HTTP_POST, .handler = camera_capture_post_handler, .user_ctx = NULL},
        {.uri = "/media/*", .method = HTTP_GET, .handler = media_get_handler, .user_ctx = NULL},
        {.uri = "/api/tools", .method = HTTP_GET, .handler = tools_get_handler, .user_ctx = NULL},
        {.uri = "/api/lua-api", .method = HTTP_GET, .handler = lua_api_get_handler, .user_ctx = NULL},
        {.uri = "/api/lua-api.md", .method = HTTP_GET, .handler = lua_api_markdown_get_handler, .user_ctx = NULL},
        {.uri = "/api/app-patterns", .method = HTTP_GET, .handler = app_patterns_get_handler, .user_ctx = NULL},
        {.uri = "/api/app-patterns.md", .method = HTTP_GET, .handler = app_patterns_markdown_get_handler, .user_ctx = NULL},
        {.uri = "/api/hardware", .method = HTTP_GET, .handler = hardware_get_handler, .user_ctx = NULL},
        {.uri = "/api/apps", .method = HTTP_GET, .handler = apps_get_handler, .user_ctx = NULL},
        {.uri = "/api/apps", .method = HTTP_DELETE, .handler = apps_delete_handler, .user_ctx = NULL},
        {.uri = "/api/components", .method = HTTP_GET, .handler = components_get_handler, .user_ctx = NULL},
        {.uri = "/api/components", .method = HTTP_DELETE, .handler = component_delete_handler, .user_ctx = NULL},
        {.uri = "/api/behaviors", .method = HTTP_GET, .handler = behaviors_get_handler, .user_ctx = NULL},
        {.uri = "/api/behaviors", .method = HTTP_DELETE, .handler = behavior_delete_handler, .user_ctx = NULL},
        {.uri = "/api/loops", .method = HTTP_GET, .handler = loops_get_handler, .user_ctx = NULL},
        {.uri = "/api/tasks", .method = HTTP_GET, .handler = tasks_get_handler, .user_ctx = NULL},
        {.uri = "/api/events/emit", .method = HTTP_POST, .handler = event_emit_post_handler, .user_ctx = NULL},
        {.uri = "/api/apps/detail", .method = HTTP_GET, .handler = app_detail_get_handler, .user_ctx = NULL},
        {.uri = "/api/components/detail", .method = HTTP_GET, .handler = component_detail_get_handler, .user_ctx = NULL},
        {.uri = "/api/apps/scaffold", .method = HTTP_POST, .handler = app_scaffold_post_handler, .user_ctx = NULL},
        {.uri = "/api/components/install", .method = HTTP_POST, .handler = component_install_post_handler, .user_ctx = NULL},
        {.uri = "/api/components/install/from-file", .method = HTTP_POST, .handler = component_install_from_file_post_handler, .user_ctx = NULL},
        {.uri = "/api/components/install/from-blob", .method = HTTP_POST, .handler = component_install_from_blob_post_handler, .user_ctx = NULL},
        {.uri = "/api/components/install/from-url", .method = HTTP_POST, .handler = component_install_from_url_post_handler, .user_ctx = NULL},
        {.uri = "/api/components/install/from-manifest", .method = HTTP_POST, .handler = component_install_from_manifest_post_handler, .user_ctx = NULL},
        {.uri = "/api/apps/source", .method = HTTP_PUT, .handler = app_source_put_handler, .user_ctx = NULL},
        {.uri = "/api/apps/install/from-file", .method = HTTP_POST, .handler = app_install_from_file_post_handler, .user_ctx = NULL},
        {.uri = "/api/apps/install/from-blob", .method = HTTP_POST, .handler = app_install_from_blob_post_handler, .user_ctx = NULL},
        {.uri = "/api/apps/install/from-url", .method = HTTP_POST, .handler = app_install_from_url_post_handler, .user_ctx = NULL},
        {.uri = "/api/apps/run", .method = HTTP_POST, .handler = app_run_post_handler, .user_ctx = NULL},
        {.uri = "/api/chat/run", .method = HTTP_POST, .handler = chat_run_post_handler, .user_ctx = NULL},
        {.uri = "/api/bench/operator-turn", .method = HTTP_POST, .handler = bench_operator_turn_post_handler, .user_ctx = NULL},
        {.uri = "/api/chat/session", .method = HTTP_GET, .handler = chat_session_get_handler, .user_ctx = NULL},
        {.uri = "/api/loops/start", .method = HTTP_POST, .handler = loop_start_post_handler, .user_ctx = NULL},
        {.uri = "/api/loops/stop", .method = HTTP_POST, .handler = loop_stop_post_handler, .user_ctx = NULL},
        {.uri = "/api/behaviors/register", .method = HTTP_POST, .handler = behavior_register_post_handler, .user_ctx = NULL},
        {.uri = "/api/behaviors/start", .method = HTTP_POST, .handler = behavior_start_post_handler, .user_ctx = NULL},
        {.uri = "/api/behaviors/stop", .method = HTTP_POST, .handler = behavior_stop_post_handler, .user_ctx = NULL},
        {.uri = "/api/tasks/start", .method = HTTP_POST, .handler = task_start_post_handler, .user_ctx = NULL},
        {.uri = "/api/tasks/stop", .method = HTTP_POST, .handler = task_stop_post_handler, .user_ctx = NULL},
    };
    const size_t route_count = sizeof(routes) / sizeof(routes[0]);
    size_t index;

    if (s_admin_server != NULL) {
        return ESP_OK;
    }

    config.server_port = CONFIG_ESPCLAW_ADMIN_PORT;
    config.stack_size = ESPCLAW_ADMIN_HTTPD_STACK_SIZE;
    config.max_uri_handlers = (uint16_t)(route_count + ESPCLAW_ADMIN_HTTPD_ROUTE_HEADROOM);
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;
    config.core_id = admin_core >= 0 ? admin_core : tskNO_AFFINITY;
    config.uri_match_fn = httpd_uri_match_wildcard;
    {
        esp_err_t err = httpd_start(&s_admin_server, &config);

        if (err != ESP_OK) {
            ESP_LOGE(
                TAG,
                "httpd_start failed err=0x%x port=%d stack=%d max_uri=%d max_open_sockets=%d",
                (unsigned int)err,
                config.server_port,
                config.stack_size,
                config.max_uri_handlers,
                config.max_open_sockets
            );
            return err;
        }
    }

    for (index = 0; index < sizeof(routes) / sizeof(routes[0]); ++index) {
        esp_err_t err = httpd_register_uri_handler(s_admin_server, &routes[index]);

        if (err != ESP_OK) {
            ESP_LOGE(
                TAG,
                "Failed to register route %s err=0x%x (%u/%u handlers, cap=%d)",
                routes[index].uri,
                (unsigned int)err,
                (unsigned int)(index + 1),
                (unsigned int)route_count,
                config.max_uri_handlers
            );
            httpd_stop(s_admin_server);
            s_admin_server = NULL;
            return err;
        }
    }

    ESP_LOGI(TAG, "Admin server listening on port %d", config.server_port);
    return ESP_OK;
}
