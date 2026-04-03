#include "espclaw/ota_state.h"

#include <stddef.h>

espclaw_ota_state_t espclaw_ota_state_init(void)
{
    espclaw_ota_state_t state;

    state.status = ESPCLAW_OTA_STATUS_IDLE;
    state.rollback_allowed = false;
    state.target_slot = 0;

    return state;
}

bool espclaw_ota_mark_downloaded(espclaw_ota_state_t *state, unsigned int target_slot)
{
    if (state == NULL || state->status != ESPCLAW_OTA_STATUS_IDLE) {
        return false;
    }

    state->status = ESPCLAW_OTA_STATUS_DOWNLOADED;
    state->target_slot = target_slot;
    return true;
}

bool espclaw_ota_mark_pending_reboot(espclaw_ota_state_t *state)
{
    if (state == NULL || state->status != ESPCLAW_OTA_STATUS_DOWNLOADED) {
        return false;
    }

    state->status = ESPCLAW_OTA_STATUS_PENDING_REBOOT;
    return true;
}

bool espclaw_ota_mark_verifying(espclaw_ota_state_t *state)
{
    if (state == NULL || state->status != ESPCLAW_OTA_STATUS_PENDING_REBOOT) {
        return false;
    }

    state->status = ESPCLAW_OTA_STATUS_VERIFYING;
    state->rollback_allowed = true;
    return true;
}

bool espclaw_ota_confirm_boot(espclaw_ota_state_t *state)
{
    if (state == NULL || state->status != ESPCLAW_OTA_STATUS_VERIFYING) {
        return false;
    }

    state->status = ESPCLAW_OTA_STATUS_CONFIRMED;
    state->rollback_allowed = false;
    return true;
}

bool espclaw_ota_require_rollback(espclaw_ota_state_t *state)
{
    if (state == NULL || !state->rollback_allowed) {
        return false;
    }

    state->status = ESPCLAW_OTA_STATUS_ROLLBACK_REQUIRED;
    return true;
}
