#ifndef ESPCLAW_BEHAVIOR_RUNTIME_H
#define ESPCLAW_BEHAVIOR_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "espclaw/task_runtime.h"

#define ESPCLAW_BEHAVIOR_ID_MAX 32
#define ESPCLAW_BEHAVIOR_TITLE_MAX 64

typedef struct {
    char behavior_id[ESPCLAW_BEHAVIOR_ID_MAX + 1];
    char title[ESPCLAW_BEHAVIOR_TITLE_MAX + 1];
    char app_id[33];
    char schedule[ESPCLAW_TASK_SCHEDULE_MAX + 1];
    char trigger[ESPCLAW_TASK_TRIGGER_MAX + 1];
    char payload[ESPCLAW_TASK_PAYLOAD_MAX];
    uint32_t period_ms;
    uint32_t max_iterations;
    bool autostart;
} espclaw_behavior_spec_t;

typedef struct {
    espclaw_behavior_spec_t spec;
    bool active;
    bool completed;
    bool stop_requested;
    uint32_t iterations_completed;
    uint32_t events_received;
    int last_status;
    char last_result[ESPCLAW_TASK_RESULT_MAX];
} espclaw_behavior_status_t;

bool espclaw_behavior_id_is_valid(const char *behavior_id);
int espclaw_behavior_register(
    const char *workspace_root,
    const espclaw_behavior_spec_t *spec,
    char *buffer,
    size_t buffer_size
);
int espclaw_behavior_remove(
    const char *workspace_root,
    const char *behavior_id,
    char *buffer,
    size_t buffer_size
);
int espclaw_behavior_load(
    const char *workspace_root,
    const char *behavior_id,
    espclaw_behavior_spec_t *spec
);
size_t espclaw_behavior_snapshot_all(
    const char *workspace_root,
    espclaw_behavior_status_t *statuses,
    size_t max_statuses
);
int espclaw_behavior_render_json(
    const char *workspace_root,
    char *buffer,
    size_t buffer_size
);
int espclaw_behavior_start(
    const char *workspace_root,
    const char *behavior_id,
    char *buffer,
    size_t buffer_size
);
int espclaw_behavior_stop(const char *behavior_id, char *buffer, size_t buffer_size);
int espclaw_behavior_start_autostart(
    const char *workspace_root,
    char *buffer,
    size_t buffer_size
);

#endif
