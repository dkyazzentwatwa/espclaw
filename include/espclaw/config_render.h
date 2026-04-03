#ifndef ESPCLAW_CONFIG_RENDER_H
#define ESPCLAW_CONFIG_RENDER_H

#include <stddef.h>

#include "espclaw/board_profile.h"

size_t espclaw_render_default_config(
    const espclaw_board_profile_t *profile,
    char *buffer,
    size_t buffer_size
);

#endif
