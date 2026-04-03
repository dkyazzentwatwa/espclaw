#include "espclaw/provider_request.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static size_t json_escape(const char *input, char *buffer, size_t buffer_size)
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

static const char *tool_stub_openai(bool enabled)
{
    if (!enabled) {
        return "[]";
    }

    return "[{"
           "\"type\":\"function\","
           "\"function\":{"
           "\"name\":\"fs.read\","
           "\"description\":\"Read file content from the workspace.\","
           "\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}"
           "}"
           "}]";
}

static const char *tool_stub_anthropic(bool enabled)
{
    if (!enabled) {
        return "[]";
    }

    return "[{"
           "\"name\":\"fs.read\","
           "\"description\":\"Read file content from the workspace.\","
           "\"input_schema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}"
           "}]";
}

size_t espclaw_render_openai_chat_request(
    const char *model,
    const char *system_prompt,
    const char *user_message,
    unsigned int max_tokens,
    bool tool_calls_enabled,
    char *buffer,
    size_t buffer_size
)
{
    char escaped_system[2048];
    char escaped_user[2048];

    if (buffer == NULL || buffer_size == 0 || model == NULL || user_message == NULL) {
        return 0;
    }

    json_escape(system_prompt != NULL ? system_prompt : "", escaped_system, sizeof(escaped_system));
    json_escape(user_message, escaped_user, sizeof(escaped_user));

    return (size_t)snprintf(
        buffer,
        buffer_size,
        "{"
        "\"model\":\"%s\","
        "\"max_tokens\":%u,"
        "\"messages\":["
        "{\"role\":\"system\",\"content\":\"%s\"},"
        "{\"role\":\"user\",\"content\":\"%s\"}"
        "],"
        "\"tools\":%s"
        "}",
        model,
        max_tokens,
        escaped_system,
        escaped_user,
        tool_stub_openai(tool_calls_enabled)
    );
}

size_t espclaw_render_anthropic_messages_request(
    const char *model,
    const char *system_prompt,
    const char *user_message,
    unsigned int max_tokens,
    bool tool_calls_enabled,
    char *buffer,
    size_t buffer_size
)
{
    char escaped_system[2048];
    char escaped_user[2048];

    if (buffer == NULL || buffer_size == 0 || model == NULL || user_message == NULL) {
        return 0;
    }

    json_escape(system_prompt != NULL ? system_prompt : "", escaped_system, sizeof(escaped_system));
    json_escape(user_message, escaped_user, sizeof(escaped_user));

    return (size_t)snprintf(
        buffer,
        buffer_size,
        "{"
        "\"model\":\"%s\","
        "\"max_tokens\":%u,"
        "\"system\":\"%s\","
        "\"messages\":["
        "{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"%s\"}]}"
        "],"
        "\"tools\":%s"
        "}",
        model,
        max_tokens,
        escaped_system,
        escaped_user,
        tool_stub_anthropic(tool_calls_enabled)
    );
}
