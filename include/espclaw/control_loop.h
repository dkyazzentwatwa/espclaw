#ifndef ESPCLAW_CONTROL_LOOP_H
#define ESPCLAW_CONTROL_LOOP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "espclaw/task_runtime.h"

#define ESPCLAW_CONTROL_LOOP_MAX 4
#define ESPCLAW_CONTROL_LOOP_ID_MAX 32
#define ESPCLAW_CONTROL_LOOP_PAYLOAD_MAX 256
#define ESPCLAW_CONTROL_LOOP_RESULT_MAX 256

typedef struct {
    bool active;
    bool completed;
    bool stop_requested;
    char loop_id[ESPCLAW_CONTROL_LOOP_ID_MAX + 1];
    char app_id[33];
    char trigger[25];
    uint32_t period_ms;
    uint32_t max_iterations;
    uint32_t iterations_completed;
    uint64_t started_ms;
    uint64_t last_started_ms;
    uint64_t last_finished_ms;
    int last_status;
    char last_result[ESPCLAW_CONTROL_LOOP_RESULT_MAX];
} espclaw_control_loop_status_t;

int espclaw_control_loop_start(
    const char *loop_id,
    const char *workspace_root,
    const char *app_id,
    const char *trigger,
    const char *payload,
    uint32_t period_ms,
    uint32_t max_iterations,
    char *buffer,
    size_t buffer_size
);
int espclaw_control_loop_stop(const char *loop_id, char *buffer, size_t buffer_size);
size_t espclaw_control_loop_snapshot_all(espclaw_control_loop_status_t *statuses, size_t max_statuses);
int espclaw_control_loop_render_json(char *buffer, size_t buffer_size);
void espclaw_control_loop_shutdown_all(void);

#endif
