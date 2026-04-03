#ifndef ESPCLAW_TELEGRAM_PROTOCOL_H
#define ESPCLAW_TELEGRAM_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    long update_id;
    char chat_id[32];
    char from_id[32];
    char text[512];
} espclaw_telegram_update_t;

bool espclaw_telegram_extract_update(const char *json, espclaw_telegram_update_t *update);
size_t espclaw_telegram_build_send_message_payload(
    const char *chat_id,
    const char *text,
    char *buffer,
    size_t buffer_size
);

#endif
