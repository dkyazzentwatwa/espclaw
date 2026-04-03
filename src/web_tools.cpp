#include "espclaw/web_tools.h"

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef ESP_PLATFORM
#ifdef ARDUINO
#include <HTTPClient.h>
#else
#include "esp_http_client.h"
#if defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE)
#include "esp_crt_bundle.h"
#endif
#endif /* ARDUINO */
#endif /* ESP_PLATFORM */

#include "espclaw/workspace.h"

static espclaw_web_http_adapter_t s_http_adapter;
static void *s_http_adapter_user_data;

#ifndef ESPCLAW_WEB_PROXY_BASE_URL
#define ESPCLAW_WEB_PROXY_BASE_URL "https://llmproxy.markster.io/v1"
#endif

typedef struct {
    long position;
    char title[160];
    char url[256];
    char snippet[256];
    char source[64];
    bool used;
} espclaw_search_result_t;

static void copy_text(char *buffer, size_t buffer_size, const char *value)
{
    if (buffer == NULL || buffer_size == 0) {
        return;
    }
    snprintf(buffer, buffer_size, "%s", value != NULL ? value : "");
}

static bool json_string_from_cursor(const char *cursor, const char *key, char *buffer, size_t buffer_size)
{
    char pattern[64];
    size_t used = 0;

    if (cursor == NULL || key == NULL || buffer == NULL || buffer_size == 0) {
        return false;
    }

    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    cursor = strstr(cursor, pattern);
    if (cursor == NULL) {
        buffer[0] = '\0';
        return false;
    }
    cursor = strchr(cursor, ':');
    if (cursor == NULL) {
        buffer[0] = '\0';
        return false;
    }
    cursor = strchr(cursor, '"');
    if (cursor == NULL) {
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
    return used > 0;
}

static bool json_long_from_cursor(const char *cursor, const char *key, long *value)
{
    char pattern[64];
    char *end_ptr = NULL;

    if (cursor == NULL || key == NULL || value == NULL) {
        return false;
    }

    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    cursor = strstr(cursor, pattern);
    if (cursor == NULL) {
        return false;
    }
    cursor = strchr(cursor, ':');
    if (cursor == NULL) {
        return false;
    }
    cursor++;
    while (*cursor != '\0' && isspace((unsigned char)*cursor)) {
        cursor++;
    }
    *value = strtol(cursor, &end_ptr, 10);
    return end_ptr != cursor;
}

static size_t append_json_escaped(char *buffer, size_t buffer_size, size_t used, const char *value)
{
    const unsigned char *cursor = (const unsigned char *)(value != NULL ? value : "");

    if (buffer == NULL || buffer_size == 0 || used >= buffer_size) {
        return used;
    }
    buffer[used++] = '"';
    while (*cursor != '\0' && used + 2 < buffer_size) {
        switch (*cursor) {
        case '\\':
        case '"':
            buffer[used++] = '\\';
            buffer[used++] = (char)*cursor;
            break;
        case '\n':
            buffer[used++] = '\\';
            buffer[used++] = 'n';
            break;
        case '\r':
            buffer[used++] = '\\';
            buffer[used++] = 'r';
            break;
        case '\t':
            buffer[used++] = '\\';
            buffer[used++] = 't';
            break;
        default:
            buffer[used++] = (char)*cursor;
            break;
        }
        cursor++;
    }
    if (used < buffer_size) {
        buffer[used++] = '"';
    }
    if (used < buffer_size) {
        buffer[used] = '\0';
    } else {
        buffer[buffer_size - 1] = '\0';
    }
    return used;
}

static size_t url_encode_component(const char *value, char *buffer, size_t buffer_size)
{
    static const char HEX[] = "0123456789ABCDEF";
    const unsigned char *cursor = (const unsigned char *)(value != NULL ? value : "");
    size_t used = 0;

    if (buffer == NULL || buffer_size == 0) {
        return 0;
    }
    while (*cursor != '\0' && used + 4 < buffer_size) {
        if (isalnum(*cursor) || *cursor == '-' || *cursor == '_' || *cursor == '.' || *cursor == '~') {
            buffer[used++] = (char)*cursor;
        } else {
            buffer[used++] = '%';
            buffer[used++] = HEX[(*cursor >> 4) & 0x0F];
            buffer[used++] = HEX[*cursor & 0x0F];
        }
        cursor++;
    }
    buffer[used] = '\0';
    return used;
}

static uint32_t fnv1a32(const char *text)
{
    uint32_t hash = 2166136261u;
    const unsigned char *cursor = (const unsigned char *)(text != NULL ? text : "");

    while (*cursor != '\0') {
        hash ^= *cursor++;
        hash *= 16777619u;
    }
    return hash;
}

static int ensure_directory(const char *path)
{
    return (mkdir(path, 0755) == 0 || errno == EEXIST) ? 0 : -1;
}

static int ensure_parent_directories(const char *path)
{
    char buffer[512];
    char *cursor;

    if (path == NULL || snprintf(buffer, sizeof(buffer), "%s", path) >= (int)sizeof(buffer)) {
        return -1;
    }
    for (cursor = buffer + 1; *cursor != '\0'; ++cursor) {
        if (*cursor != '/') {
            continue;
        }
        *cursor = '\0';
        if (ensure_directory(buffer) != 0) {
            return -1;
        }
        *cursor = '/';
    }
    return 0;
}

#ifndef ESP_PLATFORM
static int shell_single_quote(const char *value, char *buffer, size_t buffer_size)
{
    const char *cursor = value != NULL ? value : "";
    size_t used = 0;

    if (buffer == NULL || buffer_size < 3) {
        return -1;
    }
    buffer[used++] = '\'';
    while (*cursor != '\0') {
        if (*cursor == '\'') {
            if (used + 4 >= buffer_size) {
                return -1;
            }
            buffer[used++] = '\'';
            buffer[used++] = '\\';
            buffer[used++] = '\'';
            buffer[used++] = '\'';
        } else {
            if (used + 1 >= buffer_size) {
                return -1;
            }
            buffer[used++] = *cursor;
        }
        cursor++;
    }
    if (used + 2 > buffer_size) {
        return -1;
    }
    buffer[used++] = '\'';
    buffer[used] = '\0';
    return 0;
}
#endif

static int default_http_get(const char *url, char *response, size_t response_size)
{
#ifdef ESP_PLATFORM
    if (url == NULL || response == NULL || response_size == 0) {
        return -1;
    }
#ifdef ARDUINO
    HTTPClient http;
    http.begin(url);
    http.setTimeout(30000);
    int code = http.GET();
    if (code < 200 || code >= 300) {
        http.end();
        return -1;
    }
    String body = http.getString();
    http.end();
    snprintf(response, response_size, "%s", body.c_str());
    return 0;
#else
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .timeout_ms = 30000,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t client;
    esp_err_t err;
    int total_read = 0;
    int status_code = 0;

#if defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE)
    config.crt_bundle_attach = esp_crt_bundle_attach;
#endif
    client = esp_http_client_init(&config);
    if (client == NULL) {
        return -1;
    }

    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return -1;
    }
    if (esp_http_client_fetch_headers(client) < 0) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return -1;
    }
    status_code = esp_http_client_get_status_code(client);
    if (status_code < 200 || status_code >= 300) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return -1;
    }
    while (total_read < (int)response_size - 1) {
        int read_now = esp_http_client_read(client, response + total_read, (int)response_size - 1 - total_read);

        if (read_now < 0) {
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return -1;
        }
        if (read_now == 0) {
            break;
        }
        total_read += read_now;
    }
    response[total_read] = '\0';
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return 0;
#endif /* ARDUINO */
#else
    char quoted_url[2048];
    char command[2304];
    FILE *pipe;
    size_t used = 0;

    if (url == NULL || response == NULL || response_size == 0) {
        return -1;
    }
    if (shell_single_quote(url, quoted_url, sizeof(quoted_url)) != 0) {
        return -1;
    }
    snprintf(command, sizeof(command), "curl -fsSL --max-time 30 --url %s", quoted_url);
    pipe = popen(command, "r");
    if (pipe == NULL) {
        return -1;
    }
    while (!feof(pipe) && used + 1 < response_size) {
        size_t read_now = fread(response + used, 1, response_size - 1 - used, pipe);

        if (read_now == 0) {
            break;
        }
        used += read_now;
    }
    response[used] = '\0';
    if (pclose(pipe) != 0) {
        return -1;
    }
    return 0;
#endif
}

static int http_get(const char *url, char *response, size_t response_size)
{
    if (s_http_adapter != NULL) {
        return s_http_adapter(url, response, response_size, s_http_adapter_user_data);
    }
    return default_http_get(url, response, response_size);
}

static size_t reduce_search_results(const char *body, char *buffer, size_t buffer_size)
{
    espclaw_search_result_t results[5] = {0};
    char query[128];
    char provider[64];
    const char *results_cursor;
    size_t result_count = 0;
    size_t used = 0;

    query[0] = '\0';
    provider[0] = '\0';
    json_string_from_cursor(body, "query", query, sizeof(query));
    json_string_from_cursor(body, "provider", provider, sizeof(provider));

    results_cursor = strstr(body != NULL ? body : "", "\"results\"");
    while (results_cursor != NULL && result_count < sizeof(results) / sizeof(results[0])) {
        const char *object_start = strchr(results_cursor, '{');

        if (object_start == NULL) {
            break;
        }
        results[result_count].used = true;
        json_long_from_cursor(object_start, "position", &results[result_count].position);
        json_string_from_cursor(object_start, "title", results[result_count].title, sizeof(results[result_count].title));
        json_string_from_cursor(object_start, "url", results[result_count].url, sizeof(results[result_count].url));
        json_string_from_cursor(object_start, "snippet", results[result_count].snippet, sizeof(results[result_count].snippet));
        json_string_from_cursor(object_start, "source", results[result_count].source, sizeof(results[result_count].source));
        result_count++;
        results_cursor = strstr(object_start + 1, "\"position\"");
    }

    used += (size_t)snprintf(buffer + used, buffer_size - used, "{\"ok\":true,\"query\":");
    used = append_json_escaped(buffer, buffer_size, used, query);
    used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"provider\":");
    used = append_json_escaped(buffer, buffer_size, used, provider);
    used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"results\":[");
    for (size_t index = 0; index < result_count && used + 64 < buffer_size; ++index) {
        used += (size_t)snprintf(buffer + used, buffer_size - used, "%s{\"position\":%ld,\"title\":", index == 0 ? "" : ",", results[index].position);
        used = append_json_escaped(buffer, buffer_size, used, results[index].title);
        used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"url\":");
        used = append_json_escaped(buffer, buffer_size, used, results[index].url);
        used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"snippet\":");
        used = append_json_escaped(buffer, buffer_size, used, results[index].snippet);
        used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"source\":");
        used = append_json_escaped(buffer, buffer_size, used, results[index].source);
        used += (size_t)snprintf(buffer + used, buffer_size - used, "}");
    }
    used += (size_t)snprintf(buffer + used, buffer_size - used, "]}");
    return used;
}

void espclaw_web_set_http_adapter(espclaw_web_http_adapter_t adapter, void *user_data)
{
    s_http_adapter = adapter;
    s_http_adapter_user_data = user_data;
}

int espclaw_web_search(const char *query, char *buffer, size_t buffer_size)
{
    char encoded_query[512];
    char url[768];
    char *body;

    if (query == NULL || query[0] == '\0' || buffer == NULL || buffer_size == 0) {
        return -1;
    }

    body = calloc(1, 8192);
    if (body == NULL) {
        return -1;
    }
    url_encode_component(query, encoded_query, sizeof(encoded_query));
    snprintf(url, sizeof(url), ESPCLAW_WEB_PROXY_BASE_URL "/search?q=%s", encoded_query);
    if (http_get(url, body, 8192) != 0) {
        free(body);
        return -1;
    }
    reduce_search_results(body, buffer, buffer_size);
    free(body);
    return 0;
}

int espclaw_web_fetch(const char *workspace_root, const char *url, char *buffer, size_t buffer_size)
{
    char encoded_url[1024];
    char proxy_url[1280];
    char *body;
    char title[160];
    char excerpt[256];
    char markdown_excerpt[512];
    char markdown_path[64];
    char *markdown_full = NULL;
    size_t used = 0;
    bool stored = false;

    if (url == NULL || url[0] == '\0' || buffer == NULL || buffer_size == 0) {
        return -1;
    }

    body = calloc(1, 32768);
    markdown_full = calloc(1, 24576);
    if (body == NULL || markdown_full == NULL) {
        free(markdown_full);
        free(body);
        return -1;
    }

    url_encode_component(url, encoded_url, sizeof(encoded_url));
    snprintf(proxy_url, sizeof(proxy_url), ESPCLAW_WEB_PROXY_BASE_URL "/scrape?url=%s", encoded_url);
    if (http_get(proxy_url, body, 32768) != 0) {
        free(markdown_full);
        free(body);
        return -1;
    }

    title[0] = '\0';
    excerpt[0] = '\0';
    markdown_excerpt[0] = '\0';
    markdown_path[0] = '\0';
    json_string_from_cursor(body, "title", title, sizeof(title));
    json_string_from_cursor(body, "excerpt", excerpt, sizeof(excerpt));
    if (json_string_from_cursor(body, "markdown", markdown_full, 24576)) {
        copy_text(markdown_excerpt, sizeof(markdown_excerpt), markdown_full);
        if (workspace_root != NULL && workspace_root[0] != '\0') {
            snprintf(markdown_path, sizeof(markdown_path), "memory/web_fetch_%08x.md", (unsigned int)fnv1a32(url));
            if (espclaw_workspace_write_file(workspace_root, markdown_path, markdown_full) == 0) {
                stored = true;
            }
        }
    }

    used += (size_t)snprintf(buffer + used, buffer_size - used, "{\"ok\":true,\"url\":");
    used = append_json_escaped(buffer, buffer_size, used, url);
    used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"title\":");
    used = append_json_escaped(buffer, buffer_size, used, title);
    used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"excerpt\":");
    used = append_json_escaped(buffer, buffer_size, used, excerpt);
    if (markdown_excerpt[0] != '\0') {
        markdown_excerpt[sizeof(markdown_excerpt) - 1] = '\0';
        used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"markdown_excerpt\":");
        used = append_json_escaped(buffer, buffer_size, used, markdown_excerpt);
    }
    if (stored) {
        used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"stored_path\":");
        used = append_json_escaped(buffer, buffer_size, used, markdown_path);
    }
    used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"stored\":%s}", stored ? "true" : "false");

    free(markdown_full);
    free(body);
    return 0;
}

int espclaw_web_download_to_file(const char *url, const char *destination_path)
{
    FILE *handle = NULL;

    if (url == NULL || url[0] == '\0' || destination_path == NULL || destination_path[0] == '\0') {
        return -1;
    }
    if (ensure_parent_directories(destination_path) != 0) {
        return -1;
    }

#ifdef ESP_PLATFORM
    {
        esp_http_client_config_t config = {
            .url = url,
            .method = HTTP_METHOD_GET,
            .transport_type = HTTP_TRANSPORT_OVER_SSL,
            .timeout_ms = 30000,
            .buffer_size = 2048,
            .buffer_size_tx = 1024,
        };
        esp_http_client_handle_t client;
        esp_err_t err;
        char chunk[2048];
        int status_code = 0;

#if defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE)
        config.crt_bundle_attach = esp_crt_bundle_attach;
#endif
        handle = fopen(destination_path, "wb");
        if (handle == NULL) {
            return -1;
        }
        client = esp_http_client_init(&config);
        if (client == NULL) {
            fclose(handle);
            unlink(destination_path);
            return -1;
        }
        err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            esp_http_client_cleanup(client);
            fclose(handle);
            unlink(destination_path);
            return -1;
        }
        if (esp_http_client_fetch_headers(client) < 0) {
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            fclose(handle);
            unlink(destination_path);
            return -1;
        }
        status_code = esp_http_client_get_status_code(client);
        if (status_code < 200 || status_code >= 300) {
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            fclose(handle);
            unlink(destination_path);
            return -1;
        }
        while (true) {
            int read_now = esp_http_client_read(client, chunk, sizeof(chunk));

            if (read_now < 0) {
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                fclose(handle);
                unlink(destination_path);
                return -1;
            }
            if (read_now == 0) {
                break;
            }
            if (fwrite(chunk, 1, (size_t)read_now, handle) != (size_t)read_now) {
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                fclose(handle);
                unlink(destination_path);
                return -1;
            }
        }
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        fclose(handle);
        return 0;
    }
#else
    {
        if (s_http_adapter != NULL) {
            char *body = calloc(1, 65536);
            size_t body_length;

            if (body == NULL) {
                return -1;
            }
            if (s_http_adapter(url, body, 65536, s_http_adapter_user_data) != 0) {
                free(body);
                return -1;
            }
            handle = fopen(destination_path, "wb");
            if (handle == NULL) {
                free(body);
                return -1;
            }
            body_length = strlen(body);
            if (fwrite(body, 1, body_length, handle) != body_length) {
                fclose(handle);
                unlink(destination_path);
                free(body);
                return -1;
            }
            fclose(handle);
            free(body);
            return 0;
        }

        char quoted_url[2048];
        char quoted_destination[768];
        char command[3072];

        if (shell_single_quote(url, quoted_url, sizeof(quoted_url)) != 0 ||
            shell_single_quote(destination_path, quoted_destination, sizeof(quoted_destination)) != 0) {
            return -1;
        }
        snprintf(
            command,
            sizeof(command),
            "curl -fsSL --max-time 30 --output %s --url %s",
            quoted_destination,
            quoted_url
        );
        if (system(command) != 0) {
            return -1;
        }
        return 0;
    }
#endif
}
