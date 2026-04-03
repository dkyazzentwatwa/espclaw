#ifndef ESPCLAW_LUA_API_REGISTRY_H
#define ESPCLAW_LUA_API_REGISTRY_H

#include <stddef.h>

typedef struct {
    const char *category;
    const char *name;
    const char *signature;
    const char *summary;
} espclaw_lua_api_descriptor_t;

size_t espclaw_lua_api_count(void);
const espclaw_lua_api_descriptor_t *espclaw_lua_api_at(size_t index);
size_t espclaw_render_lua_api_json(char *buffer, size_t buffer_size);
size_t espclaw_render_lua_api_markdown(char *buffer, size_t buffer_size);
size_t espclaw_render_lua_api_prompt_snapshot(char *buffer, size_t buffer_size);
size_t espclaw_render_component_architecture_prompt_snapshot(char *buffer, size_t buffer_size);

#endif
