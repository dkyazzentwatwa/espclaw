#ifndef ESPCLAW_STORAGE_H
#define ESPCLAW_STORAGE_H

#include <stddef.h>
#include <stdbool.h>

#include "espclaw/board_profile.h"

typedef struct {
    espclaw_storage_backend_t backend;
    char workspace_root[128];
    size_t total_bytes;
    size_t used_bytes;
} espclaw_storage_mount_t;

typedef enum {
    ESPCLAW_ESP32CAM_SD_MODE_SDMMC = 0,
    ESPCLAW_ESP32CAM_SD_MODE_SDSPI = 1,
} espclaw_esp32cam_sd_mode_t;

typedef struct {
    const char *label;
    espclaw_esp32cam_sd_mode_t mode;
    unsigned int width;
    int clk_gpio;
    int cmd_mosi_gpio;
    int d0_miso_gpio;
    int d1_gpio;
    int d2_gpio;
    int d3_cs_gpio;
} espclaw_esp32cam_sd_attempt_t;

const char *espclaw_storage_describe_workspace_root(const char *workspace_root);
bool espclaw_storage_use_esp32cam_sdmmc_wiring(const espclaw_board_profile_t *profile);
size_t espclaw_storage_esp32cam_attempt_count(void);
bool espclaw_storage_get_esp32cam_attempt(size_t index, espclaw_esp32cam_sd_attempt_t *attempt);

#ifdef ESP_PLATFORM
#include "esp_err.h"

espclaw_storage_backend_t espclaw_storage_backend_for_profile(const espclaw_board_profile_t *profile);
esp_err_t espclaw_storage_mount_workspace(const espclaw_board_profile_t *profile, espclaw_storage_mount_t *mount);
#endif

#endif
