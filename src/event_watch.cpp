#include "espclaw/event_watch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#elif defined(ESPCLAW_WASM)
#else
#include <pthread.h>
#endif

#include "espclaw/hardware.h"
#include "espclaw/task_policy.h"
#include "espclaw/task_runtime.h"

typedef struct {
    espclaw_event_watch_status_t status;
#ifdef ESP_PLATFORM
    SemaphoreHandle_t lock;
#elif defined(ESPCLAW_WASM)
#else
    pthread_mutex_t lock;
#endif
    bool lock_ready;
} espclaw_event_watch_slot_t;

static espclaw_event_watch_slot_t s_watches[ESPCLAW_EVENT_WATCH_MAX];
static bool s_runtime_started;
static bool s_stop_requested;

#ifdef ESP_PLATFORM
static TaskHandle_t s_watch_task;
#elif defined(ESPCLAW_WASM)
#else
static pthread_t s_watch_thread;
#endif

static int watch_lock_init(espclaw_event_watch_slot_t *slot)
{
    if (slot == NULL) {
        return -1;
    }
    if (slot->lock_ready) {
        return 0;
    }
#ifdef ESP_PLATFORM
    slot->lock = xSemaphoreCreateMutex();
    if (slot->lock == NULL) {
        return -1;
    }
#elif defined(ESPCLAW_WASM)
#else
    if (pthread_mutex_init(&slot->lock, NULL) != 0) {
        return -1;
    }
#endif
    slot->lock_ready = true;
    return 0;
}

static void watch_lock(espclaw_event_watch_slot_t *slot)
{
    if (slot == NULL || !slot->lock_ready) {
        return;
    }
#ifdef ESP_PLATFORM
    xSemaphoreTake(slot->lock, portMAX_DELAY);
#elif defined(ESPCLAW_WASM)
#else
    pthread_mutex_lock(&slot->lock);
#endif
}

static void watch_unlock(espclaw_event_watch_slot_t *slot)
{
    if (slot == NULL || !slot->lock_ready) {
        return;
    }
#ifdef ESP_PLATFORM
    xSemaphoreGive(slot->lock);
#elif defined(ESPCLAW_WASM)
#else
    pthread_mutex_unlock(&slot->lock);
#endif
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

static int find_watch_index(const char *watch_id)
{
    size_t index;

    if (watch_id == NULL || watch_id[0] == '\0') {
        return -1;
    }

    for (index = 0; index < ESPCLAW_EVENT_WATCH_MAX; ++index) {
        if (s_watches[index].status.watch_id[0] != '\0' &&
            strcmp(s_watches[index].status.watch_id, watch_id) == 0) {
            return (int)index;
        }
    }

    return -1;
}

static int find_free_watch_index(void)
{
    size_t index;

    for (index = 0; index < ESPCLAW_EVENT_WATCH_MAX; ++index) {
        if (s_watches[index].status.watch_id[0] == '\0') {
            return (int)index;
        }
    }

    return -1;
}

static void watch_status_copy(const espclaw_event_watch_slot_t *slot, espclaw_event_watch_status_t *status_out)
{
    if (slot == NULL || status_out == NULL) {
        return;
    }
    *status_out = slot->status;
}

static void handle_uart_watch(espclaw_event_watch_slot_t *slot)
{
    uint8_t data[256];
    size_t length = 0;

    if (slot == NULL) {
        return;
    }
    if (espclaw_hw_uart_take_event_data(slot->status.port, data, sizeof(data) - 1, &length) != 0 || length == 0) {
        return;
    }

    data[length] = '\0';
    watch_lock(slot);
    slot->status.samples_seen++;
    slot->status.last_sample_ms = espclaw_hw_ticks_ms();
    snprintf(
        slot->status.last_payload,
        sizeof(slot->status.last_payload),
        "%.*s",
        (int)(sizeof(slot->status.last_payload) - 1),
        (const char *)data
    );
    watch_unlock(slot);

    if (espclaw_task_emit_event(slot->status.event_name, (const char *)data, NULL, 0) == 0) {
        watch_lock(slot);
        slot->status.last_emit_ms = espclaw_hw_ticks_ms();
        watch_unlock(slot);
    }
}

static void handle_adc_threshold_watch(espclaw_event_watch_slot_t *slot)
{
    int raw = 0;
    bool above = false;
    bool should_emit = false;
    char payload[ESPCLAW_EVENT_WATCH_PAYLOAD_MAX];

    if (slot == NULL) {
        return;
    }
    if (espclaw_hw_adc_read_raw(slot->status.unit, slot->status.channel, &raw) != 0) {
        return;
    }

    above = raw >= slot->status.threshold;
    watch_lock(slot);
    slot->status.samples_seen++;
    slot->status.last_sample_ms = espclaw_hw_ticks_ms();
    if (!slot->status.threshold_initialized) {
        slot->status.threshold_initialized = true;
        slot->status.last_above_threshold = above;
    } else if (slot->status.last_above_threshold != above) {
        slot->status.last_above_threshold = above;
        should_emit = true;
    }
    watch_unlock(slot);

    if (!should_emit) {
        return;
    }

    snprintf(
        payload,
        sizeof(payload),
        "{\"raw\":%d,\"threshold\":%d,\"state\":\"%s\"}",
        raw,
        slot->status.threshold,
        above ? "above" : "below"
    );

    watch_lock(slot);
    snprintf(slot->status.last_payload, sizeof(slot->status.last_payload), "%s", payload);
    watch_unlock(slot);

    if (espclaw_task_emit_event(slot->status.event_name, payload, NULL, 0) == 0) {
        watch_lock(slot);
        slot->status.last_emit_ms = espclaw_hw_ticks_ms();
        watch_unlock(slot);
    }
}

static void event_watch_step(void)
{
    size_t index;
    uint64_t now_ms = espclaw_hw_ticks_ms();

    for (index = 0; index < ESPCLAW_EVENT_WATCH_MAX; ++index) {
        espclaw_event_watch_slot_t *slot = &s_watches[index];
        espclaw_event_watch_status_t snapshot;

        if (slot->status.watch_id[0] == '\0' || !slot->status.active) {
            continue;
        }

        watch_lock(slot);
        watch_status_copy(slot, &snapshot);
        watch_unlock(slot);

        if (strcmp(snapshot.kind, "uart") == 0) {
            handle_uart_watch(slot);
            continue;
        }
        if (strcmp(snapshot.kind, "adc_threshold") == 0) {
            if (snapshot.interval_ms > 0 && snapshot.last_sample_ms > 0 &&
                now_ms - snapshot.last_sample_ms < snapshot.interval_ms) {
                continue;
            }
            handle_adc_threshold_watch(slot);
        }
    }
}

#ifdef ESP_PLATFORM
static void event_watch_worker(void *argument)
{
    (void)argument;

    while (!s_stop_requested) {
        event_watch_step();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    s_watch_task = NULL;
    vTaskDelete(NULL);
}
#elif defined(ESPCLAW_WASM)
#else
static void *event_watch_thread(void *argument)
{
    (void)argument;

    while (!s_stop_requested) {
        event_watch_step();
        espclaw_hw_sleep_ms(50);
    }
    return NULL;
}
#endif

int espclaw_event_watch_runtime_start(void)
{
    if (s_runtime_started) {
        return 0;
    }

    s_stop_requested = false;
#ifdef ESP_PLATFORM
    {
        int core = espclaw_task_policy_core_for(ESPCLAW_TASK_KIND_CONTROL_LOOP);

        if (xTaskCreatePinnedToCore(
                event_watch_worker,
                "espclaw_watch",
                4096,
                NULL,
                4,
                &s_watch_task,
                core >= 0 ? core : tskNO_AFFINITY) != pdPASS) {
            return -1;
        }
    }
#elif defined(ESPCLAW_WASM)
    s_runtime_started = true;
    return 0;
#else
    if (pthread_create(&s_watch_thread, NULL, event_watch_thread, NULL) != 0) {
        return -1;
    }
    pthread_detach(s_watch_thread);
#endif
    s_runtime_started = true;
    return 0;
}

void espclaw_event_watch_runtime_shutdown(void)
{
    s_stop_requested = true;
}

static int add_common_watch(
    const char *watch_id,
    const char *kind,
    const char *event_name,
    char *buffer,
    size_t buffer_size
)
{
    espclaw_event_watch_slot_t *slot = NULL;
    int index = find_watch_index(watch_id);

    if (buffer != NULL && buffer_size > 0) {
        buffer[0] = '\0';
    }
    if (watch_id == NULL || watch_id[0] == '\0' || kind == NULL || event_name == NULL || event_name[0] == '\0') {
        if (buffer != NULL && buffer_size > 0) {
            snprintf(buffer, buffer_size, "invalid watch arguments");
        }
        return -1;
    }

    if (index < 0) {
        index = find_free_watch_index();
        if (index < 0) {
            if (buffer != NULL && buffer_size > 0) {
                snprintf(buffer, buffer_size, "no watch slots available");
            }
            return -1;
        }
    }

    slot = &s_watches[index];
    if (watch_lock_init(slot) != 0) {
        if (buffer != NULL && buffer_size > 0) {
            snprintf(buffer, buffer_size, "failed to initialize watch");
        }
        return -1;
    }

    watch_lock(slot);
    memset(&slot->status, 0, sizeof(slot->status));
    slot->status.active = true;
    snprintf(slot->status.watch_id, sizeof(slot->status.watch_id), "%s", watch_id);
    snprintf(slot->status.kind, sizeof(slot->status.kind), "%s", kind);
    snprintf(slot->status.event_name, sizeof(slot->status.event_name), "%s", event_name);
    watch_unlock(slot);

    if (buffer != NULL && buffer_size > 0) {
        snprintf(buffer, buffer_size, "watch %s active", watch_id);
    }
    return 0;
}

int espclaw_event_watch_add_uart(
    const char *watch_id,
    const char *event_name,
    int port,
    char *buffer,
    size_t buffer_size
)
{
    int status = add_common_watch(
        watch_id,
        "uart",
        event_name != NULL && event_name[0] != '\0' ? event_name : "uart",
        buffer,
        buffer_size
    );
    int index;

    if (status != 0) {
        return status;
    }

    index = find_watch_index(watch_id);
    if (index < 0) {
        return -1;
    }

    watch_lock(&s_watches[index]);
    s_watches[index].status.port = port;
    watch_unlock(&s_watches[index]);
    return 0;
}

int espclaw_event_watch_add_adc_threshold(
    const char *watch_id,
    const char *event_name,
    int unit,
    int channel,
    int threshold,
    uint32_t interval_ms,
    char *buffer,
    size_t buffer_size
)
{
    int status = add_common_watch(
        watch_id,
        "adc_threshold",
        event_name != NULL && event_name[0] != '\0' ? event_name : "sensor",
        buffer,
        buffer_size
    );
    int index;

    if (status != 0) {
        return status;
    }

    index = find_watch_index(watch_id);
    if (index < 0) {
        return -1;
    }

    watch_lock(&s_watches[index]);
    s_watches[index].status.unit = unit;
    s_watches[index].status.channel = channel;
    s_watches[index].status.threshold = threshold;
    s_watches[index].status.interval_ms = interval_ms > 0 ? interval_ms : 100;
    watch_unlock(&s_watches[index]);
    return 0;
}

int espclaw_event_watch_remove(const char *watch_id, char *buffer, size_t buffer_size)
{
    int index = find_watch_index(watch_id);

    if (buffer != NULL && buffer_size > 0) {
        buffer[0] = '\0';
    }
    if (index < 0) {
        if (buffer != NULL && buffer_size > 0) {
            snprintf(buffer, buffer_size, "watch %s not found", watch_id != NULL ? watch_id : "");
        }
        return -1;
    }

    watch_lock(&s_watches[index]);
    memset(&s_watches[index].status, 0, sizeof(s_watches[index].status));
    watch_unlock(&s_watches[index]);

    if (buffer != NULL && buffer_size > 0) {
        snprintf(buffer, buffer_size, "watch %s removed", watch_id);
    }
    return 0;
}

size_t espclaw_event_watch_snapshot_all(espclaw_event_watch_status_t *statuses, size_t max_statuses)
{
    size_t count = 0;
    size_t index;

    if (statuses == NULL || max_statuses == 0) {
        return 0;
    }

    for (index = 0; index < ESPCLAW_EVENT_WATCH_MAX && count < max_statuses; ++index) {
        if (s_watches[index].status.watch_id[0] == '\0') {
            continue;
        }
        watch_lock(&s_watches[index]);
        watch_status_copy(&s_watches[index], &statuses[count]);
        watch_unlock(&s_watches[index]);
        count++;
    }
    return count;
}

int espclaw_event_watch_render_json(char *buffer, size_t buffer_size)
{
    espclaw_event_watch_status_t *statuses = NULL;
    size_t count;
    size_t index;
    size_t written = 0;

    if (buffer == NULL || buffer_size == 0) {
        return -1;
    }

    statuses = calloc(ESPCLAW_EVENT_WATCH_MAX, sizeof(*statuses));
    if (statuses == NULL) {
        snprintf(buffer, buffer_size, "{\"watches\":[]}");
        return -1;
    }

    count = espclaw_event_watch_snapshot_all(statuses, ESPCLAW_EVENT_WATCH_MAX);

    written += (size_t)snprintf(buffer + written, buffer_size - written, "{\"watches\":[");
    for (index = 0; index < count && written < buffer_size; ++index) {
        char escaped_payload[ESPCLAW_EVENT_WATCH_PAYLOAD_MAX * 2];

        json_escape_copy(statuses[index].last_payload, escaped_payload, sizeof(escaped_payload));
        written += (size_t)snprintf(
            buffer + written,
            buffer_size - written,
            "%s{\"watch_id\":\"%s\",\"kind\":\"%s\",\"event_name\":\"%s\",\"active\":%s,"
            "\"interval_ms\":%u,\"samples_seen\":%u,\"last_sample_ms\":%llu,\"last_emit_ms\":%llu,"
            "\"port\":%d,\"unit\":%d,\"channel\":%d,\"threshold\":%d,\"last_payload\":\"%s\"}",
            index == 0 ? "" : ",",
            statuses[index].watch_id,
            statuses[index].kind,
            statuses[index].event_name,
            statuses[index].active ? "true" : "false",
            (unsigned)statuses[index].interval_ms,
            (unsigned)statuses[index].samples_seen,
            (unsigned long long)statuses[index].last_sample_ms,
            (unsigned long long)statuses[index].last_emit_ms,
            statuses[index].port,
            statuses[index].unit,
            statuses[index].channel,
            statuses[index].threshold,
            escaped_payload
        );
    }
    written += (size_t)snprintf(buffer + written, buffer_size - written, "]}");

    if (written >= buffer_size) {
        buffer[buffer_size - 1] = '\0';
    }
    free(statuses);
    return 0;
}
