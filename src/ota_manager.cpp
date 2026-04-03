#include "espclaw/ota_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#endif

static espclaw_ota_snapshot_t s_ota_snapshot;

#ifdef ESP_PLATFORM
static esp_ota_handle_t s_ota_handle;
static const esp_partition_t *s_update_partition;
static bool s_ota_handle_active;
static bool s_confirm_task_scheduled;
static StaticTask_t s_confirm_task_buffer;
static StackType_t s_confirm_task_stack[(3072U + sizeof(StackType_t) - 1U) / sizeof(StackType_t)];
#endif

static void set_message(const char *message)
{
    snprintf(s_ota_snapshot.last_message, sizeof(s_ota_snapshot.last_message), "%s", message != NULL ? message : "");
}

static void copy_message(char *buffer, size_t buffer_size, const char *message)
{
    if (buffer == NULL || buffer_size == 0) {
        return;
    }
    snprintf(buffer, buffer_size, "%s", message != NULL ? message : "");
}

static unsigned int parse_target_slot(const char *label)
{
    if (label == NULL || strncmp(label, "ota_", 4) != 0) {
        return 0;
    }
    return (unsigned int)strtoul(label + 4, NULL, 10);
}

#ifdef ESP_PLATFORM
static void copy_partition_label(char *buffer, size_t buffer_size, const esp_partition_t *partition)
{
    if (buffer == NULL || buffer_size == 0) {
        return;
    }
    snprintf(buffer, buffer_size, "%s", partition != NULL ? partition->label : "");
}

static void refresh_snapshot_partitions(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    esp_ota_img_states_t image_state = ESP_OTA_IMG_UNDEFINED;

    copy_partition_label(s_ota_snapshot.running_partition_label, sizeof(s_ota_snapshot.running_partition_label), running);
    copy_partition_label(s_ota_snapshot.target_partition_label, sizeof(s_ota_snapshot.target_partition_label), next);
    s_ota_snapshot.supported = next != NULL;
    s_ota_snapshot.state = espclaw_ota_state_init();

    if (running != NULL && esp_ota_get_state_partition(running, &image_state) == ESP_OK) {
        if (image_state == ESP_OTA_IMG_PENDING_VERIFY) {
            s_ota_snapshot.state.status = ESPCLAW_OTA_STATUS_VERIFYING;
            s_ota_snapshot.state.rollback_allowed = true;
            s_ota_snapshot.state.target_slot = parse_target_slot(running->label);
        } else if (image_state == ESP_OTA_IMG_ABORTED) {
            s_ota_snapshot.state.status = ESPCLAW_OTA_STATUS_ROLLBACK_REQUIRED;
            s_ota_snapshot.state.target_slot = parse_target_slot(running->label);
        } else if (strncmp(s_ota_snapshot.running_partition_label, "ota_", 4) == 0) {
            s_ota_snapshot.state.status = ESPCLAW_OTA_STATUS_CONFIRMED;
            s_ota_snapshot.state.target_slot = parse_target_slot(s_ota_snapshot.running_partition_label);
        }
    }
}

static void reboot_task(void *arg)
{
    uint32_t delay_ms = (uint32_t)(uintptr_t)arg;

    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    esp_restart();
}

static void confirm_task(void *arg)
{
    uint32_t delay_ms = (uint32_t)(uintptr_t)arg;
    char message[128];

    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    if (espclaw_ota_manager_confirm_running(message, sizeof(message)) == ESP_OK) {
        ESP_LOGI("espclaw_ota", "%s", message);
    } else {
        ESP_LOGW("espclaw_ota", "%s", message);
    }
    s_confirm_task_scheduled = false;
    vTaskDelete(NULL);
}
#endif

void espclaw_ota_manager_init(void)
{
    memset(&s_ota_snapshot, 0, sizeof(s_ota_snapshot));
    s_ota_snapshot.state = espclaw_ota_state_init();
    set_message("OTA idle.");

#ifdef ESP_PLATFORM
    s_update_partition = NULL;
    s_ota_handle_active = false;
    s_confirm_task_scheduled = false;
    refresh_snapshot_partitions();
    if (!s_ota_snapshot.supported) {
        set_message("OTA disabled: no update partition is available.");
    } else if (s_ota_snapshot.state.status == ESPCLAW_OTA_STATUS_VERIFYING) {
        set_message("Booted into a pending OTA image. Waiting to confirm this firmware.");
    } else {
        set_message("OTA ready.");
    }
#else
    s_ota_snapshot.supported = true;
    snprintf(s_ota_snapshot.running_partition_label, sizeof(s_ota_snapshot.running_partition_label), "ota_0");
    snprintf(s_ota_snapshot.target_partition_label, sizeof(s_ota_snapshot.target_partition_label), "ota_1");
#endif
}

void espclaw_ota_manager_snapshot(espclaw_ota_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }
    memcpy(snapshot, &s_ota_snapshot, sizeof(*snapshot));
}

esp_err_t espclaw_ota_manager_confirm_running(char *message, size_t message_size)
{
#ifdef ESP_PLATFORM
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t image_state = ESP_OTA_IMG_UNDEFINED;

    if (running != NULL &&
        esp_ota_get_state_partition(running, &image_state) == ESP_OK &&
        image_state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();

        if (err != ESP_OK) {
            set_message("Failed to confirm the running OTA image.");
            copy_message(message, message_size, s_ota_snapshot.last_message);
            return err;
        }

        s_ota_snapshot.state.status = ESPCLAW_OTA_STATUS_CONFIRMED;
        s_ota_snapshot.state.rollback_allowed = false;
        s_ota_snapshot.state.target_slot = parse_target_slot(running->label);
        set_message("Confirmed the running OTA image.");
        copy_message(message, message_size, s_ota_snapshot.last_message);
        return ESP_OK;
    }
#endif

    if (s_ota_snapshot.supported && strncmp(s_ota_snapshot.running_partition_label, "ota_", 4) == 0) {
        s_ota_snapshot.state.status = ESPCLAW_OTA_STATUS_CONFIRMED;
        s_ota_snapshot.state.target_slot = parse_target_slot(s_ota_snapshot.running_partition_label);
        set_message("Running OTA image is already confirmed.");
    } else {
        set_message("No pending OTA image needed confirmation.");
    }
    copy_message(message, message_size, s_ota_snapshot.last_message);
    return ESP_OK;
}

esp_err_t espclaw_ota_manager_schedule_confirm(uint32_t delay_ms, char *message, size_t message_size)
{
#ifdef ESP_PLATFORM
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t image_state = ESP_OTA_IMG_UNDEFINED;

    if (running == NULL ||
        esp_ota_get_state_partition(running, &image_state) != ESP_OK ||
        image_state != ESP_OTA_IMG_PENDING_VERIFY) {
        set_message("Running OTA image is already confirmed.");
        copy_message(message, message_size, s_ota_snapshot.last_message);
        return ESP_OK;
    }
    if (s_confirm_task_scheduled) {
        set_message("OTA confirmation is already scheduled.");
        copy_message(message, message_size, s_ota_snapshot.last_message);
        return ESP_OK;
    }
    if (xTaskCreateStaticPinnedToCore(
            confirm_task,
            "espclaw_ota_confirm",
            3072,
            (void *)(uintptr_t)delay_ms,
            5,
            s_confirm_task_stack,
            &s_confirm_task_buffer,
            tskNO_AFFINITY
        ) == NULL) {
        set_message("Failed to schedule OTA confirmation.");
        copy_message(message, message_size, s_ota_snapshot.last_message);
        return ESP_FAIL;
    }
    s_confirm_task_scheduled = true;
    snprintf(
        s_ota_snapshot.last_message,
        sizeof(s_ota_snapshot.last_message),
        "Scheduled OTA confirmation in %u ms.",
        (unsigned int)delay_ms
    );
    copy_message(message, message_size, s_ota_snapshot.last_message);
    return ESP_OK;
#else
    set_message("Running OTA image is already confirmed.");
    copy_message(message, message_size, s_ota_snapshot.last_message);
    return ESP_OK;
#endif
}

esp_err_t espclaw_ota_manager_begin(size_t expected_bytes, char *message, size_t message_size)
{
    if (!s_ota_snapshot.supported) {
        set_message("OTA is not available on this partition layout.");
        copy_message(message, message_size, s_ota_snapshot.last_message);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ota_snapshot.upload_in_progress) {
        set_message("An OTA upload is already in progress.");
        copy_message(message, message_size, s_ota_snapshot.last_message);
        return ESP_ERR_INVALID_STATE;
    }

#ifdef ESP_PLATFORM
    s_update_partition = esp_ota_get_next_update_partition(NULL);
    if (s_update_partition == NULL) {
        set_message("No OTA update partition is available.");
        copy_message(message, message_size, s_ota_snapshot.last_message);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = esp_ota_begin(
        s_update_partition,
        expected_bytes > 0 ? expected_bytes : OTA_SIZE_UNKNOWN,
        &s_ota_handle
    );
    if (err != ESP_OK) {
        set_message("Failed to begin OTA write.");
        copy_message(message, message_size, s_ota_snapshot.last_message);
        return err;
    }
    s_ota_handle_active = true;
    copy_partition_label(s_ota_snapshot.target_partition_label, sizeof(s_ota_snapshot.target_partition_label), s_update_partition);
#endif

    s_ota_snapshot.upload_in_progress = true;
    s_ota_snapshot.expected_bytes = expected_bytes;
    s_ota_snapshot.written_bytes = 0;
    s_ota_snapshot.state = espclaw_ota_state_init();
    set_message("OTA upload started.");
    copy_message(message, message_size, s_ota_snapshot.last_message);
    return ESP_OK;
}

esp_err_t espclaw_ota_manager_write(const void *data, size_t data_len, char *message, size_t message_size)
{
    if (!s_ota_snapshot.upload_in_progress || data == NULL || data_len == 0) {
        set_message("OTA write attempted without an active upload.");
        copy_message(message, message_size, s_ota_snapshot.last_message);
        return ESP_ERR_INVALID_STATE;
    }

#ifdef ESP_PLATFORM
    esp_err_t err = esp_ota_write(s_ota_handle, data, data_len);
    if (err != ESP_OK) {
        set_message("Failed while writing OTA bytes.");
        copy_message(message, message_size, s_ota_snapshot.last_message);
        return err;
    }
#endif

    s_ota_snapshot.written_bytes += data_len;
    set_message("OTA bytes written.");
    copy_message(message, message_size, s_ota_snapshot.last_message);
    return ESP_OK;
}

esp_err_t espclaw_ota_manager_abort(char *message, size_t message_size)
{
#ifdef ESP_PLATFORM
    if (s_ota_handle_active) {
        esp_ota_abort(s_ota_handle);
        s_ota_handle_active = false;
    }
    s_update_partition = NULL;
#endif

    s_ota_snapshot.upload_in_progress = false;
    s_ota_snapshot.expected_bytes = 0;
    s_ota_snapshot.written_bytes = 0;
    s_ota_snapshot.state = espclaw_ota_state_init();
    set_message("OTA upload aborted.");
    copy_message(message, message_size, s_ota_snapshot.last_message);
    return ESP_OK;
}

esp_err_t espclaw_ota_manager_schedule_restart(uint32_t delay_ms)
{
#ifdef ESP_PLATFORM
    if (xTaskCreate(reboot_task, "espclaw_ota_reboot", 3072, (void *)(uintptr_t)delay_ms, 5, NULL) != pdPASS) {
        set_message("Failed to schedule OTA reboot.");
        return ESP_FAIL;
    }
#endif

    set_message("OTA reboot scheduled.");
    return ESP_OK;
}

esp_err_t espclaw_ota_manager_finish(bool schedule_reboot, char *message, size_t message_size)
{
    if (!s_ota_snapshot.upload_in_progress) {
        set_message("OTA finish attempted without an active upload.");
        copy_message(message, message_size, s_ota_snapshot.last_message);
        return ESP_ERR_INVALID_STATE;
    }

#ifdef ESP_PLATFORM
    esp_err_t err = esp_ota_end(s_ota_handle);
    if (err != ESP_OK) {
        s_ota_handle_active = false;
        set_message("Failed to finalize OTA image.");
        copy_message(message, message_size, s_ota_snapshot.last_message);
        return err;
    }
    s_ota_handle_active = false;

    err = esp_ota_set_boot_partition(s_update_partition);
    if (err != ESP_OK) {
        set_message("Failed to select OTA image for next boot.");
        copy_message(message, message_size, s_ota_snapshot.last_message);
        return err;
    }
#endif

    s_ota_snapshot.upload_in_progress = false;
    s_ota_snapshot.state = espclaw_ota_state_init();
    (void)espclaw_ota_mark_downloaded(&s_ota_snapshot.state, parse_target_slot(s_ota_snapshot.target_partition_label));
    (void)espclaw_ota_mark_pending_reboot(&s_ota_snapshot.state);

    if (schedule_reboot) {
        if (espclaw_ota_manager_schedule_restart(1500) == ESP_OK) {
            set_message("Firmware uploaded. Rebooting into the new OTA image.");
        } else {
            set_message("Firmware uploaded. Reboot is required.");
        }
    } else {
        set_message("Firmware uploaded. Reboot is required.");
    }

#ifdef ESP_PLATFORM
    s_update_partition = NULL;
#endif
    copy_message(message, message_size, s_ota_snapshot.last_message);
    return ESP_OK;
}
