#include "espclaw/telegram_protocol.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool copy_json_string_value(const char *start, char *buffer, size_t buffer_size)
{
    size_t index = 0;
    const char *cursor = start;

    if (start == NULL || buffer == NULL || buffer_size == 0 || *start != '"') {
        return false;
    }

    cursor++;
    while (*cursor != '\0' && *cursor != '"' && index + 1 < buffer_size) {
        if (*cursor == '\\' && cursor[1] != '\0') {
            cursor++;
            switch (*cursor) {
            case 'n':
                buffer[index++] = '\n';
                break;
            case 'r':
                buffer[index++] = '\r';
                break;
            case 't':
                buffer[index++] = '\t';
                break;
            default:
                buffer[index++] = *cursor;
                break;
            }
        } else {
            buffer[index++] = *cursor;
        }
        cursor++;
    }

    if (*cursor != '"') {
        return false;
    }

    buffer[index] = '\0';
    return true;
}

static const char *find_key(const char *json, const char *key)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    return strstr(json, pattern);
}

static bool extract_numeric_after_key(const char *json, const char *key, long long *value)
{
    const char *key_start = find_key(json, key);
    char *end_ptr = NULL;

    if (key_start == NULL || value == NULL) {
        return false;
    }

    key_start = strchr(key_start, ':');
    if (key_start == NULL) {
        return false;
    }
    key_start++;

    while (*key_start == ' ' || *key_start == '\n') {
        key_start++;
    }

    *value = strtoll(key_start, &end_ptr, 10);
    return end_ptr != key_start;
}

static bool extract_string_after_key(const char *json, const char *key, char *buffer, size_t buffer_size)
{
    const char *key_start = find_key(json, key);
    const char *value_start = NULL;

    if (key_start == NULL) {
        return false;
    }

    value_start = strchr(key_start, ':');
    if (value_start == NULL) {
        return false;
    }
    value_start++;

    while (*value_start == ' ' || *value_start == '\n') {
        value_start++;
    }

    return copy_json_string_value(value_start, buffer, buffer_size);
}

static size_t json_escape_string(const char *input, char *buffer, size_t buffer_size)
{
    size_t in_index = 0;
    size_t out_index = 0;

    if (buffer == NULL || buffer_size == 0) {
        return 0;
    }

    while (input != NULL && input[in_index] != '\0' && out_index + 2 < buffer_size) {
        char c = input[in_index++];

        switch (c) {
        case '\\':
        case '"':
            buffer[out_index++] = '\\';
            buffer[out_index++] = c;
            break;
        case '\n':
            buffer[out_index++] = '\\';
            buffer[out_index++] = 'n';
            break;
        case '\r':
            buffer[out_index++] = '\\';
            buffer[out_index++] = 'r';
            break;
        case '\t':
            buffer[out_index++] = '\\';
            buffer[out_index++] = 't';
            break;
        default:
            buffer[out_index++] = c;
            break;
        }
    }

    buffer[out_index] = '\0';
    return out_index;
}

bool espclaw_telegram_extract_update(const char *json, espclaw_telegram_update_t *update)
{
    const char *message_start = NULL;
    long long update_id = 0;
    long long message_id = 0;

    if (json == NULL || update == NULL) {
        return false;
    }

    memset(update, 0, sizeof(*update));

    if (!extract_numeric_after_key(json, "update_id", &update_id)) {
        return false;
    }
    update->update_id = (long)update_id;

    message_start = strstr(json, "\"message\"");
    if (message_start == NULL) {
        return false;
    }

    if (!extract_string_after_key(message_start, "text", update->text, sizeof(update->text))) {
        return false;
    }

    if (!extract_numeric_after_key(message_start, "message_id", &message_id)) {
        return false;
    }

    {
        long long chat_id = 0;
        long long from_id = 0;
        const char *chat_start = strstr(message_start, "\"chat\"");
        const char *from_start = strstr(message_start, "\"from\"");

        if (chat_start == NULL || from_start == NULL) {
            return false;
        }

        if (!extract_numeric_after_key(chat_start, "id", &chat_id)) {
            return false;
        }
        if (!extract_numeric_after_key(from_start, "id", &from_id)) {
            return false;
        }

        snprintf(update->chat_id, sizeof(update->chat_id), "%lld", chat_id);
        snprintf(update->from_id, sizeof(update->from_id), "%lld", from_id);
    }

    return true;
}

size_t espclaw_telegram_build_send_message_payload(
    const char *chat_id,
    const char *text,
    char *buffer,
    size_t buffer_size
)
{
    char escaped_text[1024];

    if (chat_id == NULL || text == NULL || buffer == NULL || buffer_size == 0) {
        return 0;
    }

    json_escape_string(text, escaped_text, sizeof(escaped_text));
    return (size_t)snprintf(
        buffer,
        buffer_size,
        "{\"chat_id\":\"%s\",\"text\":\"%s\"}",
        chat_id,
        escaped_text
    );
}
