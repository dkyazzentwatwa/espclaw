#include "espclaw/session_store.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "espclaw/workspace.h"

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

bool espclaw_session_id_is_valid(const char *session_id)
{
    size_t index;

    if (session_id == NULL || session_id[0] == '\0') {
        return false;
    }

    for (index = 0; session_id[index] != '\0'; ++index) {
        unsigned char c = (unsigned char)session_id[index];

        if (!(isalnum(c) || c == '-' || c == '_')) {
            return false;
        }
    }

    return true;
}

int espclaw_session_append_message(
    const char *workspace_root,
    const char *session_id,
    const char *role,
    const char *content
)
{
    char session_relative_path[320];
    char session_absolute_path[512];
    char escaped_role[64];
    char escaped_content[1024];
    FILE *file;

    if (workspace_root == NULL || role == NULL || content == NULL || !espclaw_session_id_is_valid(session_id)) {
        return -1;
    }

    /* Workspace bootstrap happens during runtime startup; repeating it here is unsafe on SD-backed boards. */
    snprintf(session_relative_path, sizeof(session_relative_path), "sessions/%s.jsonl", session_id);
    if (espclaw_workspace_resolve_path(workspace_root, session_relative_path, session_absolute_path, sizeof(session_absolute_path)) != 0) {
        return -1;
    }

    json_escape_string(role, escaped_role, sizeof(escaped_role));
    json_escape_string(content, escaped_content, sizeof(escaped_content));

    file = fopen(session_absolute_path, "a");
    if (file == NULL) {
        return -1;
    }

    fprintf(file, "{\"role\":\"%s\",\"content\":\"%s\"}\n", escaped_role, escaped_content);
    fclose(file);
    return 0;
}

int espclaw_session_read_transcript(
    const char *workspace_root,
    const char *session_id,
    char *buffer,
    size_t buffer_size
)
{
    char session_relative_path[320];
    char session_absolute_path[512];
    FILE *file;
    size_t bytes_read;

    if (workspace_root == NULL || buffer == NULL || buffer_size == 0 || !espclaw_session_id_is_valid(session_id)) {
        return -1;
    }

    snprintf(session_relative_path, sizeof(session_relative_path), "sessions/%s.jsonl", session_id);
    if (espclaw_workspace_resolve_path(workspace_root, session_relative_path, session_absolute_path, sizeof(session_absolute_path)) != 0) {
        return -1;
    }

    file = fopen(session_absolute_path, "r");
    if (file == NULL) {
        return -1;
    }

    bytes_read = fread(buffer, 1, buffer_size - 1, file);
    buffer[bytes_read] = '\0';
    fclose(file);
    return 0;
}
