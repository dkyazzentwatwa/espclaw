#include "espclaw/storage.h"

#include <stdio.h>
#include <string.h>

#include "espclaw/workspace.h"

const char *espclaw_storage_describe_workspace_root(const char *workspace_root)
{
    if (workspace_root == NULL || workspace_root[0] == '\0') {
        return "";
    }
    if (strncmp(workspace_root, "/sdcard/", 8) == 0 || strcmp(workspace_root, "/sdcard") == 0) {
        return "sdcard";
    }
    if (strncmp(workspace_root, "/workspace", 10) == 0) {
        return "littlefs";
    }
    return "host";
}

bool espclaw_storage_use_esp32cam_sdmmc_wiring(const espclaw_board_profile_t *profile)
{
    return profile != NULL && profile->profile_id == ESPCLAW_BOARD_PROFILE_ESP32CAM;
}

size_t espclaw_storage_esp32cam_attempt_count(void)
{
    return 2U;
}

bool espclaw_storage_get_esp32cam_attempt(size_t index, espclaw_esp32cam_sd_attempt_t *attempt)
{
    if (attempt == NULL) {
        return false;
    }

    memset(attempt, 0, sizeof(*attempt));
    switch (index) {
    case 0:
        *attempt = (espclaw_esp32cam_sd_attempt_t){
            .label = "sdspi",
            .mode = ESPCLAW_ESP32CAM_SD_MODE_SDSPI,
            .width = 1U,
            .clk_gpio = 14,
            .cmd_mosi_gpio = 15,
            .d0_miso_gpio = 2,
            .d1_gpio = -1,
            .d2_gpio = -1,
            .d3_cs_gpio = 13,
        };
        return true;
    case 1:
        *attempt = (espclaw_esp32cam_sd_attempt_t){
            .label = "sdmmc-1bit",
            .mode = ESPCLAW_ESP32CAM_SD_MODE_SDMMC,
            .width = 1U,
            .clk_gpio = 14,
            .cmd_mosi_gpio = 15,
            .d0_miso_gpio = 2,
            .d1_gpio = -1,
            .d2_gpio = -1,
            .d3_cs_gpio = -1,
        };
        return true;
    default:
        return false;
    }
}

#ifdef ESP_PLATFORM

#include "driver/spi_common.h"
#include "driver/spi_master.h"
#if SOC_SDMMC_HOST_SUPPORTED
#include "driver/sdmmc_default_configs.h"
#include "driver/sdmmc_host.h"
#endif
#include "driver/sdspi_host.h"
#ifdef ARDUINO
/* Arduino ESP32 core ships LittleFS natively — no joltwallet component needed. */
#include <LittleFS.h>
#else
#include "esp_littlefs.h"
#endif
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/gpio.h"
#include "sdmmc_cmd.h"
#include "soc/soc_caps.h"

#ifndef CONFIG_ESPCLAW_SD_MOUNT_POINT
#define CONFIG_ESPCLAW_SD_MOUNT_POINT "/sdcard"
#endif

#ifndef CONFIG_ESPCLAW_SD_SPI_MOSI
#define CONFIG_ESPCLAW_SD_SPI_MOSI -1
#endif

#ifndef CONFIG_ESPCLAW_SD_SPI_MISO
#define CONFIG_ESPCLAW_SD_SPI_MISO -1
#endif

#ifndef CONFIG_ESPCLAW_SD_SPI_SCLK
#define CONFIG_ESPCLAW_SD_SPI_SCLK -1
#endif

#ifndef CONFIG_ESPCLAW_SD_SPI_CS
#define CONFIG_ESPCLAW_SD_SPI_CS -1
#endif

#ifndef CONFIG_ESPCLAW_FLASH_MOUNT_POINT
#define CONFIG_ESPCLAW_FLASH_MOUNT_POINT "/workspace"
#endif

#ifndef CONFIG_ESPCLAW_FLASH_PARTITION_LABEL
#define CONFIG_ESPCLAW_FLASH_PARTITION_LABEL "workspace"
#endif

static const char *TAG = "espclaw_storage";

static void prepare_esp32cam_sd_pins(const espclaw_esp32cam_sd_attempt_t *attempt)
{
    const int pins[] = {
        attempt != NULL ? attempt->clk_gpio : -1,
        attempt != NULL ? attempt->cmd_mosi_gpio : -1,
        attempt != NULL ? attempt->d0_miso_gpio : -1,
        attempt != NULL ? attempt->d1_gpio : -1,
        attempt != NULL ? attempt->d2_gpio : -1,
        attempt != NULL ? attempt->d3_cs_gpio : -1,
    };
    size_t index;

    for (index = 0; index < sizeof(pins) / sizeof(pins[0]); ++index) {
        if (pins[index] < 0) {
            continue;
        }
        gpio_reset_pin((gpio_num_t)pins[index]);
        gpio_set_direction((gpio_num_t)pins[index], GPIO_MODE_INPUT);
        gpio_pulldown_dis((gpio_num_t)pins[index]);
        gpio_pullup_en((gpio_num_t)pins[index]);
    }
}

static esp_err_t mount_esp32cam_sdmmc_attempt(
    const espclaw_esp32cam_sd_attempt_t *attempt,
    esp_vfs_fat_sdmmc_mount_config_t *mount_config
)
{
#if SOC_SDMMC_HOST_SUPPORTED
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    sdmmc_card_t *card = NULL;

    host.slot = SDMMC_HOST_SLOT_1;
    host.max_freq_khz = 5000;
    slot_config.clk = (gpio_num_t)attempt->clk_gpio;
    slot_config.cmd = (gpio_num_t)attempt->cmd_mosi_gpio;
    slot_config.d0 = (gpio_num_t)attempt->d0_miso_gpio;
    slot_config.d1 = attempt->d1_gpio >= 0 ? (gpio_num_t)attempt->d1_gpio : GPIO_NUM_NC;
    slot_config.d2 = attempt->d2_gpio >= 0 ? (gpio_num_t)attempt->d2_gpio : GPIO_NUM_NC;
    slot_config.d3 = attempt->d3_cs_gpio >= 0 ? (gpio_num_t)attempt->d3_cs_gpio : GPIO_NUM_NC;
#if CONFIG_IDF_TARGET_ESP32
    slot_config.d4 = GPIO_NUM_NC;
    slot_config.d5 = GPIO_NUM_NC;
    slot_config.d6 = GPIO_NUM_NC;
    slot_config.d7 = GPIO_NUM_NC;
#endif
    slot_config.width = attempt->width;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    return esp_vfs_fat_sdmmc_mount(CONFIG_ESPCLAW_SD_MOUNT_POINT, &host, &slot_config, mount_config, &card);
#else
    (void)attempt;
    (void)mount_config;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static esp_err_t mount_esp32cam_sdspi_attempt(
    const espclaw_esp32cam_sd_attempt_t *attempt,
    esp_vfs_fat_sdmmc_mount_config_t *mount_config
)
{
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = attempt->cmd_mosi_gpio,
        .miso_io_num = attempt->d0_miso_gpio,
        .sclk_io_num = attempt->clk_gpio,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    sdmmc_card_t *card = NULL;
    esp_err_t err;

    host.slot = SPI2_HOST;
    slot_config.gpio_cs = attempt->d3_cs_gpio;
    slot_config.host_id = host.slot;

    err = spi_bus_initialize(host.slot, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = esp_vfs_fat_sdspi_mount(CONFIG_ESPCLAW_SD_MOUNT_POINT, &host, &slot_config, mount_config, &card);
    if (err != ESP_OK) {
        spi_bus_free(host.slot);
        return err;
    }
    return ESP_OK;
}

espclaw_storage_backend_t espclaw_storage_backend_for_profile(const espclaw_board_profile_t *profile)
{
#if defined(CONFIG_ESPCLAW_STORAGE_BACKEND_SD)
    (void)profile;
    return ESPCLAW_STORAGE_BACKEND_SD_CARD;
#elif defined(CONFIG_ESPCLAW_STORAGE_BACKEND_LITTLEFS)
    (void)profile;
    return ESPCLAW_STORAGE_BACKEND_LITTLEFS;
#else
    if (profile == NULL) {
        return ESPCLAW_STORAGE_BACKEND_SD_CARD;
    }
    return profile->default_storage_backend;
#endif
}

static esp_err_t mount_sd_workspace(const espclaw_board_profile_t *profile, espclaw_storage_mount_t *mount)
{
    char workspace_root[sizeof(mount->workspace_root)];

    if (profile == NULL || mount == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

#if !CONFIG_ESPCLAW_ENABLE_SD_BOOTSTRAP
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (espclaw_storage_use_esp32cam_sdmmc_wiring(profile)) {
        esp_vfs_fat_sdmmc_mount_config_t mount_config = {
            .format_if_mount_failed = false,
            .max_files = 8,
            .allocation_unit_size = 16 * 1024,
        };
        esp_err_t last_err = ESP_FAIL;
        size_t attempt_index;

        for (attempt_index = 0; attempt_index < espclaw_storage_esp32cam_attempt_count(); ++attempt_index) {
            espclaw_esp32cam_sd_attempt_t attempt;

            if (!espclaw_storage_get_esp32cam_attempt(attempt_index, &attempt)) {
                continue;
            }

            prepare_esp32cam_sd_pins(&attempt);
            if (attempt.mode == ESPCLAW_ESP32CAM_SD_MODE_SDMMC) {
                last_err = mount_esp32cam_sdmmc_attempt(&attempt, &mount_config);
            } else {
                last_err = mount_esp32cam_sdspi_attempt(&attempt, &mount_config);
            }
            if (last_err == ESP_OK) {
                ESP_LOGI(TAG, "Mounted ESP32-CAM SD workspace via %s", attempt.label);
                break;
            }
            ESP_LOGW(
                TAG,
                "ESP32-CAM SD attempt %s failed: %s (0x%x)",
                attempt.label,
                esp_err_to_name(last_err),
                (unsigned int)last_err
            );
        }
        if (last_err != ESP_OK) {
            return last_err;
        }
    } else if (CONFIG_ESPCLAW_SD_SPI_MOSI >= 0 &&
               CONFIG_ESPCLAW_SD_SPI_MISO >= 0 &&
               CONFIG_ESPCLAW_SD_SPI_SCLK >= 0 &&
               CONFIG_ESPCLAW_SD_SPI_CS >= 0) {
        spi_bus_config_t bus_cfg = {
            .mosi_io_num = CONFIG_ESPCLAW_SD_SPI_MOSI,
            .miso_io_num = CONFIG_ESPCLAW_SD_SPI_MISO,
            .sclk_io_num = CONFIG_ESPCLAW_SD_SPI_SCLK,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = 16 * 1024,
        };
        sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
        sdmmc_host_t host = SDSPI_HOST_DEFAULT();
        esp_vfs_fat_sdmmc_mount_config_t mount_config = {
            .format_if_mount_failed = false,
            .max_files = 8,
            .allocation_unit_size = 16 * 1024,
        };
        sdmmc_card_t *card = NULL;
        esp_err_t err;

        host.slot = SPI2_HOST;
        slot_config.gpio_cs = CONFIG_ESPCLAW_SD_SPI_CS;
        slot_config.host_id = host.slot;
        err = spi_bus_initialize(host.slot, &bus_cfg, SPI_DMA_CH_AUTO);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "SDSPI bus init failed");
            return ESP_FAIL;
        }
        if (esp_vfs_fat_sdspi_mount(CONFIG_ESPCLAW_SD_MOUNT_POINT, &host, &slot_config, &mount_config, &card) != ESP_OK) {
            spi_bus_free(host.slot);
            ESP_LOGW(TAG, "SDSPI mount failed");
            return ESP_FAIL;
        }
    } else {
        ESP_LOGW(TAG, "No SD pin configuration set for board profile %s", profile->id);
        return ESP_FAIL;
    }
#endif

    snprintf(workspace_root, sizeof(workspace_root), "%s/workspace", CONFIG_ESPCLAW_SD_MOUNT_POINT);
    if (espclaw_workspace_bootstrap(workspace_root) != 0) {
        ESP_LOGE(TAG, "Failed to bootstrap SD workspace at %s", workspace_root);
        return ESP_FAIL;
    }

    memset(mount, 0, sizeof(*mount));
    mount->backend = ESPCLAW_STORAGE_BACKEND_SD_CARD;
    snprintf(mount->workspace_root, sizeof(mount->workspace_root), "%s", workspace_root);
    return ESP_OK;
}

static esp_err_t mount_littlefs_workspace(espclaw_storage_mount_t *mount)
{
    if (mount == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

#ifdef ARDUINO
    /* Arduino LittleFS API: begin(formatOnFail, basePath, maxOpenFiles, partitionLabel) */
    if (!LittleFS.begin(true, CONFIG_ESPCLAW_FLASH_MOUNT_POINT, 10, CONFIG_ESPCLAW_FLASH_PARTITION_LABEL)) {
        ESP_LOGW(TAG, "LittleFS mount failed for partition %s", CONFIG_ESPCLAW_FLASH_PARTITION_LABEL);
        return ESP_FAIL;
    }
    if (espclaw_workspace_bootstrap(CONFIG_ESPCLAW_FLASH_MOUNT_POINT) != 0) {
        ESP_LOGE(TAG, "Failed to bootstrap LittleFS workspace at %s", CONFIG_ESPCLAW_FLASH_MOUNT_POINT);
        return ESP_FAIL;
    }
    memset(mount, 0, sizeof(*mount));
    mount->backend = ESPCLAW_STORAGE_BACKEND_LITTLEFS;
    snprintf(mount->workspace_root, sizeof(mount->workspace_root), "%s", CONFIG_ESPCLAW_FLASH_MOUNT_POINT);
    mount->total_bytes = LittleFS.totalBytes();
    mount->used_bytes  = LittleFS.usedBytes();
    return ESP_OK;
#else
    esp_vfs_littlefs_conf_t conf = {
        .base_path = CONFIG_ESPCLAW_FLASH_MOUNT_POINT,
        .partition_label = CONFIG_ESPCLAW_FLASH_PARTITION_LABEL,
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LittleFS mount failed for partition %s", CONFIG_ESPCLAW_FLASH_PARTITION_LABEL);
        return err;
    }
    if (espclaw_workspace_bootstrap(CONFIG_ESPCLAW_FLASH_MOUNT_POINT) != 0) {
        ESP_LOGE(TAG, "Failed to bootstrap LittleFS workspace at %s", CONFIG_ESPCLAW_FLASH_MOUNT_POINT);
        return ESP_FAIL;
    }
    memset(mount, 0, sizeof(*mount));
    mount->backend = ESPCLAW_STORAGE_BACKEND_LITTLEFS;
    snprintf(mount->workspace_root, sizeof(mount->workspace_root), "%s", CONFIG_ESPCLAW_FLASH_MOUNT_POINT);
    if (esp_littlefs_info(CONFIG_ESPCLAW_FLASH_PARTITION_LABEL, &mount->total_bytes, &mount->used_bytes) != ESP_OK) {
        mount->total_bytes = 0;
        mount->used_bytes  = 0;
    }
    return ESP_OK;
#endif
}

esp_err_t espclaw_storage_mount_workspace(const espclaw_board_profile_t *profile, espclaw_storage_mount_t *mount)
{
    espclaw_storage_backend_t backend = espclaw_storage_backend_for_profile(profile);

    if (backend == ESPCLAW_STORAGE_BACKEND_LITTLEFS) {
        return mount_littlefs_workspace(mount);
    }
    return mount_sd_workspace(profile, mount);
}

#endif
