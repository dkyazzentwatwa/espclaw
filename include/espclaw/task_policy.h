#ifndef ESPCLAW_TASK_POLICY_H
#define ESPCLAW_TASK_POLICY_H

#include "espclaw/board_profile.h"

typedef enum {
    ESPCLAW_TASK_KIND_ADMIN = 0,
    ESPCLAW_TASK_KIND_TELEGRAM = 1,
    ESPCLAW_TASK_KIND_CONTROL_LOOP = 2,
    ESPCLAW_TASK_KIND_CONSOLE = 3,
} espclaw_task_kind_t;

typedef struct {
    int cpu_cores;
    int admin_core;
    int telegram_core;
    int control_loop_core;
    int console_core;
} espclaw_task_policy_t;

void espclaw_task_policy_select(const espclaw_board_profile_t *profile);
espclaw_task_policy_t espclaw_task_policy_current(void);
int espclaw_task_policy_core_for(espclaw_task_kind_t kind);

#endif
