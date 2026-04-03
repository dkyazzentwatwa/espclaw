#include "espclaw/config_render.h"

#include <stdio.h>

size_t espclaw_render_default_config(
    const espclaw_board_profile_t *profile,
    char *buffer,
    size_t buffer_size
)
{
    int written;

    if (profile == NULL || buffer == NULL || buffer_size == 0) {
        return 0;
    }

    written = snprintf(
        buffer,
        buffer_size,
        "{\n"
        "  \"device\": {\n"
        "    \"name\": \"espclaw\",\n"
        "    \"board_profile\": \"%s\",\n"
        "    \"board_variant\": \"auto\",\n"
        "    \"cpu_cores\": %u\n"
        "  },\n"
        "  \"storage\": {\n"
        "    \"backend\": \"%s\",\n"
        "    \"workspace_root\": \"/workspace\",\n"
        "    \"secret_store\": \"nvs\"\n"
        "  },\n"
        "  \"network\": {\n"
        "    \"provisioning\": \"%s\",\n"
        "    \"wifi_mode\": \"station\"\n"
        "  },\n"
        "  \"providers\": [\n"
        "    {\n"
        "      \"id\": \"primary\",\n"
        "      \"type\": \"openai_compat\",\n"
        "      \"model\": \"gpt-4.1-mini\",\n"
        "      \"api_base\": \"https://api.openai.com/v1\"\n"
        "    }\n"
        "  ],\n"
        "  \"agent\": {\n"
        "    \"max_tool_iterations\": 8,\n"
        "    \"session_summary_limit\": 12\n"
        "  },\n"
        "  \"channels\": {\n"
        "    \"telegram\": {\n"
        "      \"enabled\": true,\n"
        "      \"transport\": \"polling\",\n"
        "      \"poll_interval_seconds\": 3,\n"
        "      \"media_enabled\": true\n"
        "    }\n"
        "  },\n"
        "  \"tools\": {\n"
        "    \"mutating_actions_require_confirm\": true,\n"
        "    \"enabled\": [\"fs.read\", \"fs.write\", \"wifi.scan\", \"camera.capture\", \"ota.apply\"]\n"
        "  },\n"
        "  \"camera\": {\n"
        "    \"enabled\": %s,\n"
        "    \"default_width\": %u,\n"
        "    \"default_height\": %u,\n"
        "    \"format\": \"jpeg\"\n"
        "  },\n"
        "  \"ota\": {\n"
        "    \"mode\": \"single_slot\",\n"
        "    \"auto_check\": false\n"
        "  },\n"
        "  \"security\": {\n"
        "    \"admin_auth_required\": true,\n"
        "    \"allow_public_admin\": false\n"
        "  }\n"
        "}\n",
        profile->id,
        (unsigned)profile->cpu_cores,
        espclaw_storage_backend_name(profile->default_storage_backend),
        profile->provisioning,
        profile->has_camera ? "true" : "false",
        (unsigned)profile->default_capture_width,
        (unsigned)profile->default_capture_height
    );

    if (written < 0) {
        return 0;
    }

    return (size_t)written;
}
