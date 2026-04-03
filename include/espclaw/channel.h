#ifndef ESPCLAW_CHANNEL_H
#define ESPCLAW_CHANNEL_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    ESPCLAW_CHANNEL_TRANSPORT_POLLING = 0,
    ESPCLAW_CHANNEL_TRANSPORT_WEBHOOK = 1
} espclaw_channel_transport_t;

typedef struct {
    const char *id;
    const char *display_name;
    espclaw_channel_transport_t transport;
    bool supports_media;
    bool enabled_in_v1;
} espclaw_channel_descriptor_t;

size_t espclaw_channel_count(void);
const espclaw_channel_descriptor_t *espclaw_channel_at(size_t index);
const espclaw_channel_descriptor_t *espclaw_find_channel(const char *id);

#endif
