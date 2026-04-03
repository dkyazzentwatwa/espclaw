#ifndef ESPCLAW_WORKSPACE_H
#define ESPCLAW_WORKSPACE_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    const char *relative_path;
    const char *default_content;
} espclaw_workspace_file_t;

typedef enum {
    ESPCLAW_WORKSPACE_BLOB_STAGE_NONE = 0,
    ESPCLAW_WORKSPACE_BLOB_STAGE_OPEN,
    ESPCLAW_WORKSPACE_BLOB_STAGE_COMMITTED,
} espclaw_workspace_blob_stage_t;

typedef struct {
    char blob_id[65];
    char target_path[256];
    char content_type[64];
    size_t bytes_written;
    espclaw_workspace_blob_stage_t stage;
} espclaw_workspace_blob_status_t;

size_t espclaw_workspace_file_count(void);
const espclaw_workspace_file_t *espclaw_workspace_file_at(size_t index);
const espclaw_workspace_file_t *espclaw_find_workspace_file(const char *relative_path);
bool espclaw_workspace_is_control_file(const char *relative_path);
int espclaw_workspace_resolve_path(
    const char *workspace_root,
    const char *relative_path,
    char *buffer,
    size_t buffer_size
);
int espclaw_workspace_bootstrap(const char *workspace_root);
int espclaw_workspace_read_file(
    const char *workspace_root,
    const char *relative_path,
    char *buffer,
    size_t buffer_size
);
int espclaw_workspace_write_file(
    const char *workspace_root,
    const char *relative_path,
    const char *content
);
int espclaw_workspace_blob_begin(
    const char *workspace_root,
    const char *blob_id,
    const char *target_path,
    const char *content_type
);
int espclaw_workspace_blob_append(
    const char *workspace_root,
    const char *blob_id,
    const void *data,
    size_t data_size,
    size_t *bytes_written_out
);
int espclaw_workspace_blob_commit(
    const char *workspace_root,
    const char *blob_id,
    char *committed_path,
    size_t committed_path_size,
    size_t *bytes_written_out
);
int espclaw_workspace_blob_status(
    const char *workspace_root,
    const char *blob_id,
    espclaw_workspace_blob_status_t *status_out
);

#endif
