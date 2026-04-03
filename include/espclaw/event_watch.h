#ifndef ESPCLAW_EVENT_WATCH_H
#define ESPCLAW_EVENT_WATCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ESPCLAW_EVENT_WATCH_MAX 8
#define ESPCLAW_EVENT_WATCH_ID_MAX 32
#define ESPCLAW_EVENT_WATCH_KIND_MAX 24
#define ESPCLAW_EVENT_WATCH_EVENT_NAME_MAX 24
#define ESPCLAW_EVENT_WATCH_PAYLOAD_MAX 128

typedef struct {
    bool active;
    char watch_id[ESPCLAW_EVENT_WATCH_ID_MAX + 1];
    char kind[ESPCLAW_EVENT_WATCH_KIND_MAX + 1];
    char event_name[ESPCLAW_EVENT_WATCH_EVENT_NAME_MAX + 1];
    uint32_t interval_ms;
    uint32_t samples_seen;
    uint64_t last_sample_ms;
    uint64_t last_emit_ms;
    int port;
    int unit;
    int channel;
    int threshold;
    bool last_above_threshold;
    bool threshold_initialized;
    char last_payload[ESPCLAW_EVENT_WATCH_PAYLOAD_MAX];
} espclaw_event_watch_status_t;

int espclaw_event_watch_runtime_start(void);
void espclaw_event_watch_runtime_shutdown(void);

int espclaw_event_watch_add_uart(
    const char *watch_id,
    const char *event_name,
    int port,
    char *buffer,
    size_t buffer_size
);
int espclaw_event_watch_add_adc_threshold(
    const char *watch_id,
    const char *event_name,
    int unit,
    int channel,
    int threshold,
    uint32_t interval_ms,
    char *buffer,
    size_t buffer_size
);
int espclaw_event_watch_remove(const char *watch_id, char *buffer, size_t buffer_size);
size_t espclaw_event_watch_snapshot_all(espclaw_event_watch_status_t *statuses, size_t max_statuses);
int espclaw_event_watch_render_json(char *buffer, size_t buffer_size);

#endif
