#include "espclaw/app_patterns.h"

#include <stdio.h>

typedef struct {
    const char *name;
    const char *summary;
    const char *when_to_use;
    const char *shape;
} espclaw_app_pattern_t;

static const char *RULES[] = {
    "A one-shot action requested for right now should use direct tool calls.",
    "A repeated or timed action requested for right now should perform the full sequence. Do not collapse it to one hardware write.",
    "A component is reusable Lua code plus metadata. Install it under /workspace/components and expose it through /workspace/lib for require(...).",
    "An app is a user-facing feature bundle under /workspace/apps/<app_id> that owns product logic and triggers.",
    "A task is a live schedule for an app. Use it when work should run now but does not need to survive reboot.",
    "A behavior is a persisted task definition. Use it when work should autostart or survive reboot.",
    "An event is a named signal. Use it only when producers and consumers should be decoupled.",
    "Default to component plus app. Introduce events only when multiple live consumers need the same hardware or sensor stream.",
    "If the user asks for reusable logic, create an app instead of narrating a direct hardware step.",
    "If the user asks to run that reusable logic now in the background, call task.start after app.install.",
    "If the user asks for persistence, autostart, or reboot survival, call behavior.register after app.install.",
    "When the user specifies counts, durations, or pin assignments, preserve those exact values in the generated tool sequence or Lua source instead of falling back to a generic hello or echo app.",
    "If Lua source already exists in the workspace, prefer app.install_from_file or component.install_from_file over large inline source strings.",
    "If the large source was uploaded through the chunked blob store, prefer app.install_from_blob or component.install_from_blob.",
    "If installing a community-shared raw source URL directly, prefer app.install_from_url or component.install_from_url.",
    "If installing a community-shared reusable driver or helper with metadata, prefer component.install_from_manifest.",
    "Use inline app.install or component.install only when the source is comfortably small for a single tool call.",
    "For large markdown or docs in the workspace, prefer context.search, context.select, context.summarize, and context.load over reading the entire file into one turn.",
};

static const char *EXECUTION_EXAMPLES[] = {
    "\"flash gpio 4 five times now\" -> direct gpio writes with delays for the full sequence, not one write",
    "\"create a Lua task to blink ten times with 1 second on and 2 seconds off, then run it\" -> app.install with the requested timing in Lua, then task.start",
    "\"make that blink app survive reboot\" -> app.install, then behavior.register",
    "\"share an MS5611 driver across weather_station and vario\" -> component.install_from_manifest or component.install_from_url, then separate apps require it",
};

static const espclaw_app_pattern_t PATTERNS[] = {
    {
        "single_app_only",
        "One app owns the entire feature and talks to hardware directly.",
        "Use for one-off demos or local features that are not reused elsewhere.",
        "app.install -> optional task.start or behavior.register"
    },
    {
        "shared_component_plus_apps",
        "A reusable component provides low-level driver or math logic and multiple apps require it.",
        "Use for drivers, filters, mixers, or sensor adapters such as MS5611.",
        "component.install_from_manifest(ms5611) or component.install_from_url(ms5611) -> app.install(weather_station) + app.install(vario)"
    },
    {
        "sampler_behavior_and_event_consumers",
        "One sampler app owns the hardware and emits events while downstream apps consume those readings.",
        "Use when multiple live consumers need the same sensor updates without duplicate bus traffic.",
        "component.install(driver) -> app.install(sampler) -> behavior.register(sampler) -> event-driven consumers"
    },
};

size_t espclaw_render_app_patterns_json(char *buffer, size_t buffer_size)
{
    size_t used = 0;
    size_t index;

    if (buffer == NULL || buffer_size == 0) {
        return 0;
    }

    used += (size_t)snprintf(buffer + used, buffer_size - used, "{\"rules\":[");
    for (index = 0; index < sizeof(RULES) / sizeof(RULES[0]) && used + 8 < buffer_size; ++index) {
        used += (size_t)snprintf(buffer + used, buffer_size - used, "%s\"%s\"", index == 0 ? "" : ",", RULES[index]);
    }
    used += (size_t)snprintf(buffer + used, buffer_size - used, "],\"patterns\":[");
    for (index = 0; index < sizeof(PATTERNS) / sizeof(PATTERNS[0]) && used + 32 < buffer_size; ++index) {
        used += (size_t)snprintf(
            buffer + used,
            buffer_size - used,
            "%s{\"name\":\"%s\",\"summary\":\"%s\",\"when_to_use\":\"%s\",\"shape\":\"%s\"}",
            index == 0 ? "" : ",",
            PATTERNS[index].name,
            PATTERNS[index].summary,
            PATTERNS[index].when_to_use,
            PATTERNS[index].shape
        );
    }
    used += (size_t)snprintf(buffer + used, buffer_size - used, "]}");
    return used;
}

size_t espclaw_render_app_patterns_markdown(char *buffer, size_t buffer_size)
{
    size_t used = 0;
    size_t index;

    if (buffer == NULL || buffer_size == 0) {
        return 0;
    }

    used += (size_t)snprintf(buffer + used, buffer_size - used, "# App Patterns\n\n## Rules\n\n");
    for (index = 0; index < sizeof(RULES) / sizeof(RULES[0]) && used + 8 < buffer_size; ++index) {
        used += (size_t)snprintf(buffer + used, buffer_size - used, "- %s\n", RULES[index]);
    }
    used += (size_t)snprintf(buffer + used, buffer_size - used, "\n## Patterns\n");
    for (index = 0; index < sizeof(PATTERNS) / sizeof(PATTERNS[0]) && used + 16 < buffer_size; ++index) {
        used += (size_t)snprintf(
            buffer + used,
            buffer_size - used,
            "\n### %s\n- %s\n- Use when: %s\n- Shape: `%s`\n",
            PATTERNS[index].name,
            PATTERNS[index].summary,
            PATTERNS[index].when_to_use,
            PATTERNS[index].shape
        );
    }
    used += (size_t)snprintf(buffer + used, buffer_size - used, "\n## Execution Examples\n");
    for (index = 0; index < sizeof(EXECUTION_EXAMPLES) / sizeof(EXECUTION_EXAMPLES[0]) && used + 16 < buffer_size; ++index) {
        used += (size_t)snprintf(buffer + used, buffer_size - used, "- %s\n", EXECUTION_EXAMPLES[index]);
    }
    return used;
}

size_t espclaw_render_app_patterns_prompt_snapshot(char *buffer, size_t buffer_size)
{
    size_t used = 0;
    size_t index;

    if (buffer == NULL || buffer_size == 0) {
        return 0;
    }

    used += (size_t)snprintf(buffer + used, buffer_size - used, "# App Architecture Contract\n");
    for (index = 0; index < sizeof(RULES) / sizeof(RULES[0]) && used + 8 < buffer_size; ++index) {
        used += (size_t)snprintf(buffer + used, buffer_size - used, "- %s\n", RULES[index]);
    }
    used += (size_t)snprintf(buffer + used, buffer_size - used, "\n# Recommended Patterns\n");
    for (index = 0; index < sizeof(PATTERNS) / sizeof(PATTERNS[0]) && used + 16 < buffer_size; ++index) {
        used += (size_t)snprintf(
            buffer + used,
            buffer_size - used,
            "- %s: %s Shape: %s\n",
            PATTERNS[index].name,
            PATTERNS[index].summary,
            PATTERNS[index].shape
        );
    }
    used += (size_t)snprintf(buffer + used, buffer_size - used, "\n# Execution Choice Examples\n");
    for (index = 0; index < sizeof(EXECUTION_EXAMPLES) / sizeof(EXECUTION_EXAMPLES[0]) && used + 16 < buffer_size; ++index) {
        used += (size_t)snprintf(buffer + used, buffer_size - used, "- %s\n", EXECUTION_EXAMPLES[index]);
    }
    return used;
}
