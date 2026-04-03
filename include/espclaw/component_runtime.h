#ifndef ESPCLAW_COMPONENT_RUNTIME_H
#define ESPCLAW_COMPONENT_RUNTIME_H

#include <stddef.h>

#define ESPCLAW_COMPONENT_ID_MAX 32
#define ESPCLAW_COMPONENT_VERSION_MAX 24
#define ESPCLAW_COMPONENT_TITLE_MAX 64
#define ESPCLAW_COMPONENT_MODULE_MAX 64
#define ESPCLAW_COMPONENT_SUMMARY_MAX 160
#define ESPCLAW_COMPONENT_URL_MAX 256
#define ESPCLAW_COMPONENT_CHECKSUM_MAX 96
#define ESPCLAW_COMPONENT_DEPENDENCY_MAX 4

typedef struct {
    char component_id[ESPCLAW_COMPONENT_ID_MAX + 1];
    char version[ESPCLAW_COMPONENT_VERSION_MAX + 1];
    char title[ESPCLAW_COMPONENT_TITLE_MAX + 1];
    char module[ESPCLAW_COMPONENT_MODULE_MAX + 1];
    char summary[ESPCLAW_COMPONENT_SUMMARY_MAX + 1];
    char manifest_url[ESPCLAW_COMPONENT_URL_MAX + 1];
    char source_url[ESPCLAW_COMPONENT_URL_MAX + 1];
    char docs_url[ESPCLAW_COMPONENT_URL_MAX + 1];
    char checksum[ESPCLAW_COMPONENT_CHECKSUM_MAX + 1];
    char dependencies[ESPCLAW_COMPONENT_DEPENDENCY_MAX][ESPCLAW_COMPONENT_URL_MAX + 1];
    size_t dependency_count;
} espclaw_component_manifest_t;

int espclaw_component_install(
    const char *workspace_root,
    const char *component_id,
    const char *title,
    const char *module_name,
    const char *summary,
    const char *version,
    const char *source
);
int espclaw_component_install_from_file(
    const char *workspace_root,
    const char *component_id,
    const char *title,
    const char *module_name,
    const char *summary,
    const char *version,
    const char *source_path
);
int espclaw_component_install_from_blob(
    const char *workspace_root,
    const char *component_id,
    const char *title,
    const char *module_name,
    const char *summary,
    const char *version,
    const char *blob_id
);
int espclaw_component_install_from_url(
    const char *workspace_root,
    const char *component_id,
    const char *title,
    const char *module_name,
    const char *summary,
    const char *version,
    const char *source_url
);
int espclaw_component_install_from_manifest(
    const char *workspace_root,
    const char *manifest_url
);
int espclaw_component_collect_ids(
    const char *workspace_root,
    char ids[][ESPCLAW_COMPONENT_ID_MAX + 1],
    size_t max_ids,
    size_t *count_out
);
int espclaw_component_load_manifest(
    const char *workspace_root,
    const char *component_id,
    espclaw_component_manifest_t *manifest
);
int espclaw_component_read_source(
    const char *workspace_root,
    const char *component_id,
    char *buffer,
    size_t buffer_size
);
int espclaw_component_remove(
    const char *workspace_root,
    const char *component_id
);
int espclaw_render_components_json(
    const char *workspace_root,
    char *buffer,
    size_t buffer_size
);
int espclaw_render_component_detail_json(
    const char *workspace_root,
    const char *component_id,
    char *buffer,
    size_t buffer_size
);

#endif
