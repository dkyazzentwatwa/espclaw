#include "espclaw/agent_loop.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifndef MBEDTLS_X509_BADCERT_EXPIRED
#define MBEDTLS_X509_BADCERT_EXPIRED 0x01
#define MBEDTLS_X509_BADCERT_REVOKED 0x02
#define MBEDTLS_X509_BADCERT_CN_MISMATCH 0x04
#define MBEDTLS_X509_BADCERT_NOT_TRUSTED 0x08
#define MBEDTLS_X509_BADCERT_MISSING 0x40
#define MBEDTLS_X509_BADCERT_OTHER 0x0100
#define MBEDTLS_X509_BADCERT_FUTURE 0x0200
#define MBEDTLS_X509_BADCERT_KEY_USAGE 0x0800
#define MBEDTLS_X509_BADCERT_EXT_KEY_USAGE 0x1000
#define MBEDTLS_X509_BADCERT_BAD_MD 0x4000
#define MBEDTLS_X509_BADCERT_BAD_PK 0x8000
#define MBEDTLS_X509_BADCERT_BAD_KEY 0x010000
#endif

#ifdef ESPCLAW_HOST_LUA
#include <unistd.h>
#else
#if defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE)
#include "esp_crt_bundle.h"
#endif
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/sha256.h"
#include "mbedtls/ssl.h"
#endif

#include "espclaw/admin_api.h"
#include "espclaw/app_patterns.h"
#include "espclaw/app_runtime.h"
#include "espclaw/auth_store.h"
#include "espclaw/behavior_runtime.h"
#include "espclaw/board_config.h"
#include "espclaw/component_runtime.h"
#include "espclaw/context_runtime.h"
#include "espclaw/event_watch.h"
#include "espclaw/hardware.h"
#include "espclaw/lua_api_registry.h"
#include "espclaw/log_buffer.h"
#include "espclaw/ota_manager.h"
#include "espclaw/provider.h"
#include "espclaw/runtime.h"
#include "espclaw/session_store.h"
#include "espclaw/storage.h"
#include "espclaw/task_runtime.h"
#include "espclaw/tool_catalog.h"
#include "espclaw/web_tools.h"
#include "espclaw/workspace.h"

#define ESPCLAW_AGENT_MEDIA_MAX 2

typedef espclaw_agent_history_message_t espclaw_history_message_t;

typedef struct {
    char id[ESPCLAW_AGENT_RESPONSE_ID_MAX + 1];
    char text[ESPCLAW_AGENT_TEXT_MAX + 1];
    espclaw_agent_tool_call_t tool_calls[ESPCLAW_AGENT_TOOL_CALL_MAX];
    size_t tool_call_count;
} espclaw_provider_response_t;

typedef struct {
    bool active;
    char relative_path[ESPCLAW_HW_CAMERA_PATH_MAX];
    char mime_type[24];
} espclaw_agent_media_ref_t;

static int load_history(
    const char *workspace_root,
    const char *session_id,
    espclaw_history_message_t *messages,
    size_t max_messages,
    size_t *count_out
);
static void free_embedded_auth_profile(espclaw_auth_profile_t *profile);
static int build_initial_request_body(
    const espclaw_auth_profile_t *profile,
    const char *instructions,
    const espclaw_history_message_t *history,
    size_t history_count,
    char *buffer,
    size_t buffer_size
);
static void copy_text(char *buffer, size_t buffer_size, const char *value);

static size_t seed_history_messages(
    espclaw_history_message_t *destination,
    size_t max_messages,
    const espclaw_history_message_t *seed_history,
    size_t seed_history_count,
    const char *user_message
)
{
    size_t copied = 0;
    size_t start = 0;
    size_t index;

    if (destination == NULL || max_messages == 0U || user_message == NULL) {
        return 0U;
    }

    if (seed_history != NULL && seed_history_count > max_messages - 1U) {
        start = seed_history_count - (max_messages - 1U);
    }
    for (index = start; seed_history != NULL && index < seed_history_count && copied + 1U < max_messages; ++index) {
        copy_text(destination[copied].role, sizeof(destination[copied].role), seed_history[index].role);
        copy_text(destination[copied].content, sizeof(destination[copied].content), seed_history[index].content);
        copied++;
    }

    copy_text(destination[copied].role, sizeof(destination[copied].role), "user");
    copy_text(destination[copied].content, sizeof(destination[copied].content), user_message);
    return copied + 1U;
}

static void *agent_calloc(size_t count, size_t size)
{
#ifdef ESP_PLATFORM
#if defined(CONFIG_SPIRAM_USE_MALLOC)
    void *ptr = heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (ptr != NULL) {
        return ptr;
    }
#endif
#endif
    return calloc(count, size);
}

static espclaw_runtime_budget_t current_runtime_budget(void)
{
#ifdef ESP_PLATFORM
    const espclaw_runtime_status_t *status = espclaw_runtime_status();

    if (status != NULL) {
        return status->profile.runtime_budget;
    }
    return espclaw_board_profile_default().runtime_budget;
#else
    return espclaw_board_profile_for(ESPCLAW_BOARD_PROFILE_ESP32S3).runtime_budget;
#endif
}

static char *tool_result_at(char *results, size_t stride, size_t index)
{
    if (results == NULL || stride == 0U) {
        return NULL;
    }
    return results + (index * stride);
}

static int parse_provider_text_response(
    const char *json,
    char *response_id,
    size_t response_id_size,
    char *text,
    size_t text_size,
    bool *has_tools_out
);
static bool extract_json_string_from(const char *json, const char *key, const char *after, char *buffer, size_t buffer_size);

static bool is_completed_terminal_response_without_output(
    const char *json,
    char *response_id,
    size_t response_id_size
);

static espclaw_agent_http_adapter_t s_http_adapter;
static void *s_http_adapter_user_data;
static const char *TAG = "espclaw_agent";

static void free_embedded_auth_profile(espclaw_auth_profile_t *profile)
{
#ifdef ESP_PLATFORM
    free(profile);
#else
    (void)profile;
#endif
}
#ifndef ESPCLAW_HOST_LUA
typedef enum {
    ESPCLAW_CODEX_TRANSPORT_BUNDLE_ANY = 0,
    ESPCLAW_CODEX_TRANSPORT_BUNDLE_TLS12 = 1,
    ESPCLAW_CODEX_TRANSPORT_CHAIN_TLS12 = 2,
    ESPCLAW_CODEX_TRANSPORT_E7_TLS12 = 3
} espclaw_codex_transport_mode_t;

static const char *CHATGPT_LE_E7_CHAIN_PEM =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIEVzCCAj+gAwIBAgIRAKp18eYrjwoiCWbTi7/UuqEwDQYJKoZIhvcNAQELBQAw\n"
    "TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh\n"
    "cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMjQwMzEzMDAwMDAw\n"
    "WhcNMjcwMzEyMjM1OTU5WjAyMQswCQYDVQQGEwJVUzEWMBQGA1UEChMNTGV0J3Mg\n"
    "RW5jcnlwdDELMAkGA1UEAxMCRTcwdjAQBgcqhkjOPQIBBgUrgQQAIgNiAARB6AST\n"
    "CFh/vjcwDMCgQer+VtqEkz7JANurZxLP+U9TCeioL6sp5Z8VRvRbYk4P1INBmbef\n"
    "QHJFHCxcSjKmwtvGBWpl/9ra8HW0QDsUaJW2qOJqceJ0ZVFT3hbUHifBM/2jgfgw\n"
    "gfUwDgYDVR0PAQH/BAQDAgGGMB0GA1UdJQQWMBQGCCsGAQUFBwMCBggrBgEFBQcD\n"
    "ATASBgNVHRMBAf8ECDAGAQH/AgEAMB0GA1UdDgQWBBSuSJ7chx1EoG/aouVgdAR4\n"
    "wpwAgDAfBgNVHSMEGDAWgBR5tFnme7bl5AFzgAiIyBpY9umbbjAyBggrBgEFBQcB\n"
    "AQQmMCQwIgYIKwYBBQUHMAKGFmh0dHA6Ly94MS5pLmxlbmNyLm9yZy8wEwYDVR0g\n"
    "BAwwCjAIBgZngQwBAgEwJwYDVR0fBCAwHjAcoBqgGIYWaHR0cDovL3gxLmMubGVu\n"
    "Y3Iub3JnLzANBgkqhkiG9w0BAQsFAAOCAgEAjx66fDdLk5ywFn3CzA1w1qfylHUD\n"
    "aEf0QZpXcJseddJGSfbUUOvbNR9N/QQ16K1lXl4VFyhmGXDT5Kdfcr0RvIIVrNxF\n"
    "h4lqHtRRCP6RBRstqbZ2zURgqakn/Xip0iaQL0IdfHBZr396FgknniRYFckKORPG\n"
    "yM3QKnd66gtMst8I5nkRQlAg/Jb+Gc3egIvuGKWboE1G89NTsN9LTDD3PLj0dUMr\n"
    "OIuqVjLB8pEC6yk9enrlrqjXQgkLEYhXzq7dLafv5Vkig6Gl0nuuqjqfp0Q1bi1o\n"
    "yVNAlXe6aUXw92CcghC9bNsKEO1+M52YY5+ofIXlS/SEQbvVYYBLZ5yeiglV6t3S\n"
    "M6H+vTG0aP9YHzLn/KVOHzGQfXDP7qM5tkf+7diZe7o2fw6O7IvN6fsQXEQQj8TJ\n"
    "UXJxv2/uJhcuy/tSDgXwHM8Uk34WNbRT7zGTGkQRX0gsbjAea/jYAoWv0ZvQRwpq\n"
    "Pe79D/i7Cep8qWnA+7AE/3B3S/3dEEYmc0lpe1366A/6GEgk3ktr9PEoQrLChs6I\n"
    "tu3wnNLB2euC8IKGLQFpGtOO/2/hiAKjyajaBP25w1jF0Wl8Bbqne3uZ2q1GyPFJ\n"
    "YRmT7/OXpmOH/FVLtwS+8ng1cAmpCujPwteJZNcDG0sF2n/sc0+SQf49fdyUK0ty\n"
    "+VUwFj9tmWxyR/M=\n"
    "-----END CERTIFICATE-----\n"
    "-----BEGIN CERTIFICATE-----\n"
    "MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw\n"
    "TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh\n"
    "cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4\n"
    "WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu\n"
    "ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY\n"
    "MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc\n"
    "h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+\n"
    "0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U\n"
    "A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW\n"
    "T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH\n"
    "B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC\n"
    "B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv\n"
    "KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn\n"
    "OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn\n"
    "jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw\n"
    "qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI\n"
    "rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV\n"
    "HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq\n"
    "hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL\n"
    "ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ\n"
    "3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK\n"
    "NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5\n"
    "ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur\n"
    "TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC\n"
    "jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc\n"
    "oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq\n"
    "4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA\n"
    "mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d\n"
    "emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=\n"
    "-----END CERTIFICATE-----\n";

static const char *chatgpt_root_x1_pem(void)
{
    static const char *CERT_END = "-----END CERTIFICATE-----\n";
    static const char *CERT_BEGIN = "-----BEGIN CERTIFICATE-----\n";
    const char *first_end = strstr(CHATGPT_LE_E7_CHAIN_PEM, CERT_END);

    if (first_end == NULL) {
        return CHATGPT_LE_E7_CHAIN_PEM;
    }
    first_end += strlen(CERT_END);
    if (strncmp(first_end, CERT_BEGIN, strlen(CERT_BEGIN)) == 0) {
        return first_end;
    }
    return CHATGPT_LE_E7_CHAIN_PEM;
}
#endif

static const char *codex_transport_mode_name(int mode)
{
    switch (mode) {
    case 0:
        return "bundle_any";
    case 1:
        return "bundle_tls12";
    case 2:
        return "chain_tls12";
    case 3:
        return "e7_tls12";
    default:
        return "unknown";
    }
}

static void log_tool_call_summary(const espclaw_agent_tool_call_t *tool_call, const char *source)
{
#ifndef ESPCLAW_HOST_LUA
    size_t args_len = 0;

    if (tool_call == NULL) {
        return;
    }
    if (tool_call->arguments_json[0] != '\0') {
        args_len = strlen(tool_call->arguments_json);
    }
    ESP_LOGI(
        TAG,
        "tool call source=%s name=%s call_id=%s args_bytes=%u",
        source != NULL ? source : "unknown",
        tool_call->name[0] != '\0' ? tool_call->name : "(none)",
        tool_call->call_id[0] != '\0' ? tool_call->call_id : "(none)",
        (unsigned int)args_len
    );
#else
    (void)tool_call;
    (void)source;
#endif
}

static size_t append_tls_flag_name(char *buffer, size_t buffer_size, size_t used, bool *first, int flags, int bit, const char *name)
{
    if ((flags & bit) == 0 || buffer == NULL || buffer_size == 0) {
        return used;
    }

    used += (size_t)snprintf(
        buffer + used,
        buffer_size > used ? buffer_size - used : 0,
        "%s%s",
        (*first) ? "" : "|",
        name
    );
    *first = false;
    return used;
}

int espclaw_agent_format_transport_error(
    int transport_err,
    int tls_code,
    int tls_flags,
    char *buffer,
    size_t buffer_size
)
{
    char flag_text[192];
    const char *transport_name = "ESP_FAIL";
    size_t used = 0;
    bool first = true;

    if (buffer == NULL || buffer_size == 0) {
        return -1;
    }

    flag_text[0] = '\0';
    used = append_tls_flag_name(flag_text, sizeof(flag_text), used, &first, tls_flags, MBEDTLS_X509_BADCERT_EXPIRED, "EXPIRED");
    used = append_tls_flag_name(flag_text, sizeof(flag_text), used, &first, tls_flags, MBEDTLS_X509_BADCERT_FUTURE, "FUTURE");
    used = append_tls_flag_name(flag_text, sizeof(flag_text), used, &first, tls_flags, MBEDTLS_X509_BADCERT_CN_MISMATCH, "CN_MISMATCH");
    used = append_tls_flag_name(flag_text, sizeof(flag_text), used, &first, tls_flags, MBEDTLS_X509_BADCERT_NOT_TRUSTED, "NOT_TRUSTED");
    used = append_tls_flag_name(flag_text, sizeof(flag_text), used, &first, tls_flags, MBEDTLS_X509_BADCERT_BAD_MD, "BAD_MD");
    used = append_tls_flag_name(flag_text, sizeof(flag_text), used, &first, tls_flags, MBEDTLS_X509_BADCERT_BAD_PK, "BAD_PK");
    used = append_tls_flag_name(flag_text, sizeof(flag_text), used, &first, tls_flags, MBEDTLS_X509_BADCERT_BAD_KEY, "BAD_KEY");
    used = append_tls_flag_name(flag_text, sizeof(flag_text), used, &first, tls_flags, MBEDTLS_X509_BADCERT_KEY_USAGE, "KEY_USAGE");
    used = append_tls_flag_name(flag_text, sizeof(flag_text), used, &first, tls_flags, MBEDTLS_X509_BADCERT_EXT_KEY_USAGE, "EXT_KEY_USAGE");
    used = append_tls_flag_name(flag_text, sizeof(flag_text), used, &first, tls_flags, MBEDTLS_X509_BADCERT_REVOKED, "REVOKED");
    used = append_tls_flag_name(flag_text, sizeof(flag_text), used, &first, tls_flags, MBEDTLS_X509_BADCERT_MISSING, "MISSING");
    used = append_tls_flag_name(flag_text, sizeof(flag_text), used, &first, tls_flags, MBEDTLS_X509_BADCERT_OTHER, "OTHER");

#ifdef ESP_PLATFORM
    if (transport_err != 0) {
        transport_name = esp_err_to_name(transport_err);
    }
#endif
    snprintf(
        buffer,
        buffer_size,
        "Provider transport failed: %s (%d)",
        transport_name,
        transport_err
    );
    if (tls_code != 0) {
        size_t offset = strlen(buffer);

        snprintf(
            buffer + offset,
            buffer_size > offset ? buffer_size - offset : 0,
            ", tls_code=%d%s%s%s",
            tls_code,
            tls_flags != 0 ? ", tls_flags=" : "",
            tls_flags != 0 ? flag_text : "",
            (tls_flags != 0 && flag_text[0] == '\0') ? "0" : ""
        );
    }

    return 0;
}

static void copy_text(char *buffer, size_t buffer_size, const char *value)
{
    if (buffer == NULL || buffer_size == 0) {
        return;
    }

    snprintf(buffer, buffer_size, "%s", value != NULL ? value : "");
}

static bool history_role_is_supported(const char *role)
{
    return role != NULL &&
           (strcmp(role, "assistant") == 0 ||
            strcmp(role, "system") == 0 ||
            strcmp(role, "developer") == 0 ||
            strcmp(role, "user") == 0 ||
            strcmp(role, "tool") == 0);
}

static bool extract_history_message(const char *line, espclaw_history_message_t *message)
{
    if (line == NULL || message == NULL) {
        return false;
    }
    if (!extract_json_string_from(line, "role", NULL, message->role, sizeof(message->role)) ||
        !extract_json_string_from(line, "content", NULL, message->content, sizeof(message->content))) {
        return false;
    }
    return history_role_is_supported(message->role);
}

static bool append_buffer_full(size_t used, size_t buffer_size)
{
    return buffer_size == 0 || used >= buffer_size;
}

static size_t append_format(char *buffer, size_t buffer_size, size_t used, const char *format, ...)
{
    va_list args;
    int written = 0;

    if (buffer == NULL || buffer_size == 0) {
        return used;
    }
    if (used >= buffer_size) {
        return buffer_size;
    }

    va_start(args, format);
    written = vsnprintf(buffer + used, buffer_size - used, format, args);
    va_end(args);
    if (written < 0) {
        return buffer_size;
    }
    if ((size_t)written >= buffer_size - used) {
        return buffer_size;
    }
    return used + (size_t)written;
}

static size_t append_escaped_json(char *buffer, size_t buffer_size, size_t used, const char *value)
{
    const char *cursor = value != NULL ? value : "";

    if (buffer == NULL || buffer_size == 0 || used >= buffer_size) {
        return used;
    }

    used = append_format(buffer, buffer_size, used, "\"");
    while (*cursor != '\0' && used + 2 < buffer_size) {
        switch (*cursor) {
        case '\\':
        case '"':
            used = append_format(buffer, buffer_size, used, "\\%c", *cursor);
            break;
        case '\n':
            used = append_format(buffer, buffer_size, used, "\\n");
            break;
        case '\r':
            used = append_format(buffer, buffer_size, used, "\\r");
            break;
        case '\t':
            used = append_format(buffer, buffer_size, used, "\\t");
            break;
        default:
            buffer[used++] = *cursor;
            buffer[used] = '\0';
            break;
        }
        cursor++;
    }
    used = append_format(buffer, buffer_size, used, "\"");
    return append_buffer_full(used, buffer_size) ? buffer_size : used;
}

static size_t base64_encode_bytes(const uint8_t *input, size_t input_length, char *output, size_t output_size)
{
    static const char TABLE[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t input_index = 0;
    size_t output_index = 0;

    if (output == NULL || output_size == 0) {
        return 0;
    }

    while (input != NULL && input_index < input_length && output_index + 4 < output_size) {
        uint32_t block = (uint32_t)input[input_index++] << 16;
        int remain = 0;

        if (input_index < input_length) {
            block |= (uint32_t)input[input_index++] << 8;
            remain++;
        }
        if (input_index < input_length) {
            block |= (uint32_t)input[input_index++];
            remain++;
        }

        output[output_index++] = TABLE[(block >> 18) & 0x3F];
        output[output_index++] = TABLE[(block >> 12) & 0x3F];
        output[output_index++] = remain >= 1 ? TABLE[(block >> 6) & 0x3F] : '=';
        output[output_index++] = remain >= 2 ? TABLE[block & 0x3F] : '=';
    }

    output[output_index] = '\0';
    return output_index;
}

static int read_file_bytes(const char *path, uint8_t *buffer, size_t max_length, size_t *length_out)
{
    FILE *file = NULL;
    size_t read_length = 0;

    if (path == NULL || buffer == NULL || length_out == NULL || max_length == 0) {
        return -1;
    }

    file = fopen(path, "rb");
    if (file == NULL) {
        return -1;
    }
    read_length = fread(buffer, 1, max_length, file);
    fclose(file);
    *length_out = read_length;
    return read_length > 0 ? 0 : -1;
}

static size_t append_input_images(
    const char *workspace_root,
    const char *provider_id,
    char *buffer,
    size_t buffer_size,
    size_t used,
    const espclaw_agent_media_ref_t *media_refs,
    size_t media_count,
    size_t image_data_max
)
{
    size_t index;
    bool codex_data_url_mode = provider_id != NULL && strcmp(provider_id, "openai_codex") == 0;

    if (workspace_root == NULL || workspace_root[0] == '\0' || image_data_max == 0U) {
        return used;
    }

    for (index = 0; index < media_count; ++index) {
        char absolute_path[512];
        uint8_t *image_data = NULL;
        char *encoded = NULL;
        size_t image_length = 0;

        if (media_refs == NULL || !media_refs[index].active) {
            continue;
        }
        if (espclaw_workspace_resolve_path(
                workspace_root,
                media_refs[index].relative_path,
                absolute_path,
                sizeof(absolute_path)) != 0) {
            continue;
        }
        image_data = (uint8_t *)calloc(1, image_data_max);
        encoded = (char *)calloc(1, (image_data_max * 4 / 3) + 8);
        if (image_data == NULL || encoded == NULL) {
            free(image_data);
            free(encoded);
            continue;
        }
        if (read_file_bytes(absolute_path, image_data, image_data_max, &image_length) != 0) {
            free(image_data);
            free(encoded);
            continue;
        }
        if (base64_encode_bytes(image_data, image_length, encoded, (image_data_max * 4 / 3) + 8) == 0) {
            free(image_data);
            free(encoded);
            continue;
        }

        if (used > 0) {
            used = append_format(buffer, buffer_size, used, ",");
        }
        if (codex_data_url_mode) {
            size_t data_url_size = strlen("data:") + strlen(media_refs[index].mime_type) + strlen(";base64,") + strlen(encoded) + 1;
            char *data_url = (char *)calloc(1, data_url_size);

            if (data_url == NULL) {
                free(image_data);
                free(encoded);
                continue;
            }
            snprintf(data_url, data_url_size, "data:%s;base64,%s", media_refs[index].mime_type, encoded);
            used = append_format(
                buffer,
                buffer_size,
                used,
                "{\"type\":\"message\",\"role\":\"user\",\"content\":[{\"type\":\"input_image\",\"image_url\":"
            );
            used = append_escaped_json(buffer, buffer_size, used, data_url);
            used = append_format(buffer, buffer_size, used, "}]}");
            free(data_url);
        } else {
            used = append_format(buffer, buffer_size, used, "{\"type\":\"input_image\",\"source\":{");
            used = append_format(buffer, buffer_size, used, "\"type\":\"base64\",\"media_type\":");
            used = append_escaped_json(buffer, buffer_size, used, media_refs[index].mime_type);
            used = append_format(buffer, buffer_size, used, ",\"data\":");
            used = append_escaped_json(buffer, buffer_size, used, encoded);
            used = append_format(buffer, buffer_size, used, "}}");
        }
        free(image_data);
        free(encoded);
        if (append_buffer_full(used, buffer_size)) {
            return buffer_size;
        }
    }

    return append_buffer_full(used, buffer_size) ? buffer_size : used;
}

static bool is_tool_wire_safe(unsigned char value)
{
    return isalnum(value) || value == '-';
}

static void encode_tool_name(const char *runtime_name, char *wire_name, size_t wire_name_size)
{
    const unsigned char *cursor = (const unsigned char *)(runtime_name != NULL ? runtime_name : "");
    size_t used = 0;

    if (wire_name == NULL || wire_name_size == 0) {
        return;
    }

    while (*cursor != '\0' && used + 1 < wire_name_size) {
        if (is_tool_wire_safe(*cursor)) {
            wire_name[used++] = (char)*cursor;
            wire_name[used] = '\0';
        } else {
            int written = snprintf(wire_name + used, wire_name_size - used, "_x%02X_", *cursor);

            if (written <= 0 || (size_t)written >= wire_name_size - used) {
                break;
            }
            used += (size_t)written;
        }
        cursor++;
    }
    wire_name[used] = '\0';
}

static void decode_tool_name(const char *wire_name, char *runtime_name, size_t runtime_name_size)
{
    const char *cursor = wire_name != NULL ? wire_name : "";
    size_t used = 0;

    if (runtime_name == NULL || runtime_name_size == 0) {
        return;
    }

    while (*cursor != '\0' && used + 1 < runtime_name_size) {
        if (cursor[0] == '_' &&
            cursor[1] == 'x' &&
            isxdigit((unsigned char)cursor[2]) &&
            isxdigit((unsigned char)cursor[3]) &&
            cursor[4] == '_') {
            char hex[3];

            hex[0] = cursor[2];
            hex[1] = cursor[3];
            hex[2] = '\0';
            runtime_name[used++] = (char)strtol(hex, NULL, 16);
            runtime_name[used] = '\0';
            cursor += 5;
            continue;
        }
        runtime_name[used++] = *cursor++;
        runtime_name[used] = '\0';
    }
    runtime_name[used] = '\0';
}

static bool contains_case_insensitive_text(const char *haystack, const char *needle)
{
    size_t needle_length;
    const char *cursor;

    if (haystack == NULL || needle == NULL) {
        return false;
    }

    needle_length = strlen(needle);
    if (needle_length == 0) {
        return true;
    }

    for (cursor = haystack; *cursor != '\0'; ++cursor) {
        size_t index = 0;

        while (index < needle_length &&
               cursor[index] != '\0' &&
               tolower((unsigned char)cursor[index]) == tolower((unsigned char)needle[index])) {
            index++;
        }
        if (index == needle_length) {
            return true;
        }
    }

    return false;
}

static const char *find_json_key_after(const char *json, const char *key, const char *after)
{
    char pattern[64];
    const char *cursor = after != NULL ? after : json;

    if (json == NULL || key == NULL) {
        return NULL;
    }

    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    return strstr(cursor, pattern);
}

static bool extract_json_string_from(const char *json, const char *key, const char *after, char *buffer, size_t buffer_size)
{
    const char *cursor = find_json_key_after(json, key, after);
    size_t used = 0;
    bool truncated = false;

    if (cursor == NULL || buffer == NULL || buffer_size == 0) {
        if (buffer != NULL && buffer_size > 0) {
            buffer[0] = '\0';
        }
        return false;
    }

    cursor = strchr(cursor, ':');
    if (cursor == NULL) {
        buffer[0] = '\0';
        return false;
    }
    cursor++;
    while (*cursor != '\0' && isspace((unsigned char)*cursor)) {
        cursor++;
    }
    if (*cursor != '"') {
        buffer[0] = '\0';
        return false;
    }
    cursor++;

    while (*cursor != '\0' && *cursor != '"') {
        char decoded = '\0';

        if (*cursor == '\\' && cursor[1] != '\0') {
            cursor++;
            switch (*cursor) {
            case 'n':
                decoded = '\n';
                break;
            case 'r':
                decoded = '\r';
                break;
            case 't':
                decoded = '\t';
                break;
            default:
                decoded = *cursor;
                break;
            }
        } else {
            decoded = *cursor;
        }

        if (used + 1 < buffer_size) {
            buffer[used++] = decoded;
        } else {
            truncated = true;
        }
        cursor++;
    }
    if (*cursor != '"') {
        buffer[0] = '\0';
        return false;
    }
    buffer[used] = '\0';
    (void)truncated;
    return true;
}

static bool extract_json_object_from(
    const char *json,
    const char *key,
    const char *after,
    char *buffer,
    size_t buffer_size
)
{
    const char *cursor = find_json_key_after(json, key, after);
    const char *start = NULL;
    const char *scan = NULL;
    size_t depth = 0;
    bool in_string = false;
    bool escaped = false;

    if (cursor == NULL || buffer == NULL || buffer_size == 0) {
        if (buffer != NULL && buffer_size > 0) {
            buffer[0] = '\0';
        }
        return false;
    }

    cursor = strchr(cursor, ':');
    if (cursor == NULL) {
        buffer[0] = '\0';
        return false;
    }
    cursor++;
    while (*cursor != '\0' && isspace((unsigned char)*cursor)) {
        cursor++;
    }
    if (*cursor != '{') {
        buffer[0] = '\0';
        return false;
    }

    start = cursor;
    for (scan = cursor; *scan != '\0'; ++scan) {
        char ch = *scan;

        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                in_string = false;
            }
            continue;
        }

        if (ch == '"') {
            in_string = true;
            continue;
        }
        if (ch == '{') {
            depth++;
            continue;
        }
        if (ch == '}') {
            if (depth == 0) {
                break;
            }
            depth--;
            if (depth == 0) {
                size_t length = (size_t)(scan - start + 1);

                if (length >= buffer_size) {
                    length = buffer_size - 1;
                }
                memcpy(buffer, start, length);
                buffer[length] = '\0';
                return true;
            }
        }
    }

    buffer[0] = '\0';
    return false;
}

static bool find_json_object_containing_span(
    const char *json,
    const char *needle,
    const char **start_out,
    size_t *length_out
)
{
    const char *cursor = needle;
    const char *start = NULL;
    const char *scan = NULL;
    size_t depth = 0;
    bool in_string = false;
    bool escaped = false;

    if (json == NULL || needle == NULL || start_out == NULL || length_out == NULL) {
        return false;
    }

    while (cursor > json) {
        cursor--;
        if (*cursor == '{') {
            start = cursor;
            break;
        }
    }
    if (start == NULL) {
        return false;
    }

    for (scan = start; *scan != '\0'; ++scan) {
        char ch = *scan;

        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                in_string = false;
            }
            continue;
        }

        if (ch == '"') {
            in_string = true;
            continue;
        }
        if (ch == '{') {
            depth++;
            continue;
        }
        if (ch == '}') {
            if (depth == 0) {
                break;
            }
            depth--;
            if (depth == 0) {
                *start_out = start;
                *length_out = (size_t)(scan - start + 1);
                return true;
            }
        }
    }

    return false;
}

#ifdef ESPCLAW_HOST_LUA
static bool extract_last_user_message(const char *request_body, char *buffer, size_t buffer_size)
{
    const char *cursor;
    bool found = false;

    if (buffer == NULL || buffer_size == 0) {
        return false;
    }

    buffer[0] = '\0';
    if (request_body == NULL) {
        return false;
    }

    cursor = request_body;
    while ((cursor = strstr(cursor, "\"role\":\"user\"")) != NULL) {
        if (extract_json_string_from(request_body, "content", cursor, buffer, buffer_size)) {
            found = true;
        }
        cursor += strlen("\"role\":\"user\"");
    }

    return found;
}

static bool user_message_requests_tool_listing(const char *request_body)
{
    char user_message[1024];

    if (request_body != NULL &&
        (contains_case_insensitive_text(request_body, "available tools to you") ||
         contains_case_insensitive_text(request_body, "list out all the available tools") ||
         contains_case_insensitive_text(request_body, "what capabilities do you have"))) {
        return true;
    }

    if (!extract_last_user_message(request_body, user_message, sizeof(user_message))) {
        return contains_case_insensitive_text(request_body, "available tools to you") ||
               contains_case_insensitive_text(request_body, "what capabilities do you have");
    }

    return contains_case_insensitive_text(user_message, "available tools") ||
           contains_case_insensitive_text(user_message, "what tools") ||
           contains_case_insensitive_text(user_message, "what capabilities") ||
           contains_case_insensitive_text(user_message, "list out all the available tools");
}
#endif

static bool user_message_requests_lua_app_contract(const char *user_message)
{
    if (user_message == NULL || user_message[0] == '\0') {
        return false;
    }

    return contains_case_insensitive_text(user_message, "lua") ||
           contains_case_insensitive_text(user_message, "app") ||
           contains_case_insensitive_text(user_message, "script") ||
           contains_case_insensitive_text(user_message, "behavior") ||
           contains_case_insensitive_text(user_message, "task") ||
           contains_case_insensitive_text(user_message, "install") ||
           contains_case_insensitive_text(user_message, "write code") ||
           contains_case_insensitive_text(user_message, "generate code");
}

static bool user_message_requests_app_install(const char *user_message)
{
    if (user_message == NULL || user_message[0] == '\0') {
        return false;
    }

    if (!contains_case_insensitive_text(user_message, "app")) {
        return false;
    }

    return contains_case_insensitive_text(user_message, "create") ||
           contains_case_insensitive_text(user_message, "install") ||
           contains_case_insensitive_text(user_message, "update");
}

static bool user_message_contains_creation_verb(const char *user_message)
{
    if (user_message == NULL || user_message[0] == '\0') {
        return false;
    }

    return contains_case_insensitive_text(user_message, "create") ||
           contains_case_insensitive_text(user_message, "make") ||
           contains_case_insensitive_text(user_message, "build") ||
           contains_case_insensitive_text(user_message, "generate") ||
           contains_case_insensitive_text(user_message, "write") ||
           contains_case_insensitive_text(user_message, "install") ||
           contains_case_insensitive_text(user_message, "update");
}

static bool user_message_requests_task_artifact(const char *user_message)
{
    if (!user_message_contains_creation_verb(user_message)) {
        return false;
    }

    return contains_case_insensitive_text(user_message, "task") ||
           contains_case_insensitive_text(user_message, "background");
}

static bool user_message_requests_behavior_artifact(const char *user_message)
{
    if (user_message == NULL || user_message[0] == '\0') {
        return false;
    }
    if (user_message_contains_creation_verb(user_message) &&
        contains_case_insensitive_text(user_message, "behavior")) {
        return true;
    }

    return user_message_contains_creation_verb(user_message) &&
           (contains_case_insensitive_text(user_message, "autostart") ||
            contains_case_insensitive_text(user_message, "survive reboot") ||
            contains_case_insensitive_text(user_message, "across reboot") ||
            contains_case_insensitive_text(user_message, "on boot") ||
            contains_case_insensitive_text(user_message, "persist"));
}

static bool user_message_requests_reusable_logic_artifact(const char *user_message)
{
    if (!user_message_contains_creation_verb(user_message)) {
        return false;
    }

    return contains_case_insensitive_text(user_message, "lua") ||
           contains_case_insensitive_text(user_message, "script") ||
           contains_case_insensitive_text(user_message, "app") ||
           user_message_requests_task_artifact(user_message) ||
           user_message_requests_behavior_artifact(user_message);
}

static bool user_message_requests_run_after_creation(const char *user_message)
{
    if (user_message == NULL || user_message[0] == '\0') {
        return false;
    }

    return contains_case_insensitive_text(user_message, "then run") ||
           contains_case_insensitive_text(user_message, "then start") ||
           contains_case_insensitive_text(user_message, "and run it") ||
           contains_case_insensitive_text(user_message, "and start it") ||
           contains_case_insensitive_text(user_message, "run it") ||
           contains_case_insensitive_text(user_message, "start it") ||
           contains_case_insensitive_text(user_message, "run now") ||
           contains_case_insensitive_text(user_message, "start now") ||
           contains_case_insensitive_text(user_message, "launch it");
}

static bool user_message_requests_web_search_and_fetch(const char *user_message)
{
    bool wants_search;
    bool wants_fetch;

    if (user_message == NULL || user_message[0] == '\0') {
        return false;
    }

    wants_search = contains_case_insensitive_text(user_message, "web.search") ||
                   contains_case_insensitive_text(user_message, "web search") ||
                   contains_case_insensitive_text(user_message, "search");
    wants_fetch = contains_case_insensitive_text(user_message, "web.fetch") ||
                  contains_case_insensitive_text(user_message, "web fetch") ||
                  contains_case_insensitive_text(user_message, "fetch");
    return wants_search && wants_fetch;
}

static bool user_message_is_short_affirmative_tool_followup(const char *user_message)
{
    if (user_message == NULL || user_message[0] == '\0' || strlen(user_message) > 64U) {
        return false;
    }

    return contains_case_insensitive_text(user_message, "yes") ||
           contains_case_insensitive_text(user_message, "ok") ||
           contains_case_insensitive_text(user_message, "okay") ||
           contains_case_insensitive_text(user_message, "sure") ||
           contains_case_insensitive_text(user_message, "do it") ||
           contains_case_insensitive_text(user_message, "do that") ||
           contains_case_insensitive_text(user_message, "go ahead") ||
           contains_case_insensitive_text(user_message, "try that") ||
           contains_case_insensitive_text(user_message, "please try") ||
           contains_case_insensitive_text(user_message, "you can") ||
           contains_case_insensitive_text(user_message, "run it") ||
           contains_case_insensitive_text(user_message, "run the tool") ||
           contains_case_insensitive_text(user_message, "please do");
}

static bool add_required_tool_name(
    char names[][ESPCLAW_AGENT_TOOL_NAME_MAX + 1],
    size_t *count_io,
    size_t max_count,
    const char *tool_name
)
{
    size_t index;

    if (names == NULL || count_io == NULL || tool_name == NULL || tool_name[0] == '\0') {
        return false;
    }

    for (index = 0; index < *count_io; ++index) {
        if (strcmp(names[index], tool_name) == 0) {
            return true;
        }
    }
    if (*count_io >= max_count) {
        return false;
    }

    copy_text(names[*count_io], ESPCLAW_AGENT_TOOL_NAME_MAX + 1U, tool_name);
    (*count_io)++;
    return true;
}

typedef struct {
    const char *phrase;
    const char *tool_name;
} espclaw_tool_phrase_alias_t;

static size_t collect_required_tool_names_from_aliases(
    const char *text,
    char names[][ESPCLAW_AGENT_TOOL_NAME_MAX + 1],
    size_t count,
    size_t max_count
)
{
    static const espclaw_tool_phrase_alias_t ALIASES[] = {
        {"task list", "task.list"},
        {"list tasks", "task.list"},
        {"show tasks", "task.list"},
        {"run task list", "task.list"},
        {"start task", "task.start"},
        {"run task", "task.start"},
        {"emit an event", "event.emit"},
        {"emit event", "event.emit"},
        {"trigger an event", "event.emit"},
        {"trigger event", "event.emit"},
        {"list apps", "app.list"},
        {"show apps", "app.list"},
        {"list behaviors", "behavior.list"},
        {"show behaviors", "behavior.list"},
        {"list tools", "tool.list"},
        {"show tools", "tool.list"},
        {"what tools", "tool.list"},
    };
    size_t index;

    if (text == NULL || text[0] == '\0' || names == NULL || max_count == 0U) {
        return count;
    }

    for (index = 0; index < sizeof(ALIASES) / sizeof(ALIASES[0]); ++index) {
        if (contains_case_insensitive_text(text, ALIASES[index].phrase)) {
            add_required_tool_name(names, &count, max_count, ALIASES[index].tool_name);
        }
    }

    return count;
}

static size_t collect_required_tool_names_from_text(
    const char *text,
    char names[][ESPCLAW_AGENT_TOOL_NAME_MAX + 1],
    size_t max_count
)
{
    size_t index;
    size_t count = 0;

    if (text == NULL || text[0] == '\0' || names == NULL || max_count == 0U) {
        return 0;
    }

    for (index = 0; index < espclaw_tool_count(); ++index) {
        const espclaw_tool_descriptor_t *tool = espclaw_tool_at(index);

        if (tool == NULL || tool->name[0] == '\0') {
            continue;
        }
        if (contains_case_insensitive_text(text, tool->name)) {
            add_required_tool_name(names, &count, max_count, tool->name);
        }
    }

    return collect_required_tool_names_from_aliases(text, names, count, max_count);
}

static size_t collect_required_tool_names(
    const char *user_message,
    const espclaw_history_message_t *history,
    size_t history_count,
    char names[][ESPCLAW_AGENT_TOOL_NAME_MAX + 1],
    size_t max_count
)
{
    size_t count = 0;
    size_t index;

    if (names == NULL || max_count == 0U) {
        return 0;
    }

    count = collect_required_tool_names_from_text(user_message, names, max_count);
    if (count > 0 || !user_message_is_short_affirmative_tool_followup(user_message) || history == NULL || history_count == 0U) {
        return count;
    }

    for (index = history_count; index > 0; --index) {
        if (strcmp(history[index - 1].role, "assistant") == 0) {
            return collect_required_tool_names_from_text(history[index - 1].content, names, max_count);
        }
    }

    return 0;
}

static void history_append_message(
    espclaw_history_message_t *history,
    size_t max_messages,
    size_t *count_io,
    const char *role,
    const char *content
)
{
    size_t count;

    if (history == NULL || count_io == NULL || max_messages == 0 || role == NULL || content == NULL) {
        return;
    }

    count = *count_io;
    if (count >= max_messages) {
        memmove(history, history + 1, sizeof(*history) * (max_messages - 1));
        count = max_messages - 1;
    }

    copy_text(history[count].role, sizeof(history[count].role), role);
    copy_text(history[count].content, sizeof(history[count].content), content);
    *count_io = count + 1;
}

static int build_app_install_retry_request_body(
    const espclaw_auth_profile_t *profile,
    const char *workspace_root,
    const char *session_id,
    const char *instructions,
    const char *assistant_reply,
    char *buffer,
    size_t buffer_size,
    size_t history_max
)
{
    static const char *RETRY_USER_MESSAGE =
        "You must not claim that an app was installed or updated unless you actually call app.install successfully in this run. "
        "Call app.install now with complete Lua source, triggers, and permissions if needed, then reply concisely.";
    espclaw_history_message_t *history = NULL;
    size_t history_count = 0;
    int status;

    if (profile == NULL || instructions == NULL || buffer == NULL || buffer_size == 0 || history_max == 0) {
        return -1;
    }

    history = (espclaw_history_message_t *)agent_calloc(history_max, sizeof(*history));
    if (history == NULL) {
        return -1;
    }

    if (workspace_root != NULL && workspace_root[0] != '\0') {
        load_history(workspace_root, session_id, history, history_max, &history_count);
    }
    if (assistant_reply != NULL && assistant_reply[0] != '\0') {
        history_append_message(history, history_max, &history_count, "assistant", assistant_reply);
    }
    history_append_message(history, history_max, &history_count, "user", RETRY_USER_MESSAGE);
    status = build_initial_request_body(profile, instructions, history, history_count, buffer, buffer_size);
    free(history);
    return status;
}

static int build_web_search_fetch_retry_request_body(
    const espclaw_auth_profile_t *profile,
    const char *workspace_root,
    const char *session_id,
    const char *instructions,
    const char *assistant_reply,
    bool need_search,
    bool need_fetch,
    char *buffer,
    size_t buffer_size,
    size_t history_max
)
{
    char retry_user_message[320];
    espclaw_history_message_t *history = NULL;
    size_t history_count = 0;
    int status;

    if (profile == NULL || instructions == NULL || buffer == NULL || buffer_size == 0 || history_max == 0) {
        return -1;
    }

    snprintf(
        retry_user_message,
        sizeof(retry_user_message),
        "This request explicitly requires both web.search and web.fetch. You have not yet successfully called%s%s%s in this run. Call the missing tool%s now, then answer concisely using the tool output%s.",
        need_search ? " web.search" : "",
        need_search && need_fetch ? " and" : "",
        need_fetch ? " web.fetch" : "",
        (need_search && need_fetch) ? "s" : "",
        (need_search && need_fetch) ? "s" : ""
    );

    history = (espclaw_history_message_t *)agent_calloc(history_max, sizeof(*history));
    if (history == NULL) {
        return -1;
    }

    if (workspace_root != NULL && workspace_root[0] != '\0') {
        load_history(workspace_root, session_id, history, history_max, &history_count);
    }
    if (assistant_reply != NULL && assistant_reply[0] != '\0') {
        history_append_message(history, history_max, &history_count, "assistant", assistant_reply);
    }
    history_append_message(history, history_max, &history_count, "user", retry_user_message);
    status = build_initial_request_body(profile, instructions, history, history_count, buffer, buffer_size);
    free(history);
    return status;
}

static int build_execution_choice_retry_request_body(
    const espclaw_auth_profile_t *profile,
    const char *workspace_root,
    const char *session_id,
    const char *instructions,
    const char *assistant_reply,
    bool need_app_install,
    bool need_task_start,
    bool need_behavior_register,
    char *buffer,
    size_t buffer_size,
    size_t history_max
)
{
    char retry_user_message[640];
    espclaw_history_message_t *history = NULL;
    size_t history_count = 0;
    size_t used = 0;
    int status;

    if (profile == NULL || instructions == NULL || buffer == NULL || buffer_size == 0 || history_max == 0) {
        return -1;
    }

    used += (size_t)snprintf(
        retry_user_message + used,
        sizeof(retry_user_message) - used,
        "This request asked for a specific execution shape. "
    );
    if (need_app_install) {
        used += (size_t)snprintf(
            retry_user_message + used,
            sizeof(retry_user_message) - used,
            "You still need app.install"
        );
    }
    if (need_task_start) {
        used += (size_t)snprintf(
            retry_user_message + used,
            sizeof(retry_user_message) - used,
            "%s task.start",
            need_app_install ? " and" : "You still need"
        );
    }
    if (need_behavior_register) {
        used += (size_t)snprintf(
            retry_user_message + used,
            sizeof(retry_user_message) - used,
            "%s behavior.register",
            (need_app_install || need_task_start) ? " and" : "You still need"
        );
    }
    used += (size_t)snprintf(
        retry_user_message + used,
        sizeof(retry_user_message) - used,
        " in this run. Choose the right execution structure: one-shot action now => direct tool calls; repeated or timed action now => perform the full sequence; reusable logic => app.install; run reusable logic now in the background => task.start after app.install; persist or autostart it => behavior.register after app.install. Call the missing tool%s now in the right order, then answer concisely with the actual result.",
        (need_app_install + need_task_start + need_behavior_register) == 1 ? "" : "s"
    );

    history = (espclaw_history_message_t *)agent_calloc(history_max, sizeof(*history));
    if (history == NULL) {
        return -1;
    }

    if (workspace_root != NULL && workspace_root[0] != '\0') {
        load_history(workspace_root, session_id, history, history_max, &history_count);
    }
    if (assistant_reply != NULL && assistant_reply[0] != '\0') {
        history_append_message(history, history_max, &history_count, "assistant", assistant_reply);
    }
    history_append_message(history, history_max, &history_count, "user", retry_user_message);
    status = build_initial_request_body(profile, instructions, history, history_count, buffer, buffer_size);
    free(history);
    return status;
}

static int build_explicit_tool_retry_request_body(
    const espclaw_auth_profile_t *profile,
    const char *workspace_root,
    const char *session_id,
    const char *instructions,
    const char *assistant_reply,
    char missing_tools[][ESPCLAW_AGENT_TOOL_NAME_MAX + 1],
    size_t missing_count,
    char *buffer,
    size_t buffer_size,
    size_t history_max
)
{
    char retry_user_message[512];
    espclaw_history_message_t *history = NULL;
    size_t history_count = 0;
    size_t used = 0;
    size_t index;
    int status;

    if (profile == NULL || instructions == NULL || missing_tools == NULL || missing_count == 0U ||
        buffer == NULL || buffer_size == 0 || history_max == 0U) {
        return -1;
    }

    used += (size_t)snprintf(
        retry_user_message + used,
        sizeof(retry_user_message) - used,
        "The operator explicitly told you to call these tools in this turn: "
    );
    for (index = 0; index < missing_count && used + 8 < sizeof(retry_user_message); ++index) {
        used += (size_t)snprintf(
            retry_user_message + used,
            sizeof(retry_user_message) - used,
            "%s%s",
            index == 0 ? "" : ", ",
            missing_tools[index]
        );
    }
    used += (size_t)snprintf(
        retry_user_message + used,
        sizeof(retry_user_message) - used,
        ". Call the missing tool%s now instead of describing what you still need, then answer concisely with the actual result.",
        missing_count == 1U ? "" : "s"
    );

    history = (espclaw_history_message_t *)agent_calloc(history_max, sizeof(*history));
    if (history == NULL) {
        return -1;
    }

    if (workspace_root != NULL && workspace_root[0] != '\0') {
        load_history(workspace_root, session_id, history, history_max, &history_count);
    }
    if (assistant_reply != NULL && assistant_reply[0] != '\0') {
        history_append_message(history, history_max, &history_count, "assistant", assistant_reply);
    }
    history_append_message(history, history_max, &history_count, "user", retry_user_message);
    status = build_initial_request_body(profile, instructions, history, history_count, buffer, buffer_size);
    free(history);
    return status;
}

static bool tool_result_reports_success(const char *tool_result)
{
    return tool_result != NULL && strstr(tool_result, "\"ok\":true") != NULL;
}

static int load_system_prompt(
    const char *workspace_root,
    const char *user_message,
    bool allow_mutations,
    bool yolo_mode,
    char *buffer,
    size_t buffer_size
)
{
    static const char *CONTROL_FILES[] = {"AGENTS.md", "IDENTITY.md", "USER.md", "HEARTBEAT.md", "memory/MEMORY.md"};
    size_t index;
    size_t used = 0;

    if (buffer == NULL || buffer_size == 0) {
        return -1;
    }

    buffer[0] = '\0';
    if (workspace_root != NULL && workspace_root[0] != '\0') {
        for (index = 0; index < sizeof(CONTROL_FILES) / sizeof(CONTROL_FILES[0]); ++index) {
            char content[2048];

            if (espclaw_workspace_read_file(workspace_root, CONTROL_FILES[index], content, sizeof(content)) != 0) {
                continue;
            }
            if (used > 0 && used + 2 < buffer_size) {
                used += (size_t)snprintf(buffer + used, buffer_size - used, "\n\n");
            }
            used += (size_t)snprintf(buffer + used, buffer_size - used, "%s", content);
            if (used >= buffer_size) {
                buffer[buffer_size - 1] = '\0';
                return 0;
            }
        }
    } else {
        used += (size_t)snprintf(
            buffer + used,
            buffer_size - used,
            "# Runtime mode\n"
            "- Workspace storage is currently unavailable.\n"
            "- Chat is running in ephemeral mode without persistent memory or workspace files.\n"
            "- Storage-dependent tools may fail until workspace storage is restored.\n"
        );
    }

    used += (size_t)snprintf(
        buffer + used,
        buffer_size - used,
        "\n\n# Tool policy\n"
        "- Use tools when they materially reduce guesswork.\n"
        "- The tool inventory snapshot below is part of the system prompt for every run.\n"
        "- When the user asks what tools or capabilities are available, use the prompt inventory first and call tool.list if you need an explicit detailed listing.\n"
        "- Read-only tools are preferred when exploring.\n"
    );
    if (yolo_mode) {
        used += (size_t)snprintf(
            buffer + used,
            buffer_size - used,
            "- YOLO mode is enabled for this run.\n"
            "- If a tool is relevant, call it directly instead of asking the operator for another approval step.\n"
            "- Prefer doing the work over narrating what you could do.\n"
        );
    } else if (allow_mutations) {
        used += (size_t)snprintf(
            buffer + used,
            buffer_size - used,
            "- This run was initiated by a trusted local operator surface. When a mutating tool is materially necessary, you may call it without asking for an extra confirmation step.\n"
            "- If a tool still reports confirmation_required, explain that to the user instead of retrying the same call.\n"
        );
    } else {
        used += (size_t)snprintf(
            buffer + used,
            buffer_size - used,
            "- Mutating tools require explicit confirmation from the user before execution.\n"
            "- When a tool reports confirmation_required, ask the user for confirmation instead of retrying the same call.\n"
        );
    }

    used += (size_t)snprintf(buffer + used, buffer_size - used, "\n# Tool Inventory Snapshot\n");
    used += (size_t)snprintf(buffer + used, buffer_size - used, "Read-only tools: ");
    {
        bool first = true;

        for (index = 0; index < espclaw_tool_count(); ++index) {
            const espclaw_tool_descriptor_t *tool = espclaw_tool_at(index);

            if (tool == NULL || tool->safety != ESPCLAW_TOOL_SAFETY_READ_ONLY) {
                continue;
            }
            used += (size_t)snprintf(
                buffer + used,
                buffer_size - used,
                "%s%s",
                first ? "" : ", ",
                tool->name
            );
            first = false;
        }
    }
    used += (size_t)snprintf(buffer + used, buffer_size - used, "\nMutating tools: ");
    {
        bool first = true;

        for (index = 0; index < espclaw_tool_count(); ++index) {
            const espclaw_tool_descriptor_t *tool = espclaw_tool_at(index);

            if (tool == NULL || tool->safety != ESPCLAW_TOOL_SAFETY_CONFIRM_REQUIRED) {
                continue;
            }
            used += (size_t)snprintf(
                buffer + used,
                buffer_size - used,
                "%s%s",
                first ? "" : ", ",
                tool->name
            );
            first = false;
        }
    }
    used += (size_t)snprintf(
        buffer + used,
        buffer_size - used,
        "\nUse hardware.list when board-specific pins, buses, or capabilities matter.\n"
        "Use lua_api.list when generating or debugging Lua apps and you need exact espclaw.* signatures or handler rules.\n"
        "Use app_patterns.list when you need to decide whether something should be a component, app, task, behavior, or event.\n"
        "Use component.list before writing a new shared driver or helper, and use component.install or component.install_from_manifest when reusable code should be shared by multiple apps.\n"
        "Use app.install_from_blob or component.install_from_blob when large Lua source was chunk-uploaded through the blob store.\n"
        "Use app.install_from_url or component.install_from_manifest for community-shared artifacts instead of pasting large inline code.\n"
        "Use context.search, context.select, context.summarize, and context.load for large workspace docs instead of trying to stuff an entire file into one prompt turn.\n"
        "Choose the execution shape semantically: one-shot action now => direct tool calls; repeated or timed behavior requested now => execute the full sequence; reusable logic => app.install; run reusable logic now in the background => task.start after app.install; persist or autostart reusable logic => behavior.register after app.install.\n"
        "When the user specifies exact pins, counts, timings, or hardware behavior, preserve those concrete details in the tool calls or generated Lua source. Do not substitute a generic hello, echo, or placeholder app.\n"
        "Do not collapse a repeated or timed request into a single hardware write, and do not answer with hardware narration when the user asked you to create runnable logic.\n"
        "If the operator explicitly tells you to run a tool by name, call that tool in this turn instead of asking them to paste its output.\n"
        "If your previous reply said a named tool still needs to be run, or described a concrete next tool step like emit an event or check task.list, and the next operator turn is an approval like yes/ok/do it/do that/try that/go ahead, treat that as approval to call the missing tool immediately.\n"
        "If the user says this is a tool-call compliance test, says the transcript is audited, or explicitly lists tools you must use, call every applicable listed tool before replying, even if some of those tool calls fail.\n"
    );
    if (user_message_requests_lua_app_contract(user_message)) {
        if (used + 2 < buffer_size) {
            used += (size_t)snprintf(buffer + used, buffer_size - used, "\n");
        }
        used += espclaw_render_app_patterns_prompt_snapshot(buffer + used, buffer_size - used);
        if (used + 2 < buffer_size) {
            used += (size_t)snprintf(buffer + used, buffer_size - used, "\n");
        }
        used += espclaw_render_component_architecture_prompt_snapshot(buffer + used, buffer_size - used);
        if (used + 2 < buffer_size) {
            used += (size_t)snprintf(buffer + used, buffer_size - used, "\n");
        }
        used += espclaw_render_lua_api_prompt_snapshot(buffer + used, buffer_size - used);
    }
    return 0;
}

static int load_history(const char *workspace_root, const char *session_id, espclaw_history_message_t *messages, size_t max_messages, size_t *count_out)
{
    char *transcript = NULL;
    const char *cursor;
    size_t count = 0;
    size_t transcript_capacity = 0;

    if (count_out == NULL) {
        return -1;
    }
    *count_out = 0;
    if (workspace_root == NULL || session_id == NULL || messages == NULL || max_messages == 0) {
        return -1;
    }
    transcript_capacity = max_messages * (sizeof(messages[0].content) + 96U);
    if (transcript_capacity < 4096U) {
        transcript_capacity = 4096U;
    }
    if (transcript_capacity > 16384U) {
        transcript_capacity = 16384U;
    }
    transcript = (char *)calloc(1, transcript_capacity);
    if (transcript == NULL) {
        return -1;
    }
    if (espclaw_session_read_transcript(workspace_root, session_id, transcript, transcript_capacity) != 0) {
        free(transcript);
        return 0;
    }

    cursor = transcript;
    while (*cursor != '\0') {
        const char *line_end = strchr(cursor, '\n');
        char line[1200];
        size_t line_len = line_end != NULL ? (size_t)(line_end - cursor) : strlen(cursor);

        if (line_len >= sizeof(line)) {
            line_len = sizeof(line) - 1;
        }
        memcpy(line, cursor, line_len);
        line[line_len] = '\0';

        if (count < max_messages) {
            if (extract_history_message(line, &messages[count])) {
                count++;
            }
        } else {
            memmove(messages, messages + 1, (max_messages - 1) * sizeof(messages[0]));
            if (extract_history_message(line, &messages[max_messages - 1])) {
                count = max_messages;
            }
        }

        if (line_end == NULL) {
            break;
        }
        cursor = line_end + 1;
    }

    *count_out = count;
    free(transcript);
    return 0;
}

static size_t append_tool_schemas(char *buffer, size_t buffer_size, size_t used)
{
    size_t index;

    used += (size_t)snprintf(buffer + used, buffer_size - used, "[");
    for (index = 0; index < espclaw_tool_count(); ++index) {
        const espclaw_tool_descriptor_t *tool = espclaw_tool_at(index);
        char wire_name[96];

        if (tool == NULL) {
            continue;
        }
        encode_tool_name(tool->name, wire_name, sizeof(wire_name));
        if (index > 0) {
            used += (size_t)snprintf(buffer + used, buffer_size - used, ",");
        }
        used += (size_t)snprintf(
            buffer + used,
            buffer_size - used,
            "{\"type\":\"function\",\"name\":"
        );
        used = append_escaped_json(buffer, buffer_size, used, wire_name);
        used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"description\":");
        used = append_escaped_json(buffer, buffer_size, used, tool->summary);
        used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"parameters\":%s}", tool->parameters_json);
    }
    used += (size_t)snprintf(buffer + used, buffer_size - used, "]");
    return used >= buffer_size ? buffer_size - 1 : used;
}

static size_t append_history_items(
    char *buffer,
    size_t buffer_size,
    size_t used,
    const espclaw_history_message_t *messages,
    size_t count
)
{
    size_t index;

    for (index = 0; index < count; ++index) {
        const char *role = messages[index].role;
        char content[1152];

        if (!history_role_is_supported(role)) {
            continue;
        }

        if (index > 0) {
            used += (size_t)snprintf(buffer + used, buffer_size - used, ",");
        }
        if (strcmp(role, "tool") == 0) {
            role = "user";
            snprintf(content, sizeof(content), "Tool output:\n%s", messages[index].content);
        } else {
            copy_text(content, sizeof(content), messages[index].content);
        }

        used += (size_t)snprintf(buffer + used, buffer_size - used, "{\"role\":");
        used = append_escaped_json(buffer, buffer_size, used, role);
        used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"content\":");
        used = append_escaped_json(buffer, buffer_size, used, content);
        used += (size_t)snprintf(buffer + used, buffer_size - used, "}");
    }
    return used >= buffer_size ? buffer_size - 1 : used;
}

static size_t append_history_input(
    char *buffer,
    size_t buffer_size,
    size_t used,
    const espclaw_history_message_t *messages,
    size_t count
)
{
    used += (size_t)snprintf(buffer + used, buffer_size - used, "[");
    used = append_history_items(buffer, buffer_size, used, messages, count);
    used += (size_t)snprintf(buffer + used, buffer_size - used, "]");
    return used >= buffer_size ? buffer_size - 1 : used;
}

int espclaw_agent_extract_sse_completed_response_json(const char *payload, char *buffer, size_t buffer_size)
{
    const char *event;
    const char *data;
    const char *json_start;
    const char *json_end;
    size_t length;

    if (payload == NULL || buffer == NULL || buffer_size == 0) {
        return -1;
    }

    event = strstr(payload, "event: response.completed");
    if (event == NULL) {
        return -1;
    }

    data = strstr(event, "\ndata: ");
    if (data == NULL) {
        return -1;
    }
    json_start = data + strlen("\ndata: ");
    json_end = strstr(json_start, "\n\n");
    if (json_end == NULL) {
        json_end = json_start + strlen(json_start);
    }

    length = (size_t)(json_end - json_start);
    if (length >= buffer_size) {
        length = buffer_size - 1;
    }
    memmove(buffer, json_start, length);
    buffer[length] = '\0';
    return length > 0 ? 0 : -1;
}

static int build_initial_request_body(
    const espclaw_auth_profile_t *profile,
    const char *instructions,
    const espclaw_history_message_t *history,
    size_t history_count,
    char *buffer,
    size_t buffer_size
)
{
    size_t used = 0;

    if (profile == NULL || instructions == NULL || history == NULL || buffer == NULL || buffer_size == 0) {
        return -1;
    }

    used += (size_t)snprintf(buffer + used, buffer_size - used, "{");
    used += (size_t)snprintf(buffer + used, buffer_size - used, "\"model\":");
    used = append_escaped_json(buffer, buffer_size, used, profile->model);
    used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"instructions\":");
    used = append_escaped_json(buffer, buffer_size, used, instructions);
    // The ChatGPT Codex backend rejects omitted store settings. Keep requests
    // explicit, but prefer non-streamed JSON responses on embedded targets to
    // avoid SSE overhead and large long-lived HTTP header state.
    used += (size_t)snprintf(
        buffer + used,
        buffer_size - used,
        strcmp(profile->provider_id, "openai_codex") == 0
            ? ",\"store\":false,\"stream\":true,\"parallel_tool_calls\":false,\"input\":"
            : ",\"store\":false,\"stream\":false,\"parallel_tool_calls\":false,\"input\":"
    );
    used = append_history_input(buffer, buffer_size, used, history, history_count);
    used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"tools\":");
    used = append_tool_schemas(buffer, buffer_size, used);
    used += (size_t)snprintf(buffer + used, buffer_size - used, "}");

    return 0;
}

static int build_followup_request_body(
    const espclaw_auth_profile_t *profile,
    const char *instructions,
    const char *workspace_root,
    const char *response_id,
    const espclaw_agent_tool_call_t *tool_calls,
    const char *results,
    size_t result_stride,
    size_t tool_count,
    const espclaw_agent_media_ref_t *media_refs,
    size_t media_count,
    size_t image_data_max,
    char *buffer,
    size_t buffer_size
)
{
    size_t index;
    size_t used = 0;

    if (profile == NULL || instructions == NULL || response_id == NULL || buffer == NULL || buffer_size == 0) {
        return -1;
    }

    used += (size_t)snprintf(buffer + used, buffer_size - used, "{");
    used += (size_t)snprintf(buffer + used, buffer_size - used, "\"model\":");
    used = append_escaped_json(buffer, buffer_size, used, profile->model);
    used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"instructions\":");
    used = append_escaped_json(buffer, buffer_size, used, instructions);
    used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"previous_response_id\":");
    used = append_escaped_json(buffer, buffer_size, used, response_id);
    used += (size_t)snprintf(
        buffer + used,
        buffer_size - used,
        strcmp(profile->provider_id, "openai_codex") == 0
            ? ",\"store\":false,\"stream\":true,\"input\":["
            : ",\"store\":false,\"stream\":false,\"input\":["
    );
    for (index = 0; index < tool_count; ++index) {
        if (index > 0) {
            used += (size_t)snprintf(buffer + used, buffer_size - used, ",");
        }
        used += (size_t)snprintf(
            buffer + used,
            buffer_size - used,
            "{\"type\":\"function_call_output\",\"call_id\":"
        );
        used = append_escaped_json(buffer, buffer_size, used, tool_calls[index].call_id);
        used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"output\":");
        used = append_escaped_json(buffer, buffer_size, used, tool_result_at((char *)results, result_stride, index));
        used += (size_t)snprintf(buffer + used, buffer_size - used, "}");
    }
    used = append_input_images(workspace_root, profile->provider_id, buffer, buffer_size, used, media_refs, media_count, image_data_max);
    used += (size_t)snprintf(buffer + used, buffer_size - used, "]}");
    return 0;
}

static size_t append_codex_tool_round_items(
    char *buffer,
    size_t buffer_size,
    size_t used,
    const espclaw_agent_tool_call_t *tool_calls,
    const char *results,
    size_t result_stride,
    size_t tool_count
)
{
    size_t index;

    for (index = 0; index < tool_count; ++index) {
        char wire_name[sizeof(tool_calls[index].name) * 4];

        if (used > 0) {
            used += (size_t)snprintf(buffer + used, buffer_size - used, ",");
        }

        encode_tool_name(tool_calls[index].name, wire_name, sizeof(wire_name));
        used += (size_t)snprintf(
            buffer + used,
            buffer_size - used,
            "{\"type\":\"function_call\",\"call_id\":"
        );
        used = append_escaped_json(buffer, buffer_size, used, tool_calls[index].call_id);
        used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"name\":");
        used = append_escaped_json(buffer, buffer_size, used, wire_name);
        used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"arguments\":");
        used = append_escaped_json(buffer, buffer_size, used, tool_calls[index].arguments_json);
        used += (size_t)snprintf(buffer + used, buffer_size - used, "},");

        used += (size_t)snprintf(
            buffer + used,
            buffer_size - used,
            "{\"type\":\"function_call_output\",\"call_id\":"
        );
        used = append_escaped_json(buffer, buffer_size, used, tool_calls[index].call_id);
        used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"output\":");
        used = append_escaped_json(buffer, buffer_size, used, tool_result_at((char *)results, result_stride, index));
        used += (size_t)snprintf(buffer + used, buffer_size - used, "}");
    }

    return used >= buffer_size ? buffer_size - 1 : used;
}

static int build_codex_followup_request_body(
    const espclaw_auth_profile_t *profile,
    const char *instructions,
    const char *workspace_root,
    const espclaw_history_message_t *history,
    size_t history_count,
    const char *codex_items_json,
    const espclaw_agent_media_ref_t *media_refs,
    size_t media_count,
    size_t image_data_max,
    char *buffer,
    size_t buffer_size
)
{
    size_t used = 0;
    bool has_history = history != NULL && history_count > 0;
    bool has_codex_items = codex_items_json != NULL && codex_items_json[0] != '\0';

    if (profile == NULL || instructions == NULL || buffer == NULL || buffer_size == 0) {
        return -1;
    }

    used += (size_t)snprintf(buffer + used, buffer_size - used, "{");
    used += (size_t)snprintf(buffer + used, buffer_size - used, "\"model\":");
    used = append_escaped_json(buffer, buffer_size, used, profile->model);
    used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"instructions\":");
    used = append_escaped_json(buffer, buffer_size, used, instructions);
    used += (size_t)snprintf(
        buffer + used,
        buffer_size - used,
        strcmp(profile->provider_id, "openai_codex") == 0
            ? ",\"store\":false,\"stream\":true,\"parallel_tool_calls\":false,\"input\":["
            : ",\"store\":false,\"stream\":false,\"parallel_tool_calls\":false,\"input\":["
    );
    if (has_history) {
        used = append_history_items(buffer, buffer_size, used, history, history_count);
    }
    if (has_codex_items) {
        if (has_history) {
            used += (size_t)snprintf(buffer + used, buffer_size - used, ",");
        }
        used += (size_t)snprintf(buffer + used, buffer_size - used, "%s", codex_items_json);
    }
    used = append_input_images(workspace_root, profile->provider_id, buffer, buffer_size, used, media_refs, media_count, image_data_max);
    used += (size_t)snprintf(buffer + used, buffer_size - used, "]");
    used += (size_t)snprintf(buffer + used, buffer_size - used, "}");
    return 0;
}

static void append_text_segment(char *buffer, size_t buffer_size, const char *value)
{
    size_t used = strlen(buffer);

    if (used >= buffer_size - 1) {
        return;
    }
    snprintf(buffer + used, buffer_size - used, "%s", value != NULL ? value : "");
}

static void append_sse_output_text_segments(const char *payload, char *buffer, size_t buffer_size)
{
    const char *cursor = payload;
    bool saw_delta = false;

    if (payload == NULL || buffer == NULL || buffer_size == 0) {
        return;
    }

    while ((cursor = strstr(cursor, "event: response.output_text.")) != NULL) {
        const char *data = strstr(cursor, "\ndata: ");
        const char *event_end;
        char line[1536];
        char segment[1024];
        size_t line_len;
        bool is_delta = strncmp(cursor, "event: response.output_text.delta", strlen("event: response.output_text.delta")) == 0;
        bool is_done = strncmp(cursor, "event: response.output_text.done", strlen("event: response.output_text.done")) == 0;

        if (!is_delta && !is_done) {
            cursor += strlen("event: response.output_text.");
            continue;
        }

        if (data == NULL) {
            break;
        }
        data += strlen("\ndata: ");
        event_end = strstr(data, "\n\n");
        if (event_end == NULL) {
            event_end = data + strlen(data);
        }
        line_len = (size_t)(event_end - data);
        if (line_len >= sizeof(line)) {
            line_len = sizeof(line) - 1;
        }
        memcpy(line, data, line_len);
        line[line_len] = '\0';
        if (is_delta && extract_json_string_from(line, "delta", NULL, segment, sizeof(segment))) {
            append_text_segment(buffer, buffer_size, segment);
            saw_delta = true;
        } else if (is_done && !saw_delta && extract_json_string_from(line, "text", NULL, segment, sizeof(segment))) {
            append_text_segment(buffer, buffer_size, segment);
        }
        cursor = event_end;
    }
}

static void synthesize_text_response_json(
    const char *response_id,
    const char *text,
    char *buffer,
    size_t buffer_size
)
{
    size_t used = 0;

    if (buffer == NULL || buffer_size == 0) {
        return;
    }

    buffer[0] = '\0';
    used = append_format(buffer, buffer_size, used, "{");
    used = append_format(buffer, buffer_size, used, "\"id\":");
    used = append_escaped_json(buffer, buffer_size, used, response_id != NULL ? response_id : "");
    used = append_format(buffer, buffer_size, used, ",\"status\":\"completed\",\"output\":[");
    used = append_format(
        buffer,
        buffer_size,
        used,
        "{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":"
    );
    used = append_escaped_json(buffer, buffer_size, used, text != NULL ? text : "");
    used = append_format(buffer, buffer_size, used, "}]}]}");
    if (used >= buffer_size) {
        buffer[buffer_size - 1] = '\0';
    }
}

int espclaw_agent_store_terminal_response_json(
    const char *source_json,
    size_t source_len,
    char *buffer,
    size_t buffer_size
)
{
    char *temp = NULL;

    if (source_json == NULL || buffer == NULL || buffer_size == 0) {
        return -1;
    }
    if (source_len + 1 > buffer_size) {
        return -1;
    }
    if (source_json == buffer) {
        temp = calloc(1, source_len + 1);
        if (temp == NULL) {
            return -1;
        }
        memcpy(temp, source_json, source_len);
        temp[source_len] = '\0';
        memcpy(buffer, temp, source_len + 1);
        free(temp);
        return 0;
    }

    memcpy(buffer, source_json, source_len);
    buffer[source_len] = '\0';
    return 0;
}

static int parse_provider_response(const char *json, espclaw_provider_response_t *response)
{
    const char *cursor;
    char *text_segment = NULL;

    if (json == NULL || response == NULL) {
        return -1;
    }

    memset(response, 0, sizeof(*response));
    extract_json_string_from(json, "id", NULL, response->id, sizeof(response->id));
    text_segment = (char *)calloc(1, sizeof(response->text));
    if (text_segment == NULL) {
        return -1;
    }

    cursor = json;
    while ((cursor = strstr(cursor, "\"type\":\"output_text\"")) != NULL) {
        if (extract_json_string_from(json, "text", cursor, text_segment, sizeof(response->text))) {
            append_text_segment(response->text, sizeof(response->text), text_segment);
        }
        cursor += strlen("\"type\":\"output_text\"");
    }
    if (response->text[0] == '\0') {
        append_sse_output_text_segments(json, response->text, sizeof(response->text));
    }

    cursor = json;
    while ((cursor = strstr(cursor, "\"type\":\"function_call\"")) != NULL &&
           response->tool_call_count < ESPCLAW_AGENT_TOOL_CALL_MAX) {
        espclaw_agent_tool_call_t *tool_call = &response->tool_calls[response->tool_call_count];
        const char *function_call_start = NULL;
        size_t function_call_length = 0;
        char *function_call_json = NULL;
        char wire_name[sizeof(tool_call->name)];

        if (find_json_object_containing_span(json, cursor, &function_call_start, &function_call_length) &&
            function_call_length > 0 &&
            (function_call_json = (char *)calloc(1, function_call_length + 1U)) != NULL) {
            memcpy(function_call_json, function_call_start, function_call_length);
            function_call_json[function_call_length] = '\0';
        }
        if (function_call_json != NULL &&
            extract_json_string_from(function_call_json, "call_id", NULL, tool_call->call_id, sizeof(tool_call->call_id)) &&
            extract_json_string_from(function_call_json, "name", NULL, wire_name, sizeof(wire_name)) &&
            extract_json_string_from(function_call_json, "arguments", NULL, tool_call->arguments_json, sizeof(tool_call->arguments_json))) {
            decode_tool_name(wire_name, tool_call->name, sizeof(tool_call->name));
            response->tool_call_count++;
        }
        free(function_call_json);
        cursor += strlen("\"type\":\"function_call\"");
    }

    free(text_segment);
    return response->id[0] != '\0' ? 0 : -1;
}

static int parse_provider_text_response(
    const char *json,
    char *response_id,
    size_t response_id_size,
    char *text,
    size_t text_size,
    bool *has_tools_out
)
{
    const char *cursor;
    char *segment = NULL;

    if (json == NULL || response_id == NULL || response_id_size == 0 || text == NULL || text_size == 0) {
        return -1;
    }

    response_id[0] = '\0';
    text[0] = '\0';
    if (has_tools_out != NULL) {
        *has_tools_out = strstr(json, "\"type\":\"function_call\"") != NULL;
    }
    extract_json_string_from(json, "id", NULL, response_id, response_id_size);
    segment = (char *)calloc(1, text_size);
    if (segment == NULL) {
        return -1;
    }

    cursor = json;
    while ((cursor = strstr(cursor, "\"type\":\"output_text\"")) != NULL) {
        if (extract_json_string_from(json, "text", cursor, segment, text_size)) {
            append_text_segment(text, text_size, segment);
        }
        cursor += strlen("\"type\":\"output_text\"");
    }
    if (text[0] == '\0') {
        append_sse_output_text_segments(json, text, text_size);
    }

    if (response_id[0] != '\0' && (text[0] != '\0' || (has_tools_out != NULL && *has_tools_out))) {
        free(segment);
        return 0;
    }
    if (text[0] != '\0' && (has_tools_out == NULL || !*has_tools_out)) {
        free(segment);
        return 0;
    }
    free(segment);
    return -1;
}

static bool is_completed_terminal_response_without_output(
    const char *json,
    char *response_id,
    size_t response_id_size
)
{
    bool has_tools = false;
    char text[32];

    if (json == NULL || response_id == NULL || response_id_size == 0) {
        return false;
    }

    response_id[0] = '\0';
    text[0] = '\0';
    extract_json_string_from(json, "id", NULL, response_id, response_id_size);
    if (response_id[0] == '\0') {
        return false;
    }
    if (strstr(json, "\"status\":\"completed\"") == NULL) {
        return false;
    }
    has_tools = strstr(json, "\"type\":\"function_call\"") != NULL;
    if (has_tools) {
        return false;
    }

    return parse_provider_text_response(
               json,
               response_id,
               response_id_size,
               text,
               sizeof(text),
               &has_tools) != 0;
}

static bool json_argument_string(const char *arguments_json, const char *key, char *buffer, size_t buffer_size)
{
    return extract_json_string_from(arguments_json, key, NULL, buffer, buffer_size);
}

static bool json_argument_string_any(
    const char *arguments_json,
    const char *const *keys,
    size_t key_count,
    char *buffer,
    size_t buffer_size
)
{
    size_t index;

    if (buffer != NULL && buffer_size > 0) {
        buffer[0] = '\0';
    }
    if (keys == NULL) {
        return false;
    }
    for (index = 0; index < key_count; ++index) {
        if (json_argument_string(arguments_json, keys[index], buffer, buffer_size)) {
            return true;
        }
    }
    return false;
}

static bool json_argument_int(const char *arguments_json, const char *key, int *value_out)
{
    const char *cursor = find_json_key_after(arguments_json, key, NULL);
    char *end_ptr = NULL;
    long parsed = 0;

    if (cursor == NULL || value_out == NULL) {
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

    parsed = strtol(cursor, &end_ptr, 10);
    if (end_ptr == cursor) {
        return false;
    }

    *value_out = (int)parsed;
    return true;
}

static bool json_argument_bool(const char *arguments_json, const char *key, bool *value_out)
{
    const char *cursor = find_json_key_after(arguments_json, key, NULL);

    if (cursor == NULL || value_out == NULL) {
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

    if (strncmp(cursor, "true", 4) == 0 || *cursor == '1') {
        *value_out = true;
        return true;
    }
    if (strncmp(cursor, "false", 5) == 0 || *cursor == '0') {
        *value_out = false;
        return true;
    }
    return false;
}

static bool json_argument_double(const char *arguments_json, const char *key, double *value_out)
{
    const char *cursor = find_json_key_after(arguments_json, key, NULL);
    char *end_ptr = NULL;
    double parsed = 0.0;

    if (cursor == NULL || value_out == NULL) {
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

    parsed = strtod(cursor, &end_ptr);
    if (end_ptr == cursor) {
        return false;
    }

    *value_out = parsed;
    return true;
}

static int resolve_default_i2c_bus(int *port_out, int *sda_pin_out, int *scl_pin_out, int *frequency_hz_out)
{
    espclaw_board_i2c_bus_t bus;

    if (port_out == NULL || sda_pin_out == NULL || scl_pin_out == NULL || frequency_hz_out == NULL) {
        return -1;
    }
    if (espclaw_board_find_i2c_bus("default", &bus) != 0) {
        return -1;
    }

    *port_out = bus.port;
    *sda_pin_out = bus.sda_pin;
    *scl_pin_out = bus.scl_pin;
    *frequency_hz_out = bus.frequency_hz;
    return 0;
}

static int ensure_i2c_ready(const char *arguments_json, int *port_out)
{
    int port = 0;
    int sda_pin = -1;
    int scl_pin = -1;
    int frequency_hz = 400000;

    if (port_out == NULL) {
        return -1;
    }
    (void)json_argument_int(arguments_json, "port", &port);
    (void)json_argument_int(arguments_json, "sda_pin", &sda_pin);
    (void)json_argument_int(arguments_json, "scl_pin", &scl_pin);
    (void)json_argument_int(arguments_json, "frequency_hz", &frequency_hz);
    if (sda_pin < 0 || scl_pin < 0) {
        (void)resolve_default_i2c_bus(&port, &sda_pin, &scl_pin, &frequency_hz);
    }
    if (sda_pin < 0 || scl_pin < 0 || espclaw_hw_i2c_begin(port, sda_pin, scl_pin, frequency_hz) != 0) {
        return -1;
    }

    *port_out = port;
    return 0;
}

static bool parse_hex_bytes(const char *text, uint8_t *data, size_t max_length, size_t *length_out)
{
    size_t count = 0;
    const char *cursor = text;

    if (text == NULL || data == NULL || length_out == NULL || max_length == 0) {
        return false;
    }

    while (*cursor != '\0') {
        unsigned int value = 0;
        int consumed = 0;

        while (*cursor != '\0' &&
               (isspace((unsigned char)*cursor) || *cursor == ',' || *cursor == '[' || *cursor == ']')) {
            cursor++;
        }
        if (*cursor == '\0') {
            break;
        }
        if (count >= max_length) {
            return false;
        }
        if (sscanf(cursor, "%x%n", &value, &consumed) != 1 || consumed <= 0 || value > 0xFFU) {
            return false;
        }
        data[count++] = (uint8_t)value;
        cursor += consumed;
    }

    *length_out = count;
    return count > 0;
}

static size_t append_json_hex_string(char *buffer, size_t buffer_size, size_t used, const uint8_t *data, size_t length)
{
    size_t index;

    if (used >= buffer_size) {
        return used;
    }
    buffer[used++] = '"';
    for (index = 0; index < length && used + 3 < buffer_size; ++index) {
        if (index > 0) {
            buffer[used++] = ' ';
        }
        used += (size_t)snprintf(buffer + used, buffer_size - used, "%02X", data[index]);
    }
    if (used < buffer_size) {
        buffer[used++] = '"';
        buffer[used] = '\0';
    }
    return used;
}

static bool normalize_app_id_text(const char *input, char *buffer, size_t buffer_size)
{
    size_t used = 0;
    bool last_was_separator = false;

    if (buffer == NULL || buffer_size == 0) {
        return false;
    }
    buffer[0] = '\0';
    if (input == NULL || input[0] == '\0') {
        return false;
    }

    for (; *input != '\0' && used + 1 < buffer_size; ++input) {
        unsigned char c = (unsigned char)*input;

        if (isalnum(c)) {
            buffer[used++] = (char)tolower(c);
            last_was_separator = false;
            continue;
        }
        if ((c == '-' || c == '_' || isspace(c)) && used > 0 && !last_was_separator) {
            buffer[used++] = '_';
            last_was_separator = true;
        }
    }

    while (used > 0 && buffer[used - 1] == '_') {
        used--;
    }
    buffer[used] = '\0';
    return used > 0 && strlen(buffer) <= ESPCLAW_APP_ID_MAX;
}

static int tool_fs_read(const char *workspace_root, const char *arguments_json, char *buffer, size_t buffer_size)
{
    char path[256];
    char content[1536];

    if (!json_argument_string(arguments_json, "path", path, sizeof(path))) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"missing path\"}");
        return -1;
    }
    if (espclaw_workspace_read_file(workspace_root, path, content, sizeof(content)) != 0) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"failed to read file\"}");
        return -1;
    }

    snprintf(buffer, buffer_size, "{\"ok\":true,\"path\":\"%s\",\"content\":", path);
    append_escaped_json(buffer, buffer_size, strlen(buffer), content);
    append_text_segment(buffer, buffer_size, "}");
    return 0;
}

static int tool_fs_list(const char *workspace_root, const char *arguments_json, char *buffer, size_t buffer_size)
{
    char relative_path[256];
    char absolute_path[512];
    DIR *dir;
    struct dirent *entry;
    size_t used = 0;
    bool first = true;

    if (!json_argument_string(arguments_json, "path", relative_path, sizeof(relative_path))) {
        copy_text(relative_path, sizeof(relative_path), ".");
    }
    if (strcmp(relative_path, ".") == 0) {
        copy_text(absolute_path, sizeof(absolute_path), workspace_root);
    } else if (espclaw_workspace_resolve_path(workspace_root, relative_path, absolute_path, sizeof(absolute_path)) != 0) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"invalid path\"}");
        return -1;
    }

    dir = opendir(absolute_path);
    if (dir == NULL) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"failed to open directory\"}");
        return -1;
    }

    used += (size_t)snprintf(buffer + used, buffer_size - used, "{\"ok\":true,\"path\":");
    used = append_escaped_json(buffer, buffer_size, used, relative_path);
    used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"entries\":[");

    while ((entry = readdir(dir)) != NULL && used + 8 < buffer_size) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (!first) {
            used += (size_t)snprintf(buffer + used, buffer_size - used, ",");
        }
        used = append_escaped_json(buffer, buffer_size, used, entry->d_name);
        first = false;
    }

    closedir(dir);
    used += (size_t)snprintf(buffer + used, buffer_size - used, "]}");
    return 0;
}

static int tool_fs_write(const char *workspace_root, const char *arguments_json, char *buffer, size_t buffer_size)
{
    char path[256];
    char content[2048];

    if (!json_argument_string(arguments_json, "path", path, sizeof(path))) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"missing path\"}");
        return -1;
    }
    if (!json_argument_string(arguments_json, "content", content, sizeof(content))) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"missing content\"}");
        return -1;
    }
    if (espclaw_workspace_write_file(workspace_root, path, content) != 0) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"failed to write file\"}");
        return -1;
    }

    snprintf(buffer, buffer_size, "{\"ok\":true,\"path\":\"%s\",\"bytes_written\":%u}", path, (unsigned)strlen(content));
    return 0;
}

static int tool_fs_delete(const char *workspace_root, const char *arguments_json, char *buffer, size_t buffer_size)
{
    char path[256];
    char absolute_path[512];

    if (!json_argument_string(arguments_json, "path", path, sizeof(path))) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"missing path\"}");
        return -1;
    }
    if (espclaw_workspace_resolve_path(workspace_root, path, absolute_path, sizeof(absolute_path)) != 0) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"invalid path\"}");
        return -1;
    }
    if (unlink(absolute_path) != 0) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"failed to delete file\"}");
        return -1;
    }

    snprintf(buffer, buffer_size, "{\"ok\":true,\"path\":\"%s\",\"deleted\":true}", path);
    return 0;
}

static int tool_system_info(const char *workspace_root, char *buffer, size_t buffer_size)
{
    struct stat statbuf;
    char apps_json[1024];
    bool workspace_ready = workspace_root != NULL && stat(workspace_root, &statbuf) == 0;
    const char *storage_backend = espclaw_storage_describe_workspace_root(workspace_root);

    espclaw_render_apps_json(workspace_root, apps_json, sizeof(apps_json));
    snprintf(
        buffer,
        buffer_size,
        "{\"ok\":true,\"workspace_ready\":%s,\"workspace_root\":\"%s\",\"storage_backend\":\"%s\",\"tool_count\":%u,\"apps\":%s}",
        workspace_ready ? "true" : "false",
        workspace_root != NULL ? workspace_root : "",
        storage_backend,
        (unsigned)espclaw_tool_count(),
        apps_json
    );
    return 0;
}

static int tool_system_logs(const char *arguments_json, char *buffer, size_t buffer_size)
{
    int tail_bytes = 0;

    (void)json_argument_int(arguments_json, "bytes", &tail_bytes);
    return espclaw_log_buffer_render_json((size_t)(tail_bytes > 0 ? tail_bytes : 4096), buffer, buffer_size);
}

static int tool_hardware_list(char *buffer, size_t buffer_size)
{
    return (int)espclaw_render_hardware_json(espclaw_board_current(), buffer, buffer_size);
}

static int tool_lua_api_list(char *buffer, size_t buffer_size)
{
    return (int)espclaw_render_lua_api_json(buffer, buffer_size);
}

static int tool_app_patterns_list(char *buffer, size_t buffer_size)
{
    return (int)espclaw_render_app_patterns_json(buffer, buffer_size);
}

static int tool_component_list(const char *workspace_root, char *buffer, size_t buffer_size)
{
    return espclaw_render_components_json(workspace_root, buffer, buffer_size);
}

static int tool_component_install(const char *workspace_root, const char *arguments_json, char *buffer, size_t buffer_size)
{
    char component_id[ESPCLAW_COMPONENT_ID_MAX + 1];
    char title[ESPCLAW_COMPONENT_TITLE_MAX + 1];
    char module[ESPCLAW_COMPONENT_MODULE_MAX + 1];
    char summary[ESPCLAW_COMPONENT_SUMMARY_MAX + 1];
    char version[ESPCLAW_COMPONENT_VERSION_MAX + 1];
    char source[4096];

    if (!json_argument_string(arguments_json, "component_id", component_id, sizeof(component_id)) ||
        !json_argument_string(arguments_json, "module", module, sizeof(module)) ||
        !json_argument_string(arguments_json, "source", source, sizeof(source))) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"missing component_id, module, or source\"}");
        return -1;
    }
    title[0] = '\0';
    summary[0] = '\0';
    version[0] = '\0';
    (void)json_argument_string(arguments_json, "title", title, sizeof(title));
    (void)json_argument_string(arguments_json, "summary", summary, sizeof(summary));
    (void)json_argument_string(arguments_json, "version", version, sizeof(version));

    if (espclaw_component_install(workspace_root, component_id, title, module, summary, version, source) != 0) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"component install failed\"}");
        return -1;
    }
    return espclaw_render_components_json(workspace_root, buffer, buffer_size);
}

static int tool_component_install_from_file(const char *workspace_root, const char *arguments_json, char *buffer, size_t buffer_size)
{
    char component_id[ESPCLAW_COMPONENT_ID_MAX + 1];
    char title[ESPCLAW_COMPONENT_TITLE_MAX + 1];
    char module[ESPCLAW_COMPONENT_MODULE_MAX + 1];
    char summary[ESPCLAW_COMPONENT_SUMMARY_MAX + 1];
    char version[ESPCLAW_COMPONENT_VERSION_MAX + 1];
    char source_path[256];

    if (!json_argument_string(arguments_json, "component_id", component_id, sizeof(component_id)) ||
        !json_argument_string(arguments_json, "module", module, sizeof(module)) ||
        !json_argument_string_any(arguments_json, (const char *[]){"source_path", "path"}, 2, source_path, sizeof(source_path))) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"missing component_id, module, or source_path\"}");
        return -1;
    }

    title[0] = '\0';
    summary[0] = '\0';
    version[0] = '\0';
    (void)json_argument_string(arguments_json, "title", title, sizeof(title));
    (void)json_argument_string(arguments_json, "summary", summary, sizeof(summary));
    (void)json_argument_string(arguments_json, "version", version, sizeof(version));

    if (espclaw_component_install_from_file(workspace_root, component_id, title, module, summary, version, source_path) != 0) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"component install from file failed\"}");
        return -1;
    }
    return espclaw_render_components_json(workspace_root, buffer, buffer_size);
}

static int tool_component_install_from_blob(const char *workspace_root, const char *arguments_json, char *buffer, size_t buffer_size)
{
    char component_id[ESPCLAW_COMPONENT_ID_MAX + 1];
    char title[ESPCLAW_COMPONENT_TITLE_MAX + 1];
    char module[ESPCLAW_COMPONENT_MODULE_MAX + 1];
    char summary[ESPCLAW_COMPONENT_SUMMARY_MAX + 1];
    char version[ESPCLAW_COMPONENT_VERSION_MAX + 1];
    char blob_id[65];

    if (!json_argument_string(arguments_json, "component_id", component_id, sizeof(component_id)) ||
        !json_argument_string(arguments_json, "module", module, sizeof(module)) ||
        !json_argument_string(arguments_json, "blob_id", blob_id, sizeof(blob_id))) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"missing component_id, module, or blob_id\"}");
        return -1;
    }

    title[0] = '\0';
    summary[0] = '\0';
    version[0] = '\0';
    (void)json_argument_string(arguments_json, "title", title, sizeof(title));
    (void)json_argument_string(arguments_json, "summary", summary, sizeof(summary));
    (void)json_argument_string(arguments_json, "version", version, sizeof(version));

    if (espclaw_component_install_from_blob(workspace_root, component_id, title, module, summary, version, blob_id) != 0) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"component install from blob failed\"}");
        return -1;
    }
    return espclaw_render_components_json(workspace_root, buffer, buffer_size);
}

static int tool_component_install_from_url(const char *workspace_root, const char *arguments_json, char *buffer, size_t buffer_size)
{
    char component_id[ESPCLAW_COMPONENT_ID_MAX + 1];
    char title[ESPCLAW_COMPONENT_TITLE_MAX + 1];
    char module[ESPCLAW_COMPONENT_MODULE_MAX + 1];
    char summary[ESPCLAW_COMPONENT_SUMMARY_MAX + 1];
    char version[ESPCLAW_COMPONENT_VERSION_MAX + 1];
    char source_url[512];

    if (!json_argument_string(arguments_json, "component_id", component_id, sizeof(component_id)) ||
        !json_argument_string(arguments_json, "module", module, sizeof(module)) ||
        !json_argument_string_any(arguments_json, (const char *[]){"source_url", "url"}, 2, source_url, sizeof(source_url))) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"missing component_id, module, or source_url\"}");
        return -1;
    }

    title[0] = '\0';
    summary[0] = '\0';
    version[0] = '\0';
    (void)json_argument_string(arguments_json, "title", title, sizeof(title));
    (void)json_argument_string(arguments_json, "summary", summary, sizeof(summary));
    (void)json_argument_string(arguments_json, "version", version, sizeof(version));

    if (espclaw_component_install_from_url(workspace_root, component_id, title, module, summary, version, source_url) != 0) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"component install from url failed\"}");
        return -1;
    }
    return espclaw_render_components_json(workspace_root, buffer, buffer_size);
}

static int tool_component_install_from_manifest(const char *workspace_root, const char *arguments_json, char *buffer, size_t buffer_size)
{
    char manifest_url[512];

    if (!json_argument_string_any(arguments_json, (const char *[]){"manifest_url", "url"}, 2, manifest_url, sizeof(manifest_url))) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"missing manifest_url\"}");
        return -1;
    }
    if (espclaw_component_install_from_manifest(workspace_root, manifest_url) != 0) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"component install from manifest failed\"}");
        return -1;
    }
    return espclaw_render_components_json(workspace_root, buffer, buffer_size);
}

static int tool_component_remove(const char *workspace_root, const char *arguments_json, char *buffer, size_t buffer_size)
{
    char component_id[ESPCLAW_COMPONENT_ID_MAX + 1];

    if (!json_argument_string(arguments_json, "component_id", component_id, sizeof(component_id))) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"missing component_id\"}");
        return -1;
    }
    if (espclaw_component_remove(workspace_root, component_id) != 0) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"component remove failed\"}");
        return -1;
    }
    return espclaw_render_components_json(workspace_root, buffer, buffer_size);
}

static int tool_web_search(const char *arguments_json, char *buffer, size_t buffer_size)
{
    char query[256];

    if (!json_argument_string(arguments_json, "query", query, sizeof(query)) || query[0] == '\0') {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"missing query\"}");
        return -1;
    }
    if (espclaw_web_search(query, buffer, buffer_size) != 0) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"web search failed\"}");
        return -1;
    }
    return 0;
}

static int tool_context_chunks(const char *workspace_root, const char *arguments_json, char *buffer, size_t buffer_size)
{
    char path[ESPCLAW_CONTEXT_PATH_MAX + 1];
    int chunk_bytes = 0;

    if (!json_argument_string_any(arguments_json, (const char *[]){"path", "source_path"}, 2, path, sizeof(path))) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"missing path\"}");
        return -1;
    }
    (void)json_argument_int(arguments_json, "chunk_bytes", &chunk_bytes);
    return espclaw_context_render_chunks_json(workspace_root, path, (size_t)(chunk_bytes > 0 ? chunk_bytes : 0), buffer, buffer_size);
}

static int tool_context_load(const char *workspace_root, const char *arguments_json, char *buffer, size_t buffer_size)
{
    char path[ESPCLAW_CONTEXT_PATH_MAX + 1];
    int chunk_index = -1;
    int chunk_bytes = 0;

    if (!json_argument_string_any(arguments_json, (const char *[]){"path", "source_path"}, 2, path, sizeof(path)) ||
        !json_argument_int(arguments_json, "chunk_index", &chunk_index) ||
        chunk_index < 0) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"missing path or chunk_index\"}");
        return -1;
    }
    (void)json_argument_int(arguments_json, "chunk_bytes", &chunk_bytes);
    return espclaw_context_render_chunk_json(
        workspace_root,
        path,
        (size_t)chunk_index,
        (size_t)(chunk_bytes > 0 ? chunk_bytes : 0),
        buffer,
        buffer_size
    );
}

static int tool_context_search(const char *workspace_root, const char *arguments_json, char *buffer, size_t buffer_size)
{
    char path[ESPCLAW_CONTEXT_PATH_MAX + 1];
    char query[ESPCLAW_CONTEXT_QUERY_MAX + 1];
    int chunk_bytes = 0;
    int limit = 0;

    if (!json_argument_string_any(arguments_json, (const char *[]){"path", "source_path"}, 2, path, sizeof(path)) ||
        !json_argument_string(arguments_json, "query", query, sizeof(query))) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"missing path or query\"}");
        return -1;
    }
    (void)json_argument_int(arguments_json, "chunk_bytes", &chunk_bytes);
    (void)json_argument_int(arguments_json, "limit", &limit);
    return espclaw_context_search_json(
        workspace_root,
        path,
        query,
        (size_t)(chunk_bytes > 0 ? chunk_bytes : 0),
        (size_t)(limit > 0 ? limit : 0),
        buffer,
        buffer_size
    );
}

static int tool_context_select(const char *workspace_root, const char *arguments_json, char *buffer, size_t buffer_size)
{
    char path[ESPCLAW_CONTEXT_PATH_MAX + 1];
    char query[ESPCLAW_CONTEXT_QUERY_MAX + 1];
    int chunk_bytes = 0;
    int limit = 0;
    int output_bytes = 0;

    if (!json_argument_string_any(arguments_json, (const char *[]){"path", "source_path"}, 2, path, sizeof(path)) ||
        !json_argument_string(arguments_json, "query", query, sizeof(query))) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"missing path or query\"}");
        return -1;
    }
    (void)json_argument_int(arguments_json, "chunk_bytes", &chunk_bytes);
    (void)json_argument_int(arguments_json, "limit", &limit);
    (void)json_argument_int(arguments_json, "output_bytes", &output_bytes);
    return espclaw_context_select_json(
        workspace_root,
        path,
        query,
        (size_t)(chunk_bytes > 0 ? chunk_bytes : 0),
        (size_t)(limit > 0 ? limit : 0),
        (size_t)(output_bytes > 0 ? output_bytes : 0),
        buffer,
        buffer_size
    );
}

static int tool_context_summarize(const char *workspace_root, const char *arguments_json, char *buffer, size_t buffer_size)
{
    char path[ESPCLAW_CONTEXT_PATH_MAX + 1];
    char query[ESPCLAW_CONTEXT_QUERY_MAX + 1];
    int chunk_bytes = 0;
    int limit = 0;
    int summary_bytes = 0;

    if (!json_argument_string_any(arguments_json, (const char *[]){"path", "source_path"}, 2, path, sizeof(path)) ||
        !json_argument_string(arguments_json, "query", query, sizeof(query))) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"missing path or query\"}");
        return -1;
    }
    (void)json_argument_int(arguments_json, "chunk_bytes", &chunk_bytes);
    (void)json_argument_int(arguments_json, "limit", &limit);
    (void)json_argument_int(arguments_json, "summary_bytes", &summary_bytes);
    return espclaw_context_summarize_json(
        workspace_root,
        path,
        query,
        (size_t)(chunk_bytes > 0 ? chunk_bytes : 0),
        (size_t)(limit > 0 ? limit : 0),
        (size_t)(summary_bytes > 0 ? summary_bytes : 0),
        buffer,
        buffer_size
    );
}

static int tool_web_fetch(const char *workspace_root, const char *arguments_json, char *buffer, size_t buffer_size)
{
    char url[512];

    if (!json_argument_string(arguments_json, "url", url, sizeof(url)) || url[0] == '\0') {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"missing url\"}");
        return -1;
    }
    if (espclaw_web_fetch(workspace_root, url, buffer, buffer_size) != 0) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"web fetch failed\"}");
        return -1;
    }
    return 0;
}

static int tool_list_tools(char *buffer, size_t buffer_size)
{
    espclaw_render_tools_json(buffer, buffer_size);
    return 0;
}

static int tool_wifi_status(char *buffer, size_t buffer_size)
{
#ifdef ESP_PLATFORM
    const espclaw_runtime_status_t *status = espclaw_runtime_status();

    if (status == NULL) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"runtime unavailable\"}");
        return -1;
    }

    snprintf(
        buffer,
        buffer_size,
        "{\"ok\":true,\"connected\":%s,\"mode\":",
        status->wifi_ready ? "true" : "false"
    );
    append_escaped_json(buffer, buffer_size, strlen(buffer), status->wifi_ready ? "sta" : (status->provisioning_active ? "provisioning" : "offline"));
    append_text_segment(buffer, buffer_size, ",\"ssid\":");
    append_escaped_json(buffer, buffer_size, strlen(buffer), status->wifi_ssid);
    append_text_segment(buffer, buffer_size, "}");
    return 0;
#else
    snprintf(buffer, buffer_size, "{\"ok\":true,\"connected\":false,\"mode\":\"simulated\",\"ssid\":\"\"}");
    return 0;
#endif
}

static int tool_wifi_scan(char *buffer, size_t buffer_size)
{
#ifdef ESP_PLATFORM
    espclaw_wifi_network_t networks[16];
    size_t count = 0;
    size_t used = 0;
    size_t index;

    if (espclaw_runtime_wifi_scan(networks, sizeof(networks) / sizeof(networks[0]), &count) != ESP_OK) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"wifi scan failed\"}");
        return -1;
    }

    used += (size_t)snprintf(buffer + used, buffer_size - used, "{\"ok\":true,\"networks\":[");
    for (index = 0; index < count && used + 32 < buffer_size; ++index) {
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
    return 0;
#else
    snprintf(buffer, buffer_size, "{\"ok\":true,\"networks\":[]}");
    return 0;
#endif
}

static int tool_ble_scan(char *buffer, size_t buffer_size)
{
    snprintf(buffer, buffer_size, "{\"ok\":true,\"supported\":false,\"devices\":[]}");
    return 0;
}

static int tool_gpio_read(const char *arguments_json, char *buffer, size_t buffer_size)
{
    int pin = -1;
    int value = 0;

    if (!json_argument_int(arguments_json, "pin", &pin)) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"missing pin\"}");
        return -1;
    }
    if (espclaw_hw_gpio_read(pin, &value) != 0) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"gpio read failed\"}");
        return -1;
    }

    snprintf(buffer, buffer_size, "{\"ok\":true,\"pin\":%d,\"value\":%d}", pin, value);
    return 0;
}

static int tool_gpio_write(const char *arguments_json, char *buffer, size_t buffer_size)
{
    int pin = -1;
    int value = 0;

    if (!json_argument_int(arguments_json, "pin", &pin) || !json_argument_int(arguments_json, "value", &value)) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"missing pin or value\"}");
        return -1;
    }
    if (espclaw_hw_gpio_write(pin, value != 0 ? 1 : 0) != 0) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"gpio write failed\"}");
        return -1;
    }

    snprintf(buffer, buffer_size, "{\"ok\":true,\"pin\":%d,\"value\":%d}", pin, value != 0 ? 1 : 0);
    return 0;
}

static int tool_pwm_write(const char *arguments_json, char *buffer, size_t buffer_size)
{
    int channel = 0;
    int duty = -1;
    int pin = -1;
    int frequency_hz = 4000;
    int resolution_bits = 10;
    int pulse_width_us = -1;
    espclaw_hw_pwm_state_t state;

    if (!json_argument_int(arguments_json, "channel", &channel)) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"missing channel\"}");
        return -1;
    }
    (void)json_argument_int(arguments_json, "pin", &pin);
    (void)json_argument_int(arguments_json, "frequency_hz", &frequency_hz);
    (void)json_argument_int(arguments_json, "resolution_bits", &resolution_bits);
    (void)json_argument_int(arguments_json, "pulse_width_us", &pulse_width_us);
    (void)json_argument_int(arguments_json, "duty", &duty);
    if (pin < 0) {
        (void)espclaw_board_resolve_pin_alias("flash_led", &pin);
    }
    if (espclaw_hw_pwm_state(channel, &state) != 0 || !state.configured) {
        if (pin < 0 || espclaw_hw_pwm_setup(channel, pin, frequency_hz, resolution_bits) != 0) {
            snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"pwm setup failed\"}");
            return -1;
        }
    }
    if (pulse_width_us > 0) {
        if (espclaw_hw_pwm_write_us(channel, pulse_width_us) != 0) {
            snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"pwm pulse write failed\"}");
            return -1;
        }
    } else if (duty >= 0) {
        if (espclaw_hw_pwm_write(channel, duty) != 0) {
            snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"pwm duty write failed\"}");
            return -1;
        }
    } else {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"missing duty or pulse_width_us\"}");
        return -1;
    }
    if (espclaw_hw_pwm_state(channel, &state) != 0) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"pwm state failed\"}");
        return -1;
    }

    snprintf(
        buffer,
        buffer_size,
        "{\"ok\":true,\"channel\":%d,\"pin\":%d,\"duty\":%d,\"pulse_width_us\":%d,\"frequency_hz\":%d}",
        channel,
        state.pin,
        state.duty,
        state.pulse_width_us,
        state.frequency_hz
    );
    return 0;
}

static int tool_ppm_write(const char *arguments_json, char *buffer, size_t buffer_size)
{
    int channel = 0;
    int pin = -1;
    int value_us = 1500;
    int frame_us = 20000;
    int pulse_us = 300;
    uint16_t outputs[1];
    espclaw_hw_ppm_state_t state;

    if (!json_argument_int(arguments_json, "channel", &channel)) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"missing channel\"}");
        return -1;
    }
    if (!json_argument_int(arguments_json, "value_us", &value_us)) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"missing value_us\"}");
        return -1;
    }
    (void)json_argument_int(arguments_json, "pin", &pin);
    (void)json_argument_int(arguments_json, "frame_us", &frame_us);
    (void)json_argument_int(arguments_json, "pulse_us", &pulse_us);
    if (pin < 0) {
        (void)espclaw_board_resolve_pin_alias("flash_led", &pin);
    }
    if (espclaw_hw_ppm_state(channel, &state) != 0 || !state.configured) {
        if (pin < 0 || espclaw_hw_ppm_begin(channel, pin, frame_us, pulse_us) != 0) {
            snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"ppm setup failed\"}");
            return -1;
        }
    }
    outputs[0] = (uint16_t)value_us;
    if (espclaw_hw_ppm_write(channel, outputs, 1) != 0 || espclaw_hw_ppm_state(channel, &state) != 0) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"ppm write failed\"}");
        return -1;
    }

    snprintf(buffer, buffer_size, "{\"ok\":true,\"channel\":%d,\"pin\":%d,\"value_us\":%u}", channel, state.pin, (unsigned)state.outputs[0]);
    return 0;
}

static int tool_adc_read(const char *arguments_json, char *buffer, size_t buffer_size)
{
    int unit = 1;
    int channel = -1;
    int raw = 0;
    int millivolts = 0;
    char name[ESPCLAW_BOARD_ALIAS_MAX + 1];

    (void)json_argument_int(arguments_json, "unit", &unit);
    if (!json_argument_int(arguments_json, "channel", &channel)) {
        espclaw_board_adc_channel_t board_channel;

        if (!json_argument_string(arguments_json, "name", name, sizeof(name)) ||
            espclaw_board_find_adc_channel(name, &board_channel) != 0) {
            snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"missing channel\"}");
            return -1;
        }
        unit = board_channel.unit;
        channel = board_channel.channel;
    }
    if (espclaw_hw_adc_read_raw(unit, channel, &raw) != 0 || espclaw_hw_adc_read_mv(unit, channel, &millivolts) != 0) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"adc read failed\"}");
        return -1;
    }

    snprintf(buffer, buffer_size, "{\"ok\":true,\"unit\":%d,\"channel\":%d,\"raw\":%d,\"millivolts\":%d}", unit, channel, raw, millivolts);
    return 0;
}

static int tool_app_list(const char *workspace_root, char *buffer, size_t buffer_size)
{
    espclaw_render_apps_json(workspace_root, buffer, buffer_size);
    return 0;
}

static int tool_app_run(const char *workspace_root, const char *arguments_json, char *buffer, size_t buffer_size)
{
    char app_id[ESPCLAW_APP_ID_MAX + 1];
    char trigger[ESPCLAW_APP_TRIGGER_NAME_MAX + 1];
    char payload[512];
    char result[1024];

    if (!json_argument_string(arguments_json, "app_id", app_id, sizeof(app_id))) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"missing app_id\"}");
        return -1;
    }
    if (!json_argument_string(arguments_json, "trigger", trigger, sizeof(trigger))) {
        copy_text(trigger, sizeof(trigger), "manual");
    }
    if (!json_argument_string(arguments_json, "payload", payload, sizeof(payload))) {
        payload[0] = '\0';
    }

    if (espclaw_app_run(workspace_root, app_id, trigger, payload, result, sizeof(result)) != 0) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"app run failed\"}");
        return -1;
    }

    snprintf(buffer, buffer_size, "{\"ok\":true,\"app_id\":\"%s\",\"result\":", app_id);
    append_escaped_json(buffer, buffer_size, strlen(buffer), result);
    append_text_segment(buffer, buffer_size, "}");
    return 0;
}

static int tool_app_install(const char *workspace_root, const char *arguments_json, char *buffer, size_t buffer_size)
{
    char app_id[ESPCLAW_APP_ID_MAX + 1];
    char raw_name[ESPCLAW_APP_TITLE_MAX + 1];
    char *source = NULL;
    char title[ESPCLAW_APP_TITLE_MAX + 1];
    char permissions[256];
    char triggers[128];
    char result[1024];
    char existing[64];
    bool app_exists = false;

    raw_name[0] = '\0';
    source = (char *)calloc(1, ESPCLAW_AGENT_TOOL_ARGS_MAX + 1U);
    if (source == NULL) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"out of memory\"}");
        return -1;
    }
    if (!json_argument_string_any(arguments_json, (const char *[]){"app_id", "name", "title"}, 3, raw_name, sizeof(raw_name))) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"missing app_id\"}");
        free(source);
        return -1;
    }
    if (!espclaw_app_id_is_valid(raw_name)) {
        if (!normalize_app_id_text(raw_name, app_id, sizeof(app_id))) {
            snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"invalid app_id\"}");
            free(source);
            return -1;
        }
    } else {
        copy_text(app_id, sizeof(app_id), raw_name);
    }
    if (!json_argument_string_any(arguments_json, (const char *[]){"source", "lua", "code"}, 3, source, ESPCLAW_AGENT_TOOL_ARGS_MAX + 1U)) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"missing source\"}");
        free(source);
        return -1;
    }
    if (!json_argument_string_any(arguments_json, (const char *[]){"title", "name", "app_id"}, 3, title, sizeof(title))) {
        copy_text(title, sizeof(title), raw_name);
    }
    if (!json_argument_string_any(arguments_json, (const char *[]){"permissions_csv", "permissions"}, 2, permissions, sizeof(permissions))) {
        copy_text(
            permissions,
            sizeof(permissions),
            "fs.read,fs.write,gpio.read,gpio.write,pwm.write,adc.read,i2c.read,i2c.write,uart.read,uart.write,camera.capture,temperature.read,imu.read,buzzer.play,ppm.write,task.control"
        );
    }
    if (!json_argument_string_any(arguments_json, (const char *[]){"triggers_csv", "triggers"}, 2, triggers, sizeof(triggers))) {
        copy_text(triggers, sizeof(triggers), "manual,boot,timer,uart,sensor");
    }

#ifdef ESP_PLATFORM
    ESP_LOGI(
        TAG,
        "app.install request raw_name=%s app_id=%s title_len=%u permissions_len=%u triggers_len=%u source_len=%u",
        raw_name,
        app_id,
        (unsigned)strlen(title),
        (unsigned)strlen(permissions),
        (unsigned)strlen(triggers),
        (unsigned)strlen(source)
    );
#endif

    app_exists = espclaw_app_read_source(workspace_root, app_id, existing, sizeof(existing)) == 0;
    if (!app_exists &&
        espclaw_app_scaffold_lua(workspace_root, app_id, title, permissions, triggers) != 0) {
        snprintf(
            buffer,
            buffer_size,
            "{\"ok\":false,\"error\":\"failed to scaffold app\",\"raw_name\":"
        );
        append_escaped_json(buffer, buffer_size, strlen(buffer), raw_name);
        append_text_segment(buffer, buffer_size, ",\"app_id\":");
        append_escaped_json(buffer, buffer_size, strlen(buffer), app_id);
        append_text_segment(buffer, buffer_size, "}");
        free(source);
        return -1;
    }
    if (espclaw_app_update_source(workspace_root, app_id, source) != 0) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"failed to save source\"}");
        free(source);
        return -1;
    }
    if (espclaw_app_run(workspace_root, app_id, "manual", "", result, sizeof(result)) != 0) {
        snprintf(buffer, buffer_size, "{\"ok\":true,\"app_id\":\"%s\",\"saved\":true,\"validated\":false}", app_id);
        free(source);
        return 0;
    }

    snprintf(buffer, buffer_size, "{\"ok\":true,\"app_id\":\"%s\",\"saved\":true,\"validated\":true,\"result\":", app_id);
    append_escaped_json(buffer, buffer_size, strlen(buffer), result);
    append_text_segment(buffer, buffer_size, "}");
    free(source);
    return 0;
}

static int tool_app_install_from_file(const char *workspace_root, const char *arguments_json, char *buffer, size_t buffer_size)
{
    char app_id[ESPCLAW_APP_ID_MAX + 1];
    char raw_name[ESPCLAW_APP_TITLE_MAX + 1];
    char title[ESPCLAW_APP_TITLE_MAX + 1];
    char permissions[256];
    char triggers[128];
    char source_path[256];
    char result[1024];

    raw_name[0] = '\0';
    if (!json_argument_string_any(arguments_json, (const char *[]){"app_id", "name", "title"}, 3, raw_name, sizeof(raw_name))) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"missing app_id\"}");
        return -1;
    }
    if (!espclaw_app_id_is_valid(raw_name)) {
        if (!normalize_app_id_text(raw_name, app_id, sizeof(app_id))) {
            snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"invalid app_id\"}");
            return -1;
        }
    } else {
        copy_text(app_id, sizeof(app_id), raw_name);
    }
    if (!json_argument_string_any(arguments_json, (const char *[]){"source_path", "path"}, 2, source_path, sizeof(source_path))) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"missing source_path\"}");
        return -1;
    }
    if (!json_argument_string_any(arguments_json, (const char *[]){"title", "name", "app_id"}, 3, title, sizeof(title))) {
        copy_text(title, sizeof(title), raw_name);
    }
    if (!json_argument_string_any(arguments_json, (const char *[]){"permissions_csv", "permissions"}, 2, permissions, sizeof(permissions))) {
        copy_text(
            permissions,
            sizeof(permissions),
            "fs.read,fs.write,gpio.read,gpio.write,pwm.write,adc.read,i2c.read,i2c.write,uart.read,uart.write,camera.capture,temperature.read,imu.read,buzzer.play,ppm.write,task.control"
        );
    }
    if (!json_argument_string_any(arguments_json, (const char *[]){"triggers_csv", "triggers"}, 2, triggers, sizeof(triggers))) {
        copy_text(triggers, sizeof(triggers), "manual,boot,timer,uart,sensor");
    }
    if (espclaw_app_install_from_file(workspace_root, app_id, title, permissions, triggers, source_path) != 0) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"failed to install app from file\"}");
        return -1;
    }
    if (espclaw_app_run(workspace_root, app_id, "manual", "", result, sizeof(result)) != 0) {
        snprintf(buffer, buffer_size, "{\"ok\":true,\"app_id\":\"%s\",\"saved\":true,\"validated\":false}", app_id);
        return 0;
    }

    snprintf(buffer, buffer_size, "{\"ok\":true,\"app_id\":\"%s\",\"saved\":true,\"validated\":true,\"result\":", app_id);
    append_escaped_json(buffer, buffer_size, strlen(buffer), result);
    append_text_segment(buffer, buffer_size, "}");
    return 0;
}

static int tool_app_install_from_blob(const char *workspace_root, const char *arguments_json, char *buffer, size_t buffer_size)
{
    char app_id[ESPCLAW_APP_ID_MAX + 1];
    char raw_name[ESPCLAW_APP_TITLE_MAX + 1];
    char title[ESPCLAW_APP_TITLE_MAX + 1];
    char permissions[256];
    char triggers[128];
    char blob_id[65];
    char result[1024];

    raw_name[0] = '\0';
    if (!json_argument_string_any(arguments_json, (const char *[]){"app_id", "name", "title"}, 3, raw_name, sizeof(raw_name))) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"missing app_id\"}");
        return -1;
    }
    if (!espclaw_app_id_is_valid(raw_name)) {
        if (!normalize_app_id_text(raw_name, app_id, sizeof(app_id))) {
            snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"invalid app_id\"}");
            return -1;
        }
    } else {
        copy_text(app_id, sizeof(app_id), raw_name);
    }
    if (!json_argument_string(arguments_json, "blob_id", blob_id, sizeof(blob_id))) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"missing blob_id\"}");
        return -1;
    }
    if (!json_argument_string_any(arguments_json, (const char *[]){"title", "name", "app_id"}, 3, title, sizeof(title))) {
        copy_text(title, sizeof(title), raw_name);
    }
    if (!json_argument_string_any(arguments_json, (const char *[]){"permissions_csv", "permissions"}, 2, permissions, sizeof(permissions))) {
        copy_text(
            permissions,
            sizeof(permissions),
            "fs.read,fs.write,gpio.read,gpio.write,pwm.write,adc.read,i2c.read,i2c.write,uart.read,uart.write,camera.capture,temperature.read,imu.read,buzzer.play,ppm.write,task.control"
        );
    }
    if (!json_argument_string_any(arguments_json, (const char *[]){"triggers_csv", "triggers"}, 2, triggers, sizeof(triggers))) {
        copy_text(triggers, sizeof(triggers), "manual,boot,timer,uart,sensor");
    }
    if (espclaw_app_install_from_blob(workspace_root, app_id, title, permissions, triggers, blob_id) != 0) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"failed to install app from blob\"}");
        return -1;
    }
    if (espclaw_app_run(workspace_root, app_id, "manual", "", result, sizeof(result)) != 0) {
        snprintf(buffer, buffer_size, "{\"ok\":true,\"app_id\":\"%s\",\"saved\":true,\"validated\":false}", app_id);
        return 0;
    }

    snprintf(buffer, buffer_size, "{\"ok\":true,\"app_id\":\"%s\",\"saved\":true,\"validated\":true,\"result\":", app_id);
    append_escaped_json(buffer, buffer_size, strlen(buffer), result);
    append_text_segment(buffer, buffer_size, "}");
    return 0;
}

static int tool_app_install_from_url(const char *workspace_root, const char *arguments_json, char *buffer, size_t buffer_size)
{
    char app_id[ESPCLAW_APP_ID_MAX + 1];
    char raw_name[ESPCLAW_APP_TITLE_MAX + 1];
    char title[ESPCLAW_APP_TITLE_MAX + 1];
    char permissions[256];
    char triggers[128];
    char source_url[512];
    char result[1024];

    raw_name[0] = '\0';
    if (!json_argument_string_any(arguments_json, (const char *[]){"app_id", "name", "title"}, 3, raw_name, sizeof(raw_name))) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"missing app_id\"}");
        return -1;
    }
    if (!espclaw_app_id_is_valid(raw_name)) {
        if (!normalize_app_id_text(raw_name, app_id, sizeof(app_id))) {
            snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"invalid app_id\"}");
            return -1;
        }
    } else {
        copy_text(app_id, sizeof(app_id), raw_name);
    }
    if (!json_argument_string_any(arguments_json, (const char *[]){"source_url", "url"}, 2, source_url, sizeof(source_url))) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"missing source_url\"}");
        return -1;
    }
    if (!json_argument_string_any(arguments_json, (const char *[]){"title", "name", "app_id"}, 3, title, sizeof(title))) {
        copy_text(title, sizeof(title), raw_name);
    }
    if (!json_argument_string_any(arguments_json, (const char *[]){"permissions_csv", "permissions"}, 2, permissions, sizeof(permissions))) {
        copy_text(
            permissions,
            sizeof(permissions),
            "fs.read,fs.write,gpio.read,gpio.write,pwm.write,adc.read,i2c.read,i2c.write,uart.read,uart.write,camera.capture,temperature.read,imu.read,buzzer.play,ppm.write,task.control"
        );
    }
    if (!json_argument_string_any(arguments_json, (const char *[]){"triggers_csv", "triggers"}, 2, triggers, sizeof(triggers))) {
        copy_text(triggers, sizeof(triggers), "manual,boot,timer,uart,sensor");
    }
    if (espclaw_app_install_from_url(workspace_root, app_id, title, permissions, triggers, source_url) != 0) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"failed to install app from url\"}");
        return -1;
    }
    if (espclaw_app_run(workspace_root, app_id, "manual", "", result, sizeof(result)) != 0) {
        snprintf(buffer, buffer_size, "{\"ok\":true,\"app_id\":\"%s\",\"saved\":true,\"validated\":false}", app_id);
        return 0;
    }

    snprintf(buffer, buffer_size, "{\"ok\":true,\"app_id\":\"%s\",\"saved\":true,\"validated\":true,\"result\":", app_id);
    append_escaped_json(buffer, buffer_size, strlen(buffer), result);
    append_text_segment(buffer, buffer_size, "}");
    return 0;
}

static int tool_app_remove(const char *workspace_root, const char *arguments_json, char *buffer, size_t buffer_size)
{
    char app_id[ESPCLAW_APP_ID_MAX + 1];

    if (!json_argument_string(arguments_json, "app_id", app_id, sizeof(app_id))) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"missing app_id\"}");
        return -1;
    }
    if (espclaw_app_remove(workspace_root, app_id) != 0) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"failed to remove app\"}");
        return -1;
    }

    snprintf(buffer, buffer_size, "{\"ok\":true,\"app_id\":\"%s\",\"removed\":true}", app_id);
    return 0;
}

static int tool_task_list(char *buffer, size_t buffer_size)
{
    return espclaw_task_render_json(buffer, buffer_size);
}

static int tool_behavior_list(const char *workspace_root, char *buffer, size_t buffer_size)
{
    return espclaw_behavior_render_json(workspace_root, buffer, buffer_size);
}

static int tool_task_start(const char *workspace_root, const char *arguments_json, char *buffer, size_t buffer_size)
{
    char task_id[ESPCLAW_TASK_ID_MAX + 1];
    char app_id[ESPCLAW_APP_ID_MAX + 1];
    char schedule[ESPCLAW_TASK_SCHEDULE_MAX + 1];
    char trigger[ESPCLAW_TASK_TRIGGER_MAX + 1];
    char payload[ESPCLAW_TASK_PAYLOAD_MAX];
    char message[256];
    int period_ms = 20;
    int iterations = 0;

    if (!json_argument_string(arguments_json, "task_id", task_id, sizeof(task_id)) ||
        !json_argument_string(arguments_json, "app_id", app_id, sizeof(app_id))) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"missing task_id or app_id\"}");
        return -1;
    }
    if (!json_argument_string(arguments_json, "schedule", schedule, sizeof(schedule))) {
        copy_text(schedule, sizeof(schedule), "periodic");
    }
    if (!json_argument_string(arguments_json, "trigger", trigger, sizeof(trigger))) {
        copy_text(trigger, sizeof(trigger), strcmp(schedule, "event") == 0 ? "sensor" : "timer");
    }
    if (!json_argument_string(arguments_json, "payload", payload, sizeof(payload))) {
        payload[0] = '\0';
    }
    (void)json_argument_int(arguments_json, "period_ms", &period_ms);
    (void)json_argument_int(arguments_json, "iterations", &iterations);

    if (espclaw_task_start_with_schedule(
            task_id,
            workspace_root,
            app_id,
            schedule,
            trigger,
            payload,
            period_ms > 0 ? (uint32_t)period_ms : 0,
            iterations > 0 ? (uint32_t)iterations : 0,
            message,
            sizeof(message)) != 0) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":");
        append_escaped_json(buffer, buffer_size, strlen(buffer), message);
        append_text_segment(buffer, buffer_size, "}");
        return -1;
    }

    espclaw_task_render_json(buffer, buffer_size);
    return 0;
}

static int tool_behavior_register(const char *workspace_root, const char *arguments_json, char *buffer, size_t buffer_size)
{
    espclaw_behavior_spec_t spec;
    char behavior_id[ESPCLAW_BEHAVIOR_ID_MAX + 1];
    char app_id[ESPCLAW_APP_ID_MAX + 1];
    char title[ESPCLAW_BEHAVIOR_TITLE_MAX + 1];
    char *source = NULL;
    char permissions[256];
    char triggers[128];
    char schedule[ESPCLAW_TASK_SCHEDULE_MAX + 1];
    char trigger[ESPCLAW_TASK_TRIGGER_MAX + 1];
    char payload[ESPCLAW_TASK_PAYLOAD_MAX];
    char message[256];
    int period_ms = 20;
    int iterations = 0;
    bool autostart = false;

    source = (char *)calloc(1, ESPCLAW_AGENT_TOOL_ARGS_MAX + 1U);
    if (source == NULL) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"out of memory\"}");
        return -1;
    }

    if (!json_argument_string(arguments_json, "behavior_id", behavior_id, sizeof(behavior_id))) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"missing behavior_id\"}");
        free(source);
        return -1;
    }
    if (!json_argument_string(arguments_json, "app_id", app_id, sizeof(app_id))) {
        copy_text(app_id, sizeof(app_id), behavior_id);
    }
    if (!json_argument_string(arguments_json, "title", title, sizeof(title))) {
        copy_text(title, sizeof(title), behavior_id);
    }
    if (!json_argument_string(arguments_json, "schedule", schedule, sizeof(schedule))) {
        copy_text(schedule, sizeof(schedule), "periodic");
    }
    if (!json_argument_string(arguments_json, "trigger", trigger, sizeof(trigger))) {
        copy_text(trigger, sizeof(trigger), strcmp(schedule, "event") == 0 ? "sensor" : "timer");
    }
    if (!json_argument_string(arguments_json, "payload", payload, sizeof(payload))) {
        payload[0] = '\0';
    }
    (void)json_argument_int(arguments_json, "period_ms", &period_ms);
    (void)json_argument_int(arguments_json, "iterations", &iterations);
    (void)json_argument_bool(arguments_json, "autostart", &autostart);

    if (json_argument_string(arguments_json, "source", source, ESPCLAW_AGENT_TOOL_ARGS_MAX + 1U)) {
        if (!json_argument_string(arguments_json, "permissions_csv", permissions, sizeof(permissions))) {
            copy_text(
                permissions,
                sizeof(permissions),
                "fs.read,fs.write,gpio.read,gpio.write,pwm.write,adc.read,i2c.read,i2c.write,uart.read,uart.write,camera.capture,temperature.read,imu.read,buzzer.play,ppm.write,task.control"
            );
        }
        if (!json_argument_string(arguments_json, "triggers_csv", triggers, sizeof(triggers))) {
            copy_text(triggers, sizeof(triggers), "manual,boot,timer,uart,sensor,event");
        }
        if (espclaw_app_read_source(workspace_root, app_id, message, sizeof(message)) != 0 &&
            espclaw_app_scaffold_lua(workspace_root, app_id, title, permissions, triggers) != 0) {
            snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"failed to scaffold app\"}");
            free(source);
            return -1;
        }
        if (espclaw_app_update_source(workspace_root, app_id, source) != 0) {
            snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"failed to save source\"}");
            free(source);
            return -1;
        }
    }

    memset(&spec, 0, sizeof(spec));
    copy_text(spec.behavior_id, sizeof(spec.behavior_id), behavior_id);
    copy_text(spec.title, sizeof(spec.title), title);
    copy_text(spec.app_id, sizeof(spec.app_id), app_id);
    copy_text(spec.schedule, sizeof(spec.schedule), schedule);
    copy_text(spec.trigger, sizeof(spec.trigger), trigger);
    copy_text(spec.payload, sizeof(spec.payload), payload);
    spec.period_ms = period_ms > 0 ? (uint32_t)period_ms : 0;
    spec.max_iterations = iterations > 0 ? (uint32_t)iterations : 0;
    spec.autostart = autostart;

    if (espclaw_behavior_register(workspace_root, &spec, message, sizeof(message)) != 0) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":");
        append_escaped_json(buffer, buffer_size, strlen(buffer), message);
        append_text_segment(buffer, buffer_size, "}");
        free(source);
        return -1;
    }

    espclaw_behavior_render_json(workspace_root, buffer, buffer_size);
    free(source);
    return 0;
}

static int tool_behavior_start(const char *workspace_root, const char *arguments_json, char *buffer, size_t buffer_size)
{
    char behavior_id[ESPCLAW_BEHAVIOR_ID_MAX + 1];
    char message[256];

    if (!json_argument_string(arguments_json, "behavior_id", behavior_id, sizeof(behavior_id))) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"missing behavior_id\"}");
        return -1;
    }
    if (espclaw_behavior_start(workspace_root, behavior_id, message, sizeof(message)) != 0) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":");
        append_escaped_json(buffer, buffer_size, strlen(buffer), message);
        append_text_segment(buffer, buffer_size, "}");
        return -1;
    }

    espclaw_behavior_render_json(workspace_root, buffer, buffer_size);
    return 0;
}

static int tool_behavior_stop(const char *arguments_json, char *buffer, size_t buffer_size)
{
    char behavior_id[ESPCLAW_BEHAVIOR_ID_MAX + 1];
    char message[256];

    if (!json_argument_string(arguments_json, "behavior_id", behavior_id, sizeof(behavior_id))) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"missing behavior_id\"}");
        return -1;
    }
    if (espclaw_behavior_stop(behavior_id, message, sizeof(message)) != 0) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":");
        append_escaped_json(buffer, buffer_size, strlen(buffer), message);
        append_text_segment(buffer, buffer_size, "}");
        return -1;
    }

    snprintf(buffer, buffer_size, "{\"ok\":true,\"message\":");
    append_escaped_json(buffer, buffer_size, strlen(buffer), message);
    append_text_segment(buffer, buffer_size, "}");
    return 0;
}

static int tool_behavior_remove(const char *workspace_root, const char *arguments_json, char *buffer, size_t buffer_size)
{
    char behavior_id[ESPCLAW_BEHAVIOR_ID_MAX + 1];
    char message[256];

    if (!json_argument_string(arguments_json, "behavior_id", behavior_id, sizeof(behavior_id))) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"missing behavior_id\"}");
        return -1;
    }
    if (espclaw_behavior_remove(workspace_root, behavior_id, message, sizeof(message)) != 0) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":");
        append_escaped_json(buffer, buffer_size, strlen(buffer), message);
        append_text_segment(buffer, buffer_size, "}");
        return -1;
    }

    espclaw_behavior_render_json(workspace_root, buffer, buffer_size);
    return 0;
}

static int tool_task_stop(const char *arguments_json, char *buffer, size_t buffer_size)
{
    char task_id[ESPCLAW_TASK_ID_MAX + 1];
    char message[256];

    if (!json_argument_string(arguments_json, "task_id", task_id, sizeof(task_id))) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"missing task_id\"}");
        return -1;
    }
    if (espclaw_task_stop(task_id, message, sizeof(message)) != 0) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":");
        append_escaped_json(buffer, buffer_size, strlen(buffer), message);
        append_text_segment(buffer, buffer_size, "}");
        return -1;
    }

    snprintf(buffer, buffer_size, "{\"ok\":true,\"message\":");
    append_escaped_json(buffer, buffer_size, strlen(buffer), message);
    append_text_segment(buffer, buffer_size, "}");
    return 0;
}

static int tool_event_emit(const char *arguments_json, char *buffer, size_t buffer_size)
{
    char name[ESPCLAW_TASK_TRIGGER_MAX + 1];
    char payload[ESPCLAW_TASK_PAYLOAD_MAX];
    char message[256];

    if (!json_argument_string(arguments_json, "name", name, sizeof(name))) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"missing name\"}");
        return -1;
    }
    if (!json_argument_string(arguments_json, "payload", payload, sizeof(payload))) {
        payload[0] = '\0';
    }
    if (espclaw_task_emit_event(name, payload, message, sizeof(message)) != 0) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":");
        append_escaped_json(buffer, buffer_size, strlen(buffer), message);
        append_text_segment(buffer, buffer_size, "}");
        return -1;
    }

    snprintf(buffer, buffer_size, "{\"ok\":true,\"message\":");
    append_escaped_json(buffer, buffer_size, strlen(buffer), message);
    append_text_segment(buffer, buffer_size, "}");
    return 0;
}

static int tool_event_watch_list(char *buffer, size_t buffer_size)
{
    return espclaw_event_watch_render_json(buffer, buffer_size);
}

static int tool_event_watch_add(const char *arguments_json, char *buffer, size_t buffer_size)
{
    char watch_id[ESPCLAW_EVENT_WATCH_ID_MAX + 1];
    char kind[ESPCLAW_EVENT_WATCH_KIND_MAX + 1];
    char event_name[ESPCLAW_EVENT_WATCH_EVENT_NAME_MAX + 1];
    char message[256];
    int port = 0;
    int unit = 1;
    int channel = 0;
    int threshold = 0;
    int interval_ms = 100;

    if (!json_argument_string(arguments_json, "watch_id", watch_id, sizeof(watch_id))) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"missing watch_id\"}");
        return -1;
    }
    if (!json_argument_string(arguments_json, "kind", kind, sizeof(kind))) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"missing kind\"}");
        return -1;
    }
    if (!json_argument_string(arguments_json, "event_name", event_name, sizeof(event_name))) {
        event_name[0] = '\0';
    }

    if (strcmp(kind, "uart") == 0) {
        (void)json_argument_int(arguments_json, "port", &port);
        if (espclaw_event_watch_add_uart(watch_id, event_name, port, message, sizeof(message)) != 0) {
            snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":");
            append_escaped_json(buffer, buffer_size, strlen(buffer), message);
            append_text_segment(buffer, buffer_size, "}");
            return -1;
        }
    } else if (strcmp(kind, "adc_threshold") == 0) {
        if (!json_argument_int(arguments_json, "unit", &unit) ||
            !json_argument_int(arguments_json, "channel", &channel) ||
            !json_argument_int(arguments_json, "threshold", &threshold)) {
            snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"adc watch requires unit, channel, and threshold\"}");
            return -1;
        }
        (void)json_argument_int(arguments_json, "interval_ms", &interval_ms);
        if (espclaw_event_watch_add_adc_threshold(
                watch_id,
                event_name,
                unit,
                channel,
                threshold,
                interval_ms > 0 ? (uint32_t)interval_ms : 100U,
                message,
                sizeof(message)) != 0) {
            snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":");
            append_escaped_json(buffer, buffer_size, strlen(buffer), message);
            append_text_segment(buffer, buffer_size, "}");
            return -1;
        }
    } else {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"unsupported watch kind\"}");
        return -1;
    }

    espclaw_event_watch_render_json(buffer, buffer_size);
    return 0;
}

static int tool_event_watch_remove(const char *arguments_json, char *buffer, size_t buffer_size)
{
    char watch_id[ESPCLAW_EVENT_WATCH_ID_MAX + 1];
    char message[256];

    if (!json_argument_string(arguments_json, "watch_id", watch_id, sizeof(watch_id))) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"missing watch_id\"}");
        return -1;
    }
    if (espclaw_event_watch_remove(watch_id, message, sizeof(message)) != 0) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":");
        append_escaped_json(buffer, buffer_size, strlen(buffer), message);
        append_text_segment(buffer, buffer_size, "}");
        return -1;
    }

    espclaw_event_watch_render_json(buffer, buffer_size);
    return 0;
}

static int tool_camera_capture(const char *workspace_root, const char *arguments_json, char *buffer, size_t buffer_size)
{
    char filename[64];
    espclaw_hw_camera_capture_t capture;

    if (!json_argument_string(arguments_json, "filename", filename, sizeof(filename))) {
        filename[0] = '\0';
    }
    if (espclaw_hw_camera_capture(
            workspace_root,
            filename[0] != '\0' ? filename : NULL,
            &capture) != 0) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":");
        append_escaped_json(buffer, buffer_size, strlen(buffer), capture.error[0] != '\0' ? capture.error : "camera capture failed");
        append_text_segment(buffer, buffer_size, "}");
        return -1;
    }

    snprintf(
        buffer,
        buffer_size,
        "{\"ok\":true,\"path\":\"%s\",\"mime_type\":\"%s\",\"bytes\":%u,\"width\":%u,\"height\":%u,\"simulated\":%s}",
        capture.relative_path,
        capture.mime_type,
        (unsigned)capture.bytes_written,
        (unsigned)capture.width,
        (unsigned)capture.height,
        capture.simulated ? "true" : "false"
    );
    return 0;
}

static int tool_uart_read(const char *arguments_json, char *buffer, size_t buffer_size)
{
    int port = 0;
    int length = 256;
    uint8_t data[512];
    size_t bytes_read = 0;

    (void)json_argument_int(arguments_json, "port", &port);
    if (json_argument_int(arguments_json, "length", &length) && (length <= 0 || length > (int)sizeof(data))) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"invalid length\"}");
        return -1;
    }
    if (!json_argument_int(arguments_json, "length", &length)) {
        length = 256;
    }
    if (espclaw_hw_uart_read(port, data, (size_t)length, &bytes_read) != 0) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"uart read failed\"}");
        return -1;
    }

    data[bytes_read < sizeof(data) ? bytes_read : sizeof(data) - 1] = '\0';
    snprintf(buffer, buffer_size, "{\"ok\":true,\"port\":%d,\"bytes_read\":%u,\"data\":", port, (unsigned)bytes_read);
    append_escaped_json(buffer, buffer_size, strlen(buffer), (const char *)data);
    append_text_segment(buffer, buffer_size, "}");
    return 0;
}

static int tool_uart_write(const char *arguments_json, char *buffer, size_t buffer_size)
{
    int port = 0;
    char data[512];
    size_t bytes_written = 0;

    if (!json_argument_int(arguments_json, "port", &port)) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"missing port\"}");
        return -1;
    }
    if (!json_argument_string(arguments_json, "data", data, sizeof(data))) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"missing data\"}");
        return -1;
    }
    if (espclaw_hw_uart_write(port, (const uint8_t *)data, strlen(data), &bytes_written) != 0) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"uart write failed\"}");
        return -1;
    }

    snprintf(buffer, buffer_size, "{\"ok\":true,\"port\":%d,\"bytes_written\":%u}", port, (unsigned)bytes_written);
    return 0;
}

static int tool_i2c_scan(const char *arguments_json, char *buffer, size_t buffer_size)
{
    uint8_t addresses[ESPCLAW_HW_I2C_SCAN_MAX];
    size_t count = 0;
    size_t used = 0;
    size_t index;
    int port = 0;

    if (ensure_i2c_ready(arguments_json, &port) != 0 || espclaw_hw_i2c_scan(port, addresses, sizeof(addresses), &count) != 0) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"i2c scan failed\"}");
        return -1;
    }

    used += (size_t)snprintf(buffer + used, buffer_size - used, "{\"ok\":true,\"port\":%d,\"addresses\":[", port);
    for (index = 0; index < count && used + 8 < buffer_size; ++index) {
        if (index > 0) {
            used += (size_t)snprintf(buffer + used, buffer_size - used, ",");
        }
        used += (size_t)snprintf(buffer + used, buffer_size - used, "%u", (unsigned)addresses[index]);
    }
    used += (size_t)snprintf(buffer + used, buffer_size - used, "]}");
    return 0;
}

static int tool_i2c_read(const char *arguments_json, char *buffer, size_t buffer_size)
{
    int port = 0;
    int address = -1;
    int reg = -1;
    int length = 0;
    uint8_t data[64];
    size_t used = 0;

    if (!json_argument_int(arguments_json, "address", &address) ||
        !json_argument_int(arguments_json, "register", &reg) ||
        !json_argument_int(arguments_json, "length", &length) ||
        length <= 0 || length > (int)sizeof(data)) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"missing address/register/length\"}");
        return -1;
    }
    if (ensure_i2c_ready(arguments_json, &port) != 0 ||
        espclaw_hw_i2c_read_reg(port, (uint8_t)address, (uint8_t)reg, data, (size_t)length) != 0) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"i2c read failed\"}");
        return -1;
    }

    used += (size_t)snprintf(buffer + used, buffer_size - used, "{\"ok\":true,\"port\":%d,\"address\":%d,\"register\":%d,\"length\":%d,\"data_hex\":", port, address, reg, length);
    used = append_json_hex_string(buffer, buffer_size, used, data, (size_t)length);
    used += (size_t)snprintf(buffer + used, buffer_size - used, "}");
    return 0;
}

static int tool_i2c_write(const char *arguments_json, char *buffer, size_t buffer_size)
{
    int port = 0;
    int address = -1;
    int reg = -1;
    char data_text[256];
    uint8_t data[64];
    size_t length = 0;

    if (!json_argument_int(arguments_json, "address", &address) ||
        !json_argument_int(arguments_json, "register", &reg) ||
        !json_argument_string(arguments_json, "data", data_text, sizeof(data_text)) ||
        !parse_hex_bytes(data_text, data, sizeof(data), &length)) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"missing address/register/data\"}");
        return -1;
    }
    if (ensure_i2c_ready(arguments_json, &port) != 0 ||
        espclaw_hw_i2c_write_reg(port, (uint8_t)address, (uint8_t)reg, data, length) != 0) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"i2c write failed\"}");
        return -1;
    }

    snprintf(buffer, buffer_size, "{\"ok\":true,\"port\":%d,\"address\":%d,\"register\":%d,\"bytes_written\":%u}", port, address, reg, (unsigned)length);
    return 0;
}

static int tool_temperature_read(const char *arguments_json, char *buffer, size_t buffer_size)
{
    int port = 0;
    int address = 0x48;
    char sensor[32];
    double temperature_c = 0.0;

    if (!json_argument_string(arguments_json, "sensor", sensor, sizeof(sensor))) {
        copy_text(sensor, sizeof(sensor), "tmp102");
    }
    (void)json_argument_int(arguments_json, "address", &address);
    if (strcmp(sensor, "tmp102") != 0) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"unsupported temperature sensor\"}");
        return -1;
    }
    if (ensure_i2c_ready(arguments_json, &port) != 0 || espclaw_hw_tmp102_read_c(port, (uint8_t)address, &temperature_c) != 0) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"temperature read failed\"}");
        return -1;
    }

    snprintf(buffer, buffer_size, "{\"ok\":true,\"sensor\":\"tmp102\",\"port\":%d,\"address\":%d,\"temperature_c\":%.4f}", port, address, temperature_c);
    return 0;
}

static int tool_imu_read(const char *arguments_json, char *buffer, size_t buffer_size)
{
    int port = 0;
    int address = 0x68;
    char sensor[32];
    espclaw_hw_mpu6050_sample_t sample;

    if (!json_argument_string(arguments_json, "sensor", sensor, sizeof(sensor))) {
        copy_text(sensor, sizeof(sensor), "mpu6050");
    }
    (void)json_argument_int(arguments_json, "address", &address);
    if (strcmp(sensor, "mpu6050") != 0) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"unsupported imu sensor\"}");
        return -1;
    }
    if (ensure_i2c_ready(arguments_json, &port) != 0 ||
        espclaw_hw_mpu6050_begin(port, (uint8_t)address) != 0 ||
        espclaw_hw_mpu6050_read(port, (uint8_t)address, &sample) != 0) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"imu read failed\"}");
        return -1;
    }

    snprintf(
        buffer,
        buffer_size,
        "{\"ok\":true,\"sensor\":\"mpu6050\",\"port\":%d,\"address\":%d,\"accel_g\":{\"x\":%.6f,\"y\":%.6f,\"z\":%.6f},\"gyro_dps\":{\"x\":%.6f,\"y\":%.6f,\"z\":%.6f},\"temperature_c\":%.6f}",
        port,
        address,
        sample.accel_x_g,
        sample.accel_y_g,
        sample.accel_z_g,
        sample.gyro_x_dps,
        sample.gyro_y_dps,
        sample.gyro_z_dps,
        sample.temperature_c
    );
    return 0;
}

static int tool_buzzer_play(const char *arguments_json, char *buffer, size_t buffer_size)
{
    int channel = 0;
    int pin = -1;
    int frequency_hz = 0;
    int duration_ms = 0;
    int duty_percent = 50;

    (void)json_argument_int(arguments_json, "channel", &channel);
    if (!json_argument_int(arguments_json, "pin", &pin) ||
        !json_argument_int(arguments_json, "frequency_hz", &frequency_hz) ||
        !json_argument_int(arguments_json, "duration_ms", &duration_ms)) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"missing pin/frequency_hz/duration_ms\"}");
        return -1;
    }
    (void)json_argument_int(arguments_json, "duty_percent", &duty_percent);
    if (espclaw_hw_buzzer_tone(channel, pin, frequency_hz, duration_ms, duty_percent) != 0) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"buzzer play failed\"}");
        return -1;
    }

    snprintf(buffer, buffer_size, "{\"ok\":true,\"channel\":%d,\"pin\":%d,\"frequency_hz\":%d,\"duration_ms\":%d}", channel, pin, frequency_hz, duration_ms);
    return 0;
}

static int tool_pid_compute(const char *arguments_json, char *buffer, size_t buffer_size)
{
    double setpoint = 0.0;
    double measurement = 0.0;
    double dt_seconds = 0.0;
    double kp = 0.0;
    double ki = 0.0;
    double kd = 0.0;
    double integral = 0.0;
    double previous_error = 0.0;
    double output_min = -1000000000.0;
    double output_max = 1000000000.0;
    double integral_out = 0.0;
    double error_out = 0.0;
    double output = 0.0;

    if (!json_argument_double(arguments_json, "setpoint", &setpoint) ||
        !json_argument_double(arguments_json, "measurement", &measurement) ||
        !json_argument_double(arguments_json, "dt_seconds", &dt_seconds) ||
        !json_argument_double(arguments_json, "kp", &kp) ||
        !json_argument_double(arguments_json, "ki", &ki) ||
        !json_argument_double(arguments_json, "kd", &kd)) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"missing pid parameters\"}");
        return -1;
    }
    (void)json_argument_double(arguments_json, "integral", &integral);
    (void)json_argument_double(arguments_json, "previous_error", &previous_error);
    (void)json_argument_double(arguments_json, "output_min", &output_min);
    (void)json_argument_double(arguments_json, "output_max", &output_max);
    output = espclaw_hw_pid_step(
        setpoint,
        measurement,
        integral,
        previous_error,
        kp,
        ki,
        kd,
        dt_seconds,
        output_min,
        output_max,
        &integral_out,
        &error_out);
    snprintf(buffer, buffer_size, "{\"ok\":true,\"output\":%.6f,\"integral\":%.6f,\"error\":%.6f}", output, integral_out, error_out);
    return 0;
}

static int tool_control_mix(const char *arguments_json, char *buffer, size_t buffer_size)
{
    char mode[32];
    double throttle = 0.0;
    double steering = 0.0;
    double roll = 0.0;
    double pitch = 0.0;
    double yaw = 0.0;
    double output_min = -1.0;
    double output_max = 1.0;

    if (!json_argument_string(arguments_json, "mode", mode, sizeof(mode))) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"missing mode\"}");
        return -1;
    }
    (void)json_argument_double(arguments_json, "throttle", &throttle);
    (void)json_argument_double(arguments_json, "steering", &steering);
    (void)json_argument_double(arguments_json, "turn", &steering);
    (void)json_argument_double(arguments_json, "roll", &roll);
    (void)json_argument_double(arguments_json, "pitch", &pitch);
    (void)json_argument_double(arguments_json, "yaw", &yaw);
    (void)json_argument_double(arguments_json, "output_min", &output_min);
    (void)json_argument_double(arguments_json, "output_max", &output_max);

    if (strcmp(mode, "differential") == 0 || strcmp(mode, "rover") == 0) {
        double left = 0.0;
        double right = 0.0;

        if (espclaw_hw_mix_differential_drive(throttle, steering, output_min, output_max, &left, &right) != 0) {
            snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"differential mix failed\"}");
            return -1;
        }
        snprintf(buffer, buffer_size, "{\"ok\":true,\"mode\":\"differential\",\"left\":%.6f,\"right\":%.6f}", left, right);
        return 0;
    }
    if (strcmp(mode, "quad_x") == 0 || strcmp(mode, "quad") == 0) {
        double front_left = 0.0;
        double front_right = 0.0;
        double rear_right = 0.0;
        double rear_left = 0.0;

        if (espclaw_hw_mix_quad_x(throttle, roll, pitch, yaw, output_min, output_max, &front_left, &front_right, &rear_right, &rear_left) != 0) {
            snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"quad mix failed\"}");
            return -1;
        }
        snprintf(
            buffer,
            buffer_size,
            "{\"ok\":true,\"mode\":\"quad_x\",\"front_left\":%.6f,\"front_right\":%.6f,\"rear_right\":%.6f,\"rear_left\":%.6f}",
            front_left,
            front_right,
            rear_right,
            rear_left
        );
        return 0;
    }

    snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"unsupported control mix mode\"}");
    return -1;
}

static int tool_spi_transfer(const char *arguments_json, char *buffer, size_t buffer_size)
{
    int bus = 0;
    char data[256];

    (void)json_argument_int(arguments_json, "bus", &bus);
    if (!json_argument_string(arguments_json, "data", data, sizeof(data))) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"missing data\"}");
        return -1;
    }
    snprintf(buffer, buffer_size, "{\"ok\":false,\"supported\":false,\"bus\":%d,\"error\":\"spi transfer not implemented\"}", bus);
    return -1;
}

static int tool_ota_check(char *buffer, size_t buffer_size)
{
#ifdef ESP_PLATFORM
    espclaw_ota_snapshot_t snapshot;

    espclaw_ota_manager_snapshot(&snapshot);
    snprintf(
        buffer,
        buffer_size,
        "{\"ok\":true,\"supported\":true,\"running_partition\":\"%s\",\"target_partition\":\"%s\",\"upload_in_progress\":%s,\"status\":",
        snapshot.running_partition_label,
        snapshot.target_partition_label,
        snapshot.upload_in_progress ? "true" : "false"
    );
    append_escaped_json(buffer, buffer_size, strlen(buffer), snapshot.last_message[0] != '\0' ? snapshot.last_message : "ready");
    append_text_segment(buffer, buffer_size, "}");
    return 0;
#else
    snprintf(buffer, buffer_size, "{\"ok\":true,\"supported\":false}");
    return 0;
#endif
}

static int tool_execute(
    const char *workspace_root,
    const espclaw_agent_tool_call_t *tool_call,
    bool allow_mutations,
    espclaw_agent_media_ref_t *media_ref,
    char *buffer,
    size_t buffer_size
)
{
    if (tool_call == NULL || buffer == NULL || buffer_size == 0) {
        return -1;
    }
    if (media_ref != NULL) {
        memset(media_ref, 0, sizeof(*media_ref));
    }

    if (espclaw_tool_requires_confirmation(tool_call->name) && !allow_mutations) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"confirmation_required\"}");
        return -1;
    }

    if (strcmp(tool_call->name, "fs.read") == 0) {
        return tool_fs_read(workspace_root, tool_call->arguments_json, buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "fs.write") == 0) {
        return tool_fs_write(workspace_root, tool_call->arguments_json, buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "fs.list") == 0) {
        return tool_fs_list(workspace_root, tool_call->arguments_json, buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "fs.delete") == 0) {
        return tool_fs_delete(workspace_root, tool_call->arguments_json, buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "system.info") == 0) {
        return tool_system_info(workspace_root, buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "system.logs") == 0) {
        return tool_system_logs(tool_call->arguments_json, buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "hardware.list") == 0) {
        return tool_hardware_list(buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "lua_api.list") == 0) {
        return tool_lua_api_list(buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "app_patterns.list") == 0) {
        return tool_app_patterns_list(buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "tool.list") == 0) {
        return tool_list_tools(buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "camera.capture") == 0) {
        int status = tool_camera_capture(workspace_root, tool_call->arguments_json, buffer, buffer_size);

        if (status == 0 && media_ref != NULL) {
            media_ref->active = json_argument_string(buffer, "path", media_ref->relative_path, sizeof(media_ref->relative_path));
            if (media_ref->active) {
                json_argument_string(buffer, "mime_type", media_ref->mime_type, sizeof(media_ref->mime_type));
            }
        }
        return status;
    }
    if (strcmp(tool_call->name, "web.search") == 0) {
        return tool_web_search(tool_call->arguments_json, buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "web.fetch") == 0) {
        return tool_web_fetch(workspace_root, tool_call->arguments_json, buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "wifi.status") == 0) {
        return tool_wifi_status(buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "wifi.scan") == 0) {
        return tool_wifi_scan(buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "ble.scan") == 0) {
        return tool_ble_scan(buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "gpio.read") == 0) {
        return tool_gpio_read(tool_call->arguments_json, buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "gpio.write") == 0) {
        return tool_gpio_write(tool_call->arguments_json, buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "pwm.write") == 0) {
        return tool_pwm_write(tool_call->arguments_json, buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "ppm.write") == 0) {
        return tool_ppm_write(tool_call->arguments_json, buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "adc.read") == 0) {
        return tool_adc_read(tool_call->arguments_json, buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "i2c.scan") == 0) {
        return tool_i2c_scan(tool_call->arguments_json, buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "i2c.read") == 0) {
        return tool_i2c_read(tool_call->arguments_json, buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "i2c.write") == 0) {
        return tool_i2c_write(tool_call->arguments_json, buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "temperature.read") == 0) {
        return tool_temperature_read(tool_call->arguments_json, buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "imu.read") == 0) {
        return tool_imu_read(tool_call->arguments_json, buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "buzzer.play") == 0) {
        return tool_buzzer_play(tool_call->arguments_json, buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "pid.compute") == 0) {
        return tool_pid_compute(tool_call->arguments_json, buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "control.mix") == 0) {
        return tool_control_mix(tool_call->arguments_json, buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "spi.transfer") == 0) {
        return tool_spi_transfer(tool_call->arguments_json, buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "ota.check") == 0) {
        return tool_ota_check(buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "app.list") == 0) {
        return tool_app_list(workspace_root, buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "component.list") == 0) {
        return tool_component_list(workspace_root, buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "app.run") == 0) {
        return tool_app_run(workspace_root, tool_call->arguments_json, buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "app.install") == 0) {
        return tool_app_install(workspace_root, tool_call->arguments_json, buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "app.install_from_file") == 0) {
        return tool_app_install_from_file(workspace_root, tool_call->arguments_json, buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "app.install_from_blob") == 0) {
        return tool_app_install_from_blob(workspace_root, tool_call->arguments_json, buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "app.install_from_url") == 0) {
        return tool_app_install_from_url(workspace_root, tool_call->arguments_json, buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "component.install") == 0) {
        return tool_component_install(workspace_root, tool_call->arguments_json, buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "component.install_from_file") == 0) {
        return tool_component_install_from_file(workspace_root, tool_call->arguments_json, buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "component.install_from_blob") == 0) {
        return tool_component_install_from_blob(workspace_root, tool_call->arguments_json, buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "component.install_from_url") == 0) {
        return tool_component_install_from_url(workspace_root, tool_call->arguments_json, buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "component.install_from_manifest") == 0) {
        return tool_component_install_from_manifest(workspace_root, tool_call->arguments_json, buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "context.chunks") == 0) {
        return tool_context_chunks(workspace_root, tool_call->arguments_json, buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "context.load") == 0) {
        return tool_context_load(workspace_root, tool_call->arguments_json, buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "context.search") == 0) {
        return tool_context_search(workspace_root, tool_call->arguments_json, buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "context.select") == 0) {
        return tool_context_select(workspace_root, tool_call->arguments_json, buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "context.summarize") == 0) {
        return tool_context_summarize(workspace_root, tool_call->arguments_json, buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "app.remove") == 0) {
        return tool_app_remove(workspace_root, tool_call->arguments_json, buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "component.remove") == 0) {
        return tool_component_remove(workspace_root, tool_call->arguments_json, buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "task.list") == 0) {
        return tool_task_list(buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "behavior.list") == 0) {
        return tool_behavior_list(workspace_root, buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "task.start") == 0) {
        return tool_task_start(workspace_root, tool_call->arguments_json, buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "behavior.register") == 0) {
        return tool_behavior_register(workspace_root, tool_call->arguments_json, buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "behavior.start") == 0) {
        return tool_behavior_start(workspace_root, tool_call->arguments_json, buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "behavior.stop") == 0) {
        return tool_behavior_stop(tool_call->arguments_json, buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "behavior.remove") == 0) {
        return tool_behavior_remove(workspace_root, tool_call->arguments_json, buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "task.stop") == 0) {
        return tool_task_stop(tool_call->arguments_json, buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "event.emit") == 0) {
        return tool_event_emit(tool_call->arguments_json, buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "event.watch_list") == 0) {
        return tool_event_watch_list(buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "event.watch_add") == 0) {
        return tool_event_watch_add(tool_call->arguments_json, buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "event.watch_remove") == 0) {
        return tool_event_watch_remove(tool_call->arguments_json, buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "uart.read") == 0) {
        return tool_uart_read(tool_call->arguments_json, buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "uart.write") == 0) {
        return tool_uart_write(tool_call->arguments_json, buffer, buffer_size);
    }

    snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"unsupported_tool\"}");
    return -1;
}

int espclaw_agent_execute_tool(
    const char *workspace_root,
    const char *tool_name,
    const char *arguments_json,
    bool allow_mutations,
    char *buffer,
    size_t buffer_size
)
{
    espclaw_agent_tool_call_t tool_call;

    if (tool_name == NULL || buffer == NULL || buffer_size == 0) {
        return -1;
    }

    memset(&tool_call, 0, sizeof(tool_call));
    snprintf(tool_call.call_id, sizeof(tool_call.call_id), "manual");
    snprintf(tool_call.name, sizeof(tool_call.name), "%s", tool_name);
    snprintf(tool_call.arguments_json, sizeof(tool_call.arguments_json), "%s", arguments_json != NULL ? arguments_json : "{}");
    log_tool_call_summary(&tool_call, "manual");
    return tool_execute(workspace_root, &tool_call, allow_mutations, NULL, buffer, buffer_size);
}

#ifdef ESPCLAW_HOST_LUA
static int mock_transport(
    const char *url,
    const espclaw_auth_profile_t *profile,
    const char *body,
    char *response,
    size_t response_size
)
{
    (void)profile;

    if (strncmp(url, "mock://", 7) != 0) {
        return -1;
    }

    if (strstr(url, "tool-loop") != NULL) {
        if (user_message_requests_tool_listing(body)) {
            if (strstr(body, "\"type\":\"function_call_output\"") != NULL || strstr(body, "\"previous_response_id\"") != NULL) {
                snprintf(
                    response,
                    response_size,
                    "{"
                    "\"id\":\"resp_mock_tools_final\","
                    "\"status\":\"completed\","
                    "\"output\":[{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"I can inspect files, apps, hardware, networking, and OTA state.\"}]}]"
                    "}"
                );
                return 0;
            }

            snprintf(
                response,
                response_size,
                "{"
                "\"id\":\"resp_mock_tools_first\","
                "\"status\":\"completed\","
                "\"output\":["
                "{\"type\":\"function_call\",\"call_id\":\"call_tool_list\",\"name\":\"tool_x2E_list\",\"arguments\":\"{}\",\"status\":\"completed\"}"
                "]"
                "}"
            );
            return 0;
        }

        if (strstr(body, "\"type\":\"function_call_output\"") != NULL || strstr(body, "\"previous_response_id\"") != NULL) {
            snprintf(
                response,
                response_size,
                "{"
                "\"id\":\"resp_mock_final\","
                "\"status\":\"completed\","
                "\"output\":[{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"I checked the device and listed the installed apps.\"}]}]"
                "}"
            );
            return 0;
        }

        snprintf(
            response,
            response_size,
            "{"
            "\"id\":\"resp_mock_first\","
            "\"status\":\"completed\","
            "\"output\":["
            "{\"type\":\"function_call\",\"call_id\":\"call_sys\",\"name\":\"system.info\",\"arguments\":\"{}\",\"status\":\"completed\"},"
            "{\"type\":\"function_call\",\"call_id\":\"call_apps\",\"name\":\"app.list\",\"arguments\":\"{}\",\"status\":\"completed\"}"
            "]"
            "}"
        );
        return 0;
    }

    if (strstr(url, "fs-read") != NULL) {
        if (strstr(body, "\"type\":\"function_call_output\"") != NULL || strstr(body, "\"previous_response_id\"") != NULL) {
            snprintf(
                response,
                response_size,
                "{"
                "\"id\":\"resp_mock_fs_final\","
                "\"status\":\"completed\","
                "\"output\":[{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"I read the requested file and summarized it.\"}]}]"
                "}"
            );
            return 0;
        }

        snprintf(
            response,
            response_size,
            "{"
            "\"id\":\"resp_mock_fs_first\","
            "\"status\":\"completed\","
            "\"output\":["
            "{\"type\":\"function_call\",\"call_id\":\"call_read\",\"name\":\"fs.read\",\"arguments\":\"{\\\"path\\\":\\\"AGENTS.md\\\"}\",\"status\":\"completed\"}"
            "]"
            "}"
        );
        return 0;
    }

    snprintf(
        response,
        response_size,
        "{"
        "\"id\":\"resp_mock_text\","
        "\"status\":\"completed\","
        "\"output\":[{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"Mock provider reply.\"}]}]"
        "}"
    );
    return 0;
}

static int host_http_post(
    const char *url,
    const espclaw_auth_profile_t *profile,
    const char *body,
    char *response,
    size_t response_size,
    char *error_text,
    size_t error_text_size
)
{
    char body_path[] = "/tmp/espclaw-body-XXXXXX";
    char command[4096];
    FILE *body_file;
    FILE *pipe;
    int fd;
    size_t total_read = 0;
    char discard[1024];
    bool is_codex = profile != NULL && strcmp(profile->provider_id, "openai_codex") == 0;

    if (strncmp(url, "mock://", 7) == 0) {
        return mock_transport(url, profile, body, response, response_size);
    }

    fd = mkstemp(body_path);
    if (fd < 0) {
        return -1;
    }
    body_file = fdopen(fd, "w");
    if (body_file == NULL) {
        close(fd);
        unlink(body_path);
        return -1;
    }
    fputs(body, body_file);
    fclose(body_file);

    if (is_codex && profile->account_id[0] != '\0') {
        snprintf(
            command,
            sizeof(command),
            "curl -sS -N -X POST '%s/responses' "
            "-H 'Content-Type: application/json' "
            "-H 'Authorization: Bearer %s' "
            "-H 'originator: codex_cli_rs' "
            "-H 'OpenAI-Beta: responses=experimental' "
            "-H 'Chatgpt-Account-Id: %s' "
            "--data-binary @%s 2>/dev/null",
            url,
            profile->access_token,
            profile->account_id,
            body_path
        );
    } else if (is_codex) {
        snprintf(
            command,
            sizeof(command),
            "curl -sS -N -X POST '%s/responses' "
            "-H 'Content-Type: application/json' "
            "-H 'Authorization: Bearer %s' "
            "-H 'originator: codex_cli_rs' "
            "-H 'OpenAI-Beta: responses=experimental' "
            "--data-binary @%s 2>/dev/null",
            url,
            profile->access_token,
            body_path
        );
    } else {
        snprintf(
            command,
            sizeof(command),
            "curl -sS -X POST '%s/responses' "
            "-H 'Content-Type: application/json' "
            "-H 'Authorization: Bearer %s' "
            "--data-binary @%s 2>/dev/null",
            url,
            profile->access_token,
            body_path
        );
    }

    pipe = popen(command, "r");
    if (pipe == NULL) {
        copy_text(error_text, error_text_size, "Failed to spawn curl.");
        unlink(body_path);
        return -1;
    }
    while (total_read + 1 < response_size) {
        size_t n = fread(response + total_read, 1, response_size - 1 - total_read, pipe);
        if (n == 0) {
            break;
        }
        total_read += n;
    }
    while (fread(discard, 1, sizeof(discard), pipe) > 0) {
    }
    response[total_read] = '\0';
    pclose(pipe);
    unlink(body_path);
    if (is_codex && total_read > 0) {
        char *completed = (char *)calloc(1, response_size);

        if (completed != NULL && espclaw_agent_extract_sse_completed_response_json(response, completed, response_size) == 0) {
            copy_text(response, response_size, completed);
            total_read = strlen(response);
        }
        free(completed);
    }
    if (total_read == 0) {
        copy_text(error_text, error_text_size, "Provider returned an empty response.");
    }
    return total_read > 0 ? 0 : -1;
}

static bool ascii_equal_ignore_case(char left, char right)
{
    return tolower((unsigned char)left) == tolower((unsigned char)right);
}

static bool headers_contain_value(
    const char *headers,
    size_t headers_len,
    const char *name,
    const char *value
)
{
    const char *cursor = headers;
    size_t name_len = strlen(name);
    size_t value_len = strlen(value);

    while (cursor != NULL && cursor < headers + headers_len) {
        const char *line_end = strstr(cursor, "\r\n");
        const char *colon;
        size_t line_len;

        if (line_end == NULL || line_end > headers + headers_len) {
            line_end = headers + headers_len;
        }
        line_len = (size_t)(line_end - cursor);
        colon = memchr(cursor, ':', line_len);
        if (colon != NULL && (size_t)(colon - cursor) == name_len) {
            size_t index;
            const char *value_start = colon + 1;

            for (index = 0; index < name_len; ++index) {
                if (!ascii_equal_ignore_case(cursor[index], name[index])) {
                    break;
                }
            }
            if (index == name_len) {
                while (value_start < line_end && isspace((unsigned char)*value_start)) {
                    value_start++;
                }
                while (value_start + value_len <= line_end) {
                    size_t value_index;

                    for (value_index = 0; value_index < value_len; ++value_index) {
                        if (!ascii_equal_ignore_case(value_start[value_index], value[value_index])) {
                            break;
                        }
                    }
                    if (value_index == value_len) {
                        return true;
                    }
                    value_start++;
                }
            }
        }

        if (line_end >= headers + headers_len) {
            break;
        }
        cursor = line_end + 2;
    }
    return false;
}

static int decode_chunked_body_in_place(char *buffer, size_t *buffer_len, char *error_text, size_t error_text_size)
{
    char *read_cursor = buffer;
    char *write_cursor = buffer;
    char *buffer_end = buffer + *buffer_len;

    while (read_cursor < buffer_end) {
        char *line_end = strstr(read_cursor, "\r\n");
        unsigned long chunk_len = 0;
        char *parse_end = NULL;

        if (line_end == NULL || line_end > buffer_end) {
            copy_text(error_text, error_text_size, "Provider returned a malformed chunked body.");
            return -1;
        }

        *line_end = '\0';
        chunk_len = strtoul(read_cursor, &parse_end, 16);
        if (parse_end == read_cursor || (*parse_end != '\0' && *parse_end != ';')) {
            copy_text(error_text, error_text_size, "Provider returned an invalid chunk length.");
            return -1;
        }
        read_cursor = line_end + 2;

        if (chunk_len == 0) {
            *buffer_len = (size_t)(write_cursor - buffer);
            buffer[*buffer_len] = '\0';
            return 0;
        }

        if (chunk_len > (unsigned long)(buffer_end - read_cursor)) {
            copy_text(error_text, error_text_size, "Provider chunk exceeded received body size.");
            return -1;
        }

        if (write_cursor != read_cursor) {
            memmove(write_cursor, read_cursor, chunk_len);
        }
        write_cursor += chunk_len;
        read_cursor += chunk_len;

        if (read_cursor + 2 > buffer_end || read_cursor[0] != '\r' || read_cursor[1] != '\n') {
            copy_text(error_text, error_text_size, "Provider chunk delimiter was malformed.");
            return -1;
        }
        read_cursor += 2;
    }

    copy_text(error_text, error_text_size, "Provider chunked body ended unexpectedly.");
    return -1;
}

int espclaw_agent_extract_http_body_in_place(char *buffer, size_t *buffer_len, char *error_text, size_t error_text_size)
{
    char *header_end;
    int status_code = 0;
    char *body;
    size_t body_len;
    bool is_chunked = false;

    if (buffer == NULL || buffer_len == NULL) {
        return -1;
    }

    header_end = strstr(buffer, "\r\n\r\n");
    if (header_end == NULL) {
        copy_text(error_text, error_text_size, "Provider returned a malformed HTTP response.");
        return -1;
    }
    if (sscanf(buffer, "HTTP/%*u.%*u %d", &status_code) != 1) {
        copy_text(error_text, error_text_size, "Provider returned an invalid HTTP status line.");
        return -1;
    }
    is_chunked = headers_contain_value(buffer, (size_t)(header_end - buffer), "Transfer-Encoding", "chunked");

    body = header_end + 4;
    body_len = *buffer_len - (size_t)(body - buffer);
    memmove(buffer, body, body_len);
    buffer[body_len] = '\0';
    *buffer_len = body_len;

    if (status_code < 200 || status_code >= 300) {
        char snippet[160];
        size_t snippet_len = body_len < sizeof(snippet) - 1 ? body_len : sizeof(snippet) - 1;
        size_t i;

        memcpy(snippet, buffer, snippet_len);
        snippet[snippet_len] = '\0';
        for (i = 0; i < snippet_len; ++i) {
            if (snippet[i] == '\r' || snippet[i] == '\n') {
                snippet[i] = ' ';
            }
        }
        snprintf(error_text, error_text_size, "Provider returned HTTP %d: %s", status_code, snippet);
        return -1;
    }

    if (is_chunked && decode_chunked_body_in_place(buffer, buffer_len, error_text, error_text_size) != 0) {
        return -1;
    }

    return 0;
}

int espclaw_agent_reduce_sse_stream_to_response_json(
    const char *payload,
    char *buffer,
    size_t buffer_size,
    char *error_text,
    size_t error_text_size
)
{
    char *http_buffer;
    size_t http_len;
    char response_id[ESPCLAW_AGENT_RESPONSE_ID_MAX + 1];
    char text[ESPCLAW_AGENT_TEXT_MAX + 1];
    bool has_tools = false;

    if (payload == NULL || buffer == NULL || buffer_size == 0) {
        return -1;
    }

    http_buffer = calloc(1, strlen(payload) + 1);
    if (http_buffer == NULL) {
        copy_text(error_text, error_text_size, "Failed to allocate provider SSE buffer.");
        return -1;
    }

    strcpy(http_buffer, payload);
    http_len = strlen(http_buffer);
    if (espclaw_agent_extract_http_body_in_place(http_buffer, &http_len, error_text, error_text_size) != 0) {
        free(http_buffer);
        return -1;
    }
    if (espclaw_agent_extract_sse_completed_response_json(http_buffer, buffer, buffer_size) == 0) {
        char normalized[ESPCLAW_AGENT_TEXT_MAX * 4];
        char streamed_text[ESPCLAW_AGENT_TEXT_MAX + 1];

        if (extract_json_object_from(buffer, "response", NULL, normalized, sizeof(normalized))) {
            copy_text(buffer, buffer_size, normalized);
        }
        if (parse_provider_text_response(buffer, response_id, sizeof(response_id), text, sizeof(text), &has_tools) != 0 &&
            !has_tools) {
            streamed_text[0] = '\0';
            append_sse_output_text_segments(http_buffer, streamed_text, sizeof(streamed_text));
            if (streamed_text[0] != '\0') {
                synthesize_text_response_json(response_id, streamed_text, buffer, buffer_size);
            }
        }
        free(http_buffer);
        return 0;
    }
    if (parse_provider_text_response(http_buffer, response_id, sizeof(response_id), text, sizeof(text), &has_tools) != 0 ||
        text[0] == '\0') {
        free(http_buffer);
        copy_text(error_text, error_text_size, "Provider stream ended before response.completed.");
        return -1;
    }

    snprintf(
        buffer,
        buffer_size,
        "{"
        "\"id\":\"%s\","
        "\"status\":\"completed\","
        "\"output\":[{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":",
        response_id
    );
    {
        size_t used = strlen(buffer);

        used = append_escaped_json(buffer, buffer_size, used, text);
        if (used < buffer_size - 4) {
            snprintf(buffer + used, buffer_size - used, "}]}]}");
        }
    }

    free(http_buffer);
    return 0;
}
#else
static const unsigned char CHATGPT_LE_E7_SHA256[32] __attribute__((unused)) = {
    0xae, 0xb1, 0xfd, 0x74, 0x10, 0xe8, 0x3b, 0xc9,
    0x6f, 0x5d, 0xa3, 0xc6, 0xa7, 0xc2, 0xc1, 0xbb,
    0x83, 0x6d, 0x1f, 0xa5, 0xcb, 0x86, 0xe7, 0x08,
    0x51, 0x58, 0x90, 0xe4, 0x28, 0xa8, 0x77, 0x0b,
};

static bool __attribute__((unused)) cert_chain_contains_sha256(const mbedtls_x509_crt *cert, const unsigned char *expected_digest, size_t expected_len)
{
    unsigned char digest[32];

    while (cert != NULL) {
        if (expected_len == sizeof(digest) &&
            mbedtls_sha256(cert->raw.p, cert->raw.len, digest, 0) == 0 &&
            memcmp(digest, expected_digest, sizeof(digest)) == 0) {
            return true;
        }
        cert = cert->next;
    }
    return false;
}

static bool ascii_equal_ignore_case(char left, char right)
{
    return tolower((unsigned char)left) == tolower((unsigned char)right);
}

static bool headers_contain_value(
    const char *headers,
    size_t headers_len,
    const char *name,
    const char *value
)
{
    const char *cursor = headers;
    size_t name_len = strlen(name);
    size_t value_len = strlen(value);

    while (cursor != NULL && cursor < headers + headers_len) {
        const char *line_end = strstr(cursor, "\r\n");
        const char *colon;
        size_t line_len;

        if (line_end == NULL || line_end > headers + headers_len) {
            line_end = headers + headers_len;
        }
        line_len = (size_t)(line_end - cursor);
        colon = memchr(cursor, ':', line_len);
        if (colon != NULL && (size_t)(colon - cursor) == name_len) {
            size_t index;
            const char *value_start = colon + 1;

            for (index = 0; index < name_len; ++index) {
                if (!ascii_equal_ignore_case(cursor[index], name[index])) {
                    break;
                }
            }
            if (index == name_len) {
                while (value_start < line_end && isspace((unsigned char)*value_start)) {
                    value_start++;
                }
                while (value_start + value_len <= line_end) {
                    size_t value_index;

                    for (value_index = 0; value_index < value_len; ++value_index) {
                        if (!ascii_equal_ignore_case(value_start[value_index], value[value_index])) {
                            break;
                        }
                    }
                    if (value_index == value_len) {
                        return true;
                    }
                    value_start++;
                }
            }
        }

        if (line_end >= headers + headers_len) {
            break;
        }
        cursor = line_end + 2;
    }
    return false;
}

static int decode_chunked_body_in_place(char *buffer, size_t *buffer_len, char *error_text, size_t error_text_size)
{
    char *read_cursor = buffer;
    char *write_cursor = buffer;
    char *buffer_end = buffer + *buffer_len;

    while (read_cursor < buffer_end) {
        char *line_end = strstr(read_cursor, "\r\n");
        unsigned long chunk_len = 0;
        char *parse_end = NULL;

        if (line_end == NULL || line_end > buffer_end) {
            copy_text(error_text, error_text_size, "Provider returned a malformed chunked body.");
            return -1;
        }

        *line_end = '\0';
        chunk_len = strtoul(read_cursor, &parse_end, 16);
        if (parse_end == read_cursor || (*parse_end != '\0' && *parse_end != ';')) {
            copy_text(error_text, error_text_size, "Provider returned an invalid chunk length.");
            return -1;
        }
        read_cursor = line_end + 2;

        if (chunk_len == 0) {
            *buffer_len = (size_t)(write_cursor - buffer);
            buffer[*buffer_len] = '\0';
            return 0;
        }

        if (chunk_len > (unsigned long)(buffer_end - read_cursor)) {
            copy_text(error_text, error_text_size, "Provider chunk exceeded received body size.");
            return -1;
        }

        if (write_cursor != read_cursor) {
            memmove(write_cursor, read_cursor, chunk_len);
        }
        write_cursor += chunk_len;
        read_cursor += chunk_len;

        if (read_cursor + 2 > buffer_end || read_cursor[0] != '\r' || read_cursor[1] != '\n') {
            copy_text(error_text, error_text_size, "Provider chunk delimiter was malformed.");
            return -1;
        }
        read_cursor += 2;
    }

    copy_text(error_text, error_text_size, "Provider chunked body ended unexpectedly.");
    return -1;
}

int espclaw_agent_extract_http_body_in_place(char *buffer, size_t *buffer_len, char *error_text, size_t error_text_size)
{
    char *header_end;
    int status_code = 0;
    char *body;
    size_t body_len;
    bool is_chunked = false;

    if (buffer == NULL || buffer_len == NULL) {
        return -1;
    }

    header_end = strstr(buffer, "\r\n\r\n");
    if (header_end == NULL) {
        copy_text(error_text, error_text_size, "Provider returned a malformed HTTP response.");
        return -1;
    }
    if (sscanf(buffer, "HTTP/%*u.%*u %d", &status_code) != 1) {
        copy_text(error_text, error_text_size, "Provider returned an invalid HTTP status line.");
        return -1;
    }
    is_chunked = headers_contain_value(buffer, (size_t)(header_end - buffer), "Transfer-Encoding", "chunked");

    body = header_end + 4;
    body_len = *buffer_len - (size_t)(body - buffer);
    memmove(buffer, body, body_len);
    buffer[body_len] = '\0';
    *buffer_len = body_len;

    if (status_code < 200 || status_code >= 300) {
        char snippet[160];
        size_t snippet_len = body_len < sizeof(snippet) - 1 ? body_len : sizeof(snippet) - 1;
        size_t i;

        memcpy(snippet, buffer, snippet_len);
        snippet[snippet_len] = '\0';
        for (i = 0; i < snippet_len; ++i) {
            if (snippet[i] == '\r' || snippet[i] == '\n') {
                snippet[i] = ' ';
            }
        }
        snprintf(error_text, error_text_size, "Provider returned HTTP %d: %s", status_code, snippet);
        return -1;
    }

    if (is_chunked && decode_chunked_body_in_place(buffer, buffer_len, error_text, error_text_size) != 0) {
        return -1;
    }

    return 0;
}

typedef struct {
    bool headers_done;
    bool is_chunked;
    bool saw_completed;
    bool saw_output_text_delta;
    bool chunk_needs_crlf;
    bool chunk_trailer;
    bool line_has_content;
    bool line_value_started;
    int status_code;
    size_t chunk_remaining;
    char response_id[ESPCLAW_AGENT_RESPONSE_ID_MAX + 1];
    char headers[4096];
    size_t headers_len;
    char line_prefix[7];
    size_t line_prefix_len;
    unsigned int line_mode;
    char event[96];
    size_t event_len;
    char chunk_line[32];
    size_t chunk_line_len;
    char output_text[ESPCLAW_AGENT_TEXT_MAX + 1];
    char *data;
    size_t data_len;
    size_t data_capacity;
} espclaw_codex_sse_parser_t;

enum {
    ESPCLAW_SSE_LINE_PREFIX = 0,
    ESPCLAW_SSE_LINE_EVENT = 1,
    ESPCLAW_SSE_LINE_DATA = 2,
    ESPCLAW_SSE_LINE_IGNORE = 3
};

static void codex_sse_reset_line(espclaw_codex_sse_parser_t *parser)
{
    if (parser == NULL) {
        return;
    }

    parser->line_has_content = false;
    parser->line_value_started = false;
    parser->line_prefix_len = 0;
    parser->line_mode = ESPCLAW_SSE_LINE_PREFIX;
    parser->event_len = 0;
    parser->line_prefix[0] = '\0';
}

static void codex_sse_reset_event(espclaw_codex_sse_parser_t *parser)
{
    if (parser == NULL) {
        return;
    }
    parser->event[0] = '\0';
    parser->event_len = 0;
    parser->data_len = 0;
    if (parser->data != NULL && parser->data_capacity > 0) {
        parser->data[0] = '\0';
    }
}

static bool codex_sse_append_data_char(
    espclaw_codex_sse_parser_t *parser,
    char value,
    char *error_text,
    size_t error_text_size
)
{
    size_t needed = 0;

    if (parser == NULL || parser->data == NULL) {
        return false;
    }

    if (!parser->line_value_started && parser->data_len > 0) {
        needed++;
    }
    needed += parser->data_len + 1;
    if (needed >= parser->data_capacity) {
        copy_text(error_text, error_text_size, "Provider SSE event exceeded the embedded receive buffer.");
        return false;
    }

    if (!parser->line_value_started && parser->data_len > 0) {
        parser->data[parser->data_len++] = '\n';
    }
    parser->data[parser->data_len++] = value;
    parser->data[parser->data_len] = '\0';
    parser->line_value_started = true;
    return true;
}

static bool codex_sse_append_event_char(
    espclaw_codex_sse_parser_t *parser,
    char value
)
{
    if (parser == NULL) {
        return false;
    }
    if (!parser->line_value_started) {
        parser->event_len = 0;
        parser->event[0] = '\0';
    }
    if (parser->event_len + 1 >= sizeof(parser->event)) {
        return false;
    }

    parser->event[parser->event_len++] = value;
    parser->event[parser->event_len] = '\0';
    parser->line_value_started = true;
    return true;
}

static void codex_sse_commit_empty_data_line(espclaw_codex_sse_parser_t *parser)
{
    if (parser == NULL || parser->data == NULL || parser->line_mode != ESPCLAW_SSE_LINE_DATA || parser->line_value_started) {
        return;
    }
    if (parser->data_len > 0 && parser->data_len + 1 < parser->data_capacity) {
        parser->data[parser->data_len++] = '\n';
        parser->data[parser->data_len] = '\0';
    }
}

static unsigned int codex_sse_next_line_mode(const char *prefix, size_t prefix_len)
{
    if (prefix_len == 0U) {
        return ESPCLAW_SSE_LINE_PREFIX;
    }
    if (strncmp("event:", prefix, prefix_len) == 0) {
        return prefix_len == strlen("event:") ? ESPCLAW_SSE_LINE_EVENT : ESPCLAW_SSE_LINE_PREFIX;
    }
    if (strncmp("data:", prefix, prefix_len) == 0) {
        return prefix_len == strlen("data:") ? ESPCLAW_SSE_LINE_DATA : ESPCLAW_SSE_LINE_PREFIX;
    }
    return ESPCLAW_SSE_LINE_IGNORE;
}

static int codex_sse_finalize_event(
    espclaw_codex_sse_parser_t *parser,
    char *buffer,
    size_t buffer_size,
    char *error_text,
    size_t error_text_size
)
{
    char snippet[160];

    if (parser == NULL) {
        return -1;
    }
    if (parser->event[0] == '\0' && parser->data_len == 0) {
        return 0;
    }

    if (parser->response_id[0] == '\0') {
        extract_json_string_from(parser->data, "id", NULL, parser->response_id, sizeof(parser->response_id));
    }

    if (strcmp(parser->event, "response.output_text.delta") == 0) {
        char segment[1024];

        if (extract_json_string_from(parser->data, "delta", NULL, segment, sizeof(segment))) {
            append_text_segment(parser->output_text, sizeof(parser->output_text), segment);
            parser->saw_output_text_delta = true;
        }
        codex_sse_reset_event(parser);
        return 0;
    }

    if (strcmp(parser->event, "response.output_text.done") == 0) {
        char segment[1024];

        if (!parser->saw_output_text_delta &&
            extract_json_string_from(parser->data, "text", NULL, segment, sizeof(segment))) {
            append_text_segment(parser->output_text, sizeof(parser->output_text), segment);
        }
        codex_sse_reset_event(parser);
        return 0;
    }

    if (strcmp(parser->event, "response.completed") == 0) {
        char parsed_id[ESPCLAW_AGENT_RESPONSE_ID_MAX + 1];
        char parsed_text[ESPCLAW_AGENT_TEXT_MAX + 1];
        bool has_tools = false;
        const char *terminal_json = parser->data;
        size_t terminal_json_len = parser->data_len;
        char *normalized = NULL;

        normalized = (char *)calloc(1, buffer_size);
        if (normalized != NULL &&
            extract_json_object_from(parser->data, "response", NULL, normalized, buffer_size)) {
            terminal_json = normalized;
            terminal_json_len = strlen(normalized);
        }
        if (terminal_json_len + 1 > buffer_size) {
            free(normalized);
            copy_text(error_text, error_text_size, "Provider completed response exceeded the embedded receive buffer.");
            return -1;
        }
        if (espclaw_agent_store_terminal_response_json(terminal_json, terminal_json_len, buffer, buffer_size) != 0) {
            free(normalized);
            copy_text(error_text, error_text_size, "Failed to preserve the provider completed response.");
            return -1;
        }
        if (parse_provider_text_response(
                buffer,
                parsed_id,
                sizeof(parsed_id),
                parsed_text,
                sizeof(parsed_text),
                &has_tools) != 0 &&
            parser->output_text[0] != '\0' &&
            !has_tools) {
            const char *response_id = parser->response_id[0] != '\0' ? parser->response_id : parsed_id;

            synthesize_text_response_json(response_id, parser->output_text, buffer, buffer_size);
        }
        free(normalized);
        parser->saw_completed = true;
        /* On embedded raw-Codex paths the parser scratch space aliases the final
         * response buffer, so resetting the event here would wipe the completed
         * JSON we just preserved. The stream ends immediately after this. */
        return 1;
    }

    if (strcmp(parser->event, "response.failed") == 0 ||
        strcmp(parser->event, "response.incomplete") == 0 ||
        strcmp(parser->event, "error") == 0) {
        size_t snippet_len = parser->data_len < sizeof(snippet) - 1 ? parser->data_len : sizeof(snippet) - 1;
        size_t index;

        memcpy(snippet, parser->data, snippet_len);
        snippet[snippet_len] = '\0';
        for (index = 0; index < snippet_len; ++index) {
            if (snippet[index] == '\r' || snippet[index] == '\n') {
                snippet[index] = ' ';
            }
        }
        snprintf(error_text, error_text_size, "Provider emitted %s: %s", parser->event, snippet);
        return -1;
    }

    codex_sse_reset_event(parser);
    return 0;
}

static int codex_sse_feed_http_headers(
    espclaw_codex_sse_parser_t *parser,
    const char *chunk,
    size_t chunk_len,
    size_t *consumed_out,
    char *error_text,
    size_t error_text_size
)
{
    const char *header_end;
    size_t copy_len = chunk_len;

    if (parser == NULL || chunk == NULL || consumed_out == NULL) {
        return -1;
    }

    if (parser->headers_len + copy_len >= sizeof(parser->headers)) {
        copy_text(error_text, error_text_size, "Provider HTTP headers exceeded the embedded parser limit.");
        return -1;
    }

    memcpy(parser->headers + parser->headers_len, chunk, copy_len);
    parser->headers_len += copy_len;
    parser->headers[parser->headers_len] = '\0';

    header_end = strstr(parser->headers, "\r\n\r\n");
    if (header_end == NULL) {
        *consumed_out = chunk_len;
        return 0;
    }

    if (sscanf(parser->headers, "HTTP/%*u.%*u %d", &parser->status_code) != 1) {
        copy_text(error_text, error_text_size, "Provider returned an invalid HTTP status line.");
        return -1;
    }
    parser->is_chunked = headers_contain_value(
        parser->headers,
        (size_t)(header_end - parser->headers),
        "Transfer-Encoding",
        "chunked"
    );
    if (parser->status_code < 200 || parser->status_code >= 300) {
        char body_snippet[160];
        const char *body = header_end + 4;
        size_t body_len = strlen(body);
        size_t snippet_len = body_len < sizeof(body_snippet) - 1 ? body_len : sizeof(body_snippet) - 1;
        size_t index;

        memcpy(body_snippet, body, snippet_len);
        body_snippet[snippet_len] = '\0';
        for (index = 0; index < snippet_len; ++index) {
            if (body_snippet[index] == '\r' || body_snippet[index] == '\n') {
                body_snippet[index] = ' ';
            }
        }
        snprintf(error_text, error_text_size, "Provider returned HTTP %d: %s", parser->status_code, body_snippet);
        return -1;
    }

    parser->headers_done = true;
    *consumed_out = (size_t)((header_end + 4) - parser->headers) - (parser->headers_len - chunk_len);
    return 0;
}

static int codex_sse_feed_body_bytes(
    espclaw_codex_sse_parser_t *parser,
    const char *chunk,
    size_t chunk_len,
    char *buffer,
    size_t buffer_size,
    char *error_text,
    size_t error_text_size
)
{
    size_t index;

    if (parser == NULL || chunk == NULL) {
        return -1;
    }

    for (index = 0; index < chunk_len; ++index) {
        char ch = chunk[index];

        if (ch == '\r') {
            continue;
        }

        if (ch == '\n') {
            if (!parser->line_has_content) {
                int finalize = codex_sse_finalize_event(parser, buffer, buffer_size, error_text, error_text_size);

                if (finalize != 0) {
                    return finalize;
                }
                codex_sse_reset_line(parser);
                continue;
            }

            codex_sse_commit_empty_data_line(parser);
            codex_sse_reset_line(parser);
            continue;
        }

        parser->line_has_content = true;
        if (parser->line_mode == ESPCLAW_SSE_LINE_PREFIX) {
            if (parser->line_prefix_len + 1 >= sizeof(parser->line_prefix)) {
                parser->line_mode = ESPCLAW_SSE_LINE_IGNORE;
            } else {
                parser->line_prefix[parser->line_prefix_len++] = ch;
                parser->line_prefix[parser->line_prefix_len] = '\0';
                parser->line_mode = codex_sse_next_line_mode(parser->line_prefix, parser->line_prefix_len);
            }
            continue;
        }

        if (parser->line_mode == ESPCLAW_SSE_LINE_EVENT) {
            if (!parser->line_value_started && ch == ' ') {
                continue;
            }
            if (!codex_sse_append_event_char(parser, ch)) {
                copy_text(error_text, error_text_size, "Provider SSE event name exceeded the embedded parser limit.");
                return -1;
            }
            continue;
        }

        if (parser->line_mode == ESPCLAW_SSE_LINE_DATA) {
            if (!parser->line_value_started && ch == ' ') {
                continue;
            }
            if (!codex_sse_append_data_char(parser, ch, error_text, error_text_size)) {
                return -1;
            }
            continue;
        }
    }

    return 0;
}

static int __attribute__((unused)) codex_sse_feed_chunked_bytes(
    espclaw_codex_sse_parser_t *parser,
    const char *chunk,
    size_t chunk_len,
    char *buffer,
    size_t buffer_size,
    char *error_text,
    size_t error_text_size
)
{
    size_t index = 0;

    if (parser == NULL || chunk == NULL) {
        return -1;
    }

    while (index < chunk_len) {
        if (parser->chunk_remaining > 0) {
            size_t available = chunk_len - index;
            size_t to_consume = available < parser->chunk_remaining ? available : parser->chunk_remaining;
            int feed_result = codex_sse_feed_body_bytes(
                parser,
                chunk + index,
                to_consume,
                buffer,
                buffer_size,
                error_text,
                error_text_size
            );

            if (feed_result != 0) {
                return feed_result;
            }
            parser->chunk_remaining -= to_consume;
            index += to_consume;
            if (parser->chunk_remaining == 0) {
                parser->chunk_needs_crlf = true;
            }
            continue;
        }

        if (parser->chunk_needs_crlf) {
            char ch = chunk[index++];

            if (ch == '\r') {
                continue;
            }
            if (ch != '\n') {
                copy_text(error_text, error_text_size, "Provider chunk delimiter was malformed.");
                return -1;
            }
            parser->chunk_needs_crlf = false;
            continue;
        }

        if (parser->chunk_trailer) {
            /* Ignore the terminating CRLF/trailers after the zero-sized chunk. */
            index = chunk_len;
            break;
        }

        {
            char ch = chunk[index++];

            if (ch == '\r') {
                continue;
            }
            if (ch != '\n') {
                if (parser->chunk_line_len + 1 >= sizeof(parser->chunk_line)) {
                    copy_text(error_text, error_text_size, "Provider chunk header exceeded the embedded parser limit.");
                    return -1;
                }
                parser->chunk_line[parser->chunk_line_len++] = ch;
                parser->chunk_line[parser->chunk_line_len] = '\0';
                continue;
            }

            if (parser->chunk_line_len == 0) {
                continue;
            }

            {
                char *parse_end = NULL;
                unsigned long chunk_size = strtoul(parser->chunk_line, &parse_end, 16);

                if (parse_end == parser->chunk_line || (*parse_end != '\0' && *parse_end != ';')) {
                    copy_text(error_text, error_text_size, "Provider returned an invalid chunk length.");
                    return -1;
                }
                parser->chunk_line_len = 0;
                parser->chunk_line[0] = '\0';
                if (chunk_size == 0) {
                    parser->chunk_trailer = true;
                    continue;
                }
                parser->chunk_remaining = (size_t)chunk_size;
            }
        }
    }

    return 0;
}

static int codex_sse_finish_stream(
    espclaw_codex_sse_parser_t *parser,
    char *buffer,
    size_t buffer_size,
    char *error_text,
    size_t error_text_size
)
{
    if (parser == NULL) {
        return -1;
    }
    if (!parser->headers_done) {
        copy_text(error_text, error_text_size, "Provider returned an incomplete HTTP response.");
        return -1;
    }

    if (parser->line_has_content) {
        codex_sse_commit_empty_data_line(parser);
        codex_sse_reset_line(parser);
    }

    if (parser->event[0] != '\0' || parser->data_len > 0) {
        int finalize = codex_sse_finalize_event(parser, buffer, buffer_size, error_text, error_text_size);

        if (finalize != 0) {
            return finalize > 0 ? 0 : -1;
        }
    }

    if (parser->saw_completed) {
        return 0;
    }
    copy_text(error_text, error_text_size, "Provider stream ended before response.completed.");
    return -1;
}

int espclaw_agent_reduce_sse_stream_to_response_json(
    const char *payload,
    char *buffer,
    size_t buffer_size,
    char *error_text,
    size_t error_text_size
)
{
    espclaw_codex_sse_parser_t *parser;
    size_t consumed = 0;
    int result;

    if (payload == NULL || buffer == NULL || buffer_size == 0) {
        return -1;
    }

    parser = calloc(1, sizeof(*parser));
    if (parser == NULL) {
        copy_text(error_text, error_text_size, "Failed to allocate provider SSE parser.");
        return -1;
    }
    parser->data = calloc(1, buffer_size);
    parser->data_capacity = buffer_size;
    if (parser->data == NULL) {
        free(parser);
        copy_text(error_text, error_text_size, "Failed to allocate provider SSE event buffer.");
        return -1;
    }

    result = codex_sse_feed_http_headers(parser, payload, strlen(payload), &consumed, error_text, error_text_size);
    if (result == 0 && parser->headers_done) {
        result = codex_sse_feed_body_bytes(
            parser,
            payload + consumed,
            strlen(payload) - consumed,
            buffer,
            buffer_size,
            error_text,
            error_text_size
        );
    }
    if (result == 0) {
        result = codex_sse_finish_stream(parser, buffer, buffer_size, error_text, error_text_size);
    }

    free(parser->data);
    free(parser);
    return result;
}

#if !defined(ESP_PLATFORM)
static int raw_codex_http_post(
    const char *endpoint,
    const espclaw_auth_profile_t *profile,
    const char *body,
    char *response,
    size_t response_size,
    char *error_text,
    size_t error_text_size
)
{
    static const char *PERSONALIZATION = "espclaw_codex";
    static const char *HOST = "chatgpt.com";
    static const char *REQUEST_LINE_FMT = "POST %s HTTP/1.1\r\n";
    static const char *CONTENT_LENGTH_FMT = "Content-Length: %u\r\n";
    mbedtls_net_context server_fd;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_entropy_context entropy;
    const mbedtls_x509_crt *peer_cert;
    const char *path = endpoint + strlen("https://chatgpt.com");
    espclaw_codex_sse_parser_t *parser = NULL;
    bool header_consumed = false;
    char line[256];
    char read_buffer[1024];
    const unsigned char *write_ptr = NULL;
    size_t write_remaining = 0;
    size_t body_written = 0;
    unsigned int body_write_count = 0;
    size_t total_read = 0;
    unsigned int read_count = 0;
    int ret = -1;
    int result = -1;

#define WRITE_LITERAL_OR_FAIL(literal, label)                                              \
    do {                                                                                   \
        write_ptr = (const unsigned char *)(literal);                                      \
        write_remaining = strlen((const char *)write_ptr);                                 \
        while (write_remaining > 0) {                                                      \
            ret = mbedtls_ssl_write(&ssl, write_ptr, write_remaining);                     \
            if (ret > 0) {                                                                 \
                write_ptr += ret;                                                          \
                write_remaining -= (size_t)ret;                                            \
                continue;                                                                  \
            }                                                                              \
            if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {  \
                snprintf(error_text, error_text_size, label ": -0x%04x", -ret);          \
                goto cleanup;                                                              \
            }                                                                              \
        }                                                                                  \
    } while (0)

    mbedtls_net_init(&server_fd);
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_entropy_init(&entropy);

    if (profile == NULL || body == NULL || response == NULL || response_size < 2 || path == NULL || *path == '\0') {
        copy_text(error_text, error_text_size, "Invalid Codex transport parameters.");
        goto cleanup;
    }

    ret = mbedtls_ctr_drbg_seed(
        &ctr_drbg,
        mbedtls_entropy_func,
        &entropy,
        (const unsigned char *)PERSONALIZATION,
        strlen(PERSONALIZATION)
    );
    if (ret != 0) {
        snprintf(error_text, error_text_size, "Codex TLS seed failed: -0x%04x", -ret);
        goto cleanup;
    }
    ret = mbedtls_net_connect(&server_fd, HOST, "443", MBEDTLS_NET_PROTO_TCP);
    if (ret != 0) {
        snprintf(error_text, error_text_size, "Codex TCP connect failed: -0x%04x", -ret);
        goto cleanup;
    }
    ret = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        snprintf(error_text, error_text_size, "Codex TLS config failed: -0x%04x", -ret);
        goto cleanup;
    }

    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &ctr_drbg);
    mbedtls_ssl_conf_min_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_read_timeout(&conf, 15000);

    ret = mbedtls_ssl_setup(&ssl, &conf);
    if (ret != 0) {
        snprintf(error_text, error_text_size, "Codex TLS setup failed: -0x%04x", -ret);
        goto cleanup;
    }
    ret = mbedtls_ssl_set_hostname(&ssl, HOST);
    if (ret != 0) {
        snprintf(error_text, error_text_size, "Codex SNI setup failed: -0x%04x", -ret);
        goto cleanup;
    }
    mbedtls_ssl_set_bio(&ssl, &server_fd, mbedtls_net_send, mbedtls_net_recv, mbedtls_net_recv_timeout);

    while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            snprintf(error_text, error_text_size, "Codex TLS handshake failed: -0x%04x", -ret);
            goto cleanup;
        }
    }
    ESP_LOGD(TAG, "raw Codex TLS handshake complete for %s", path);

    peer_cert = mbedtls_ssl_get_peer_cert(&ssl);
    if (peer_cert == NULL || !cert_chain_contains_sha256(peer_cert, CHATGPT_LE_E7_SHA256, sizeof(CHATGPT_LE_E7_SHA256))) {
        copy_text(error_text, error_text_size, "Codex TLS pinning failed: expected Let's Encrypt E7 in peer chain.");
        goto cleanup;
    }

    parser = calloc(1, sizeof(*parser));
    if (parser == NULL) {
        copy_text(error_text, error_text_size, "Failed to allocate provider SSE parser.");
        goto cleanup;
    }
    parser->data = response;
    parser->data_capacity = response_size;
    parser->data[0] = '\0';

    snprintf(line, sizeof(line), REQUEST_LINE_FMT, path);
    WRITE_LITERAL_OR_FAIL(line, "Codex TLS request-line write failed");
    WRITE_LITERAL_OR_FAIL("Host: ", "Codex TLS host header prefix write failed");
    WRITE_LITERAL_OR_FAIL(HOST, "Codex TLS host header host write failed");
    WRITE_LITERAL_OR_FAIL("\r\n", "Codex TLS host header suffix write failed");
    WRITE_LITERAL_OR_FAIL("User-Agent: ESPClaw/1.0\r\n", "Codex TLS user-agent write failed");
    WRITE_LITERAL_OR_FAIL("Accept: text/event-stream\r\n", "Codex TLS accept header write failed");
    WRITE_LITERAL_OR_FAIL("Content-Type: application/json\r\n", "Codex TLS content-type write failed");
    WRITE_LITERAL_OR_FAIL("Authorization: Bearer ", "Codex TLS auth header prefix write failed");
    WRITE_LITERAL_OR_FAIL(profile->access_token, "Codex TLS auth token write failed");
    WRITE_LITERAL_OR_FAIL("\r\n", "Codex TLS auth header suffix write failed");
    WRITE_LITERAL_OR_FAIL("originator: codex_cli_rs\r\n", "Codex TLS originator write failed");
    WRITE_LITERAL_OR_FAIL("OpenAI-Beta: responses=experimental\r\n", "Codex TLS beta header write failed");
    if (profile->account_id[0] != '\0') {
        WRITE_LITERAL_OR_FAIL("Chatgpt-Account-Id: ", "Codex TLS account header prefix write failed");
        WRITE_LITERAL_OR_FAIL(profile->account_id, "Codex TLS account id write failed");
        WRITE_LITERAL_OR_FAIL("\r\n", "Codex TLS account header suffix write failed");
    }
    snprintf(line, sizeof(line), CONTENT_LENGTH_FMT, (unsigned int)strlen(body));
    WRITE_LITERAL_OR_FAIL(line, "Codex TLS content-length write failed");
    WRITE_LITERAL_OR_FAIL("Connection: close\r\n\r\n", "Codex TLS connection header write failed");
    write_ptr = (const unsigned char *)body;
    write_remaining = strlen(body);
    while (write_remaining > 0) {
        size_t write_chunk = write_remaining > 1024 ? 1024 : write_remaining;

        ret = mbedtls_ssl_write(&ssl, write_ptr, write_chunk);
        if (ret > 0) {
            body_written += (size_t)ret;
            body_write_count++;
            write_ptr += ret;
            write_remaining -= (size_t)ret;
            continue;
        }
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            ESP_LOGE(
                TAG,
                "raw Codex body write failed after %u writes and %u/%u bytes: -0x%04x",
                body_write_count,
                (unsigned int)body_written,
                (unsigned int)strlen(body),
                -ret
            );
            snprintf(error_text, error_text_size, "Codex TLS body write failed: -0x%04x", -ret);
            goto cleanup;
        }
    }
    ESP_LOGD(
        TAG,
        "raw Codex request sent body_bytes=%u writes=%u",
        (unsigned int)strlen(body),
        body_write_count
    );

    response[0] = '\0';

    while (true) {
        ret = mbedtls_ssl_read(&ssl, (unsigned char *)read_buffer, sizeof(read_buffer) - 1);
        if (ret > 0) {
            size_t consumed = 0;
            int feed_result;

            total_read += (size_t)ret;
            read_count++;
            read_buffer[ret] = '\0';
            if (read_count == 1) {
                ESP_LOGD(TAG, "raw Codex first bytes received count=%d", ret);
            }
            if (!header_consumed) {
                feed_result = codex_sse_feed_http_headers(
                    parser,
                    read_buffer,
                    (size_t)ret,
                    &consumed,
                    error_text,
                    error_text_size
                );
                if (feed_result != 0) {
                    goto cleanup;
                }
                if (!parser->headers_done) {
                    continue;
                }
                header_consumed = true;
                ESP_LOGD(TAG, "raw Codex headers parsed status=%d chunked=%d", parser->status_code, parser->is_chunked ? 1 : 0);
            }

            if ((size_t)ret > consumed) {
                feed_result = parser->is_chunked
                                  ? codex_sse_feed_chunked_bytes(
                                        parser,
                                        read_buffer + consumed,
                                        (size_t)ret - consumed,
                                        response,
                                        response_size,
                                        error_text,
                                        error_text_size)
                                  : codex_sse_feed_body_bytes(
                                        parser,
                                        read_buffer + consumed,
                                        (size_t)ret - consumed,
                                        response,
                                        response_size,
                                        error_text,
                                        error_text_size);
                if (feed_result < 0) {
                    goto cleanup;
                }
                if (feed_result > 0) {
                    ESP_LOGD(TAG, "raw Codex completed after %u reads and %u bytes", read_count, (unsigned int)total_read);
                    result = 0;
                    goto cleanup;
                }
            }
            continue;
        }
        if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            if (codex_sse_finish_stream(parser, response, response_size, error_text, error_text_size) == 0) {
                ESP_LOGD(TAG, "raw Codex stream closed after %u reads and %u bytes", read_count, (unsigned int)total_read);
                result = 0;
            }
            goto cleanup;
        }
        if (ret == MBEDTLS_ERR_SSL_TIMEOUT) {
            if (codex_sse_finish_stream(parser, response, response_size, error_text, error_text_size) == 0) {
                ESP_LOGD(TAG, "raw Codex timed out after %u reads and %u bytes but produced a terminal response", read_count, (unsigned int)total_read);
                result = 0;
            } else if (error_text[0] == '\0') {
                copy_text(error_text, error_text_size, "Provider SSE read timed out before response.completed.");
            }
            ESP_LOGW(TAG, "raw Codex read timeout after %u reads and %u bytes", read_count, (unsigned int)total_read);
            goto cleanup;
        }
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            snprintf(error_text, error_text_size, "Codex TLS read failed: -0x%04x", -ret);
            goto cleanup;
        }
    }

cleanup:
    mbedtls_ssl_close_notify(&ssl);
    mbedtls_net_free(&server_fd);
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    if (parser != NULL) {
        free(parser);
    }
#undef WRITE_LITERAL_OR_FAIL
    return result;
}
#endif

static void configure_codex_transport(
    esp_http_client_config_t *config,
    const char *url,
    espclaw_codex_transport_mode_t mode
)
{
    if (config == NULL || url == NULL) {
        return;
    }

    if (strstr(url, "https://chatgpt.com") == url) {
        config->common_name = "chatgpt.com";
    }
#if defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE)
    config->crt_bundle_attach = esp_crt_bundle_attach;
    config->cert_pem = NULL;
    config->cert_len = 0;
#endif
    config->tls_version = ESP_HTTP_CLIENT_TLS_VER_ANY;

    if (mode == ESPCLAW_CODEX_TRANSPORT_BUNDLE_TLS12 ||
        mode == ESPCLAW_CODEX_TRANSPORT_CHAIN_TLS12 ||
        mode == ESPCLAW_CODEX_TRANSPORT_E7_TLS12) {
        config->tls_version = ESP_HTTP_CLIENT_TLS_VER_TLS_1_2;
    }
    if (mode == ESPCLAW_CODEX_TRANSPORT_CHAIN_TLS12) {
#if defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE)
        config->crt_bundle_attach = NULL;
#endif
        config->cert_pem = CHATGPT_LE_E7_CHAIN_PEM;
        config->cert_len = strlen(CHATGPT_LE_E7_CHAIN_PEM) + 1;
    } else if (mode == ESPCLAW_CODEX_TRANSPORT_E7_TLS12) {
#if defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE)
        config->crt_bundle_attach = NULL;
#endif
        config->cert_pem = chatgpt_root_x1_pem();
        config->cert_len = strlen(config->cert_pem) + 1;
    }
}

static int host_http_post(
    const char *url,
    const espclaw_auth_profile_t *profile,
    const char *body,
    char *response,
    size_t response_size,
    char *error_text,
    size_t error_text_size
)
{
    esp_http_client_handle_t client;
    char authorization[ESPCLAW_AUTH_TOKEN_MAX + 16];
    char endpoint[256];
    esp_err_t err = ESP_OK;
    bool is_codex = profile != NULL && strcmp(profile->provider_id, "openai_codex") == 0;
    bool is_chatgpt_codex = is_codex && strstr(url, "https://chatgpt.com") == url;
    int tls_code = 0;
    int tls_flags = 0;
    int attempt_count = 1;
    int attempt = 0;

    snprintf(endpoint, sizeof(endpoint), "%s/responses", url);
    if (is_chatgpt_codex) {
#if !defined(ESP_PLATFORM)
        if (raw_codex_http_post(endpoint, profile, body, response, response_size, error_text, error_text_size) == 0) {
            return 0;
        }
        ESP_LOGE(TAG, "raw Codex transport failed: %s", error_text);
#endif
    }
    for (attempt = 0; attempt < attempt_count; ++attempt) {
        espclaw_codex_transport_mode_t transport_mode =
            is_chatgpt_codex ? ESPCLAW_CODEX_TRANSPORT_CHAIN_TLS12 : (espclaw_codex_transport_mode_t)attempt;
        esp_http_client_config_t config = {
            .url = endpoint,
            .transport_type = HTTP_TRANSPORT_OVER_SSL,
            .timeout_ms = 30000,
            .buffer_size = 4096,
            .buffer_size_tx = 2048,
        };
        int total_read = 0;
        int status_code = 0;

        if (is_codex) {
            configure_codex_transport(&config, url, transport_mode);
            if (is_chatgpt_codex) {
                config.buffer_size = 8192;
            }
        }
#if defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE)
        else {
            config.crt_bundle_attach = esp_crt_bundle_attach;
        }
#endif

        client = esp_http_client_init(&config);
        if (client == NULL) {
            copy_text(error_text, error_text_size, "Failed to initialize HTTP client.");
            return -1;
        }

        esp_http_client_set_method(client, HTTP_METHOD_POST);
        esp_http_client_set_header(client, "Content-Type", "application/json");
        snprintf(authorization, sizeof(authorization), "Bearer %s", profile->access_token);
        esp_http_client_set_header(client, "Authorization", authorization);
        if (is_codex) {
            esp_http_client_set_header(client, "Accept", "text/event-stream");
            esp_http_client_set_header(client, "originator", "codex_cli_rs");
            esp_http_client_set_header(client, "OpenAI-Beta", "responses=experimental");
        }
        if (is_codex && profile->account_id[0] != '\0') {
            esp_http_client_set_header(client, "Chatgpt-Account-Id", profile->account_id);
        }
        if (is_codex) {
            espclaw_codex_sse_parser_t *parser = NULL;
            char read_buffer[512];
            int headers_result = 0;
            int write_offset = 0;
            bool retry_attempt = false;
            size_t body_length = strlen(body);

            response[0] = '\0';
            ESP_LOGI(
                TAG,
                "provider pre-open heap free=%u largest=%u body_bytes=%u response_cap=%u",
                (unsigned int)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                (unsigned int)body_length,
                (unsigned int)response_size
            );

            err = esp_http_client_open(client, (int)body_length);
            if (err != ESP_OK) {
                tls_code = 0;
                tls_flags = 0;
                esp_http_client_get_and_clear_last_tls_error(client, &tls_code, &tls_flags);
                espclaw_agent_format_transport_error(err, tls_code, tls_flags, error_text, error_text_size);
                ESP_LOGE(
                    TAG,
                    "provider open failed url=%s mode=%s err=%s (%d) tls_code=%d tls_flags=0x%x",
                    endpoint,
                    codex_transport_mode_name(transport_mode),
                    esp_err_to_name(err),
                    (int)err,
                    tls_code,
                    tls_flags
                );
                free(parser);
                esp_http_client_cleanup(client);
                if (attempt + 1 >= attempt_count) {
                    return -1;
                }
                continue;
            }

            while (write_offset < (int)body_length) {
                int written_now = esp_http_client_write(client, body + write_offset, (int)body_length - write_offset);

                if (written_now <= 0) {
                    copy_text(error_text, error_text_size, "Provider request body upload failed.");
                    ESP_LOGE(
                        TAG,
                        "provider body write failed url=%s mode=%s offset=%d length=%u",
                        endpoint,
                        codex_transport_mode_name(transport_mode),
                        write_offset,
                        (unsigned int)body_length
                    );
                    free(parser);
                    esp_http_client_close(client);
                    esp_http_client_cleanup(client);
                    if (attempt + 1 >= attempt_count) {
                        return -1;
                    }
                    retry_attempt = true;
                    break;
                }
                write_offset += written_now;
            }
            if (retry_attempt) {
                continue;
            }

            headers_result = esp_http_client_fetch_headers(client);
            status_code = esp_http_client_get_status_code(client);
            ESP_LOGI(
                TAG,
                "provider headers fetched url=%s mode=%s status=%d content_length=%d chunked=%d heap_free=%u largest=%u",
                endpoint,
                codex_transport_mode_name(transport_mode),
                status_code,
                headers_result,
                esp_http_client_is_chunked_response(client) ? 1 : 0,
                (unsigned int)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)
            );
            if (headers_result < 0 && headers_result != -ESP_ERR_HTTP_EAGAIN) {
                copy_text(error_text, error_text_size, "Provider failed while reading HTTP headers.");
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                if (attempt + 1 >= attempt_count) {
                    return -1;
                }
                continue;
            }
            if (status_code < 200 || status_code >= 300) {
                while (total_read < (int)response_size - 1) {
                    int read_now = esp_http_client_read(client, response + total_read, (int)response_size - 1 - total_read);

                    if (read_now <= 0) {
                        break;
                    }
                    total_read += read_now;
                }
                response[total_read] = '\0';
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                {
                    char snippet[160];
                    size_t snippet_len = 0;

                    while (snippet_len < sizeof(snippet) - 1 && snippet_len < (size_t)total_read) {
                        char c = response[snippet_len];

                        snippet[snippet_len] = (c == '\r' || c == '\n') ? ' ' : c;
                        snippet_len++;
                    }
                    snippet[snippet_len] = '\0';
                    snprintf(
                        error_text,
                        error_text_size,
                        "Provider returned HTTP %d%s%s",
                        status_code,
                        snippet[0] != '\0' ? ": " : "",
                        snippet
                    );
                    ESP_LOGE(TAG, "provider http status=%d body=%s", status_code, snippet);
                }
                return -1;
            }

            parser = calloc(1, sizeof(*parser));
            if (parser == NULL) {
                copy_text(error_text, error_text_size, "Failed to allocate provider SSE parser.");
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                return -1;
            }
            /* The esp_http_client path has already completed HTTP header parsing
             * via esp_http_client_fetch_headers(), so the embedded SSE reducer
             * must start in body mode or codex_sse_finish_stream() will reject
             * the completed stream as an incomplete HTTP response. */
            parser->headers_done = true;
            parser->status_code = status_code;
            parser->is_chunked = esp_http_client_is_chunked_response(client);
            parser->data = response;
            parser->data_capacity = response_size;

            while (true) {
                int read_now = esp_http_client_read(client, read_buffer, sizeof(read_buffer));
                int feed_result = 0;

                if (read_now < 0) {
                    copy_text(error_text, error_text_size, "Provider SSE read failed.");
                    free(parser);
                    esp_http_client_close(client);
                    esp_http_client_cleanup(client);
                    if (attempt + 1 >= attempt_count) {
                        return -1;
                    }
                    retry_attempt = true;
                    break;
                }
                if (read_now == 0) {
                    if (headers_result > 0 && total_read >= headers_result) {
                        break;
                    }
                    if ((headers_result == 0 || headers_result == -ESP_ERR_HTTP_EAGAIN) &&
                        esp_http_client_is_complete_data_received(client)) {
                        break;
                    }
                    continue;
                }
                total_read += read_now;
                if (total_read == read_now) {
                    ESP_LOGI(TAG, "provider first body bytes received count=%d", read_now);
                }
                feed_result = codex_sse_feed_body_bytes(
                    parser,
                    read_buffer,
                    (size_t)read_now,
                    response,
                    response_size,
                    error_text,
                    error_text_size
                );
                if (feed_result < 0) {
                    free(parser);
                    esp_http_client_close(client);
                    esp_http_client_cleanup(client);
                    if (attempt + 1 >= attempt_count) {
                        return -1;
                    }
                    retry_attempt = true;
                    break;
                }
                if (feed_result > 0) {
                    ESP_LOGI(TAG, "provider completed after %d body bytes", total_read);
                    break;
                }
            }
            if (retry_attempt) {
                continue;
            }

            err = codex_sse_finish_stream(parser, response, response_size, error_text, error_text_size);
            free(parser);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            if (err == 0) {
                return 0;
            }
            if (attempt + 1 >= attempt_count) {
                return -1;
            }
            continue;
        }

        esp_http_client_set_post_field(client, body, (int)strlen(body));
        err = esp_http_client_perform(client);
        if (err != ESP_OK) {
            tls_code = 0;
            tls_flags = 0;
            esp_http_client_get_and_clear_last_tls_error(client, &tls_code, &tls_flags);
            espclaw_agent_format_transport_error(err, tls_code, tls_flags, error_text, error_text_size);
            ESP_LOGE(
                TAG,
                "provider perform failed url=%s mode=%s err=%s (%d) tls_code=%d tls_flags=0x%x",
                endpoint,
                codex_transport_mode_name(transport_mode),
                esp_err_to_name(err),
                (int)err,
                tls_code,
                tls_flags
            );
            esp_http_client_cleanup(client);
            if (attempt + 1 >= attempt_count) {
                return -1;
            }
            continue;
        }
        status_code = esp_http_client_get_status_code(client);
        while (total_read < (int)response_size - 1) {
            int read_now = esp_http_client_read(client, response + total_read, (int)response_size - 1 - total_read);
            if (read_now <= 0) {
                break;
            }
            total_read += read_now;
        }
        response[total_read] = '\0';
        esp_http_client_cleanup(client);
        if (status_code < 200 || status_code >= 300) {
            char snippet[160];
            size_t snippet_len = 0;

            while (snippet_len < sizeof(snippet) - 1 && snippet_len < (size_t)total_read) {
                char c = response[snippet_len];

                snippet[snippet_len] = (c == '\r' || c == '\n') ? ' ' : c;
                snippet_len++;
            }
            snippet[snippet_len] = '\0';
            snprintf(
                error_text,
                error_text_size,
                "Provider returned HTTP %d%s%s",
                status_code,
                snippet[0] != '\0' ? ": " : "",
                snippet
            );
            ESP_LOGE(TAG, "provider http status=%d body=%s", status_code, snippet);
            return -1;
        }
        if (is_codex && total_read > 0) {
            char *completed = (char *)calloc(1, response_size);

            if (completed != NULL && espclaw_agent_extract_sse_completed_response_json(response, completed, response_size) == 0) {
                copy_text(response, response_size, completed);
                total_read = (int)strlen(response);
            }
            free(completed);
        }
        if (total_read == 0) {
            snprintf(error_text, error_text_size, "Provider returned an empty HTTP %d response.", status_code);
            ESP_LOGE(TAG, "provider empty response status=%d", status_code);
        }
        return total_read > 0 ? 0 : -1;
    }
    return -1;
}
#endif

static int call_provider(
    const espclaw_auth_profile_t *profile,
    const char *body,
    char *response,
    size_t response_size,
    char *error_text,
    size_t error_text_size
)
{
    if (s_http_adapter != NULL) {
        return s_http_adapter(profile->base_url, profile, body, response, response_size, s_http_adapter_user_data);
    }
    return host_http_post(profile->base_url, profile, body, response, response_size, error_text, error_text_size);
}

static void build_tool_summary(
    const espclaw_provider_response_t *provider_response,
    char *buffer,
    size_t buffer_size
)
{
    size_t index;
    size_t used = 0;

    if (buffer == NULL || buffer_size == 0 || provider_response == NULL) {
        return;
    }

    used += (size_t)snprintf(buffer + used, buffer_size - used, "Requested tools:");
    for (index = 0; index < provider_response->tool_call_count; ++index) {
        used += (size_t)snprintf(
            buffer + used,
            buffer_size - used,
            "%s %s",
            index == 0 ? "" : ",",
            provider_response->tool_calls[index].name
        );
    }
}

static int espclaw_agent_loop_run_with_options(
    const char *workspace_root,
    const char *session_id,
    const char *user_message,
    bool allow_mutations,
    bool yolo_mode,
    const espclaw_history_message_t *seed_history,
    size_t seed_history_count,
    bool persist_transcript,
    espclaw_agent_run_result_t *result
)
{
#ifdef ESP_PLATFORM
    espclaw_auth_profile_t *profile = NULL;
#else
    espclaw_auth_profile_t profile_storage;
    espclaw_auth_profile_t *profile = &profile_storage;
#endif
    espclaw_runtime_budget_t runtime_budget;
    espclaw_history_message_t *history = NULL;
    char *request_body = NULL;
    char *response_body = NULL;
    char *codex_items_json = NULL;
    char *instructions = NULL;
    char previous_response_id[ESPCLAW_AGENT_RESPONSE_ID_MAX + 1];
    espclaw_provider_response_t *provider_response = NULL;
    char *tool_results = NULL;
    espclaw_agent_media_ref_t *media_refs = NULL;
    size_t history_count = 0;
    unsigned int iteration = 0;
    size_t tool_result_stride = 0;
    bool enforce_reusable_app_install = user_message_requests_reusable_logic_artifact(user_message);
    bool enforce_task_start = user_message_requests_task_artifact(user_message) &&
                              user_message_requests_run_after_creation(user_message);
    bool enforce_behavior_register = user_message_requests_behavior_artifact(user_message);
    bool enforce_app_install = user_message_requests_app_install(user_message) &&
                               !enforce_task_start &&
                               !enforce_behavior_register;
    bool saw_app_install_success = false;
    bool saw_task_start_success = false;
    bool saw_behavior_register_success = false;
    unsigned int app_install_retry_count = 0;
    unsigned int execution_choice_retry_count = 0;
    bool enforce_web_search_fetch = user_message_requests_web_search_and_fetch(user_message);
    bool saw_web_search_success = false;
    bool saw_web_fetch_success = false;
    unsigned int web_search_fetch_retry_count = 0;
    char explicit_required_tools[ESPCLAW_AGENT_TOOL_CALL_MAX][ESPCLAW_AGENT_TOOL_NAME_MAX + 1];
    bool explicit_required_tool_called[ESPCLAW_AGENT_TOOL_CALL_MAX] = {0};
    size_t explicit_required_tool_count = 0;
    unsigned int explicit_tool_retry_count = 0;

    if (session_id == NULL || user_message == NULL || result == NULL) {
        return -1;
    }

    memset(result, 0, sizeof(*result));
#ifdef ESP_PLATFORM
    profile = (espclaw_auth_profile_t *)calloc(1, sizeof(*profile));
    if (profile == NULL) {
        copy_text(result->final_text, sizeof(result->final_text), "Out of memory loading provider credentials.");
        return -1;
    }
#endif
    espclaw_auth_profile_default(profile);
    if (espclaw_auth_store_load(profile) != 0 || !espclaw_auth_profile_is_ready(profile)) {
        copy_text(result->final_text, sizeof(result->final_text), "Provider credentials are not configured.");
#ifdef ESP_PLATFORM
        free_embedded_auth_profile(profile);
#endif
        return -1;
    }
#ifdef ESP_PLATFORM
    if (!espclaw_runtime_wait_for_time_sync(15000)) {
        copy_text(
            result->final_text,
            sizeof(result->final_text),
            "Device clock is not synchronized yet. Wait a few seconds after Wi-Fi connects and try again."
        );
        free_embedded_auth_profile(profile);
        return -1;
    }
#endif

    runtime_budget = current_runtime_budget();
    tool_result_stride = runtime_budget.agent_tool_result_max + 1U;

#ifdef ESP_PLATFORM
    ESP_LOGD(
        TAG,
        "agent alloc preflight free=%u largest=%u req=%u resp=%u hist=%u items=%u instr=%u",
        (unsigned int)heap_caps_get_free_size(MALLOC_CAP_8BIT),
        (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
        (unsigned int)runtime_budget.agent_request_buffer_max,
        (unsigned int)runtime_budget.agent_response_buffer_max,
        (unsigned int)runtime_budget.agent_history_max,
        (unsigned int)runtime_budget.agent_codex_items_max,
        (unsigned int)runtime_budget.agent_instructions_max
    );
    if (!heap_caps_check_integrity_all(false)) {
        copy_text(result->final_text, sizeof(result->final_text), "Heap integrity check failed before model request.");
        return -1;
    }
#endif

    request_body = (char *)agent_calloc(1, runtime_budget.agent_request_buffer_max);
    response_body = (char *)agent_calloc(1, runtime_budget.agent_response_buffer_max);
    history = (espclaw_history_message_t *)agent_calloc(runtime_budget.agent_history_max, sizeof(*history));
    codex_items_json = (char *)agent_calloc(1, runtime_budget.agent_codex_items_max);
    instructions = (char *)agent_calloc(1, runtime_budget.agent_instructions_max);
    if (request_body == NULL || response_body == NULL || history == NULL || codex_items_json == NULL || instructions == NULL) {
        free(instructions);
        free(codex_items_json);
        free(history);
        free(request_body);
        free(response_body);
        free_embedded_auth_profile(profile);
        copy_text(result->final_text, sizeof(result->final_text), "Out of memory building model request.");
        return -1;
    }

    if (load_system_prompt(
            workspace_root,
            user_message,
            allow_mutations,
            yolo_mode,
            instructions,
            runtime_budget.agent_instructions_max) != 0) {
        free(instructions);
        free(codex_items_json);
        free(history);
        free(request_body);
        free(response_body);
        free_embedded_auth_profile(profile);
        copy_text(result->final_text, sizeof(result->final_text), "Failed to load system prompt.");
        return -1;
    }

    codex_items_json[0] = '\0';
    if (persist_transcript && workspace_root != NULL && workspace_root[0] != '\0') {
        espclaw_session_append_message(workspace_root, session_id, "user", user_message);
        load_history(workspace_root, session_id, history, runtime_budget.agent_history_max, &history_count);
    } else if (runtime_budget.agent_history_max > 0) {
        history_count = seed_history_messages(
            history,
            runtime_budget.agent_history_max,
            seed_history,
            seed_history_count,
            user_message
        );
    }
    explicit_required_tool_count = collect_required_tool_names(
        user_message,
        history,
        history_count,
        explicit_required_tools,
        ESPCLAW_AGENT_TOOL_CALL_MAX
    );
    if (build_initial_request_body(profile, instructions, history, history_count, request_body, runtime_budget.agent_request_buffer_max) != 0) {
        free(media_refs);
        free(tool_results);
        free(provider_response);
        free(instructions);
        free(codex_items_json);
        free(history);
        free(request_body);
        free(response_body);
        free_embedded_auth_profile(profile);
        copy_text(result->final_text, sizeof(result->final_text), "Failed to build the model request.");
        return -1;
    }
    free(history);
    history = NULL;
    history_count = 0;

    previous_response_id[0] = '\0';
    while (iteration < 8) {
        size_t media_count = 0;
        size_t tool_index;
        int provider_status = 0;
        int parse_status = 0;
        bool has_tools = false;

        memset(response_body, 0, runtime_budget.agent_response_buffer_max);
        provider_status = call_provider(
            profile,
            request_body,
            response_body,
            runtime_budget.agent_response_buffer_max,
            result->final_text,
            sizeof(result->final_text)
        );
        free(request_body);
        request_body = NULL;
        parse_status = provider_status == 0
                           ? parse_provider_text_response(
                                 response_body,
                                 previous_response_id,
                                 sizeof(previous_response_id),
                                 result->final_text,
                                 sizeof(result->final_text),
                                 &has_tools)
                           : -1;
        if (provider_status != 0 || parse_status != 0) {
            if (provider_status == 0) {
                char completed_id[ESPCLAW_AGENT_RESPONSE_ID_MAX + 1];
                char snippet[160];
                size_t snippet_len = 0;

                if (result->used_tools &&
                    is_completed_terminal_response_without_output(
                        response_body,
                        completed_id,
                        sizeof(completed_id))) {
                    copy_text(previous_response_id, sizeof(previous_response_id), completed_id);
                    copy_text(result->response_id, sizeof(result->response_id), completed_id);
                    result->final_text[0] = '\0';
                    result->ok = true;
                    if (persist_transcript && workspace_root != NULL && workspace_root[0] != '\0') {
                        espclaw_session_append_message(workspace_root, session_id, "assistant", "");
                    }
                    free(instructions);
                    free(codex_items_json);
                    free(history);
                    free(request_body);
                    free(response_body);
                    free_embedded_auth_profile(profile);
                    return 0;
                }

                while (snippet_len < sizeof(snippet) - 1 && response_body[snippet_len] != '\0') {
                    char c = response_body[snippet_len];

                    snippet[snippet_len] = (c == '\r' || c == '\n') ? ' ' : c;
                    snippet_len++;
                }
                snippet[snippet_len] = '\0';
                snprintf(
                    result->final_text,
                    sizeof(result->final_text),
                    "Model response parse failed.%s%s",
                    snippet[0] != '\0' ? " Body: " : "",
                    snippet
                );
#ifdef ESP_PLATFORM
                ESP_LOGE(TAG, "model response parse failed body=%s", snippet);
#endif
            } else if (result->final_text[0] == '\0') {
                copy_text(result->final_text, sizeof(result->final_text), "Model call failed.");
            }
            free(instructions);
            free(codex_items_json);
            free(history);
            free(request_body);
            free(response_body);
            free_embedded_auth_profile(profile);
            return -1;
        }

        copy_text(result->response_id, sizeof(result->response_id), previous_response_id);
        iteration++;
        result->iterations = iteration;

        if (!has_tools) {
            bool need_execution_app_install = enforce_reusable_app_install && !saw_app_install_success;
            bool need_execution_task_start = enforce_task_start && !saw_task_start_success;
            bool need_execution_behavior_register = enforce_behavior_register && !saw_behavior_register_success;

            if (enforce_web_search_fetch &&
                (!saw_web_search_success || !saw_web_fetch_success) &&
                web_search_fetch_retry_count == 0) {
                request_body = (char *)agent_calloc(1, runtime_budget.agent_request_buffer_max);
                if (request_body == NULL) {
                    free(instructions);
                    free(codex_items_json);
                    free(history);
                    free(response_body);
                    free_embedded_auth_profile(profile);
                    copy_text(result->final_text, sizeof(result->final_text), "Out of memory retrying required web tool request.");
                    return -1;
                }
                if (build_web_search_fetch_retry_request_body(
                        profile,
                        workspace_root,
                        session_id,
                        instructions,
                        result->final_text,
                        !saw_web_search_success,
                        !saw_web_fetch_success,
                        request_body,
                        runtime_budget.agent_request_buffer_max,
                        runtime_budget.agent_history_max) != 0) {
                    free(instructions);
                    free(codex_items_json);
                    free(history);
                    free(request_body);
                    free(response_body);
                    free_embedded_auth_profile(profile);
                    copy_text(result->final_text, sizeof(result->final_text), "Failed to retry the required web tool request.");
                    return -1;
                }
                web_search_fetch_retry_count++;
                continue;
            }
            if (explicit_required_tool_count > 0U && explicit_tool_retry_count == 0U) {
                size_t missing_count = 0;
                char missing_tools[ESPCLAW_AGENT_TOOL_CALL_MAX][ESPCLAW_AGENT_TOOL_NAME_MAX + 1];

                for (tool_index = 0; tool_index < explicit_required_tool_count; ++tool_index) {
                    if (!explicit_required_tool_called[tool_index]) {
                        copy_text(
                            missing_tools[missing_count],
                            sizeof(missing_tools[missing_count]),
                            explicit_required_tools[tool_index]
                        );
                        missing_count++;
                    }
                }
                if (missing_count > 0U) {
                    request_body = (char *)agent_calloc(1, runtime_budget.agent_request_buffer_max);
                    if (request_body == NULL) {
                        free(instructions);
                        free(codex_items_json);
                        free(history);
                        free(response_body);
                        free_embedded_auth_profile(profile);
                        copy_text(result->final_text, sizeof(result->final_text), "Out of memory retrying explicit tool request.");
                        return -1;
                    }
                    if (build_explicit_tool_retry_request_body(
                            profile,
                            workspace_root,
                            session_id,
                            instructions,
                            result->final_text,
                            missing_tools,
                            missing_count,
                            request_body,
                            runtime_budget.agent_request_buffer_max,
                            runtime_budget.agent_history_max) != 0) {
                        free(instructions);
                        free(codex_items_json);
                        free(history);
                        free(request_body);
                        free(response_body);
                        free_embedded_auth_profile(profile);
                        copy_text(result->final_text, sizeof(result->final_text), "Failed to retry the explicit tool request.");
                        return -1;
                    }
                    explicit_tool_retry_count++;
                    continue;
                }
            }
            if ((need_execution_app_install || need_execution_task_start || need_execution_behavior_register) &&
                execution_choice_retry_count < 2U) {
                request_body = (char *)agent_calloc(1, runtime_budget.agent_request_buffer_max);
                if (request_body == NULL) {
                    free(instructions);
                    free(codex_items_json);
                    free(history);
                    free(response_body);
                    free_embedded_auth_profile(profile);
                    copy_text(result->final_text, sizeof(result->final_text), "Out of memory retrying execution-choice request.");
                    return -1;
                }
                if (build_execution_choice_retry_request_body(
                        profile,
                        workspace_root,
                        session_id,
                        instructions,
                        result->final_text,
                        need_execution_app_install,
                        need_execution_task_start,
                        need_execution_behavior_register,
                        request_body,
                        runtime_budget.agent_request_buffer_max,
                        runtime_budget.agent_history_max) != 0) {
                    free(instructions);
                    free(codex_items_json);
                    free(history);
                    free(request_body);
                    free(response_body);
                    free_embedded_auth_profile(profile);
                    copy_text(result->final_text, sizeof(result->final_text), "Failed to retry the execution-choice request.");
                    return -1;
                }
                execution_choice_retry_count++;
                continue;
            }
            if (enforce_app_install && !saw_app_install_success && app_install_retry_count == 0) {
                request_body = (char *)agent_calloc(1, runtime_budget.agent_request_buffer_max);
                if (request_body == NULL) {
                    free(instructions);
                    free(codex_items_json);
                    free(history);
                    free(response_body);
                    free_embedded_auth_profile(profile);
                    copy_text(result->final_text, sizeof(result->final_text), "Out of memory retrying app installation request.");
                    return -1;
                }
                if (build_app_install_retry_request_body(
                        profile,
                        workspace_root,
                        session_id,
                        instructions,
                        result->final_text,
                        request_body,
                        runtime_budget.agent_request_buffer_max,
                        runtime_budget.agent_history_max) != 0) {
                    free(instructions);
                    free(codex_items_json);
                    free(history);
                    free(request_body);
                    free(response_body);
                    free_embedded_auth_profile(profile);
                    copy_text(result->final_text, sizeof(result->final_text), "Failed to retry the app installation request.");
                    return -1;
                }
                app_install_retry_count++;
                continue;
            }
            result->ok = true;
            if (persist_transcript && workspace_root != NULL && workspace_root[0] != '\0') {
                espclaw_session_append_message(workspace_root, session_id, "assistant", result->final_text);
            }
            free(instructions);
            free(codex_items_json);
            free(history);
            free(request_body);
            free(response_body);
            free_embedded_auth_profile(profile);
            return 0;
        }

        free(provider_response);
        provider_response = (espclaw_provider_response_t *)agent_calloc(1, sizeof(*provider_response));
        if (provider_response == NULL || parse_provider_response(response_body, provider_response) != 0) {
            free(provider_response);
            free(instructions);
            free(codex_items_json);
            free(history);
            free(request_body);
            free(response_body);
            free_embedded_auth_profile(profile);
            copy_text(result->final_text, sizeof(result->final_text), "Out of memory parsing model response.");
            return -1;
        }

        result->used_tools = true;
        if (provider_response->text[0] != '\0') {
            if (persist_transcript && workspace_root != NULL && workspace_root[0] != '\0') {
                espclaw_session_append_message(workspace_root, session_id, "assistant", provider_response->text);
            }
        } else {
            char tool_summary[256];

            build_tool_summary(provider_response, tool_summary, sizeof(tool_summary));
            if (persist_transcript && workspace_root != NULL && workspace_root[0] != '\0') {
                espclaw_session_append_message(workspace_root, session_id, "assistant", tool_summary);
            }
        }

        free(tool_results);
        free(media_refs);
        tool_results = (char *)agent_calloc(ESPCLAW_AGENT_TOOL_CALL_MAX, tool_result_stride);
        media_refs = (espclaw_agent_media_ref_t *)agent_calloc(ESPCLAW_AGENT_MEDIA_MAX, sizeof(*media_refs));
        if (tool_results == NULL || media_refs == NULL) {
            free(media_refs);
            free(tool_results);
            free(provider_response);
            free(instructions);
            free(codex_items_json);
            free(history);
            free(request_body);
            free(response_body);
            free_embedded_auth_profile(profile);
            copy_text(result->final_text, sizeof(result->final_text), "Out of memory executing tool round.");
            return -1;
        }
        memset(tool_results, 0, ESPCLAW_AGENT_TOOL_CALL_MAX * tool_result_stride);
        memset(media_refs, 0, ESPCLAW_AGENT_MEDIA_MAX * sizeof(*media_refs));

        for (tool_index = 0; tool_index < provider_response->tool_call_count; ++tool_index) {
            espclaw_agent_media_ref_t tool_media = {0};
            char *tool_result_buffer = tool_result_at(tool_results, tool_result_stride, tool_index);
            size_t required_index;

            for (required_index = 0; required_index < explicit_required_tool_count; ++required_index) {
                if (strcmp(provider_response->tool_calls[tool_index].name, explicit_required_tools[required_index]) == 0) {
                    explicit_required_tool_called[required_index] = true;
                }
            }

            log_tool_call_summary(&provider_response->tool_calls[tool_index], "model");
            tool_execute(
                workspace_root,
                &provider_response->tool_calls[tool_index],
                allow_mutations,
                &tool_media,
                tool_result_buffer,
                tool_result_stride
            );
            if (strcmp(provider_response->tool_calls[tool_index].name, "web.search") == 0 &&
                tool_result_reports_success(tool_result_buffer)) {
                saw_web_search_success = true;
            }
            if (strcmp(provider_response->tool_calls[tool_index].name, "web.fetch") == 0 &&
                tool_result_reports_success(tool_result_buffer)) {
                saw_web_fetch_success = true;
            }
            if (strcmp(provider_response->tool_calls[tool_index].name, "app.install") == 0 &&
                tool_result_reports_success(tool_result_buffer)) {
                saw_app_install_success = true;
            }
            if (strcmp(provider_response->tool_calls[tool_index].name, "task.start") == 0 &&
                tool_result_reports_success(tool_result_buffer)) {
                saw_task_start_success = true;
            }
            if (strcmp(provider_response->tool_calls[tool_index].name, "behavior.register") == 0 &&
                tool_result_reports_success(tool_result_buffer)) {
                saw_behavior_register_success = true;
            }
            if (tool_media.active && media_count < ESPCLAW_AGENT_MEDIA_MAX) {
                media_refs[media_count++] = tool_media;
            }
            if (persist_transcript && workspace_root != NULL && workspace_root[0] != '\0') {
                espclaw_session_append_message(
                    workspace_root,
                    session_id,
                    "tool",
                    tool_result_buffer
                );
            }
        }

        if (enforce_web_search_fetch &&
            (!saw_web_search_success || !saw_web_fetch_success) &&
            web_search_fetch_retry_count == 0) {
            request_body = (char *)agent_calloc(1, runtime_budget.agent_request_buffer_max);
            if (request_body == NULL) {
                free(media_refs);
                free(tool_results);
                free(provider_response);
                free(instructions);
                free(codex_items_json);
                free(history);
                free(response_body);
                free_embedded_auth_profile(profile);
                copy_text(result->final_text, sizeof(result->final_text), "Out of memory retrying required web tool request.");
                return -1;
            }
            if (build_web_search_fetch_retry_request_body(
                    profile,
                    workspace_root,
                    session_id,
                    instructions,
                    NULL,
                    !saw_web_search_success,
                    !saw_web_fetch_success,
                    request_body,
                    runtime_budget.agent_request_buffer_max,
                    runtime_budget.agent_history_max) != 0) {
                free(media_refs);
                free(tool_results);
                free(provider_response);
                free(instructions);
                free(codex_items_json);
                free(history);
                free(request_body);
                free(response_body);
                free_embedded_auth_profile(profile);
                copy_text(result->final_text, sizeof(result->final_text), "Failed to retry the required web tool request.");
                return -1;
            }
            web_search_fetch_retry_count++;
            free(media_refs);
            free(tool_results);
            free(provider_response);
            media_refs = NULL;
            tool_results = NULL;
            provider_response = NULL;
            continue;
        }

        if (((enforce_reusable_app_install && !saw_app_install_success) ||
             (enforce_task_start && !saw_task_start_success) ||
             (enforce_behavior_register && !saw_behavior_register_success)) &&
            execution_choice_retry_count < 2U) {
            request_body = (char *)agent_calloc(1, runtime_budget.agent_request_buffer_max);
            if (request_body == NULL) {
                free(media_refs);
                free(tool_results);
                free(provider_response);
                free(instructions);
                free(codex_items_json);
                free(history);
                free(response_body);
                free_embedded_auth_profile(profile);
                copy_text(result->final_text, sizeof(result->final_text), "Out of memory retrying execution-choice request.");
                return -1;
            }
            if (build_execution_choice_retry_request_body(
                    profile,
                    workspace_root,
                    session_id,
                    instructions,
                    NULL,
                    enforce_reusable_app_install && !saw_app_install_success,
                    enforce_task_start && !saw_task_start_success,
                    enforce_behavior_register && !saw_behavior_register_success,
                    request_body,
                    runtime_budget.agent_request_buffer_max,
                    runtime_budget.agent_history_max) != 0) {
                free(media_refs);
                free(tool_results);
                free(provider_response);
                free(instructions);
                free(codex_items_json);
                free(history);
                free(request_body);
                free(response_body);
                free_embedded_auth_profile(profile);
                copy_text(result->final_text, sizeof(result->final_text), "Failed to retry the execution-choice request.");
                return -1;
            }
            execution_choice_retry_count++;
            free(media_refs);
            free(tool_results);
            free(provider_response);
            media_refs = NULL;
            tool_results = NULL;
            provider_response = NULL;
            continue;
        }

        if (enforce_app_install && !saw_app_install_success && app_install_retry_count == 0) {
            request_body = (char *)agent_calloc(1, runtime_budget.agent_request_buffer_max);
            if (request_body == NULL) {
                free(media_refs);
                free(tool_results);
                free(provider_response);
                free(instructions);
                free(codex_items_json);
                free(history);
                free(response_body);
                free_embedded_auth_profile(profile);
                copy_text(result->final_text, sizeof(result->final_text), "Out of memory retrying app installation request.");
                return -1;
            }
            if (build_app_install_retry_request_body(
                    profile,
                    workspace_root,
                    session_id,
                    instructions,
                    NULL,
                    request_body,
                    runtime_budget.agent_request_buffer_max,
                    runtime_budget.agent_history_max) != 0) {
                free(media_refs);
                free(tool_results);
                free(provider_response);
                free(instructions);
                free(codex_items_json);
                free(history);
                free(request_body);
                free(response_body);
                free_embedded_auth_profile(profile);
                copy_text(result->final_text, sizeof(result->final_text), "Failed to retry the app installation request.");
                return -1;
            }
            app_install_retry_count++;
            free(media_refs);
            free(tool_results);
            free(provider_response);
            media_refs = NULL;
            tool_results = NULL;
            provider_response = NULL;
            continue;
        }

        request_body = (char *)agent_calloc(1, runtime_budget.agent_request_buffer_max);
        if (request_body == NULL) {
            free(media_refs);
            free(tool_results);
            free(provider_response);
            free(instructions);
            free(codex_items_json);
            free(history);
            free(response_body);
            free_embedded_auth_profile(profile);
            copy_text(result->final_text, sizeof(result->final_text), "Out of memory building follow-up request.");
            return -1;
        }
        if (strcmp(profile->provider_id, "openai_codex") == 0) {
            size_t codex_items_used = strlen(codex_items_json);

            history = (espclaw_history_message_t *)agent_calloc(runtime_budget.agent_history_max, sizeof(*history));
            if (history == NULL) {
                free(media_refs);
                free(tool_results);
                free(provider_response);
                free(instructions);
                free(codex_items_json);
                free(request_body);
                free(response_body);
                free_embedded_auth_profile(profile);
                copy_text(result->final_text, sizeof(result->final_text), "Out of memory loading follow-up history.");
                return -1;
            }
            if (persist_transcript && workspace_root != NULL && workspace_root[0] != '\0') {
                load_history(workspace_root, session_id, history, runtime_budget.agent_history_max, &history_count);
            } else {
                history_count = 0;
            }

            codex_items_used = append_codex_tool_round_items(
                codex_items_json,
                runtime_budget.agent_codex_items_max,
                codex_items_used,
                provider_response->tool_calls,
                tool_results,
                tool_result_stride,
                provider_response->tool_call_count
            );
            if (build_codex_followup_request_body(
                    profile,
                    instructions,
                    workspace_root,
                    history,
                    history_count,
                    codex_items_json,
                    media_refs,
                    media_count,
                    runtime_budget.agent_image_data_max,
                    request_body,
                    runtime_budget.agent_request_buffer_max) != 0) {
                free(media_refs);
                free(tool_results);
                free(provider_response);
                free(instructions);
                free(codex_items_json);
                free(history);
                history = NULL;
                free(request_body);
                free(response_body);
                free_embedded_auth_profile(profile);
                copy_text(result->final_text, sizeof(result->final_text), "Failed to continue after tool execution.");
                return -1;
            }
            free(history);
            history = NULL;
            history_count = 0;
        } else if (build_followup_request_body(
                       profile,
                       instructions,
                       workspace_root,
                       previous_response_id,
                       provider_response->tool_calls,
                       tool_results,
                       tool_result_stride,
                       provider_response->tool_call_count,
                       media_refs,
                       media_count,
                       runtime_budget.agent_image_data_max,
                       request_body,
                       runtime_budget.agent_request_buffer_max) != 0) {
            free(media_refs);
            free(tool_results);
            free(provider_response);
            free(instructions);
            free(codex_items_json);
            free(history);
            free(request_body);
            free(response_body);
            free_embedded_auth_profile(profile);
            copy_text(result->final_text, sizeof(result->final_text), "Failed to continue after tool execution.");
            return -1;
        }
    }

    result->hit_iteration_limit = true;
    copy_text(result->final_text, sizeof(result->final_text), "Agent run hit the tool iteration limit.");
    if (persist_transcript && workspace_root != NULL && workspace_root[0] != '\0') {
        espclaw_session_append_message(workspace_root, session_id, "assistant", result->final_text);
    }
    free(media_refs);
    free(tool_results);
    free(provider_response);
    free(instructions);
    free(codex_items_json);
    free(history);
    free(request_body);
    free(response_body);
    free_embedded_auth_profile(profile);
    return -1;
}

int espclaw_agent_loop_run(
    const char *workspace_root,
    const char *session_id,
    const char *user_message,
    bool allow_mutations,
    bool yolo_mode,
    espclaw_agent_run_result_t *result
)
{
    return espclaw_agent_loop_run_with_options(
        workspace_root,
        session_id,
        user_message,
        allow_mutations,
        yolo_mode,
        NULL,
        0U,
        true,
        result
    );
}

int espclaw_agent_loop_run_stateless(
    const char *workspace_root,
    const char *session_id,
    const char *user_message,
    bool allow_mutations,
    bool yolo_mode,
    espclaw_agent_run_result_t *result
)
{
    /*
     * Embedded Telegram turns share the same SD-backed workspace as the rest of the runtime.
     * Keep them stateless so they do not hit session transcript reads/writes from the polling task.
     */
    return espclaw_agent_loop_run_with_options(
        workspace_root,
        session_id,
        user_message,
        allow_mutations,
        yolo_mode,
        NULL,
        0U,
        false,
        result
    );
}

int espclaw_agent_loop_run_stateless_with_history(
    const char *workspace_root,
    const char *session_id,
    const char *user_message,
    bool allow_mutations,
    bool yolo_mode,
    const espclaw_agent_history_message_t *history,
    size_t history_count,
    espclaw_agent_run_result_t *result
)
{
    return espclaw_agent_loop_run_with_options(
        workspace_root,
        session_id,
        user_message,
        allow_mutations,
        yolo_mode,
        history,
        history_count,
        false,
        result
    );
}

void espclaw_agent_set_http_adapter(espclaw_agent_http_adapter_t adapter, void *user_data)
{
    s_http_adapter = adapter;
    s_http_adapter_user_data = user_data;
}
