#ifndef ESPCLAW_LOG_BUFFER_H
#define ESPCLAW_LOG_BUFFER_H

#include <stddef.h>

void espclaw_log_buffer_init(void);
void espclaw_log_buffer_reset(void);
void espclaw_log_buffer_append(const char *text);
size_t espclaw_log_buffer_capacity(void);
size_t espclaw_log_buffer_size(void);
size_t espclaw_log_buffer_dropped_bytes(void);
size_t espclaw_log_buffer_copy_tail(char *buffer, size_t buffer_size, size_t tail_bytes);
int espclaw_log_buffer_render_json(size_t tail_bytes, char *buffer, size_t buffer_size);

#endif
