#include "espclaw/console_chat.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "espclaw/auth_store.h"
#include "espclaw/session_store.h"
#include "espclaw/tool_catalog.h"
#include "espclaw/workspace.h"

static espclaw_console_runtime_adapter_t s_runtime_adapter;

static void copy_text(char *buffer, size_t buffer_size, const char *value)
{
    if (buffer == NULL || buffer_size == 0) {
        return;
    }
    snprintf(buffer, buffer_size, "%s", value != NULL ? value : "");
}

static const char *skip_spaces(const char *cursor)
{
    while (cursor != NULL && *cursor != '\0' && isspace((unsigned char)*cursor)) {
        cursor++;
    }
    return cursor;
}

static void trim_trailing_spaces(char *text)
{
    size_t length;

    if (text == NULL) {
        return;
    }
    length = strlen(text);
    while (length > 0 && isspace((unsigned char)text[length - 1])) {
        text[--length] = '\0';
    }
}

static bool extract_token(const char **cursor_ptr, char *buffer, size_t buffer_size)
{
    const char *cursor;
    size_t used = 0;
    char quote = '\0';

    if (cursor_ptr == NULL || *cursor_ptr == NULL || buffer == NULL || buffer_size == 0) {
        return false;
    }

    cursor = skip_spaces(*cursor_ptr);
    if (cursor == NULL || *cursor == '\0') {
        buffer[0] = '\0';
        *cursor_ptr = cursor;
        return false;
    }

    if (*cursor == '"' || *cursor == '\'') {
        quote = *cursor++;
    }

    while (*cursor != '\0' && used + 1 < buffer_size) {
        if (quote != '\0') {
            if (*cursor == '\\' && cursor[1] != '\0') {
                cursor++;
                buffer[used++] = *cursor++;
                continue;
            }
            if (*cursor == quote) {
                cursor++;
                break;
            }
        } else if (isspace((unsigned char)*cursor)) {
            break;
        }
        buffer[used++] = *cursor++;
    }

    buffer[used] = '\0';
    cursor = skip_spaces(cursor);
    *cursor_ptr = cursor;
    return used > 0;
}

static void append_console_exchange(const char *workspace_root, const char *session_id, const char *input, const char *reply)
{
#ifdef ESP_PLATFORM
    /*
     * Console chat runs on UART/http control threads on embedded targets; persisting every exchange
     * through the SD-backed workspace has proven unsafe on the ESP32-CAM runtime. Keep console I/O
     * live and best-effort by skipping transcript writes here.
     */
    (void)workspace_root;
    (void)session_id;
    (void)input;
    (void)reply;
    return;
#else
    if (workspace_root == NULL || workspace_root[0] == '\0' || session_id == NULL || session_id[0] == '\0') {
        return;
    }
    espclaw_session_append_message(workspace_root, session_id, "user", input);
    espclaw_session_append_message(workspace_root, session_id, "assistant", reply);
#endif
}

static const espclaw_runtime_status_t *console_runtime_status(void)
{
#ifdef ESP_PLATFORM
    if (s_runtime_adapter.status != NULL) {
        return s_runtime_adapter.status();
    }
    return espclaw_runtime_status();
#else
    return s_runtime_adapter.status != NULL ? s_runtime_adapter.status() : NULL;
#endif
}

static esp_err_t console_wifi_scan(espclaw_wifi_network_t *networks, size_t max_networks, size_t *count_out)
{
#ifdef ESP_PLATFORM
    if (s_runtime_adapter.wifi_scan != NULL) {
        return s_runtime_adapter.wifi_scan(networks, max_networks, count_out);
    }
    return espclaw_runtime_wifi_scan(networks, max_networks, count_out);
#else
    return s_runtime_adapter.wifi_scan != NULL ? s_runtime_adapter.wifi_scan(networks, max_networks, count_out) : -1;
#endif
}

static esp_err_t console_wifi_join(const char *ssid, const char *password, char *message, size_t message_size)
{
#ifdef ESP_PLATFORM
    if (s_runtime_adapter.wifi_join != NULL) {
        return s_runtime_adapter.wifi_join(ssid, password, message, message_size);
    }
    return espclaw_runtime_wifi_join(ssid, password, message, message_size);
#else
    return s_runtime_adapter.wifi_join != NULL ? s_runtime_adapter.wifi_join(ssid, password, message, message_size) : -1;
#endif
}

static esp_err_t console_telegram_get_config(espclaw_telegram_config_t *config)
{
#ifdef ESP_PLATFORM
    if (s_runtime_adapter.telegram_get_config != NULL) {
        return s_runtime_adapter.telegram_get_config(config);
    }
    return espclaw_runtime_get_telegram_config(config);
#else
    return s_runtime_adapter.telegram_get_config != NULL ? s_runtime_adapter.telegram_get_config(config) : -1;
#endif
}

static esp_err_t console_telegram_set_config(
    const espclaw_telegram_config_t *config,
    char *message,
    size_t message_size
)
{
#ifdef ESP_PLATFORM
    if (s_runtime_adapter.telegram_set_config != NULL) {
        return s_runtime_adapter.telegram_set_config(config, message, message_size);
    }
    return espclaw_runtime_set_telegram_config(config, message, message_size);
#else
    return s_runtime_adapter.telegram_set_config != NULL
        ? s_runtime_adapter.telegram_set_config(config, message, message_size)
        : -1;
#endif
}

static esp_err_t console_factory_reset(char *message, size_t message_size)
{
#ifdef ESP_PLATFORM
    if (s_runtime_adapter.factory_reset != NULL) {
        return s_runtime_adapter.factory_reset(message, message_size);
    }
    return espclaw_runtime_factory_reset(message, message_size);
#else
    return s_runtime_adapter.factory_reset != NULL ? s_runtime_adapter.factory_reset(message, message_size) : -1;
#endif
}

static void console_reboot(void)
{
#ifdef ESP_PLATFORM
    if (s_runtime_adapter.reboot != NULL) {
        s_runtime_adapter.reboot();
        return;
    }
    espclaw_runtime_reboot();
#else
    if (s_runtime_adapter.reboot != NULL) {
        s_runtime_adapter.reboot();
    }
#endif
}

static size_t render_help(char *buffer, size_t buffer_size)
{
    return (size_t)snprintf(
        buffer,
        buffer_size,
        "Slash commands:\n"
        "/help\n"
        "/status\n"
        "/tools\n"
        "/tool <name> [json]\n"
        "/wifi status\n"
        "/wifi scan\n"
        "/wifi join <ssid> [password]\n"
        "/telegram status\n"
        "/telegram token <bot_token>\n"
        "/telegram poll <seconds>\n"
        "/telegram enable | disable | clear-token\n"
        "/yolo status | on | off\n"
        "/memory\n"
        "/reboot\n"
        "/factory-reset\n\n"
        "Any non-slash input runs a normal LLM turn."
    );
}

static size_t render_status(char *buffer, size_t buffer_size)
{
    const espclaw_runtime_status_t *status = console_runtime_status();
    const char *provider_id = "not configured";
    const char *model = "n/a";
#ifdef ESP_PLATFORM
    espclaw_auth_profile_t *profile = (espclaw_auth_profile_t *)calloc(1, sizeof(*profile));

    if (profile != NULL) {
        espclaw_auth_profile_default(profile);
        espclaw_auth_store_load(profile);
        if (espclaw_auth_profile_is_ready(profile)) {
            provider_id = profile->provider_id;
            model = profile->model;
        }
    }
#else
    espclaw_auth_profile_t profile_storage;
    espclaw_auth_profile_t *profile = &profile_storage;

    espclaw_auth_profile_default(profile);
    espclaw_auth_store_load(profile);
    if (espclaw_auth_profile_is_ready(profile)) {
        provider_id = profile->provider_id;
        model = profile->model;
    }
#endif
    size_t written = (size_t)snprintf(
        buffer,
        buffer_size,
        "Board: %s\nStorage: %s (%s)\nWorkspace: %s\nWi-Fi: %s%s%s\nProvisioning: %s\nProvider: %s (%s)\nYOLO: %s",
        status != NULL && status->profile.display_name != NULL ? status->profile.display_name : "unknown",
        status != NULL ? espclaw_storage_backend_name(status->storage_backend) : "unknown",
        status != NULL && status->storage_ready ? "ready" : "not ready",
        status != NULL && status->workspace_root[0] != '\0' ? status->workspace_root : "unavailable",
        status != NULL && status->wifi_ready ? "connected" : "offline",
        status != NULL && status->wifi_ssid[0] != '\0' ? " to " : "",
        status != NULL && status->wifi_ssid[0] != '\0' ? status->wifi_ssid : "",
        status != NULL && status->provisioning_active ? "active" : "inactive",
        provider_id,
        model,
        espclaw_runtime_get_yolo_mode() ? "enabled" : "disabled"
    );

#ifdef ESP_PLATFORM
    free(profile);
#endif
    return written;
}

static size_t render_memory_status(const char *workspace_root, char *buffer, size_t buffer_size)
{
    return (size_t)snprintf(
        buffer,
        buffer_size,
        "Workspace markdown control files:\n"
        "- AGENTS.md\n"
        "- IDENTITY.md\n"
        "- USER.md\n"
        "- HEARTBEAT.md\n"
        "- memory/MEMORY.md\n\n"
        "Today they are injected into every system prompt when workspace storage is ready.%s\n"
        "Update them with /tool fs.write {\"path\":\"USER.md\",\"content\":\"...\"} or from Lua via espclaw.fs.write().",
        workspace_root != NULL && workspace_root[0] != '\0'
            ? ""
            : "\nWorkspace storage is currently unavailable, so the device is in ephemeral prompt mode."
    );
}

static size_t render_wifi_status(char *buffer, size_t buffer_size)
{
    const espclaw_runtime_status_t *status = console_runtime_status();

    return (size_t)snprintf(
        buffer,
        buffer_size,
        "Wi-Fi: %s%s%s\nProvisioning: %s",
        status != NULL && status->wifi_ready ? "connected" : "offline",
        status != NULL && status->wifi_ssid[0] != '\0' ? " to " : "",
        status != NULL && status->wifi_ssid[0] != '\0' ? status->wifi_ssid : "",
        status != NULL && status->provisioning_active ? "active" : "inactive"
    );
}

static size_t render_telegram_status(char *buffer, size_t buffer_size)
{
    espclaw_telegram_config_t config;

    memset(&config, 0, sizeof(config));
    if (console_telegram_get_config(&config) != 0) {
        return (size_t)snprintf(buffer, buffer_size, "Telegram config is unavailable.");
    }

    return (size_t)snprintf(
        buffer,
        buffer_size,
        "Telegram: %s\nConfigured: %s\nReady: %s\nToken: %s\nPoll interval: %lu s",
        config.enabled ? "enabled" : "disabled",
        config.configured ? "yes" : "no",
        config.ready ? "yes" : "no",
        config.token_hint[0] != '\0' ? config.token_hint : "unset",
        (unsigned long)config.poll_interval_seconds
    );
}

static int render_tool_names(char *buffer, size_t buffer_size)
{
    size_t index;
    size_t used = 0;

    used += (size_t)snprintf(buffer + used, buffer_size - used, "Available tools:");
    for (index = 0; index < espclaw_tool_count() && used + 8 < buffer_size; ++index) {
        const espclaw_tool_descriptor_t *tool = espclaw_tool_at(index);

        used += (size_t)snprintf(
            buffer + used,
            buffer_size - used,
            "\n- %s%s",
            tool != NULL ? tool->name : "",
            tool != NULL && tool->safety == ESPCLAW_TOOL_SAFETY_CONFIRM_REQUIRED ? " [mutates]" : ""
        );
    }
    return 0;
}

void espclaw_console_set_runtime_adapter(const espclaw_console_runtime_adapter_t *adapter)
{
    if (adapter == NULL) {
        memset(&s_runtime_adapter, 0, sizeof(s_runtime_adapter));
        return;
    }
    s_runtime_adapter = *adapter;
}

int espclaw_console_run(
    const char *workspace_root,
    const char *session_id,
    const char *input,
    bool allow_mutations,
    bool yolo_mode,
    espclaw_agent_run_result_t *result
)
{
    char command[32];
    const char *cursor;

    if (input == NULL || result == NULL) {
        return -1;
    }

    memset(result, 0, sizeof(*result));
    cursor = skip_spaces(input);
    if (cursor == NULL || *cursor == '\0') {
        copy_text(result->final_text, sizeof(result->final_text), "Enter /help or ask a question.");
        result->ok = true;
        return 0;
    }

    if (*cursor != '/') {
        return espclaw_agent_loop_run(workspace_root, session_id, cursor, allow_mutations, yolo_mode, result);
    }

    cursor++;
    if (!extract_token(&cursor, command, sizeof(command))) {
        render_help(result->final_text, sizeof(result->final_text));
        result->ok = true;
        append_console_exchange(workspace_root, session_id, input, result->final_text);
        return 0;
    }

    if (strcmp(command, "help") == 0) {
        render_help(result->final_text, sizeof(result->final_text));
    } else if (strcmp(command, "status") == 0) {
        render_status(result->final_text, sizeof(result->final_text));
    } else if (strcmp(command, "tools") == 0) {
        render_tool_names(result->final_text, sizeof(result->final_text));
    } else if (strcmp(command, "memory") == 0) {
        render_memory_status(workspace_root, result->final_text, sizeof(result->final_text));
    } else if (strcmp(command, "wifi") == 0) {
        char subcommand[32];

        if (!extract_token(&cursor, subcommand, sizeof(subcommand)) || strcmp(subcommand, "status") == 0) {
            render_wifi_status(result->final_text, sizeof(result->final_text));
        } else if (strcmp(subcommand, "scan") == 0) {
            espclaw_wifi_network_t networks[8];
            size_t count = 0;
            size_t index;
            size_t used = 0;

            if (console_wifi_scan(networks, sizeof(networks) / sizeof(networks[0]), &count) != 0) {
                copy_text(result->final_text, sizeof(result->final_text), "Wi-Fi scan failed.");
            } else if (count == 0) {
                copy_text(result->final_text, sizeof(result->final_text), "No Wi-Fi networks found.");
            } else {
                used += (size_t)snprintf(result->final_text + used, sizeof(result->final_text) - used, "Wi-Fi networks:");
                for (index = 0; index < count && used + 32 < sizeof(result->final_text); ++index) {
                    used += (size_t)snprintf(
                        result->final_text + used,
                        sizeof(result->final_text) - used,
                        "\n- %s (rssi=%d ch=%d%s)",
                        networks[index].ssid,
                        networks[index].rssi,
                        networks[index].channel,
                        networks[index].secure ? ", secure" : ", open"
                    );
                }
            }
        } else if (strcmp(subcommand, "join") == 0) {
            char ssid[64];
            char password[128];

            if (!extract_token(&cursor, ssid, sizeof(ssid))) {
                copy_text(result->final_text, sizeof(result->final_text), "Usage: /wifi join <ssid> [password]");
            } else {
                password[0] = '\0';
                if (*skip_spaces(cursor) != '\0') {
                    extract_token(&cursor, password, sizeof(password));
                }
                if (console_wifi_join(ssid, password, result->final_text, sizeof(result->final_text)) != 0 &&
                    result->final_text[0] == '\0') {
                    copy_text(result->final_text, sizeof(result->final_text), "Wi-Fi join failed.");
                }
            }
        } else {
            copy_text(result->final_text, sizeof(result->final_text), "Usage: /wifi status | /wifi scan | /wifi join <ssid> [password]");
        }
    } else if (strcmp(command, "telegram") == 0) {
        char subcommand[32];
        espclaw_telegram_config_t config;

        memset(&config, 0, sizeof(config));
        if (!extract_token(&cursor, subcommand, sizeof(subcommand)) || strcmp(subcommand, "status") == 0) {
            render_telegram_status(result->final_text, sizeof(result->final_text));
        } else if (console_telegram_get_config(&config) != 0) {
            copy_text(result->final_text, sizeof(result->final_text), "Telegram config is unavailable.");
        } else if (strcmp(subcommand, "token") == 0) {
            char token[sizeof(config.bot_token)];

            if (!extract_token(&cursor, token, sizeof(token))) {
                copy_text(result->final_text, sizeof(result->final_text), "Usage: /telegram token <bot_token>");
            } else {
                copy_text(config.bot_token, sizeof(config.bot_token), token);
                config.enabled = true;
                if (console_telegram_set_config(&config, result->final_text, sizeof(result->final_text)) != 0 &&
                    result->final_text[0] == '\0') {
                    copy_text(result->final_text, sizeof(result->final_text), "Failed to save Telegram token.");
                }
            }
        } else if (strcmp(subcommand, "poll") == 0) {
            char seconds_text[16];
            unsigned long seconds;

            if (!extract_token(&cursor, seconds_text, sizeof(seconds_text))) {
                copy_text(result->final_text, sizeof(result->final_text), "Usage: /telegram poll <seconds>");
            } else {
                seconds = strtoul(seconds_text, NULL, 10);
                if (seconds == 0U) {
                    copy_text(result->final_text, sizeof(result->final_text), "Poll interval must be greater than 0.");
                } else {
                    config.poll_interval_seconds = (uint32_t)seconds;
                    if (console_telegram_set_config(&config, result->final_text, sizeof(result->final_text)) != 0 &&
                        result->final_text[0] == '\0') {
                        copy_text(result->final_text, sizeof(result->final_text), "Failed to save Telegram poll interval.");
                    }
                }
            }
        } else if (strcmp(subcommand, "enable") == 0) {
            config.enabled = true;
            if (console_telegram_set_config(&config, result->final_text, sizeof(result->final_text)) != 0 &&
                result->final_text[0] == '\0') {
                copy_text(result->final_text, sizeof(result->final_text), "Failed to enable Telegram.");
            }
        } else if (strcmp(subcommand, "disable") == 0) {
            config.enabled = false;
            if (console_telegram_set_config(&config, result->final_text, sizeof(result->final_text)) != 0 &&
                result->final_text[0] == '\0') {
                copy_text(result->final_text, sizeof(result->final_text), "Failed to disable Telegram.");
            }
        } else if (strcmp(subcommand, "clear-token") == 0) {
            config.bot_token[0] = '\0';
            if (console_telegram_set_config(&config, result->final_text, sizeof(result->final_text)) != 0 &&
                result->final_text[0] == '\0') {
                copy_text(result->final_text, sizeof(result->final_text), "Failed to clear Telegram token.");
            }
        } else {
            copy_text(
                result->final_text,
                sizeof(result->final_text),
                "Usage: /telegram status | /telegram token <bot_token> | /telegram poll <seconds> | /telegram enable | /telegram disable | /telegram clear-token"
            );
        }
    } else if (strcmp(command, "yolo") == 0) {
        char subcommand[16];

        if (!extract_token(&cursor, subcommand, sizeof(subcommand)) || strcmp(subcommand, "status") == 0) {
            snprintf(
                result->final_text,
                sizeof(result->final_text),
                "YOLO mode is %s.",
                espclaw_runtime_get_yolo_mode() ? "enabled" : "disabled"
            );
        } else if (strcmp(subcommand, "on") == 0 || strcmp(subcommand, "enable") == 0) {
            if (espclaw_runtime_set_yolo_mode(true, result->final_text, sizeof(result->final_text)) != 0 &&
                result->final_text[0] == '\0') {
                copy_text(result->final_text, sizeof(result->final_text), "Failed to enable YOLO mode.");
            }
        } else if (strcmp(subcommand, "off") == 0 || strcmp(subcommand, "disable") == 0) {
            if (espclaw_runtime_set_yolo_mode(false, result->final_text, sizeof(result->final_text)) != 0 &&
                result->final_text[0] == '\0') {
                copy_text(result->final_text, sizeof(result->final_text), "Failed to disable YOLO mode.");
            }
        } else {
            copy_text(result->final_text, sizeof(result->final_text), "Usage: /yolo status | on | off");
        }
    } else if (strcmp(command, "tool") == 0) {
        char tool_name[ESPCLAW_AGENT_TOOL_NAME_MAX + 1];
        char tool_result[ESPCLAW_AGENT_TEXT_MAX + 1];
        char json_body[ESPCLAW_AGENT_TOOL_ARGS_MAX + 1];

        if (!extract_token(&cursor, tool_name, sizeof(tool_name))) {
            copy_text(result->final_text, sizeof(result->final_text), "Usage: /tool <name> [json]");
        } else {
            const char *json_cursor = skip_spaces(cursor);

            if (json_cursor == NULL || *json_cursor == '\0') {
                copy_text(json_body, sizeof(json_body), "{}");
            } else {
                copy_text(json_body, sizeof(json_body), json_cursor);
                trim_trailing_spaces(json_body);
            }
            if (espclaw_agent_execute_tool(
                    workspace_root,
                    tool_name,
                    json_body,
                    true,
                    tool_result,
                    sizeof(tool_result)) != 0) {
                size_t used = (size_t)snprintf(
                    result->final_text,
                    sizeof(result->final_text),
                    "Tool %s returned: ",
                    tool_name);
                if (used >= sizeof(result->final_text)) {
                    used = sizeof(result->final_text) - 1;
                }
                copy_text(result->final_text + used, sizeof(result->final_text) - used, tool_result);
            } else {
                snprintf(result->final_text, sizeof(result->final_text), "%s", tool_result);
                result->used_tools = true;
            }
        }
    } else if (strcmp(command, "reboot") == 0) {
        copy_text(result->final_text, sizeof(result->final_text), "Rebooting now.");
        result->ok = true;
        append_console_exchange(workspace_root, session_id, input, result->final_text);
        console_reboot();
        return 0;
    } else if (strcmp(command, "factory-reset") == 0) {
        if (console_factory_reset(result->final_text, sizeof(result->final_text)) != 0 && result->final_text[0] == '\0') {
            copy_text(result->final_text, sizeof(result->final_text), "Factory reset failed.");
        }
        result->ok = true;
        append_console_exchange(workspace_root, session_id, input, result->final_text);
        return 0;
    } else {
        render_help(result->final_text, sizeof(result->final_text));
    }

    result->ok = true;
    append_console_exchange(workspace_root, session_id, input, result->final_text);
    return 0;
}
