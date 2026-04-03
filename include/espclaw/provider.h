#ifndef ESPCLAW_PROVIDER_H
#define ESPCLAW_PROVIDER_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    ESPCLAW_PROVIDER_OPENAI_COMPAT = 0,
    ESPCLAW_PROVIDER_ANTHROPIC_MESSAGES = 1,
    ESPCLAW_PROVIDER_OPENAI_CODEX = 2
} espclaw_provider_type_t;

typedef struct {
    espclaw_provider_type_t type;
    const char *id;
    const char *display_name;
    const char *default_model;
    const char *default_base_url;
    bool supports_vision;
    bool supports_tool_calls;
    bool supports_streaming;
    bool requires_account_id;
} espclaw_provider_descriptor_t;

size_t espclaw_provider_count(void);
const espclaw_provider_descriptor_t *espclaw_provider_at(size_t index);
const espclaw_provider_descriptor_t *espclaw_find_provider(const char *id);

#endif
