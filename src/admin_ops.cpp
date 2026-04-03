#include "espclaw/admin_ops.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "espclaw/app_runtime.h"

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    return -1;
}

static void url_decode_into(const char *value, char *buffer, size_t buffer_size)
{
    size_t used = 0;

    if (buffer == NULL || buffer_size == 0) {
        return;
    }

    while (value != NULL && *value != '\0' && *value != '&' && used + 1 < buffer_size) {
        if (*value == '+' ) {
            buffer[used++] = ' ';
            value++;
        } else if (*value == '%' && isxdigit((unsigned char)value[1]) && isxdigit((unsigned char)value[2])) {
            int hi = hex_value(value[1]);
            int lo = hex_value(value[2]);

            if (hi >= 0 && lo >= 0) {
                buffer[used++] = (char)((hi << 4) | lo);
                value += 3;
            } else {
                buffer[used++] = *value++;
            }
        } else {
            buffer[used++] = *value++;
        }
    }

    buffer[used] = '\0';
}

static size_t append_json_escaped(char *buffer, size_t buffer_size, size_t used, const char *value)
{
    const char *cursor = value != NULL ? value : "";

    if (buffer == NULL || buffer_size == 0 || used >= buffer_size) {
        return used;
    }

    used += (size_t)snprintf(buffer + used, buffer_size - used, "\"");
    if (used >= buffer_size) {
        buffer[buffer_size - 1] = '\0';
        return buffer_size - 1;
    }

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
        if (used >= buffer_size) {
            buffer[buffer_size - 1] = '\0';
            return buffer_size - 1;
        }
    }

    used += (size_t)snprintf(buffer + used, buffer_size - used, "\"");
    if (used >= buffer_size) {
        buffer[buffer_size - 1] = '\0';
        return buffer_size - 1;
    }
    return used;
}

bool espclaw_admin_query_value(
    const char *query,
    const char *key,
    char *buffer,
    size_t buffer_size
)
{
    size_t key_length;
    const char *cursor;

    if (query == NULL || key == NULL || buffer == NULL || buffer_size == 0) {
        return false;
    }

    key_length = strlen(key);
    cursor = query;
    while (*cursor != '\0') {
        const char *pair_end = strchr(cursor, '&');
        size_t pair_length = pair_end != NULL ? (size_t)(pair_end - cursor) : strlen(cursor);

        if (pair_length > key_length && strncmp(cursor, key, key_length) == 0 && cursor[key_length] == '=') {
            url_decode_into(cursor + key_length + 1, buffer, buffer_size);
            return true;
        }

        if (pair_end == NULL) {
            break;
        }
        cursor = pair_end + 1;
    }

    buffer[0] = '\0';
    return false;
}

bool espclaw_admin_json_string_value(
    const char *json,
    const char *key,
    char *buffer,
    size_t buffer_size
)
{
    char pattern[64];
    const char *cursor;
    size_t used = 0;

    if (json == NULL || key == NULL || buffer == NULL || buffer_size == 0) {
        return false;
    }

    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    cursor = strstr(json, pattern);
    if (cursor == NULL) {
        buffer[0] = '\0';
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
    return true;
}

bool espclaw_admin_json_long_value(
    const char *json,
    const char *key,
    long *value
)
{
    char pattern[64];
    const char *cursor;
    char *end_ptr = NULL;

    if (json == NULL || key == NULL || value == NULL) {
        return false;
    }

    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    cursor = strstr(json, pattern);
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

bool espclaw_admin_json_i64_value(
    const char *json,
    const char *key,
    int64_t *value
)
{
    char pattern[64];
    const char *cursor;
    char *end_ptr = NULL;

    if (json == NULL || key == NULL || value == NULL) {
        return false;
    }

    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    cursor = strstr(json, pattern);
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

    *value = strtoll(cursor, &end_ptr, 10);
    return end_ptr != cursor;
}

size_t espclaw_admin_render_result_json(
    bool ok,
    const char *message,
    char *buffer,
    size_t buffer_size
)
{
    size_t used = 0;

    if (buffer == NULL || buffer_size == 0) {
        return 0;
    }

    used += (size_t)snprintf(buffer, buffer_size, "{\"ok\":%s,\"message\":", ok ? "true" : "false");
    if (used >= buffer_size) {
        buffer[buffer_size - 1] = '\0';
        return buffer_size - 1;
    }

    used = append_json_escaped(buffer, buffer_size, used, message != NULL ? message : "");
    if (used + 2 >= buffer_size) {
        buffer[buffer_size - 1] = '\0';
        return buffer_size - 1;
    }

    used += (size_t)snprintf(buffer + used, buffer_size - used, "}");
    return used >= buffer_size ? buffer_size - 1 : used;
}

size_t espclaw_admin_render_run_result_json(
    const char *app_id,
    const char *trigger,
    bool ok,
    const char *result,
    char *buffer,
    size_t buffer_size
)
{
    size_t used = 0;

    if (buffer == NULL || buffer_size == 0) {
        return 0;
    }

    used += (size_t)snprintf(buffer, buffer_size, "{\"ok\":%s,\"app_id\":", ok ? "true" : "false");
    if (used >= buffer_size) {
        buffer[buffer_size - 1] = '\0';
        return buffer_size - 1;
    }

    used = append_json_escaped(buffer, buffer_size, used, app_id != NULL ? app_id : "");
    used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"trigger\":");
    if (used >= buffer_size) {
        buffer[buffer_size - 1] = '\0';
        return buffer_size - 1;
    }

    used = append_json_escaped(buffer, buffer_size, used, trigger != NULL ? trigger : "");
    used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"result\":");
    if (used >= buffer_size) {
        buffer[buffer_size - 1] = '\0';
        return buffer_size - 1;
    }

    used = append_json_escaped(buffer, buffer_size, used, result != NULL ? result : "");
    used += (size_t)snprintf(buffer + used, buffer_size - used, "}");
    return used >= buffer_size ? buffer_size - 1 : used;
}

int espclaw_admin_scaffold_app(
    const char *workspace_root,
    const char *app_id,
    const char *title,
    const char *permissions_csv,
    const char *triggers_csv
)
{
    char app_title[ESPCLAW_APP_TITLE_MAX + 1];

    if (workspace_root == NULL || !espclaw_app_id_is_valid(app_id)) {
        return -1;
    }

    snprintf(app_title, sizeof(app_title), "%s", title != NULL && title[0] != '\0' ? title : app_id);
    return espclaw_app_scaffold_lua(
        workspace_root,
        app_id,
        app_title,
        permissions_csv != NULL && permissions_csv[0] != '\0' ? permissions_csv : "fs.read,fs.write",
        triggers_csv != NULL && triggers_csv[0] != '\0' ? triggers_csv : "boot,telegram,manual"
    );
}

int espclaw_admin_scaffold_default_app(
    const char *workspace_root,
    const char *app_id
)
{
    char app_title[ESPCLAW_APP_TITLE_MAX + 1];

    if (workspace_root == NULL || !espclaw_app_id_is_valid(app_id)) {
        return -1;
    }

    snprintf(app_title, sizeof(app_title), "%s app", app_id);
    return espclaw_admin_scaffold_app(
        workspace_root,
        app_id,
        app_title,
        "fs.read,fs.write",
        "boot,telegram,manual"
    );
}
