#ifndef ESPCLAW_AGENT_LOOP_H
#define ESPCLAW_AGENT_LOOP_H

#include <stdbool.h>
#include <stddef.h>

#include "espclaw/auth_store.h"

#define ESPCLAW_AGENT_SESSION_ID_MAX 63
#define ESPCLAW_AGENT_RESPONSE_ID_MAX 95
#define ESPCLAW_AGENT_TOOL_CALL_MAX 8
#define ESPCLAW_AGENT_TOOL_NAME_MAX 63

#ifdef ESP_PLATFORM
#define ESPCLAW_AGENT_TEXT_MAX 8191
#define ESPCLAW_AGENT_TOOL_ARGS_MAX 4095
#else
#define ESPCLAW_AGENT_TEXT_MAX 8191
#define ESPCLAW_AGENT_TOOL_ARGS_MAX 4095
#endif

typedef struct {
    char call_id[ESPCLAW_AGENT_RESPONSE_ID_MAX + 1];
    char name[ESPCLAW_AGENT_TOOL_NAME_MAX + 1];
    char arguments_json[ESPCLAW_AGENT_TOOL_ARGS_MAX + 1];
} espclaw_agent_tool_call_t;

typedef struct {
    char role[16];
    char content[1024];
} espclaw_agent_history_message_t;

typedef struct {
    bool ok;
    bool used_tools;
    bool hit_iteration_limit;
    unsigned int iterations;
    char response_id[ESPCLAW_AGENT_RESPONSE_ID_MAX + 1];
    char final_text[ESPCLAW_AGENT_TEXT_MAX + 1];
} espclaw_agent_run_result_t;

typedef int (*espclaw_agent_http_adapter_t)(
    const char *url,
    const espclaw_auth_profile_t *profile,
    const char *body,
    char *response,
    size_t response_size,
    void *user_data
);

int espclaw_agent_loop_run(
    const char *workspace_root,
    const char *session_id,
    const char *user_message,
    bool allow_mutations,
    bool yolo_mode,
    espclaw_agent_run_result_t *result
);

int espclaw_agent_loop_run_stateless(
    const char *workspace_root,
    const char *session_id,
    const char *user_message,
    bool allow_mutations,
    bool yolo_mode,
    espclaw_agent_run_result_t *result
);

int espclaw_agent_loop_run_stateless_with_history(
    const char *workspace_root,
    const char *session_id,
    const char *user_message,
    bool allow_mutations,
    bool yolo_mode,
    const espclaw_agent_history_message_t *history,
    size_t history_count,
    espclaw_agent_run_result_t *result
);

int espclaw_agent_execute_tool(
    const char *workspace_root,
    const char *tool_name,
    const char *arguments_json,
    bool allow_mutations,
    char *buffer,
    size_t buffer_size
);

void espclaw_agent_set_http_adapter(espclaw_agent_http_adapter_t adapter, void *user_data);

int espclaw_agent_format_transport_error(
    int transport_err,
    int tls_code,
    int tls_flags,
    char *buffer,
    size_t buffer_size
);

int espclaw_agent_extract_sse_completed_response_json(
    const char *payload,
    char *buffer,
    size_t buffer_size
);

int espclaw_agent_extract_http_body_in_place(
    char *buffer,
    size_t *buffer_len,
    char *error_text,
    size_t error_text_size
);

int espclaw_agent_reduce_sse_stream_to_response_json(
    const char *payload,
    char *buffer,
    size_t buffer_size,
    char *error_text,
    size_t error_text_size
);

int espclaw_agent_store_terminal_response_json(
    const char *source_json,
    size_t source_len,
    char *buffer,
    size_t buffer_size
);

#endif
