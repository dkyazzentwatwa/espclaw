#include "espclaw/admin_api.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "espclaw/app_runtime.h"
#include "espclaw/auth_store.h"
#include "espclaw/behavior_runtime.h"
#include "espclaw/session_store.h"
#include "espclaw/task_policy.h"
#include "espclaw/tool_catalog.h"
#include "espclaw/workspace.h"

enum { ESPCLAW_ADMIN_APP_LIST_MAX = 32 };

static const char *ota_status_label(espclaw_ota_status_t status)
{
    switch (status) {
    case ESPCLAW_OTA_STATUS_IDLE:
        return "idle";
    case ESPCLAW_OTA_STATUS_DOWNLOADED:
        return "downloaded";
    case ESPCLAW_OTA_STATUS_PENDING_REBOOT:
        return "pending_reboot";
    case ESPCLAW_OTA_STATUS_VERIFYING:
        return "verifying";
    case ESPCLAW_OTA_STATUS_CONFIRMED:
        return "confirmed";
    case ESPCLAW_OTA_STATUS_ROLLBACK_REQUIRED:
        return "rollback_required";
    default:
        return "unknown";
    }
}

static size_t append_json_chunk(char *buffer, size_t buffer_size, size_t used, const char *fmt, ...)
{
    int written;
    va_list args;

    if (buffer == NULL || buffer_size == 0 || used >= buffer_size) {
        return used;
    }

    va_start(args, fmt);
    written = vsnprintf(buffer + used, buffer_size - used, fmt, args);
    va_end(args);

    if (written < 0) {
        return used;
    }

    if ((size_t)written >= buffer_size - used) {
        return buffer_size - 1;
    }

    return used + (size_t)written;
}

static size_t append_json_escaped_string(
    char *buffer,
    size_t buffer_size,
    size_t used,
    const char *value
)
{
    const char *cursor = value != NULL ? value : "";

    used = append_json_chunk(buffer, buffer_size, used, "\"");
    while (*cursor != '\0' && used + 2 < buffer_size) {
        char c = *cursor++;

        switch (c) {
        case '\\':
        case '"':
            used = append_json_chunk(buffer, buffer_size, used, "\\%c", c);
            break;
        case '\n':
            used = append_json_chunk(buffer, buffer_size, used, "\\n");
            break;
        case '\r':
            used = append_json_chunk(buffer, buffer_size, used, "\\r");
            break;
        case '\t':
            used = append_json_chunk(buffer, buffer_size, used, "\\t");
            break;
        default:
            used = append_json_chunk(buffer, buffer_size, used, "%c", c);
            break;
        }
    }
    used = append_json_chunk(buffer, buffer_size, used, "\"");
    return used;
}

static size_t append_string_array_json(
    char *buffer,
    size_t buffer_size,
    size_t used,
    char items[][ESPCLAW_APP_PERMISSION_NAME_MAX + 1],
    size_t count
)
{
    size_t index;

    used = append_json_chunk(buffer, buffer_size, used, "[");
    for (index = 0; index < count; ++index) {
        if (index > 0) {
            used = append_json_chunk(buffer, buffer_size, used, ",");
        }
        used = append_json_escaped_string(buffer, buffer_size, used, items[index]);
    }
    used = append_json_chunk(buffer, buffer_size, used, "]");
    return used;
}

static size_t append_trigger_array_json(
    char *buffer,
    size_t buffer_size,
    size_t used,
    char items[][ESPCLAW_APP_TRIGGER_NAME_MAX + 1],
    size_t count
)
{
    size_t index;

    used = append_json_chunk(buffer, buffer_size, used, "[");
    for (index = 0; index < count; ++index) {
        if (index > 0) {
            used = append_json_chunk(buffer, buffer_size, used, ",");
        }
        used = append_json_escaped_string(buffer, buffer_size, used, items[index]);
    }
    used = append_json_chunk(buffer, buffer_size, used, "]");
    return used;
}

static const char *tool_safety_label(espclaw_tool_safety_t safety)
{
    switch (safety) {
    case ESPCLAW_TOOL_SAFETY_CONFIRM_REQUIRED:
        return "confirm_required";
    case ESPCLAW_TOOL_SAFETY_READ_ONLY:
    default:
        return "read_only";
    }
}

size_t espclaw_render_admin_status_json(
    const espclaw_board_profile_t *profile,
    espclaw_storage_backend_t storage_backend,
    const char *provider_id,
    const char *channel_id,
    bool workspace_ready,
    bool yolo_mode,
    const espclaw_ota_state_t *ota_state,
    char *buffer,
    size_t buffer_size
)
{
    const char *provider = provider_id != NULL ? provider_id : "";
    const char *channel = channel_id != NULL ? channel_id : "";
    const char *profile_id = "";
    const char *provisioning = "";
    const char *storage = espclaw_storage_backend_name(storage_backend);
    const char *ota_status = "unknown";
    bool rollback_allowed = false;
    unsigned int target_slot = 0;

    if (buffer == NULL || buffer_size == 0) {
        return 0;
    }

    if (profile != NULL) {
        profile_id = profile->id;
        provisioning = profile->provisioning;
    }

    if (ota_state != NULL) {
        ota_status = ota_status_label(ota_state->status);
        rollback_allowed = ota_state->rollback_allowed;
        target_slot = ota_state->target_slot;
    }

    return (size_t)snprintf(
        buffer,
        buffer_size,
        "{"
        "\"board_profile\":\"%s\","
        "\"provisioning\":\"%s\","
        "\"storage_backend\":\"%s\","
        "\"workspace_ready\":%s,"
        "\"yolo_mode\":%s,"
        "\"provider\":\"%s\","
        "\"channel\":\"%s\","
        "\"ota\":{"
        "\"status\":\"%s\","
        "\"rollback_allowed\":%s,"
        "\"target_slot\":%u"
        "}"
        "}",
        profile_id,
        provisioning,
        storage,
        workspace_ready ? "true" : "false",
        yolo_mode ? "true" : "false",
        provider,
        channel,
        ota_status,
        rollback_allowed ? "true" : "false",
        target_slot
    );
}

size_t espclaw_render_workspace_files_json(
    const char *workspace_root,
    char *buffer,
    size_t buffer_size
)
{
    size_t used = 0;
    size_t index;

    if (buffer == NULL || buffer_size == 0) {
        return 0;
    }

    used = append_json_chunk(buffer, buffer_size, used, "{\"files\":[");

    for (index = 0; index < espclaw_workspace_file_count(); ++index) {
        char absolute_path[512];
        struct stat file_stat;
        const espclaw_workspace_file_t *file = espclaw_workspace_file_at(index);
        bool exists = false;

        if (file == NULL) {
            continue;
        }

        if (index > 0) {
            used = append_json_chunk(buffer, buffer_size, used, ",");
        }

        if (workspace_root != NULL &&
            espclaw_workspace_resolve_path(workspace_root, file->relative_path, absolute_path, sizeof(absolute_path)) == 0 &&
            stat(absolute_path, &file_stat) == 0) {
            exists = true;
        }

        used = append_json_chunk(
            buffer,
            buffer_size,
            used,
            "{\"path\":\"%s\",\"exists\":%s}",
            file->relative_path,
            exists ? "true" : "false"
        );
    }

    used = append_json_chunk(buffer, buffer_size, used, "]}");
    return used;
}

size_t espclaw_render_apps_json(
    const char *workspace_root,
    char *buffer,
    size_t buffer_size
)
{
    char ids[ESPCLAW_ADMIN_APP_LIST_MAX][ESPCLAW_APP_ID_MAX + 1];
    size_t count = 0;
    size_t used = 0;
    size_t index;

    if (buffer == NULL || buffer_size == 0) {
        return 0;
    }

    used = append_json_chunk(buffer, buffer_size, used, "{\"apps\":[");
    if (workspace_root != NULL && espclaw_app_collect_ids(workspace_root, ids, ESPCLAW_ADMIN_APP_LIST_MAX, &count) == 0) {
        for (index = 0; index < count; ++index) {
            espclaw_app_manifest_t manifest;

            if (espclaw_app_load_manifest(workspace_root, ids[index], &manifest) != 0) {
                continue;
            }

            if (index > 0) {
                used = append_json_chunk(buffer, buffer_size, used, ",");
            }

            used = append_json_chunk(buffer, buffer_size, used, "{");
            used = append_json_chunk(buffer, buffer_size, used, "\"id\":");
            used = append_json_escaped_string(buffer, buffer_size, used, manifest.app_id);
            used = append_json_chunk(buffer, buffer_size, used, ",\"title\":");
            used = append_json_escaped_string(buffer, buffer_size, used, manifest.title);
            used = append_json_chunk(buffer, buffer_size, used, ",\"version\":");
            used = append_json_escaped_string(buffer, buffer_size, used, manifest.version);
            used = append_json_chunk(buffer, buffer_size, used, ",\"entrypoint\":");
            used = append_json_escaped_string(buffer, buffer_size, used, manifest.entrypoint);
            used = append_json_chunk(buffer, buffer_size, used, ",\"permissions\":");
            used = append_string_array_json(
                buffer,
                buffer_size,
                used,
                manifest.permissions,
                manifest.permission_count
            );
            used = append_json_chunk(buffer, buffer_size, used, ",\"triggers\":");
            used = append_trigger_array_json(
                buffer,
                buffer_size,
                used,
                manifest.triggers,
                manifest.trigger_count
            );
            used = append_json_chunk(buffer, buffer_size, used, "}");
        }
    }
    used = append_json_chunk(buffer, buffer_size, used, "]}");
    return used;
}

size_t espclaw_render_tools_json(
    char *buffer,
    size_t buffer_size
)
{
    size_t used = 0;
    size_t index;

    if (buffer == NULL || buffer_size == 0) {
        return 0;
    }

    used = append_json_chunk(buffer, buffer_size, used, "{\"tools\":[");
    for (index = 0; index < espclaw_tool_count(); ++index) {
        const espclaw_tool_descriptor_t *tool = espclaw_tool_at(index);

        if (tool == NULL) {
            continue;
        }
        if (index > 0) {
            used = append_json_chunk(buffer, buffer_size, used, ",");
        }
        used = append_json_chunk(buffer, buffer_size, used, "{\"name\":");
        used = append_json_escaped_string(buffer, buffer_size, used, tool->name);
        used = append_json_chunk(buffer, buffer_size, used, ",\"summary\":");
        used = append_json_escaped_string(buffer, buffer_size, used, tool->summary);
        used = append_json_chunk(buffer, buffer_size, used, ",\"safety\":");
        used = append_json_escaped_string(buffer, buffer_size, used, tool_safety_label(tool->safety));
        used = append_json_chunk(buffer, buffer_size, used, ",\"parameters\":%s}", tool->parameters_json);
    }
    used = append_json_chunk(buffer, buffer_size, used, "]}");
    return used;
}

size_t espclaw_render_behaviors_json(
    const char *workspace_root,
    char *buffer,
    size_t buffer_size
)
{
    if (buffer == NULL || buffer_size == 0) {
        return 0;
    }
    if (espclaw_behavior_render_json(workspace_root, buffer, buffer_size) != 0) {
        return (size_t)snprintf(buffer, buffer_size, "{\"behaviors\":[]}");
    }
    return strlen(buffer);
}

size_t espclaw_render_app_detail_json(
    const char *workspace_root,
    const char *app_id,
    char *buffer,
    size_t buffer_size
)
{
    espclaw_app_manifest_t manifest;
    char source[4096];
    size_t used = 0;
    int read_result;

    if (buffer == NULL || buffer_size == 0) {
        return 0;
    }

    if (workspace_root == NULL || app_id == NULL ||
        espclaw_app_load_manifest(workspace_root, app_id, &manifest) != 0) {
        return (size_t)snprintf(buffer, buffer_size, "{\"error\":\"app_not_found\"}");
    }

    source[0] = '\0';
    read_result = espclaw_app_read_source(workspace_root, app_id, source, sizeof(source));

    used = append_json_chunk(buffer, buffer_size, used, "{\"app\":{");
    used = append_json_chunk(buffer, buffer_size, used, "\"id\":");
    used = append_json_escaped_string(buffer, buffer_size, used, manifest.app_id);
    used = append_json_chunk(buffer, buffer_size, used, ",\"title\":");
    used = append_json_escaped_string(buffer, buffer_size, used, manifest.title);
    used = append_json_chunk(buffer, buffer_size, used, ",\"version\":");
    used = append_json_escaped_string(buffer, buffer_size, used, manifest.version);
    used = append_json_chunk(buffer, buffer_size, used, ",\"entrypoint\":");
    used = append_json_escaped_string(buffer, buffer_size, used, manifest.entrypoint);
    used = append_json_chunk(buffer, buffer_size, used, ",\"permissions\":");
    used = append_string_array_json(
        buffer,
        buffer_size,
        used,
        manifest.permissions,
        manifest.permission_count
    );
    used = append_json_chunk(buffer, buffer_size, used, ",\"triggers\":");
    used = append_trigger_array_json(
        buffer,
        buffer_size,
        used,
        manifest.triggers,
        manifest.trigger_count
    );
    used = append_json_chunk(buffer, buffer_size, used, ",\"source_available\":%s", read_result == 0 ? "true" : "false");
    used = append_json_chunk(buffer, buffer_size, used, ",\"source\":");
    used = append_json_escaped_string(buffer, buffer_size, used, read_result == 0 ? source : "");
    used = append_json_chunk(buffer, buffer_size, used, "}}");
    return used;
}

size_t espclaw_render_auth_profile_json(
    const espclaw_auth_profile_t *profile,
    char *buffer,
    size_t buffer_size
)
{
    size_t used = 0;

    if (buffer == NULL || buffer_size == 0) {
        return 0;
    }
    if (profile == NULL) {
        return (size_t)snprintf(buffer, buffer_size, "{\"configured\":false}");
    }

    used = append_json_chunk(buffer, buffer_size, used, "{");
    used = append_json_chunk(buffer, buffer_size, used, "\"configured\":%s", profile->configured ? "true" : "false");
    used = append_json_chunk(buffer, buffer_size, used, ",\"provider_id\":");
    used = append_json_escaped_string(buffer, buffer_size, used, profile->provider_id);
    used = append_json_chunk(buffer, buffer_size, used, ",\"model\":");
    used = append_json_escaped_string(buffer, buffer_size, used, profile->model);
    used = append_json_chunk(buffer, buffer_size, used, ",\"base_url\":");
    used = append_json_escaped_string(buffer, buffer_size, used, profile->base_url);
    used = append_json_chunk(buffer, buffer_size, used, ",\"account_id\":");
    used = append_json_escaped_string(buffer, buffer_size, used, profile->account_id);
    used = append_json_chunk(buffer, buffer_size, used, ",\"source\":");
    used = append_json_escaped_string(buffer, buffer_size, used, profile->source);
    used = append_json_chunk(buffer, buffer_size, used, ",\"has_refresh_token\":%s", profile->refresh_token[0] != '\0' ? "true" : "false");
    used = append_json_chunk(buffer, buffer_size, used, ",\"expires_at\":%lld", (long long)profile->expires_at);
    used = append_json_chunk(buffer, buffer_size, used, "}");
    return used;
}

size_t espclaw_render_board_json(
    const espclaw_board_descriptor_t *board,
    char *buffer,
    size_t buffer_size
)
{
    espclaw_task_policy_t policy = espclaw_task_policy_current();
    size_t used = 0;
    size_t index;

    if (buffer == NULL || buffer_size == 0) {
        return 0;
    }
    if (board == NULL) {
        return (size_t)snprintf(buffer, buffer_size, "{\"configured\":false}");
    }

    used = append_json_chunk(buffer, buffer_size, used, "{\"configured\":true,\"variant\":");
    used = append_json_escaped_string(buffer, buffer_size, used, board->variant_id);
    used = append_json_chunk(buffer, buffer_size, used, ",\"display_name\":");
    used = append_json_escaped_string(buffer, buffer_size, used, board->display_name);
    used = append_json_chunk(buffer, buffer_size, used, ",\"source\":");
    used = append_json_escaped_string(buffer, buffer_size, used, board->source);
    used = append_json_chunk(
        buffer,
        buffer_size,
        used,
        ",\"task_policy\":{\"cpu_cores\":%d,\"admin_core\":%d,\"telegram_core\":%d,\"control_loop_core\":%d}",
        policy.cpu_cores,
        policy.admin_core,
        policy.telegram_core,
        policy.control_loop_core
    );

    used = append_json_chunk(buffer, buffer_size, used, ",\"pins\":[");
    for (index = 0; index < board->pin_count; ++index) {
        if (index > 0) {
            used = append_json_chunk(buffer, buffer_size, used, ",");
        }
        used = append_json_chunk(buffer, buffer_size, used, "{\"name\":");
        used = append_json_escaped_string(buffer, buffer_size, used, board->pins[index].name);
        used = append_json_chunk(buffer, buffer_size, used, ",\"pin\":%d}", board->pins[index].pin);
    }

    used = append_json_chunk(buffer, buffer_size, used, "],\"i2c\":[");
    for (index = 0; index < board->i2c_bus_count; ++index) {
        if (index > 0) {
            used = append_json_chunk(buffer, buffer_size, used, ",");
        }
        used = append_json_chunk(buffer, buffer_size, used, "{\"name\":");
        used = append_json_escaped_string(buffer, buffer_size, used, board->i2c_buses[index].name);
        used = append_json_chunk(
            buffer,
            buffer_size,
            used,
            ",\"port\":%d,\"sda\":%d,\"scl\":%d,\"frequency_hz\":%d}",
            board->i2c_buses[index].port,
            board->i2c_buses[index].sda_pin,
            board->i2c_buses[index].scl_pin,
            board->i2c_buses[index].frequency_hz
        );
    }

    used = append_json_chunk(buffer, buffer_size, used, "],\"uart\":[");
    for (index = 0; index < board->uart_count; ++index) {
        if (index > 0) {
            used = append_json_chunk(buffer, buffer_size, used, ",");
        }
        used = append_json_chunk(buffer, buffer_size, used, "{\"name\":");
        used = append_json_escaped_string(buffer, buffer_size, used, board->uarts[index].name);
        used = append_json_chunk(
            buffer,
            buffer_size,
            used,
            ",\"port\":%d,\"tx\":%d,\"rx\":%d,\"baud_rate\":%d}",
            board->uarts[index].port,
            board->uarts[index].tx_pin,
            board->uarts[index].rx_pin,
            board->uarts[index].baud_rate
        );
    }

    used = append_json_chunk(buffer, buffer_size, used, "],\"adc\":[");
    for (index = 0; index < board->adc_count; ++index) {
        if (index > 0) {
            used = append_json_chunk(buffer, buffer_size, used, ",");
        }
        used = append_json_chunk(buffer, buffer_size, used, "{\"name\":");
        used = append_json_escaped_string(buffer, buffer_size, used, board->adc_channels[index].name);
        used = append_json_chunk(
            buffer,
            buffer_size,
            used,
            ",\"unit\":%d,\"channel\":%d}",
            board->adc_channels[index].unit,
            board->adc_channels[index].channel
        );
    }
    used = append_json_chunk(buffer, buffer_size, used, "]}");
    return used;
}

size_t espclaw_render_hardware_json(
    const espclaw_board_descriptor_t *board,
    char *buffer,
    size_t buffer_size
)
{
    static const char *CAPABILITIES[] = {
        "apps", "tasks", "events", "event_watches", "fs", "gpio", "pwm", "ppm", "buzzer",
        "adc", "i2c", "uart", "camera", "temperature", "imu", "pid", "control"
    };
    size_t index;
    size_t used = 0;

    if (buffer == NULL || buffer_size == 0) {
        return 0;
    }
    if (board == NULL) {
        return (size_t)snprintf(buffer, buffer_size, "{\"configured\":false}");
    }

    used = append_json_chunk(buffer, buffer_size, used, "{\"configured\":true,\"board\":");
    used = append_json_chunk(buffer, buffer_size, used, "{");
    used = append_json_chunk(buffer, buffer_size, used, "\"variant\":");
    used = append_json_escaped_string(buffer, buffer_size, used, board->variant_id);
    used = append_json_chunk(buffer, buffer_size, used, ",\"display_name\":");
    used = append_json_escaped_string(buffer, buffer_size, used, board->display_name);
    used = append_json_chunk(buffer, buffer_size, used, ",\"source\":");
    used = append_json_escaped_string(buffer, buffer_size, used, board->source);
    used = append_json_chunk(buffer, buffer_size, used, "},\"capabilities\":[");
    for (index = 0; index < sizeof(CAPABILITIES) / sizeof(CAPABILITIES[0]); ++index) {
        if (index > 0) {
            used = append_json_chunk(buffer, buffer_size, used, ",");
        }
        used = append_json_escaped_string(buffer, buffer_size, used, CAPABILITIES[index]);
    }
    used = append_json_chunk(buffer, buffer_size, used, "],\"pins\":[");
    for (index = 0; index < board->pin_count; ++index) {
        if (index > 0) {
            used = append_json_chunk(buffer, buffer_size, used, ",");
        }
        used = append_json_chunk(buffer, buffer_size, used, "{\"name\":");
        used = append_json_escaped_string(buffer, buffer_size, used, board->pins[index].name);
        used = append_json_chunk(buffer, buffer_size, used, ",\"pin\":%d}", board->pins[index].pin);
    }
    used = append_json_chunk(buffer, buffer_size, used, "],\"i2c_buses\":[");
    for (index = 0; index < board->i2c_bus_count; ++index) {
        if (index > 0) {
            used = append_json_chunk(buffer, buffer_size, used, ",");
        }
        used = append_json_chunk(buffer, buffer_size, used, "{\"name\":");
        used = append_json_escaped_string(buffer, buffer_size, used, board->i2c_buses[index].name);
        used = append_json_chunk(
            buffer,
            buffer_size,
            used,
            ",\"port\":%d,\"sda\":%d,\"scl\":%d,\"frequency_hz\":%d}",
            board->i2c_buses[index].port,
            board->i2c_buses[index].sda_pin,
            board->i2c_buses[index].scl_pin,
            board->i2c_buses[index].frequency_hz
        );
    }
    used = append_json_chunk(buffer, buffer_size, used, "],\"uarts\":[");
    for (index = 0; index < board->uart_count; ++index) {
        if (index > 0) {
            used = append_json_chunk(buffer, buffer_size, used, ",");
        }
        used = append_json_chunk(buffer, buffer_size, used, "{\"name\":");
        used = append_json_escaped_string(buffer, buffer_size, used, board->uarts[index].name);
        used = append_json_chunk(
            buffer,
            buffer_size,
            used,
            ",\"port\":%d,\"tx\":%d,\"rx\":%d,\"baud_rate\":%d}",
            board->uarts[index].port,
            board->uarts[index].tx_pin,
            board->uarts[index].rx_pin,
            board->uarts[index].baud_rate
        );
    }
    used = append_json_chunk(buffer, buffer_size, used, "],\"adc_channels\":[");
    for (index = 0; index < board->adc_count; ++index) {
        if (index > 0) {
            used = append_json_chunk(buffer, buffer_size, used, ",");
        }
        used = append_json_chunk(buffer, buffer_size, used, "{\"name\":");
        used = append_json_escaped_string(buffer, buffer_size, used, board->adc_channels[index].name);
        used = append_json_chunk(
            buffer,
            buffer_size,
            used,
            ",\"unit\":%d,\"channel\":%d}",
            board->adc_channels[index].unit,
            board->adc_channels[index].channel
        );
    }
    used = append_json_chunk(buffer, buffer_size, used, "]}");
    return used;
}

size_t espclaw_render_board_presets_json(
    const espclaw_board_profile_t *profile,
    char *buffer,
    size_t buffer_size
)
{
    size_t used = 0;
    size_t index;
    size_t count;

    if (buffer == NULL || buffer_size == 0) {
        return 0;
    }

    count = espclaw_board_preset_count(profile);
    used = append_json_chunk(buffer, buffer_size, used, "{\"presets\":[");
    for (index = 0; index < count; ++index) {
        espclaw_board_descriptor_t descriptor;

        if (espclaw_board_preset_at(profile, index, &descriptor) != 0) {
            continue;
        }
        if (index > 0) {
            used = append_json_chunk(buffer, buffer_size, used, ",");
        }
        used = append_json_chunk(buffer, buffer_size, used, "{\"variant\":");
        used = append_json_escaped_string(buffer, buffer_size, used, descriptor.variant_id);
        used = append_json_chunk(buffer, buffer_size, used, ",\"display_name\":");
        used = append_json_escaped_string(buffer, buffer_size, used, descriptor.display_name);
        used = append_json_chunk(buffer, buffer_size, used, ",\"profile_id\":%d", descriptor.profile_id);
        used = append_json_chunk(buffer, buffer_size, used, "}");
    }
    used = append_json_chunk(buffer, buffer_size, used, "]}");
    return used;
}

size_t espclaw_render_board_config_json(
    const char *workspace_root,
    const espclaw_board_profile_t *profile,
    const espclaw_board_descriptor_t *board,
    char *buffer,
    size_t buffer_size
)
{
    char raw_json[2048];
    size_t used = 0;
    bool from_workspace = false;

    if (buffer == NULL || buffer_size == 0) {
        return 0;
    }

    if (workspace_root != NULL &&
        espclaw_workspace_read_file(workspace_root, "config/board.json", raw_json, sizeof(raw_json)) == 0) {
        from_workspace = true;
    } else {
        espclaw_board_descriptor_t fallback;

        if (board != NULL) {
            espclaw_board_render_minimal_config_json(board, raw_json, sizeof(raw_json));
        } else if (profile != NULL) {
            espclaw_board_descriptor_default_for_profile(profile, &fallback);
            espclaw_board_render_minimal_config_json(&fallback, raw_json, sizeof(raw_json));
        } else {
            snprintf(raw_json, sizeof(raw_json), "{\n  \"variant\": \"auto\"\n}\n");
        }
    }

    used = append_json_chunk(buffer, buffer_size, used, "{\"ok\":true,\"source\":");
    used = append_json_escaped_string(buffer, buffer_size, used, from_workspace ? "workspace" : "generated");
    used = append_json_chunk(buffer, buffer_size, used, ",\"raw_json\":");
    used = append_json_escaped_string(buffer, buffer_size, used, raw_json);
    used = append_json_chunk(buffer, buffer_size, used, "}");
    return used;
}

size_t espclaw_render_session_transcript_json(
    const char *workspace_root,
    const char *session_id,
    char *buffer,
    size_t buffer_size
)
{
    char *transcript = NULL;
    size_t used = 0;
    const char *source = "";

    if (buffer == NULL || buffer_size == 0) {
        return 0;
    }

    transcript = (char *)calloc(1, 8192);
    if (workspace_root != NULL && session_id != NULL &&
        transcript != NULL &&
        espclaw_session_read_transcript(workspace_root, session_id, transcript, 8192) == 0) {
        source = transcript;
    }

    used = append_json_chunk(buffer, buffer_size, used, "{\"session_id\":");
    used = append_json_escaped_string(buffer, buffer_size, used, session_id != NULL ? session_id : "");
    used = append_json_chunk(buffer, buffer_size, used, ",\"transcript\":");
    used = append_json_escaped_string(buffer, buffer_size, used, source);
    used = append_json_chunk(buffer, buffer_size, used, "}");
    free(transcript);
    return used;
}

size_t espclaw_render_system_monitor_json(
    const espclaw_system_monitor_snapshot_t *snapshot,
    char *buffer,
    size_t buffer_size
)
{
    size_t used = 0;
    unsigned int index;
    espclaw_system_monitor_snapshot_t empty;
    const espclaw_system_monitor_snapshot_t *value = snapshot;

    if (buffer == NULL || buffer_size == 0) {
        return 0;
    }
    if (value == NULL) {
        memset(&empty, 0, sizeof(empty));
        value = &empty;
    }

    used = append_json_chunk(
        buffer,
        buffer_size,
        used,
        "{"
        "\"available\":%s,"
        "\"memory_class\":",
        value->available ? "true" : "false"
    );
    used = append_json_escaped_string(buffer, buffer_size, used, value->memory_class != NULL ? value->memory_class : "unknown");
    used = append_json_chunk(
        buffer,
        buffer_size,
        used,
        ","
        "\"cpu_cores\":%u,"
        "\"dual_core\":%s,"
        "\"cpu_mhz\":%u,"
        "\"flash_chip_size_bytes\":%u,"
        "\"app_partition_size_bytes\":%u,"
        "\"app_image_size_bytes\":%u,"
        "\"workspace_total_bytes\":%u,"
        "\"workspace_used_bytes\":%u,"
        "\"ram_total_bytes\":%u,"
        "\"ram_free_bytes\":%u,"
        "\"ram_min_free_bytes\":%u,"
        "\"ram_largest_free_block_bytes\":%u,"
        "\"agent_estimated_heap_bytes\":%u,"
        "\"recommended_free_heap_bytes\":%u,"
        "\"agent_request_buffer_bytes\":%u,"
        "\"agent_response_buffer_bytes\":%u,"
        "\"agent_codex_items_bytes\":%u,"
        "\"agent_instructions_bytes\":%u,"
        "\"agent_tool_result_bytes\":%u,"
        "\"agent_image_data_bytes\":%u,"
        "\"agent_history_slots\":%u,"
        "\"cpu_load_percent\":[",
        value->cpu_cores,
        value->dual_core ? "true" : "false",
        value->cpu_mhz,
        (unsigned int)value->flash_chip_size_bytes,
        (unsigned int)value->app_partition_size_bytes,
        (unsigned int)value->app_image_size_bytes,
        (unsigned int)value->workspace_total_bytes,
        (unsigned int)value->workspace_used_bytes,
        (unsigned int)value->ram_total_bytes,
        (unsigned int)value->ram_free_bytes,
        (unsigned int)value->ram_min_free_bytes,
        (unsigned int)value->ram_largest_free_block_bytes,
        (unsigned int)value->agent_estimated_heap_bytes,
        (unsigned int)value->recommended_free_heap_bytes,
        (unsigned int)value->agent_request_buffer_bytes,
        (unsigned int)value->agent_response_buffer_bytes,
        (unsigned int)value->agent_codex_items_bytes,
        (unsigned int)value->agent_instructions_bytes,
        (unsigned int)value->agent_tool_result_bytes,
        (unsigned int)value->agent_image_data_bytes,
        (unsigned int)value->agent_history_slots
    );
    for (index = 0; index < value->cpu_cores && index < ESPCLAW_SYSTEM_MONITOR_MAX_CORES; ++index) {
        if (index > 0U) {
            used = append_json_chunk(buffer, buffer_size, used, ",");
        }
        used = append_json_chunk(buffer, buffer_size, used, "%u", value->cpu_load_percent[index]);
    }
    used = append_json_chunk(buffer, buffer_size, used, "]}");
    return used;
}

size_t espclaw_render_camera_status_json(
    const espclaw_hw_camera_status_t *status,
    char *buffer,
    size_t buffer_size
)
{
    size_t used = 0;
    espclaw_hw_camera_status_t empty;
    const espclaw_hw_camera_status_t *value = status;

    if (buffer == NULL || buffer_size == 0) {
        return 0;
    }
    if (value == NULL) {
        memset(&empty, 0, sizeof(empty));
        value = &empty;
    }

    used = append_json_chunk(
        buffer,
        buffer_size,
        used,
        "{"
        "\"supported\":%s,"
        "\"initialized\":%s,"
        "\"simulated\":%s,"
        "\"last_capture_ok\":%s,"
        "\"last_width\":%u,"
        "\"last_height\":%u,"
        "\"last_bytes_written\":%u,"
        "\"board_variant\":",
        value->supported ? "true" : "false",
        value->initialized ? "true" : "false",
        value->simulated ? "true" : "false",
        value->last_capture_ok ? "true" : "false",
        (unsigned int)value->last_width,
        (unsigned int)value->last_height,
        (unsigned int)value->last_bytes_written
    );
    used = append_json_escaped_string(buffer, buffer_size, used, value->board_variant);
    used = append_json_chunk(buffer, buffer_size, used, ",\"last_relative_path\":");
    used = append_json_escaped_string(buffer, buffer_size, used, value->last_relative_path);
    used = append_json_chunk(buffer, buffer_size, used, ",\"last_error\":");
    used = append_json_escaped_string(buffer, buffer_size, used, value->last_error);
    used = append_json_chunk(buffer, buffer_size, used, "}");
    return used;
}

size_t espclaw_render_ota_status_json(
    const espclaw_ota_snapshot_t *snapshot,
    char *buffer,
    size_t buffer_size
)
{
    const char *status = "unknown";

    if (buffer == NULL || buffer_size == 0) {
        return 0;
    }
    if (snapshot != NULL) {
        status = ota_status_label(snapshot->state.status);
    }

    return (size_t)snprintf(
        buffer,
        buffer_size,
        "{"
        "\"supported\":%s,"
        "\"upload_in_progress\":%s,"
        "\"expected_bytes\":%u,"
        "\"written_bytes\":%u,"
        "\"running_partition\":\"%s\","
        "\"target_partition\":\"%s\","
        "\"status\":\"%s\","
        "\"rollback_allowed\":%s,"
        "\"target_slot\":%u,"
        "\"message\":\"%s\""
        "}",
        snapshot != NULL && snapshot->supported ? "true" : "false",
        snapshot != NULL && snapshot->upload_in_progress ? "true" : "false",
        snapshot != NULL ? (unsigned int)snapshot->expected_bytes : 0U,
        snapshot != NULL ? (unsigned int)snapshot->written_bytes : 0U,
        snapshot != NULL ? snapshot->running_partition_label : "",
        snapshot != NULL ? snapshot->target_partition_label : "",
        status,
        snapshot != NULL && snapshot->state.rollback_allowed ? "true" : "false",
        snapshot != NULL ? snapshot->state.target_slot : 0U,
        snapshot != NULL ? snapshot->last_message : ""
    );
}
