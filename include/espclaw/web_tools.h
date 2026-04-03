#ifndef ESPCLAW_WEB_TOOLS_H
#define ESPCLAW_WEB_TOOLS_H

#include <stddef.h>

typedef int (*espclaw_web_http_adapter_t)(
    const char *url,
    char *response,
    size_t response_size,
    void *user_data
);

void espclaw_web_set_http_adapter(espclaw_web_http_adapter_t adapter, void *user_data);

int espclaw_web_search(
    const char *query,
    char *buffer,
    size_t buffer_size
);

int espclaw_web_fetch(
    const char *workspace_root,
    const char *url,
    char *buffer,
    size_t buffer_size
);

int espclaw_web_download_to_file(
    const char *url,
    const char *destination_path
);

#endif
