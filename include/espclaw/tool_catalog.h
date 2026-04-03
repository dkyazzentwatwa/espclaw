#ifndef ESPCLAW_TOOL_CATALOG_H
#define ESPCLAW_TOOL_CATALOG_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    ESPCLAW_TOOL_SAFETY_READ_ONLY = 0,
    ESPCLAW_TOOL_SAFETY_CONFIRM_REQUIRED = 1
} espclaw_tool_safety_t;

typedef struct {
    const char *name;
    const char *summary;
    const char *parameters_json;
    espclaw_tool_safety_t safety;
} espclaw_tool_descriptor_t;

size_t espclaw_tool_count(void);
const espclaw_tool_descriptor_t *espclaw_tool_at(size_t index);
const espclaw_tool_descriptor_t *espclaw_find_tool(const char *name);
bool espclaw_tool_requires_confirmation(const char *name);

#endif
