#ifndef ESPCLAW_RUNTIME_H
#define ESPCLAW_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

#ifdef ESP_PLATFORM
#include "esp_err.h"
#else
#ifndef ESPCLAW_HOST_ESP_ERR_T_DEFINED
#define ESPCLAW_HOST_ESP_ERR_T_DEFINED 1
typedef int esp_err_t;
#endif
#endif

#include "espclaw/agent_loop.h"
#include "espclaw/board_profile.h"
#include "espclaw/provisioning.h"

typedef struct {
    espclaw_board_profile_t profile;
    espclaw_storage_backend_t storage_backend;
    bool storage_ready;
    bool wifi_ready;
    bool wifi_boot_deferred;
    bool provisioning_active;
    bool telegram_ready;
    char workspace_root[128];
    char wifi_ssid[33];
    char onboarding_ssid[33];
    size_t storage_total_bytes;
    size_t storage_used_bytes;
} espclaw_runtime_status_t;

typedef struct {
    char ssid[33];
    int rssi;
    int channel;
    bool secure;
} espclaw_wifi_network_t;

typedef struct {
    bool enabled;
    bool configured;
    bool ready;
    uint32_t poll_interval_seconds;
    char bot_token[192];
    char token_hint[24];
} espclaw_telegram_config_t;

typedef enum {
    ESPCLAW_OPERATOR_SURFACE_WEB = 0,
    ESPCLAW_OPERATOR_SURFACE_UART = 1,
    ESPCLAW_OPERATOR_SURFACE_TELEGRAM = 2,
} espclaw_operator_surface_t;

typedef struct {
    size_t free_internal_before;
    size_t largest_internal_before;
    size_t free_internal_after;
    size_t largest_internal_after;
} espclaw_operator_bench_metrics_t;

esp_err_t espclaw_runtime_start(espclaw_board_profile_id_t profile_id, espclaw_runtime_status_t *status);
esp_err_t espclaw_runtime_start_operator_surfaces(void);
const espclaw_runtime_status_t *espclaw_runtime_status(void);
esp_err_t espclaw_runtime_wifi_scan(
    espclaw_wifi_network_t *networks,
    size_t max_networks,
    size_t *count_out
);
esp_err_t espclaw_runtime_wifi_join(
    const char *ssid,
    const char *password,
    char *message,
    size_t message_size
);
esp_err_t espclaw_runtime_get_telegram_config(espclaw_telegram_config_t *config);
esp_err_t espclaw_runtime_set_telegram_config(
    const espclaw_telegram_config_t *config,
    char *message,
    size_t message_size
);
bool espclaw_runtime_get_yolo_mode(void);
esp_err_t espclaw_runtime_set_yolo_mode(bool enabled, char *message, size_t message_size);
esp_err_t espclaw_runtime_bench_operator_turn(
    espclaw_operator_surface_t surface,
    const char *session_id,
    const char *input,
    bool allow_mutations,
    bool yolo_mode,
    espclaw_agent_run_result_t *result,
    espclaw_operator_bench_metrics_t *metrics
);
esp_err_t espclaw_runtime_get_provisioning_descriptor(espclaw_provisioning_descriptor_t *descriptor);
bool espclaw_runtime_time_is_sane(void);
bool espclaw_runtime_wait_for_time_sync(uint32_t timeout_ms);
esp_err_t espclaw_runtime_factory_reset(char *message, size_t message_size);
void espclaw_runtime_reboot(void);
#ifdef ESP_PLATFORM
bool espclaw_runtime_should_defer_wifi_boot(const espclaw_board_profile_t *profile, bool storage_ready);
bool espclaw_runtime_should_force_softap_only_boot(
    const espclaw_board_profile_t *profile,
    bool storage_ready,
    bool has_saved_wifi_credentials
);
#else
static inline bool espclaw_runtime_should_defer_wifi_boot(const espclaw_board_profile_t *profile, bool storage_ready)
{
    return profile != NULL &&
           profile->profile_id == ESPCLAW_BOARD_PROFILE_ESP32CAM &&
           !storage_ready;
}
static inline bool espclaw_runtime_should_force_softap_only_boot(
    const espclaw_board_profile_t *profile,
    bool storage_ready,
    bool has_saved_wifi_credentials
)
{
    return profile != NULL &&
           profile->profile_id == ESPCLAW_BOARD_PROFILE_ESP32CAM &&
           !storage_ready &&
           !has_saved_wifi_credentials;
}
#endif

#endif
