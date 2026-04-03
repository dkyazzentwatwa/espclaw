#include "espclaw/channel.h"

#include <stddef.h>
#include <string.h>

static const espclaw_channel_descriptor_t CHANNELS[] = {
    {
        .id = "telegram",
        .display_name = "Telegram",
        .transport = ESPCLAW_CHANNEL_TRANSPORT_POLLING,
        .supports_media = true,
        .enabled_in_v1 = true,
    },
    {
        .id = "slack",
        .display_name = "Slack",
        .transport = ESPCLAW_CHANNEL_TRANSPORT_POLLING,
        .supports_media = true,
        .enabled_in_v1 = false,
    },
    {
        .id = "whatsapp",
        .display_name = "WhatsApp",
        .transport = ESPCLAW_CHANNEL_TRANSPORT_WEBHOOK,
        .supports_media = true,
        .enabled_in_v1 = false,
    },
};

size_t espclaw_channel_count(void)
{
    return sizeof(CHANNELS) / sizeof(CHANNELS[0]);
}

const espclaw_channel_descriptor_t *espclaw_channel_at(size_t index)
{
    if (index >= espclaw_channel_count()) {
        return NULL;
    }
    return &CHANNELS[index];
}

const espclaw_channel_descriptor_t *espclaw_find_channel(const char *id)
{
    size_t index;

    if (id == NULL) {
        return NULL;
    }

    for (index = 0; index < espclaw_channel_count(); ++index) {
        if (strcmp(CHANNELS[index].id, id) == 0) {
            return &CHANNELS[index];
        }
    }

    return NULL;
}
