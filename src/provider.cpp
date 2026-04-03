#include "espclaw/provider.h"

#include <stddef.h>
#include <string.h>

static const espclaw_provider_descriptor_t PROVIDERS[] = {
    {
        .type = ESPCLAW_PROVIDER_OPENAI_COMPAT,
        .id = "openai_compat",
        .display_name = "OpenAI-Compatible",
        .default_model = "gpt-4.1-mini",
        .default_base_url = "https://api.openai.com/v1",
        .supports_vision = true,
        .supports_tool_calls = true,
        .supports_streaming = true,
        .requires_account_id = false,
    },
    {
        .type = ESPCLAW_PROVIDER_ANTHROPIC_MESSAGES,
        .id = "anthropic_messages",
        .display_name = "Anthropic Messages",
        .default_model = "claude-3-7-sonnet-latest",
        .default_base_url = "https://api.anthropic.com/v1",
        .supports_vision = true,
        .supports_tool_calls = true,
        .supports_streaming = true,
        .requires_account_id = false,
    },
    {
        .type = ESPCLAW_PROVIDER_OPENAI_CODEX,
        .id = "openai_codex",
        .display_name = "OpenAI Codex (ChatGPT OAuth)",
        .default_model = "gpt-5.3-codex",
        .default_base_url = "https://chatgpt.com/backend-api/codex",
        .supports_vision = true,
        .supports_tool_calls = true,
        .supports_streaming = true,
        .requires_account_id = true,
    },
};

size_t espclaw_provider_count(void)
{
    return sizeof(PROVIDERS) / sizeof(PROVIDERS[0]);
}

const espclaw_provider_descriptor_t *espclaw_provider_at(size_t index)
{
    if (index >= espclaw_provider_count()) {
        return NULL;
    }
    return &PROVIDERS[index];
}

const espclaw_provider_descriptor_t *espclaw_find_provider(const char *id)
{
    size_t index;

    if (id == NULL) {
        return NULL;
    }

    for (index = 0; index < espclaw_provider_count(); ++index) {
        if (strcmp(PROVIDERS[index].id, id) == 0) {
            return &PROVIDERS[index];
        }
    }

    return NULL;
}
