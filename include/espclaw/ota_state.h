#ifndef ESPCLAW_OTA_STATE_H
#define ESPCLAW_OTA_STATE_H

#include <stdbool.h>

typedef enum {
    ESPCLAW_OTA_STATUS_IDLE = 0,
    ESPCLAW_OTA_STATUS_DOWNLOADED = 1,
    ESPCLAW_OTA_STATUS_PENDING_REBOOT = 2,
    ESPCLAW_OTA_STATUS_VERIFYING = 3,
    ESPCLAW_OTA_STATUS_CONFIRMED = 4,
    ESPCLAW_OTA_STATUS_ROLLBACK_REQUIRED = 5
} espclaw_ota_status_t;

typedef struct {
    espclaw_ota_status_t status;
    bool rollback_allowed;
    unsigned int target_slot;
} espclaw_ota_state_t;

espclaw_ota_state_t espclaw_ota_state_init(void);
bool espclaw_ota_mark_downloaded(espclaw_ota_state_t *state, unsigned int target_slot);
bool espclaw_ota_mark_pending_reboot(espclaw_ota_state_t *state);
bool espclaw_ota_mark_verifying(espclaw_ota_state_t *state);
bool espclaw_ota_confirm_boot(espclaw_ota_state_t *state);
bool espclaw_ota_require_rollback(espclaw_ota_state_t *state);

#endif
