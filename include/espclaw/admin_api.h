#ifndef ESPCLAW_ADMIN_API_H
#define ESPCLAW_ADMIN_API_H

#include <stdbool.h>
#include <stddef.h>

#include "espclaw/auth_store.h"
#include "espclaw/board_config.h"
#include "espclaw/board_profile.h"
#include "espclaw/hardware.h"
#include "espclaw/ota_manager.h"
#include "espclaw/ota_state.h"
#include "espclaw/system_monitor.h"

size_t espclaw_render_admin_status_json(
    const espclaw_board_profile_t *profile,
    espclaw_storage_backend_t storage_backend,
    const char *provider_id,
    const char *channel_id,
    bool workspace_ready,
    bool yolo_mode,
    const espclaw_ota_state_t *ota_state,
    char *buffer,
    size_t buffer_size
);

size_t espclaw_render_workspace_files_json(
    const char *workspace_root,
    char *buffer,
    size_t buffer_size
);
size_t espclaw_render_apps_json(
    const char *workspace_root,
    char *buffer,
    size_t buffer_size
);
size_t espclaw_render_tools_json(
    char *buffer,
    size_t buffer_size
);
size_t espclaw_render_behaviors_json(
    const char *workspace_root,
    char *buffer,
    size_t buffer_size
);
size_t espclaw_render_app_detail_json(
    const char *workspace_root,
    const char *app_id,
    char *buffer,
    size_t buffer_size
);
size_t espclaw_render_auth_profile_json(
    const espclaw_auth_profile_t *profile,
    char *buffer,
    size_t buffer_size
);
size_t espclaw_render_board_json(
    const espclaw_board_descriptor_t *board,
    char *buffer,
    size_t buffer_size
);
size_t espclaw_render_hardware_json(
    const espclaw_board_descriptor_t *board,
    char *buffer,
    size_t buffer_size
);
size_t espclaw_render_board_presets_json(
    const espclaw_board_profile_t *profile,
    char *buffer,
    size_t buffer_size
);
size_t espclaw_render_board_config_json(
    const char *workspace_root,
    const espclaw_board_profile_t *profile,
    const espclaw_board_descriptor_t *board,
    char *buffer,
    size_t buffer_size
);
size_t espclaw_render_session_transcript_json(
    const char *workspace_root,
    const char *session_id,
    char *buffer,
    size_t buffer_size
);
size_t espclaw_render_system_monitor_json(
    const espclaw_system_monitor_snapshot_t *snapshot,
    char *buffer,
    size_t buffer_size
);
size_t espclaw_render_camera_status_json(
    const espclaw_hw_camera_status_t *status,
    char *buffer,
    size_t buffer_size
);
size_t espclaw_render_ota_status_json(
    const espclaw_ota_snapshot_t *snapshot,
    char *buffer,
    size_t buffer_size
);

#endif
