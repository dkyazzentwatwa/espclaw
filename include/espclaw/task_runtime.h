#ifndef ESPCLAW_TASK_RUNTIME_H
#define ESPCLAW_TASK_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ESPCLAW_TASK_RUNTIME_MAX 8
#define ESPCLAW_TASK_ID_MAX 32
#define ESPCLAW_TASK_TRIGGER_MAX 24
#define ESPCLAW_TASK_PAYLOAD_MAX 256
#define ESPCLAW_TASK_RESULT_MAX 256
#define ESPCLAW_TASK_SCHEDULE_MAX 16

typedef struct {
    bool active;
    bool completed;
    bool stop_requested;
    char task_id[ESPCLAW_TASK_ID_MAX + 1];
    char app_id[33];
    char trigger[ESPCLAW_TASK_TRIGGER_MAX + 1];
    char schedule[ESPCLAW_TASK_SCHEDULE_MAX + 1];
    uint32_t period_ms;
    uint32_t max_iterations;
    uint32_t iterations_completed;
    uint32_t events_received;
    uint64_t started_ms;
    uint64_t last_started_ms;
    uint64_t last_finished_ms;
    int last_status;
    char last_result[ESPCLAW_TASK_RESULT_MAX];
} espclaw_task_status_t;

int espclaw_task_start(
    const char *task_id,
    const char *workspace_root,
    const char *app_id,
    const char *trigger,
    const char *payload,
    uint32_t period_ms,
    uint32_t max_iterations,
    char *buffer,
    size_t buffer_size
);
int espclaw_task_start_with_schedule(
    const char *task_id,
    const char *workspace_root,
    const char *app_id,
    const char *schedule,
    const char *trigger,
    const char *payload,
    uint32_t period_ms,
    uint32_t max_iterations,
    char *buffer,
    size_t buffer_size
);
int espclaw_task_stop(const char *task_id, char *buffer, size_t buffer_size);
int espclaw_task_emit_event(
    const char *event_name,
    const char *payload,
    char *buffer,
    size_t buffer_size
);
size_t espclaw_task_snapshot_all(espclaw_task_status_t *statuses, size_t max_statuses);
int espclaw_task_render_json(char *buffer, size_t buffer_size);
void espclaw_task_shutdown_all(void);

#endif
