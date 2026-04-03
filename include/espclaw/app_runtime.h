#ifndef ESPCLAW_APP_RUNTIME_H
#define ESPCLAW_APP_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>

#define ESPCLAW_APP_ID_MAX 32
#define ESPCLAW_APP_VERSION_MAX 24
#define ESPCLAW_APP_TITLE_MAX 64
#define ESPCLAW_APP_ENTRYPOINT_MAX 64
#define ESPCLAW_APP_PERMISSION_MAX 32
#define ESPCLAW_APP_PERMISSION_NAME_MAX 32
#define ESPCLAW_APP_TRIGGER_MAX 8
#define ESPCLAW_APP_TRIGGER_NAME_MAX 24

typedef struct {
    char app_id[ESPCLAW_APP_ID_MAX + 1];
    char version[ESPCLAW_APP_VERSION_MAX + 1];
    char title[ESPCLAW_APP_TITLE_MAX + 1];
    char entrypoint[ESPCLAW_APP_ENTRYPOINT_MAX + 1];
    size_t permission_count;
    char permissions[ESPCLAW_APP_PERMISSION_MAX][ESPCLAW_APP_PERMISSION_NAME_MAX + 1];
    size_t trigger_count;
    char triggers[ESPCLAW_APP_TRIGGER_MAX][ESPCLAW_APP_TRIGGER_NAME_MAX + 1];
} espclaw_app_manifest_t;

typedef struct espclaw_app_vm espclaw_app_vm_t;

bool espclaw_app_id_is_valid(const char *app_id);
int espclaw_app_parse_manifest_json(const char *json, espclaw_app_manifest_t *manifest);
int espclaw_app_load_manifest(
    const char *workspace_root,
    const char *app_id,
    espclaw_app_manifest_t *manifest
);
bool espclaw_app_has_permission(const espclaw_app_manifest_t *manifest, const char *permission);
bool espclaw_app_supports_trigger(const espclaw_app_manifest_t *manifest, const char *trigger);
int espclaw_app_scaffold_lua(
    const char *workspace_root,
    const char *app_id,
    const char *title,
    const char *permissions_csv,
    const char *triggers_csv
);
int espclaw_app_collect_ids(
    const char *workspace_root,
    char ids[][ESPCLAW_APP_ID_MAX + 1],
    size_t max_ids,
    size_t *count_out
);
int espclaw_app_read_source(
    const char *workspace_root,
    const char *app_id,
    char *buffer,
    size_t buffer_size
);
int espclaw_app_update_source(
    const char *workspace_root,
    const char *app_id,
    const char *source
);
int espclaw_app_install_from_file(
    const char *workspace_root,
    const char *app_id,
    const char *title,
    const char *permissions_csv,
    const char *triggers_csv,
    const char *source_path
);
int espclaw_app_install_from_blob(
    const char *workspace_root,
    const char *app_id,
    const char *title,
    const char *permissions_csv,
    const char *triggers_csv,
    const char *blob_id
);
int espclaw_app_install_from_url(
    const char *workspace_root,
    const char *app_id,
    const char *title,
    const char *permissions_csv,
    const char *triggers_csv,
    const char *source_url
);
int espclaw_app_remove(
    const char *workspace_root,
    const char *app_id
);
int espclaw_app_vm_open(
    const char *workspace_root,
    const char *app_id,
    espclaw_app_vm_t **vm_out,
    char *buffer,
    size_t buffer_size
);
int espclaw_app_vm_step(
    espclaw_app_vm_t *vm,
    const char *trigger,
    const char *payload,
    char *buffer,
    size_t buffer_size
);
void espclaw_app_vm_close(espclaw_app_vm_t *vm);
int espclaw_app_run(
    const char *workspace_root,
    const char *app_id,
    const char *trigger,
    const char *payload,
    char *buffer,
    size_t buffer_size
);
int espclaw_app_run_boot_apps(
    const char *workspace_root,
    char *buffer,
    size_t buffer_size
);

#endif
