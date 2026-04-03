#ifndef ESPCLAW_APP_PATTERNS_H
#define ESPCLAW_APP_PATTERNS_H

#include <stddef.h>

size_t espclaw_render_app_patterns_json(char *buffer, size_t buffer_size);
size_t espclaw_render_app_patterns_markdown(char *buffer, size_t buffer_size);
size_t espclaw_render_app_patterns_prompt_snapshot(char *buffer, size_t buffer_size);

#endif
