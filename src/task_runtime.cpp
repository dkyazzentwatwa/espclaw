#include "espclaw/task_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#elif defined(ESPCLAW_WASM)
#else
#include <pthread.h>
#endif

#include "espclaw/app_runtime.h"
#include "espclaw/hardware.h"
#include "espclaw/task_policy.h"

static const char *TAG = "espclaw_task";

typedef struct {
    espclaw_task_status_t status;
    char workspace_root[512];
    char payload[ESPCLAW_TASK_PAYLOAD_MAX];
    char pending_payloads[4][ESPCLAW_TASK_PAYLOAD_MAX];
    size_t pending_head;
    size_t pending_tail;
    size_t pending_count;
#ifdef ESP_PLATFORM
    TaskHandle_t task;
    portMUX_TYPE lock;
#elif defined(ESPCLAW_WASM)
#else
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t condition;
    bool thread_created;
#endif
    bool lock_ready;
} espclaw_task_slot_t;

static espclaw_task_slot_t s_tasks[ESPCLAW_TASK_RUNTIME_MAX];

#ifdef ESP_PLATFORM
#if CONFIG_ESPCLAW_BOARD_PROFILE_ESP32CAM
#define ESPCLAW_TASK_RUNTIME_STACK_WORDS 8192
#else
#define ESPCLAW_TASK_RUNTIME_STACK_WORDS 16384
#endif
#endif

static int task_lock_init(espclaw_task_slot_t *slot)
{
    if (slot == NULL) {
        return -1;
    }
    if (slot->lock_ready) {
        return 0;
    }

#ifdef ESP_PLATFORM
    portMUX_INITIALIZE(&slot->lock);
#elif defined(ESPCLAW_WASM)
#else
    if (pthread_mutex_init(&slot->lock, NULL) != 0) {
        return -1;
    }
    if (pthread_cond_init(&slot->condition, NULL) != 0) {
        pthread_mutex_destroy(&slot->lock);
        return -1;
    }
#endif
    slot->lock_ready = true;
    return 0;
}

static void task_lock(espclaw_task_slot_t *slot)
{
    if (slot == NULL || !slot->lock_ready) {
        return;
    }
#ifdef ESP_PLATFORM
    taskENTER_CRITICAL(&slot->lock);
#elif defined(ESPCLAW_WASM)
#else
    pthread_mutex_lock(&slot->lock);
#endif
}

static void task_unlock(espclaw_task_slot_t *slot)
{
    if (slot == NULL || !slot->lock_ready) {
        return;
    }
#ifdef ESP_PLATFORM
    taskEXIT_CRITICAL(&slot->lock);
#elif defined(ESPCLAW_WASM)
#else
    pthread_mutex_unlock(&slot->lock);
#endif
}

static void copy_status_locked(const espclaw_task_slot_t *slot, espclaw_task_status_t *status)
{
    if (slot == NULL || status == NULL) {
        return;
    }
    *status = slot->status;
}

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

static int find_task_index(const char *task_id)
{
    size_t index;

    if (task_id == NULL || task_id[0] == '\0') {
        return -1;
    }

    for (index = 0; index < ESPCLAW_TASK_RUNTIME_MAX; ++index) {
        if (s_tasks[index].status.task_id[0] != '\0' &&
            strcmp(s_tasks[index].status.task_id, task_id) == 0) {
            return (int)index;
        }
    }

    return -1;
}

static int find_free_task_index(void)
{
    size_t index;

    for (index = 0; index < ESPCLAW_TASK_RUNTIME_MAX; ++index) {
        if (!s_tasks[index].status.active) {
            return (int)index;
        }
    }

    return -1;
}

static bool schedule_is_event(const char *schedule)
{
    return schedule != NULL && strcmp(schedule, "event") == 0;
}

static void notify_task_worker(espclaw_task_slot_t *slot)
{
    if (slot == NULL) {
        return;
    }
#ifdef ESP_PLATFORM
    if (slot->task != NULL) {
        xTaskNotifyGive(slot->task);
    }
#elif defined(ESPCLAW_WASM)
#else
    pthread_cond_signal(&slot->condition);
#endif
}

static void mark_task_finished(espclaw_task_slot_t *slot, int last_status, const char *result)
{
    char result_copy[ESPCLAW_TASK_RESULT_MAX];

    if (slot == NULL) {
        return;
    }

    snprintf(result_copy, sizeof(result_copy), "%s", result != NULL ? result : "");

    task_lock(slot);
    slot->status.active = false;
    slot->status.completed = true;
    slot->status.last_status = last_status;
    slot->status.last_finished_ms = espclaw_hw_ticks_ms();
    snprintf(slot->status.last_result, sizeof(slot->status.last_result), "%s", result_copy);
    task_unlock(slot);
}

static void run_task_worker(espclaw_task_slot_t *slot)
{
    espclaw_app_vm_t *vm = NULL;
    char result[ESPCLAW_TASK_RESULT_MAX];
    char step_payload[ESPCLAW_TASK_PAYLOAD_MAX];
    uint64_t next_deadline_ms = 0;
    uint32_t completed = 0;
    int open_status;
    bool event_schedule = false;

    if (slot == NULL) {
        return;
    }

    open_status = espclaw_app_vm_open(
        slot->workspace_root,
        slot->status.app_id,
        &vm,
        result,
        sizeof(result)
    );
    if (open_status != 0) {
        mark_task_finished(slot, -1, result);
        return;
    }

    next_deadline_ms = espclaw_hw_ticks_ms();
    event_schedule = schedule_is_event(slot->status.schedule);
    while (true) {
        uint64_t now_ms = espclaw_hw_ticks_ms();
        uint32_t period_ms;
        uint32_t max_iterations;
        bool stop_requested;
        bool should_run = true;
        int step_status;

        task_lock(slot);
        stop_requested = slot->status.stop_requested;
        period_ms = slot->status.period_ms;
        max_iterations = slot->status.max_iterations;
        if (event_schedule) {
            while (!slot->status.stop_requested && slot->pending_count == 0) {
                task_unlock(slot);
#ifdef ESP_PLATFORM
                ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
#elif defined(ESPCLAW_WASM)
                task_lock(slot);
                stop_requested = true;
                should_run = false;
                break;
#else
                pthread_mutex_lock(&slot->lock);
                while (!slot->status.stop_requested && slot->pending_count == 0) {
                    pthread_cond_wait(&slot->condition, &slot->lock);
                }
                pthread_mutex_unlock(&slot->lock);
#endif
                task_lock(slot);
            }
            stop_requested = slot->status.stop_requested;
            if (!stop_requested && slot->pending_count > 0) {
                snprintf(step_payload, sizeof(step_payload), "%s", slot->pending_payloads[slot->pending_head]);
                slot->pending_payloads[slot->pending_head][0] = '\0';
                slot->pending_head = (slot->pending_head + 1U) % 4U;
                slot->pending_count--;
            } else {
                step_payload[0] = '\0';
                should_run = false;
            }
        } else {
            snprintf(step_payload, sizeof(step_payload), "%s", slot->payload);
        }
        task_unlock(slot);

        if (stop_requested || (max_iterations > 0 && completed >= max_iterations)) {
            break;
        }
        if (!should_run) {
            continue;
        }
        if (now_ms < next_deadline_ms) {
            espclaw_hw_sleep_ms((uint32_t)(next_deadline_ms - now_ms));
        }

        task_lock(slot);
        slot->status.last_started_ms = espclaw_hw_ticks_ms();
        task_unlock(slot);

        step_status = espclaw_app_vm_step(
            vm,
            slot->status.trigger,
            step_payload,
            result,
            sizeof(result)
        );

        task_lock(slot);
        completed = ++slot->status.iterations_completed;
        if (event_schedule) {
            slot->status.events_received++;
        }
        slot->status.last_status = step_status;
        slot->status.last_finished_ms = espclaw_hw_ticks_ms();
        snprintf(slot->status.last_result, sizeof(slot->status.last_result), "%s", result);
        stop_requested = slot->status.stop_requested;
        max_iterations = slot->status.max_iterations;
        period_ms = slot->status.period_ms;
        task_unlock(slot);

        if (step_status != 0 || stop_requested || (max_iterations > 0 && completed >= max_iterations)) {
            break;
        }

        next_deadline_ms += period_ms;
        if (next_deadline_ms < espclaw_hw_ticks_ms()) {
            next_deadline_ms = espclaw_hw_ticks_ms() + period_ms;
        }
    }

    espclaw_app_vm_close(vm);
    mark_task_finished(slot, slot->status.last_status, slot->status.last_result);
}

#ifdef ESPCLAW_WASM
static int run_task_step_once_wasm(espclaw_task_slot_t *slot, const char *payload, bool keep_active)
{
    espclaw_app_vm_t *vm = NULL;
    char result[ESPCLAW_TASK_RESULT_MAX];
    int open_status;
    int step_status;

    if (slot == NULL) {
        return -1;
    }

    open_status = espclaw_app_vm_open(
        slot->workspace_root,
        slot->status.app_id,
        &vm,
        result,
        sizeof(result)
    );
    if (open_status != 0) {
        mark_task_finished(slot, -1, result);
        return -1;
    }

    task_lock(slot);
    slot->status.last_started_ms = espclaw_hw_ticks_ms();
    task_unlock(slot);

    step_status = espclaw_app_vm_step(
        vm,
        slot->status.trigger,
        payload != NULL ? payload : "",
        result,
        sizeof(result)
    );
    espclaw_app_vm_close(vm);

    task_lock(slot);
    slot->status.iterations_completed++;
    if (schedule_is_event(slot->status.schedule)) {
        slot->status.events_received++;
    }
    slot->status.last_status = step_status;
    slot->status.last_finished_ms = espclaw_hw_ticks_ms();
    snprintf(slot->status.last_result, sizeof(slot->status.last_result), "%s", result);
    if (!keep_active || step_status != 0) {
        slot->status.active = false;
        slot->status.completed = true;
    }
    task_unlock(slot);

    return step_status;
}
#endif

#ifdef ESP_PLATFORM
static void task_runtime_worker(void *argument)
{
    espclaw_task_slot_t *slot = (espclaw_task_slot_t *)argument;

    run_task_worker(slot);
    if (slot != NULL) {
        slot->task = NULL;
    }
    vTaskDelete(NULL);
}
#else
static void *task_runtime_thread(void *argument)
{
    espclaw_task_slot_t *slot = (espclaw_task_slot_t *)argument;

    run_task_worker(slot);
    return NULL;
}
#endif

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
)
{
    return espclaw_task_start_with_schedule(
        task_id,
        workspace_root,
        app_id,
        "periodic",
        trigger,
        payload,
        period_ms,
        max_iterations,
        buffer,
        buffer_size
    );
}

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
)
{
    espclaw_task_slot_t *slot = NULL;
    espclaw_app_manifest_t manifest;
    bool event_schedule = schedule_is_event(schedule);
    int index;

    if (buffer != NULL && buffer_size > 0) {
        buffer[0] = '\0';
    }
    if (task_id == NULL || workspace_root == NULL || app_id == NULL || trigger == NULL || schedule == NULL ||
        trigger[0] == '\0' || (event_schedule ? false : period_ms == 0)) {
        if (buffer != NULL && buffer_size > 0) {
            snprintf(buffer, buffer_size, "invalid task arguments");
        }
        return -1;
    }
    if (espclaw_app_load_manifest(workspace_root, app_id, &manifest) != 0) {
        if (buffer != NULL && buffer_size > 0) {
            snprintf(buffer, buffer_size, "app %s not found", app_id);
        }
        return -1;
    }

    index = find_task_index(task_id);
    if (index >= 0) {
        if (s_tasks[index].status.active) {
            if (buffer != NULL && buffer_size > 0) {
                snprintf(buffer, buffer_size, "task %s is already active", task_id);
            }
            return -1;
        }
    } else {
        index = find_free_task_index();
        if (index < 0) {
            if (buffer != NULL && buffer_size > 0) {
                snprintf(buffer, buffer_size, "no task slots available");
            }
            return -1;
        }
    }

    slot = &s_tasks[index];
    if (task_lock_init(slot) != 0) {
        if (buffer != NULL && buffer_size > 0) {
            snprintf(buffer, buffer_size, "failed to initialize task lock");
        }
        return -1;
    }

    task_lock(slot);
    memset(&slot->status, 0, sizeof(slot->status));
    memset(slot->pending_payloads, 0, sizeof(slot->pending_payloads));
    slot->pending_head = 0;
    slot->pending_tail = 0;
    slot->pending_count = 0;
    snprintf(slot->status.task_id, sizeof(slot->status.task_id), "%s", task_id);
    snprintf(slot->status.app_id, sizeof(slot->status.app_id), "%s", app_id);
    snprintf(slot->status.trigger, sizeof(slot->status.trigger), "%s", trigger);
    snprintf(slot->status.schedule, sizeof(slot->status.schedule), "%s", schedule);
    slot->status.active = true;
    slot->status.completed = false;
    slot->status.stop_requested = false;
    slot->status.period_ms = event_schedule ? 0 : period_ms;
    slot->status.max_iterations = max_iterations;
    slot->status.started_ms = espclaw_hw_ticks_ms();
    snprintf(slot->workspace_root, sizeof(slot->workspace_root), "%s", workspace_root);
    snprintf(slot->payload, sizeof(slot->payload), "%s", payload != NULL ? payload : "");
    task_unlock(slot);

#ifdef ESP_PLATFORM
    {
        int core = espclaw_task_policy_core_for(ESPCLAW_TASK_KIND_CONTROL_LOOP);
        size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        size_t largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

        if (xTaskCreatePinnedToCore(
                task_runtime_worker,
                "espclaw_task",
                ESPCLAW_TASK_RUNTIME_STACK_WORDS,
                slot,
                5,
                &slot->task,
                core >= 0 ? core : tskNO_AFFINITY) != pdPASS) {
            ESP_LOGE(
                TAG,
                "Failed to start task worker task_id=%s app_id=%s stack=%u free_internal=%u largest_internal=%u",
                task_id,
                app_id,
                (unsigned int)ESPCLAW_TASK_RUNTIME_STACK_WORDS,
                (unsigned int)free_internal,
                (unsigned int)largest_internal
            );
            task_lock(slot);
            memset(&slot->status, 0, sizeof(slot->status));
            slot->payload[0] = '\0';
            task_unlock(slot);
            if (buffer != NULL && buffer_size > 0) {
                snprintf(
                    buffer,
                    buffer_size,
                    "failed to create task worker stack=%u free_internal=%u largest_internal=%u",
                    (unsigned int)ESPCLAW_TASK_RUNTIME_STACK_WORDS,
                    (unsigned int)free_internal,
                    (unsigned int)largest_internal
                );
            }
            return -1;
        }
    }
#elif defined(ESPCLAW_WASM)
    if (event_schedule) {
        if (buffer != NULL && buffer_size > 0) {
            snprintf(buffer, buffer_size, "task %s armed for event trigger %s", task_id, trigger);
        }
        return 0;
    }

    if (run_task_step_once_wasm(slot, slot->payload, false) != 0) {
        if (buffer != NULL && buffer_size > 0) {
            snprintf(buffer, buffer_size, "task %s failed", task_id);
        }
        return -1;
    }
#else
    if (pthread_create(&slot->thread, NULL, task_runtime_thread, slot) != 0) {
        task_lock(slot);
        slot->status.active = false;
        task_unlock(slot);
        if (buffer != NULL && buffer_size > 0) {
            snprintf(buffer, buffer_size, "failed to create task thread");
        }
        return -1;
    }
    pthread_detach(slot->thread);
    slot->thread_created = true;
#endif

    if (buffer != NULL && buffer_size > 0) {
        snprintf(buffer, buffer_size, "task %s started", task_id);
    }
    return 0;
}

int espclaw_task_stop(const char *task_id, char *buffer, size_t buffer_size)
{
    int index = find_task_index(task_id);

    if (buffer != NULL && buffer_size > 0) {
        buffer[0] = '\0';
    }
    if (index < 0) {
        if (buffer != NULL && buffer_size > 0) {
            snprintf(buffer, buffer_size, "task %s not found", task_id != NULL ? task_id : "");
        }
        return -1;
    }

    task_lock(&s_tasks[index]);
    s_tasks[index].status.stop_requested = true;
    task_unlock(&s_tasks[index]);
    notify_task_worker(&s_tasks[index]);
    if (buffer != NULL && buffer_size > 0) {
        snprintf(buffer, buffer_size, "task %s stop requested", task_id);
    }
    return 0;
}

int espclaw_task_emit_event(
    const char *event_name,
    const char *payload,
    char *buffer,
    size_t buffer_size
)
{
    size_t index;
    size_t delivered = 0;

    if (buffer != NULL && buffer_size > 0) {
        buffer[0] = '\0';
    }
    if (event_name == NULL || event_name[0] == '\0') {
        if (buffer != NULL && buffer_size > 0) {
            snprintf(buffer, buffer_size, "missing event name");
        }
        return -1;
    }

    for (index = 0; index < ESPCLAW_TASK_RUNTIME_MAX; ++index) {
        espclaw_task_slot_t *slot = &s_tasks[index];
        char payload_copy[ESPCLAW_TASK_PAYLOAD_MAX];

        if (!slot->status.active || !schedule_is_event(slot->status.schedule) ||
            strcmp(slot->status.trigger, event_name) != 0) {
            continue;
        }

        task_lock(slot);
        if (slot->pending_count < 4U) {
            snprintf(slot->pending_payloads[slot->pending_tail], sizeof(slot->pending_payloads[slot->pending_tail]), "%s", payload != NULL ? payload : "");
            slot->pending_tail = (slot->pending_tail + 1U) % 4U;
            slot->pending_count++;
        } else {
            size_t overwrite_index = (slot->pending_tail + 3U) % 4U;

            snprintf(slot->pending_payloads[overwrite_index], sizeof(slot->pending_payloads[overwrite_index]), "%s", payload != NULL ? payload : "");
        }
#ifdef ESPCLAW_WASM
        payload_copy[0] = '\0';
        if (slot->pending_count > 0) {
            snprintf(payload_copy, sizeof(payload_copy), "%s", slot->pending_payloads[slot->pending_head]);
            slot->pending_payloads[slot->pending_head][0] = '\0';
            slot->pending_head = (slot->pending_head + 1U) % 4U;
            slot->pending_count--;
        }
#endif
        task_unlock(slot);
#ifdef ESPCLAW_WASM
        (void)run_task_step_once_wasm(slot, payload_copy, true);
#else
        notify_task_worker(slot);
#endif
        delivered++;
    }

    if (buffer != NULL && buffer_size > 0) {
        snprintf(buffer, buffer_size, "event %s delivered to %u task(s)", event_name, (unsigned)delivered);
    }
    return delivered > 0 ? 0 : -1;
}

size_t espclaw_task_snapshot_all(espclaw_task_status_t *statuses, size_t max_statuses)
{
    size_t count = 0;
    size_t index;

    if (statuses == NULL || max_statuses == 0) {
        return 0;
    }

    for (index = 0; index < ESPCLAW_TASK_RUNTIME_MAX && count < max_statuses; ++index) {
        if (s_tasks[index].status.task_id[0] == '\0') {
            continue;
        }
        task_lock(&s_tasks[index]);
        copy_status_locked(&s_tasks[index], &statuses[count]);
        task_unlock(&s_tasks[index]);
        count++;
    }

    return count;
}

int espclaw_task_render_json(char *buffer, size_t buffer_size)
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
        snprintf(buffer, buffer_size, "{\"tasks\":[]}");
        return -1;
    }

    count = espclaw_task_snapshot_all(statuses, ESPCLAW_TASK_RUNTIME_MAX);

    written += (size_t)snprintf(buffer + written, buffer_size - written, "{\"tasks\":[");
    for (index = 0; index < count && written < buffer_size; ++index) {
        char escaped_result[ESPCLAW_TASK_RESULT_MAX * 2];

        json_escape_copy(statuses[index].last_result, escaped_result, sizeof(escaped_result));
        written += (size_t)snprintf(
            buffer + written,
            buffer_size - written,
            "%s{\"task_id\":\"%s\",\"app_id\":\"%s\",\"trigger\":\"%s\",\"schedule\":\"%s\","
            "\"active\":%s,\"completed\":%s,\"stop_requested\":%s,\"period_ms\":%u,\"max_iterations\":%u,"
            "\"iterations_completed\":%u,\"events_received\":%u,\"started_ms\":%llu,\"last_started_ms\":%llu,"
            "\"last_finished_ms\":%llu,\"last_status\":%d,\"last_result\":\"%s\"}",
            index == 0 ? "" : ",",
            statuses[index].task_id,
            statuses[index].app_id,
            statuses[index].trigger,
            statuses[index].schedule,
            statuses[index].active ? "true" : "false",
            statuses[index].completed ? "true" : "false",
            statuses[index].stop_requested ? "true" : "false",
            (unsigned int)statuses[index].period_ms,
            (unsigned int)statuses[index].max_iterations,
            (unsigned int)statuses[index].iterations_completed,
            (unsigned int)statuses[index].events_received,
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

void espclaw_task_shutdown_all(void)
{
    size_t index;

    for (index = 0; index < ESPCLAW_TASK_RUNTIME_MAX; ++index) {
        if (s_tasks[index].status.task_id[0] == '\0') {
            continue;
        }
        task_lock(&s_tasks[index]);
        s_tasks[index].status.stop_requested = true;
        task_unlock(&s_tasks[index]);
        notify_task_worker(&s_tasks[index]);
    }
}
