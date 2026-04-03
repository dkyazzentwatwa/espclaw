#ifndef ESPCLAW_ADMIN_OPS_H
#define ESPCLAW_ADMIN_OPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool espclaw_admin_query_value(
    const char *query,
    const char *key,
    char *buffer,
    size_t buffer_size
);
bool espclaw_admin_json_string_value(
    const char *json,
    const char *key,
    char *buffer,
    size_t buffer_size
);
bool espclaw_admin_json_long_value(
    const char *json,
    const char *key,
    long *value
);
bool espclaw_admin_json_i64_value(
    const char *json,
    const char *key,
    int64_t *value
);
size_t espclaw_admin_render_result_json(
    bool ok,
    const char *message,
    char *buffer,
    size_t buffer_size
);
size_t espclaw_admin_render_run_result_json(
    const char *app_id,
    const char *trigger,
    bool ok,
    const char *result,
    char *buffer,
    size_t buffer_size
);
int espclaw_admin_scaffold_app(
    const char *workspace_root,
    const char *app_id,
    const char *title,
    const char *permissions_csv,
    const char *triggers_csv
);
int espclaw_admin_scaffold_default_app(
    const char *workspace_root,
    const char *app_id
);

#endif
