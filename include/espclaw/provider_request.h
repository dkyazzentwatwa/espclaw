#ifndef ESPCLAW_PROVIDER_REQUEST_H
#define ESPCLAW_PROVIDER_REQUEST_H

#include <stdbool.h>
#include <stddef.h>

size_t espclaw_render_openai_chat_request(
    const char *model,
    const char *system_prompt,
    const char *user_message,
    unsigned int max_tokens,
    bool tool_calls_enabled,
    char *buffer,
    size_t buffer_size
);

size_t espclaw_render_anthropic_messages_request(
    const char *model,
    const char *system_prompt,
    const char *user_message,
    unsigned int max_tokens,
    bool tool_calls_enabled,
    char *buffer,
    size_t buffer_size
);

#endif
