#ifndef ESPCLAW_CONTEXT_RUNTIME_H
#define ESPCLAW_CONTEXT_RUNTIME_H

#include <stddef.h>

#define ESPCLAW_CONTEXT_PATH_MAX 256
#define ESPCLAW_CONTEXT_QUERY_MAX 256

int espclaw_context_render_chunks_json(
    const char *workspace_root,
    const char *relative_path,
    size_t chunk_bytes,
    char *buffer,
    size_t buffer_size
);
int espclaw_context_render_chunk_json(
    const char *workspace_root,
    const char *relative_path,
    size_t chunk_index,
    size_t chunk_bytes,
    char *buffer,
    size_t buffer_size
);
int espclaw_context_search_json(
    const char *workspace_root,
    const char *relative_path,
    const char *query,
    size_t chunk_bytes,
    size_t limit,
    char *buffer,
    size_t buffer_size
);
int espclaw_context_select_json(
    const char *workspace_root,
    const char *relative_path,
    const char *query,
    size_t chunk_bytes,
    size_t limit,
    size_t output_bytes,
    char *buffer,
    size_t buffer_size
);
int espclaw_context_summarize_json(
    const char *workspace_root,
    const char *relative_path,
    const char *query,
    size_t chunk_bytes,
    size_t limit,
    size_t summary_bytes,
    char *buffer,
    size_t buffer_size
);

#endif
