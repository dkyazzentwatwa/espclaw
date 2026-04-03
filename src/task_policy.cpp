#include "espclaw/task_policy.h"

static espclaw_task_policy_t s_policy = {
    .cpu_cores = 1,
    .admin_core = -1,
    .telegram_core = -1,
    .control_loop_core = -1,
    .console_core = -1,
};

void espclaw_task_policy_select(const espclaw_board_profile_t *profile)
{
    s_policy.cpu_cores = 1;
    s_policy.admin_core = -1;
    s_policy.telegram_core = -1;
    s_policy.control_loop_core = -1;
    s_policy.console_core = -1;

    if (profile == NULL) {
        return;
    }

    if (profile->cpu_cores > 1) {
        s_policy.cpu_cores = profile->cpu_cores;
        s_policy.admin_core = 0;
        s_policy.telegram_core = 0;
        s_policy.control_loop_core = 1;
        s_policy.console_core = 0;
    }
}

espclaw_task_policy_t espclaw_task_policy_current(void)
{
    return s_policy;
}

int espclaw_task_policy_core_for(espclaw_task_kind_t kind)
{
    switch (kind) {
    case ESPCLAW_TASK_KIND_ADMIN:
        return s_policy.admin_core;
    case ESPCLAW_TASK_KIND_TELEGRAM:
        return s_policy.telegram_core;
    case ESPCLAW_TASK_KIND_CONTROL_LOOP:
        return s_policy.control_loop_core;
    case ESPCLAW_TASK_KIND_CONSOLE:
        return s_policy.console_core;
    default:
        return -1;
    }
}
