#ifndef ESPCLAW_OTA_MANAGER_H
#define ESPCLAW_OTA_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "espclaw/ota_state.h"

#ifdef ESP_PLATFORM
#include "esp_err.h"
#else
#ifndef ESPCLAW_HOST_ESP_ERR_T_DEFINED
#define ESPCLAW_HOST_ESP_ERR_T_DEFINED 1
typedef int esp_err_t;
#endif
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#endif

typedef struct {
    bool supported;
    bool upload_in_progress;
    size_t expected_bytes;
    size_t written_bytes;
    espclaw_ota_state_t state;
    char running_partition_label[17];
    char target_partition_label[17];
    char last_message[128];
} espclaw_ota_snapshot_t;

void espclaw_ota_manager_init(void);
void espclaw_ota_manager_snapshot(espclaw_ota_snapshot_t *snapshot);
esp_err_t espclaw_ota_manager_confirm_running(char *message, size_t message_size);
esp_err_t espclaw_ota_manager_schedule_confirm(uint32_t delay_ms, char *message, size_t message_size);
esp_err_t espclaw_ota_manager_begin(size_t expected_bytes, char *message, size_t message_size);
esp_err_t espclaw_ota_manager_write(const void *data, size_t data_len, char *message, size_t message_size);
esp_err_t espclaw_ota_manager_finish(bool schedule_reboot, char *message, size_t message_size);
esp_err_t espclaw_ota_manager_abort(char *message, size_t message_size);
esp_err_t espclaw_ota_manager_schedule_restart(uint32_t delay_ms);

#endif
