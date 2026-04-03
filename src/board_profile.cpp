#include "espclaw/board_profile.h"

static espclaw_runtime_budget_t s3_runtime_budget(void)
{
    return (espclaw_runtime_budget_t){
        .memory_class = "full",
        .agent_history_max = 24,
        .agent_request_buffer_max = 65536,
        .agent_response_buffer_max = 65536,
        .agent_codex_items_max = 16384,
        .agent_instructions_max = 10240,
        .agent_tool_result_max = 1023,
        .agent_image_data_max = 32768,
        .agent_estimated_heap_bytes = 196608,
        .recommended_free_heap_bytes = 131072,
    };
}

static espclaw_runtime_budget_t esp32cam_runtime_budget(void)
{
    return (espclaw_runtime_budget_t){
        .memory_class = "balanced",
        .agent_history_max = 12,
        .agent_request_buffer_max = 65536,
        .agent_response_buffer_max = 131072,
        .agent_codex_items_max = 16384,
        .agent_instructions_max = 10240,
        .agent_tool_result_max = 1023,
        .agent_image_data_max = 32768,
        .agent_estimated_heap_bytes = 196608,
        .recommended_free_heap_bytes = 131072,
    };
}

const char *espclaw_storage_backend_name(espclaw_storage_backend_t backend)
{
    switch (backend) {
    case ESPCLAW_STORAGE_BACKEND_LITTLEFS:
        return "littlefs";
    case ESPCLAW_STORAGE_BACKEND_SD_CARD:
    default:
        return "sdcard";
    }
}

espclaw_board_profile_t espclaw_board_profile_for(espclaw_board_profile_id_t profile_id)
{
    switch (profile_id) {
    case ESPCLAW_BOARD_PROFILE_ESP32CAM:
        return (espclaw_board_profile_t){
            .profile_id = ESPCLAW_BOARD_PROFILE_ESP32CAM,
            .id = "esp32cam",
            .display_name = "ESP32-CAM",
            .provisioning = "softap",
            .default_storage_backend = ESPCLAW_STORAGE_BACKEND_SD_CARD,
            .cpu_cores = 2,
            .has_camera = true,
            .has_psram = true,
            .supports_ble_provisioning = false,
            .supports_concurrent_capture = false,
            .default_capture_width = 800,
            .default_capture_height = 600,
            .max_concurrent_sessions = 1,
            .ota_slot_count = 2,
            .runtime_budget = esp32cam_runtime_budget(),
        };
    case ESPCLAW_BOARD_PROFILE_ESP32S3:
    default:
        return (espclaw_board_profile_t){
            .profile_id = ESPCLAW_BOARD_PROFILE_ESP32S3,
            .id = "esp32s3",
            .display_name = "ESP32-S3",
            .provisioning = "ble",
            .default_storage_backend = ESPCLAW_STORAGE_BACKEND_SD_CARD,
            .cpu_cores = 2,
            .has_camera = true,
            .has_psram = true,
            .supports_ble_provisioning = true,
            .supports_concurrent_capture = true,
            .default_capture_width = 1280,
            .default_capture_height = 720,
            .max_concurrent_sessions = 2,
            .ota_slot_count = 2,
            .runtime_budget = s3_runtime_budget(),
        };
    }
}

espclaw_board_profile_t espclaw_board_profile_default(void)
{
#ifdef ESP_PLATFORM
#if defined(CONFIG_IDF_TARGET_ESP32)
    return espclaw_board_profile_for(ESPCLAW_BOARD_PROFILE_ESP32CAM);
#else
    return espclaw_board_profile_for(ESPCLAW_BOARD_PROFILE_ESP32S3);
#endif
#else
    return espclaw_board_profile_for(ESPCLAW_BOARD_PROFILE_ESP32S3);
#endif
}
