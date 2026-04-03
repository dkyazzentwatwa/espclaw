#ifndef ESPCLAW_BOARD_PROFILE_H
#define ESPCLAW_BOARD_PROFILE_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    ESPCLAW_STORAGE_BACKEND_SD_CARD = 0,
    ESPCLAW_STORAGE_BACKEND_LITTLEFS = 1
} espclaw_storage_backend_t;

typedef enum {
    ESPCLAW_BOARD_PROFILE_ESP32S3 = 0,
    ESPCLAW_BOARD_PROFILE_ESP32CAM = 1
} espclaw_board_profile_id_t;

typedef struct {
    const char *memory_class;
    size_t agent_history_max;
    size_t agent_request_buffer_max;
    size_t agent_response_buffer_max;
    size_t agent_codex_items_max;
    size_t agent_instructions_max;
    size_t agent_tool_result_max;
    size_t agent_image_data_max;
    size_t agent_estimated_heap_bytes;
    size_t recommended_free_heap_bytes;
} espclaw_runtime_budget_t;

typedef struct {
    espclaw_board_profile_id_t profile_id;
    const char *id;
    const char *display_name;
    const char *provisioning;
    espclaw_storage_backend_t default_storage_backend;
    int cpu_cores;
    bool has_camera;
    bool has_psram;
    bool supports_ble_provisioning;
    bool supports_concurrent_capture;
    size_t default_capture_width;
    size_t default_capture_height;
    size_t max_concurrent_sessions;
    size_t ota_slot_count;
    espclaw_runtime_budget_t runtime_budget;
} espclaw_board_profile_t;

espclaw_board_profile_t espclaw_board_profile_for(espclaw_board_profile_id_t profile_id);
espclaw_board_profile_t espclaw_board_profile_default(void);
const char *espclaw_storage_backend_name(espclaw_storage_backend_t backend);

#endif
