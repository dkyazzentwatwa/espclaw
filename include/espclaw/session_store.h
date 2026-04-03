#ifndef ESPCLAW_SESSION_STORE_H
#define ESPCLAW_SESSION_STORE_H

#include <stdbool.h>
#include <stddef.h>

bool espclaw_session_id_is_valid(const char *session_id);
int espclaw_session_append_message(
    const char *workspace_root,
    const char *session_id,
    const char *role,
    const char *content
);
int espclaw_session_read_transcript(
    const char *workspace_root,
    const char *session_id,
    char *buffer,
    size_t buffer_size
);

#endif
