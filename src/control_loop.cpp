#include "espclaw/control_loop.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "espclaw/task_runtime.h"

static int json_escape_copy(const char *input, char *output, size_t output_size)
{
    size_t written = 0;

    if (output == NULL || output_size == 0) {
        return -1;
    }

    while (input != NULL && *input != '\0' && written + 1 < output_size) {
        const char *replacement = NULL;

        switch (*input) {
        case '\\':
            replacement = "\\\\";
            break;
        case '"':
            replacement = "\\\"";
            break;
        case '\n':
            replacement = "\\n";
            break;
        case '\r':
            replacement = "\\r";
            break;
        case '\t':
            replacement = "\\t";
            break;
        default:
            break;
        }

        if (replacement != NULL) {
            size_t replacement_len = strlen(replacement);
            if (written + replacement_len >= output_size) {
                break;
            }
            memcpy(output + written, replacement, replacement_len);
            written += replacement_len;
        } else {
            output[written++] = *input;
        }
        input++;
    }

    output[written] = '\0';
    return 0;
}

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
)
{
    return espclaw_task_start(
        loop_id,
        workspace_root,
        app_id,
        trigger,
        payload,
        period_ms,
        max_iterations,
        buffer,
        buffer_size
    );
}

int espclaw_control_loop_stop(const char *loop_id, char *buffer, size_t buffer_size)
{
    return espclaw_task_stop(loop_id, buffer, buffer_size);
}

size_t espclaw_control_loop_snapshot_all(espclaw_control_loop_status_t *statuses, size_t max_statuses)
{
    espclaw_task_status_t *task_statuses = NULL;
    size_t count;
    size_t index;

    if (statuses == NULL || max_statuses == 0) {
        return 0;
    }

    task_statuses = calloc(ESPCLAW_TASK_RUNTIME_MAX, sizeof(*task_statuses));
    if (task_statuses == NULL) {
        return 0;
    }

    count = espclaw_task_snapshot_all(task_statuses, ESPCLAW_TASK_RUNTIME_MAX);
    if (count > max_statuses) {
        count = max_statuses;
    }
    if (count > ESPCLAW_CONTROL_LOOP_MAX) {
        count = ESPCLAW_CONTROL_LOOP_MAX;
    }
    for (index = 0; index < count; ++index) {
        memset(&statuses[index], 0, sizeof(statuses[index]));
        snprintf(statuses[index].loop_id, sizeof(statuses[index].loop_id), "%s", task_statuses[index].task_id);
        snprintf(statuses[index].app_id, sizeof(statuses[index].app_id), "%s", task_statuses[index].app_id);
        snprintf(statuses[index].trigger, sizeof(statuses[index].trigger), "%s", task_statuses[index].trigger);
        statuses[index].active = task_statuses[index].active;
        statuses[index].completed = task_statuses[index].completed;
        statuses[index].stop_requested = task_statuses[index].stop_requested;
        statuses[index].period_ms = task_statuses[index].period_ms;
        statuses[index].max_iterations = task_statuses[index].max_iterations;
        statuses[index].iterations_completed = task_statuses[index].iterations_completed;
        statuses[index].started_ms = task_statuses[index].started_ms;
        statuses[index].last_started_ms = task_statuses[index].last_started_ms;
        statuses[index].last_finished_ms = task_statuses[index].last_finished_ms;
        statuses[index].last_status = task_statuses[index].last_status;
        snprintf(statuses[index].last_result, sizeof(statuses[index].last_result), "%s", task_statuses[index].last_result);
    }
    free(task_statuses);
    return count;
}

int espclaw_control_loop_render_json(char *buffer, size_t buffer_size)
{
    espclaw_task_status_t *statuses = NULL;
    size_t count;
    size_t index;
    size_t written = 0;

    if (buffer == NULL || buffer_size == 0) {
        return -1;
    }

    statuses = calloc(ESPCLAW_TASK_RUNTIME_MAX, sizeof(*statuses));
    if (statuses == NULL) {
        snprintf(buffer, buffer_size, "{\"loops\":[]}");
        return -1;
    }

    count = espclaw_task_snapshot_all(statuses, ESPCLAW_TASK_RUNTIME_MAX);

    written += (size_t)snprintf(buffer + written, buffer_size - written, "{\"loops\":[");
    for (index = 0; index < count && written < buffer_size; ++index) {
        char escaped_result[ESPCLAW_TASK_RESULT_MAX * 2];

        json_escape_copy(statuses[index].last_result, escaped_result, sizeof(escaped_result));
        written += (size_t)snprintf(
            buffer + written,
            buffer_size - written,
            "%s{\"loop_id\":\"%s\",\"app_id\":\"%s\",\"trigger\":\"%s\",\"active\":%s,"
            "\"completed\":%s,\"stop_requested\":%s,\"period_ms\":%u,\"max_iterations\":%u,"
            "\"iterations_completed\":%u,\"started_ms\":%llu,\"last_started_ms\":%llu,"
            "\"last_finished_ms\":%llu,\"last_status\":%d,\"last_result\":\"%s\"}",
            index == 0 ? "" : ",",
            statuses[index].task_id,
            statuses[index].app_id,
            statuses[index].trigger,
            statuses[index].active ? "true" : "false",
            statuses[index].completed ? "true" : "false",
            statuses[index].stop_requested ? "true" : "false",
            (unsigned int)statuses[index].period_ms,
            (unsigned int)statuses[index].max_iterations,
            (unsigned int)statuses[index].iterations_completed,
            (unsigned long long)statuses[index].started_ms,
            (unsigned long long)statuses[index].last_started_ms,
            (unsigned long long)statuses[index].last_finished_ms,
            statuses[index].last_status,
            escaped_result
        );
    }
    written += (size_t)snprintf(buffer + written, buffer_size - written, "]}");

    if (written >= buffer_size) {
        buffer[buffer_size - 1] = '\0';
    }
    free(statuses);
    return 0;
}

void espclaw_control_loop_shutdown_all(void)
{
    espclaw_task_shutdown_all();
}
