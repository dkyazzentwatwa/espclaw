#include "espclaw/board_config.h"

#include <stdbool.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "espclaw/workspace.h"

typedef struct {
    const char *variant_id;
    const char *display_name;
    espclaw_board_profile_id_t profile_id;
    const espclaw_board_pin_alias_t *pins;
    size_t pin_count;
    const espclaw_board_i2c_bus_t *i2c_buses;
    size_t i2c_bus_count;
    const espclaw_board_uart_t *uarts;
    size_t uart_count;
    const espclaw_board_adc_channel_t *adc_channels;
    size_t adc_count;
} espclaw_builtin_board_t;

static const espclaw_board_pin_alias_t GENERIC_ESP32S3_PINS[] = {
    {"led", 48},
};

static const espclaw_board_pin_alias_t AI_THINKER_ESP32CAM_PINS[] = {
    {"flash_led", 4},
    {"camera_pwdn", 32},
    {"camera_xclk", 0},
    {"camera_siod", 26},
    {"camera_sioc", 27},
    {"camera_y2", 5},
    {"camera_y3", 18},
    {"camera_y4", 19},
    {"camera_y5", 21},
    {"camera_y6", 36},
    {"camera_y7", 39},
    {"camera_y8", 34},
    {"camera_y9", 35},
    {"camera_vsync", 25},
    {"camera_href", 23},
    {"camera_pclk", 22},
};

static const espclaw_builtin_board_t BUILTIN_BOARDS[] = {
    {
        .variant_id = "generic_esp32s3",
        .display_name = "Generic ESP32-S3 Dev Board",
        .profile_id = ESPCLAW_BOARD_PROFILE_ESP32S3,
        .pins = GENERIC_ESP32S3_PINS,
        .pin_count = sizeof(GENERIC_ESP32S3_PINS) / sizeof(GENERIC_ESP32S3_PINS[0]),
        .i2c_buses = NULL,
        .i2c_bus_count = 0,
        .uarts = NULL,
        .uart_count = 0,
        .adc_channels = NULL,
        .adc_count = 0,
    },
    {
        .variant_id = "ai_thinker_esp32cam",
        .display_name = "AI Thinker ESP32-CAM",
        .profile_id = ESPCLAW_BOARD_PROFILE_ESP32CAM,
        .pins = AI_THINKER_ESP32CAM_PINS,
        .pin_count = sizeof(AI_THINKER_ESP32CAM_PINS) / sizeof(AI_THINKER_ESP32CAM_PINS[0]),
        .i2c_buses = NULL,
        .i2c_bus_count = 0,
        .uarts = NULL,
        .uart_count = 0,
        .adc_channels = NULL,
        .adc_count = 0,
    },
};

static espclaw_board_descriptor_t s_current_board;
static char s_board_config_json[2048];

static void copy_text(char *buffer, size_t buffer_size, const char *value)
{
    if (buffer == NULL || buffer_size == 0) {
        return;
    }
    snprintf(buffer, buffer_size, "%s", value != NULL ? value : "");
}

static void clear_descriptor(espclaw_board_descriptor_t *descriptor)
{
    if (descriptor == NULL) {
        return;
    }
    memset(descriptor, 0, sizeof(*descriptor));
    copy_text(descriptor->source, sizeof(descriptor->source), "builtin");
}

static const espclaw_builtin_board_t *builtin_board_for_variant(const char *variant_id)
{
    size_t index;

    if (variant_id == NULL || variant_id[0] == '\0') {
        return NULL;
    }

    for (index = 0; index < sizeof(BUILTIN_BOARDS) / sizeof(BUILTIN_BOARDS[0]); ++index) {
        if (strcmp(BUILTIN_BOARDS[index].variant_id, variant_id) == 0) {
            return &BUILTIN_BOARDS[index];
        }
    }

    return NULL;
}

static const espclaw_builtin_board_t *builtin_board_for_profile(espclaw_board_profile_id_t profile_id)
{
    switch (profile_id) {
    case ESPCLAW_BOARD_PROFILE_ESP32CAM:
        return builtin_board_for_variant("ai_thinker_esp32cam");
    case ESPCLAW_BOARD_PROFILE_ESP32S3:
    default:
        return builtin_board_for_variant("generic_esp32s3");
    }
}

static size_t builtin_board_count_for_profile(espclaw_board_profile_id_t profile_id)
{
    size_t index;
    size_t count = 0;

    for (index = 0; index < sizeof(BUILTIN_BOARDS) / sizeof(BUILTIN_BOARDS[0]); ++index) {
        if (BUILTIN_BOARDS[index].profile_id == profile_id) {
            count++;
        }
    }
    return count;
}

static const espclaw_builtin_board_t *builtin_board_at_profile_index(
    espclaw_board_profile_id_t profile_id,
    size_t index
)
{
    size_t builtin_index;
    size_t seen = 0;

    for (builtin_index = 0; builtin_index < sizeof(BUILTIN_BOARDS) / sizeof(BUILTIN_BOARDS[0]); ++builtin_index) {
        if (BUILTIN_BOARDS[builtin_index].profile_id != profile_id) {
            continue;
        }
        if (seen == index) {
            return &BUILTIN_BOARDS[builtin_index];
        }
        seen++;
    }
    return NULL;
}

static void fill_from_builtin(const espclaw_builtin_board_t *builtin, espclaw_board_descriptor_t *descriptor)
{
    size_t index;

    clear_descriptor(descriptor);
    if (builtin == NULL || descriptor == NULL) {
        return;
    }

    descriptor->profile_id = builtin->profile_id;
    copy_text(descriptor->variant_id, sizeof(descriptor->variant_id), builtin->variant_id);
    copy_text(descriptor->display_name, sizeof(descriptor->display_name), builtin->display_name);

    descriptor->pin_count = builtin->pin_count;
    for (index = 0; index < builtin->pin_count && index < ESPCLAW_BOARD_PIN_COUNT_MAX; ++index) {
        descriptor->pins[index] = builtin->pins[index];
    }

    descriptor->i2c_bus_count = builtin->i2c_bus_count;
    for (index = 0; index < builtin->i2c_bus_count && index < ESPCLAW_BOARD_I2C_BUS_COUNT_MAX; ++index) {
        descriptor->i2c_buses[index] = builtin->i2c_buses[index];
    }

    descriptor->uart_count = builtin->uart_count;
    for (index = 0; index < builtin->uart_count && index < ESPCLAW_BOARD_UART_COUNT_MAX; ++index) {
        descriptor->uarts[index] = builtin->uarts[index];
    }

    descriptor->adc_count = builtin->adc_count;
    for (index = 0; index < builtin->adc_count && index < ESPCLAW_BOARD_ADC_COUNT_MAX; ++index) {
        descriptor->adc_channels[index] = builtin->adc_channels[index];
    }
}

static const char *skip_ws(const char *cursor)
{
    while (cursor != NULL && *cursor != '\0' && isspace((unsigned char)*cursor)) {
        cursor++;
    }
    return cursor;
}

static bool extract_string_value(const char *json, const char *key, char *buffer, size_t buffer_size)
{
    char pattern[64];
    const char *cursor;
    size_t used = 0;

    if (json == NULL || key == NULL || buffer == NULL || buffer_size == 0) {
        return false;
    }

    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    cursor = strstr(json, pattern);
    if (cursor == NULL) {
        buffer[0] = '\0';
        return false;
    }
    cursor = strchr(cursor, ':');
    if (cursor == NULL) {
        buffer[0] = '\0';
        return false;
    }
    cursor = skip_ws(cursor + 1);
    if (cursor == NULL || *cursor != '"') {
        buffer[0] = '\0';
        return false;
    }
    cursor++;

    while (*cursor != '\0' && *cursor != '"' && used + 1 < buffer_size) {
        if (*cursor == '\\' && cursor[1] != '\0') {
            cursor++;
        }
        buffer[used++] = *cursor++;
    }

    buffer[used] = '\0';
    return *cursor == '"';
}

static bool extract_long_value(const char *json, const char *key, long *value_out)
{
    char pattern[64];
    const char *cursor;
    char *end_ptr = NULL;

    if (json == NULL || key == NULL || value_out == NULL) {
        return false;
    }

    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    cursor = strstr(json, pattern);
    if (cursor == NULL) {
        return false;
    }
    cursor = strchr(cursor, ':');
    if (cursor == NULL) {
        return false;
    }
    cursor = skip_ws(cursor + 1);
    if (cursor == NULL) {
        return false;
    }

    *value_out = strtol(cursor, &end_ptr, 10);
    return end_ptr != cursor;
}

static bool find_named_object(const char *json, const char *section, const char *name, const char **object_start_out, const char **object_end_out)
{
    char section_pattern[64];
    char name_pattern[64];
    const char *section_start;
    const char *object_start;
    const char *cursor;
    int depth = 0;

    if (json == NULL || section == NULL || name == NULL || object_start_out == NULL || object_end_out == NULL) {
        return false;
    }

    snprintf(section_pattern, sizeof(section_pattern), "\"%s\"", section);
    section_start = strstr(json, section_pattern);
    if (section_start == NULL) {
        return false;
    }
    section_start = strchr(section_start, '{');
    if (section_start == NULL) {
        return false;
    }

    snprintf(name_pattern, sizeof(name_pattern), "\"%s\"", name);
    cursor = strstr(section_start, name_pattern);
    if (cursor == NULL) {
        return false;
    }
    object_start = strchr(cursor, '{');
    if (object_start == NULL) {
        return false;
    }

    cursor = object_start;
    while (*cursor != '\0') {
        if (*cursor == '{') {
            depth++;
        } else if (*cursor == '}') {
            depth--;
            if (depth == 0) {
                *object_start_out = object_start;
                *object_end_out = cursor;
                return true;
            }
        }
        cursor++;
    }

    return false;
}

static bool find_section_object(const char *json, const char *section, const char **object_start_out, const char **object_end_out)
{
    char section_pattern[64];
    const char *section_start;
    const char *cursor;
    int depth = 0;

    if (json == NULL || section == NULL || object_start_out == NULL || object_end_out == NULL) {
        return false;
    }

    snprintf(section_pattern, sizeof(section_pattern), "\"%s\"", section);
    section_start = strstr(json, section_pattern);
    if (section_start == NULL) {
        return false;
    }
    section_start = strchr(section_start, '{');
    if (section_start == NULL) {
        return false;
    }

    cursor = section_start;
    while (*cursor != '\0') {
        if (*cursor == '{') {
            depth++;
        } else if (*cursor == '}') {
            depth--;
            if (depth == 0) {
                *object_start_out = section_start;
                *object_end_out = cursor;
                return true;
            }
        }
        cursor++;
    }

    return false;
}

static void apply_pin_override(espclaw_board_descriptor_t *descriptor, const char *name, long pin)
{
    size_t index;

    if (descriptor == NULL || name == NULL || pin < 0) {
        return;
    }

    for (index = 0; index < descriptor->pin_count; ++index) {
        if (strcmp(descriptor->pins[index].name, name) == 0) {
            descriptor->pins[index].pin = (int)pin;
            return;
        }
    }

    if (descriptor->pin_count < ESPCLAW_BOARD_PIN_COUNT_MAX) {
        copy_text(descriptor->pins[descriptor->pin_count].name, sizeof(descriptor->pins[descriptor->pin_count].name), name);
        descriptor->pins[descriptor->pin_count].pin = (int)pin;
        descriptor->pin_count++;
    }
}

static void apply_pin_overrides_from_json(const char *json, espclaw_board_descriptor_t *descriptor)
{
    const char *object_start = NULL;
    const char *object_end = NULL;
    const char *cursor;

    if (json == NULL || descriptor == NULL || !find_section_object(json, "pins", &object_start, &object_end)) {
        return;
    }

    cursor = object_start + 1;
    while (cursor < object_end) {
        char name[ESPCLAW_BOARD_ALIAS_MAX + 1];
        size_t name_length = 0;
        long value;
        char *end_ptr = NULL;

        cursor = strchr(cursor, '"');
        if (cursor == NULL || cursor >= object_end) {
            break;
        }
        cursor++;
        while (cursor < object_end && *cursor != '"' && name_length + 1 < sizeof(name)) {
            name[name_length++] = *cursor++;
        }
        if (cursor >= object_end || *cursor != '"') {
            break;
        }
        name[name_length] = '\0';
        cursor = strchr(cursor, ':');
        if (cursor == NULL || cursor >= object_end) {
            break;
        }
        cursor = skip_ws(cursor + 1);
        value = strtol(cursor, &end_ptr, 10);
        if (end_ptr != cursor) {
            apply_pin_override(descriptor, name, value);
        }
        cursor = end_ptr != NULL ? end_ptr : cursor + 1;
    }
}

static void apply_i2c_overrides_from_json(const char *json, espclaw_board_descriptor_t *descriptor)
{
    const char *object_start = NULL;
    const char *object_end = NULL;
    char object_buffer[256];
    size_t length;
    long value;

    if (json == NULL || descriptor == NULL || !find_named_object(json, "i2c", "default", &object_start, &object_end)) {
        return;
    }

    length = (size_t)(object_end - object_start + 1);
    if (length >= sizeof(object_buffer)) {
        length = sizeof(object_buffer) - 1;
    }
    memcpy(object_buffer, object_start, length);
    object_buffer[length] = '\0';

    if (descriptor->i2c_bus_count == 0 && descriptor->i2c_bus_count < ESPCLAW_BOARD_I2C_BUS_COUNT_MAX) {
        copy_text(descriptor->i2c_buses[0].name, sizeof(descriptor->i2c_buses[0].name), "default");
        descriptor->i2c_buses[0].port = 0;
        descriptor->i2c_buses[0].sda_pin = -1;
        descriptor->i2c_buses[0].scl_pin = -1;
        descriptor->i2c_buses[0].frequency_hz = 400000;
        descriptor->i2c_bus_count = 1;
    }
    if (descriptor->i2c_bus_count == 0) {
        return;
    }

    if (extract_long_value(object_buffer, "port", &value)) {
        descriptor->i2c_buses[0].port = (int)value;
    }
    if (extract_long_value(object_buffer, "sda", &value)) {
        descriptor->i2c_buses[0].sda_pin = (int)value;
    }
    if (extract_long_value(object_buffer, "scl", &value)) {
        descriptor->i2c_buses[0].scl_pin = (int)value;
    }
    if (extract_long_value(object_buffer, "frequency_hz", &value)) {
        descriptor->i2c_buses[0].frequency_hz = (int)value;
    }
}

static void apply_uart_overrides_from_json(const char *json, espclaw_board_descriptor_t *descriptor)
{
    const char *object_start = NULL;
    const char *object_end = NULL;
    char object_buffer[256];
    size_t length;
    long value;

    if (json == NULL || descriptor == NULL || !find_named_object(json, "uart", "console", &object_start, &object_end)) {
        return;
    }

    length = (size_t)(object_end - object_start + 1);
    if (length >= sizeof(object_buffer)) {
        length = sizeof(object_buffer) - 1;
    }
    memcpy(object_buffer, object_start, length);
    object_buffer[length] = '\0';

    if (descriptor->uart_count == 0 && descriptor->uart_count < ESPCLAW_BOARD_UART_COUNT_MAX) {
        copy_text(descriptor->uarts[0].name, sizeof(descriptor->uarts[0].name), "console");
        descriptor->uarts[0].port = 0;
        descriptor->uarts[0].tx_pin = -1;
        descriptor->uarts[0].rx_pin = -1;
        descriptor->uarts[0].baud_rate = 115200;
        descriptor->uart_count = 1;
    }
    if (descriptor->uart_count == 0) {
        return;
    }

    if (extract_long_value(object_buffer, "port", &value)) {
        descriptor->uarts[0].port = (int)value;
    }
    if (extract_long_value(object_buffer, "tx", &value)) {
        descriptor->uarts[0].tx_pin = (int)value;
    }
    if (extract_long_value(object_buffer, "rx", &value)) {
        descriptor->uarts[0].rx_pin = (int)value;
    }
    if (extract_long_value(object_buffer, "baud_rate", &value)) {
        descriptor->uarts[0].baud_rate = (int)value;
    }
}

static void apply_adc_overrides_from_json(const char *json, espclaw_board_descriptor_t *descriptor)
{
    const char *object_start = NULL;
    const char *object_end = NULL;
    char object_buffer[256];
    size_t length;
    long value;

    if (json == NULL || descriptor == NULL || !find_named_object(json, "adc", "battery", &object_start, &object_end)) {
        return;
    }

    length = (size_t)(object_end - object_start + 1);
    if (length >= sizeof(object_buffer)) {
        length = sizeof(object_buffer) - 1;
    }
    memcpy(object_buffer, object_start, length);
    object_buffer[length] = '\0';

    if (descriptor->adc_count == 0 && descriptor->adc_count < ESPCLAW_BOARD_ADC_COUNT_MAX) {
        copy_text(descriptor->adc_channels[0].name, sizeof(descriptor->adc_channels[0].name), "battery");
        descriptor->adc_channels[0].unit = 1;
        descriptor->adc_channels[0].channel = -1;
        descriptor->adc_count = 1;
    }
    if (descriptor->adc_count == 0) {
        return;
    }

    if (extract_long_value(object_buffer, "unit", &value)) {
        descriptor->adc_channels[0].unit = (int)value;
    }
    if (extract_long_value(object_buffer, "channel", &value)) {
        descriptor->adc_channels[0].channel = (int)value;
    }
}

size_t espclaw_board_preset_count(const espclaw_board_profile_t *profile)
{
    if (profile == NULL) {
        return 0;
    }
    return builtin_board_count_for_profile(profile->profile_id);
}

int espclaw_board_preset_at(
    const espclaw_board_profile_t *profile,
    size_t index,
    espclaw_board_descriptor_t *descriptor
)
{
    const espclaw_builtin_board_t *builtin;

    if (profile == NULL || descriptor == NULL) {
        return -1;
    }

    builtin = builtin_board_at_profile_index(profile->profile_id, index);
    if (builtin == NULL) {
        return -1;
    }

    fill_from_builtin(builtin, descriptor);
    return 0;
}

void espclaw_board_descriptor_default_for_profile(
    const espclaw_board_profile_t *profile,
    espclaw_board_descriptor_t *descriptor
)
{
    const espclaw_builtin_board_t *builtin =
        builtin_board_for_profile(profile != NULL ? profile->profile_id : ESPCLAW_BOARD_PROFILE_ESP32S3);

    fill_from_builtin(builtin, descriptor);
}

int espclaw_board_descriptor_load(
    const char *workspace_root,
    const espclaw_board_profile_t *profile,
    espclaw_board_descriptor_t *descriptor
)
{
    char variant[ESPCLAW_BOARD_VARIANT_MAX + 1];
    const espclaw_builtin_board_t *builtin = NULL;

    if (profile == NULL || descriptor == NULL) {
        return -1;
    }

    espclaw_board_descriptor_default_for_profile(profile, descriptor);
    if (workspace_root == NULL ||
        espclaw_workspace_read_file(
            workspace_root,
            "config/board.json",
            s_board_config_json,
            sizeof(s_board_config_json)
        ) != 0) {
        return 0;
    }

    if (extract_string_value(s_board_config_json, "variant", variant, sizeof(variant)) &&
        variant[0] != '\0' &&
        strcmp(variant, "auto") != 0) {
        builtin = builtin_board_for_variant(variant);
        if (builtin != NULL && builtin->profile_id == profile->profile_id) {
            fill_from_builtin(builtin, descriptor);
        }
    }

    apply_pin_overrides_from_json(s_board_config_json, descriptor);
    apply_i2c_overrides_from_json(s_board_config_json, descriptor);
    apply_uart_overrides_from_json(s_board_config_json, descriptor);
    apply_adc_overrides_from_json(s_board_config_json, descriptor);
    copy_text(descriptor->source, sizeof(descriptor->source), "workspace");
    return 0;
}

int espclaw_board_configure_current(
    const char *workspace_root,
    const espclaw_board_profile_t *profile
)
{
    if (profile == NULL) {
        return -1;
    }
    if (espclaw_board_descriptor_load(workspace_root, profile, &s_current_board) != 0) {
        espclaw_board_descriptor_default_for_profile(profile, &s_current_board);
        return -1;
    }
    return 0;
}

const espclaw_board_descriptor_t *espclaw_board_current(void)
{
    if (s_current_board.variant_id[0] == '\0') {
        return NULL;
    }
    return &s_current_board;
}

int espclaw_board_resolve_pin_alias(const char *name, int *pin_out)
{
    size_t index;

    if (name == NULL || pin_out == NULL) {
        return -1;
    }

    for (index = 0; index < s_current_board.pin_count; ++index) {
        if (strcmp(s_current_board.pins[index].name, name) == 0) {
            *pin_out = s_current_board.pins[index].pin;
            return 0;
        }
    }
    return -1;
}

int espclaw_board_find_i2c_bus(const char *name, espclaw_board_i2c_bus_t *bus_out)
{
    size_t index;

    if (name == NULL || bus_out == NULL) {
        return -1;
    }

    for (index = 0; index < s_current_board.i2c_bus_count; ++index) {
        if (strcmp(s_current_board.i2c_buses[index].name, name) == 0) {
            *bus_out = s_current_board.i2c_buses[index];
            return 0;
        }
    }
    return -1;
}

int espclaw_board_find_uart(const char *name, espclaw_board_uart_t *uart_out)
{
    size_t index;

    if (name == NULL || uart_out == NULL) {
        return -1;
    }

    for (index = 0; index < s_current_board.uart_count; ++index) {
        if (strcmp(s_current_board.uarts[index].name, name) == 0) {
            *uart_out = s_current_board.uarts[index];
            return 0;
        }
    }
    return -1;
}

int espclaw_board_find_adc_channel(const char *name, espclaw_board_adc_channel_t *channel_out)
{
    size_t index;

    if (name == NULL || channel_out == NULL) {
        return -1;
    }

    for (index = 0; index < s_current_board.adc_count; ++index) {
        if (strcmp(s_current_board.adc_channels[index].name, name) == 0) {
            *channel_out = s_current_board.adc_channels[index];
            return 0;
        }
    }
    return -1;
}

size_t espclaw_board_render_minimal_config_json(
    const espclaw_board_descriptor_t *descriptor,
    char *buffer,
    size_t buffer_size
)
{
    if (buffer == NULL || buffer_size == 0) {
        return 0;
    }

    return (size_t)snprintf(
        buffer,
        buffer_size,
        "{\n"
        "  \"variant\": \"%s\"\n"
        "}\n",
        descriptor != NULL && descriptor->variant_id[0] != '\0' ? descriptor->variant_id : "auto"
    );
}

int espclaw_board_write_variant_config(const char *workspace_root, const char *variant_id)
{
    char json[128];
    const espclaw_builtin_board_t *builtin;

    if (workspace_root == NULL || variant_id == NULL || variant_id[0] == '\0') {
        return -1;
    }

    builtin = builtin_board_for_variant(variant_id);
    if (builtin == NULL) {
        return -1;
    }

    snprintf(json, sizeof(json), "{\n  \"variant\": \"%s\"\n}\n", builtin->variant_id);
    return espclaw_workspace_write_file(workspace_root, "config/board.json", json);
}
