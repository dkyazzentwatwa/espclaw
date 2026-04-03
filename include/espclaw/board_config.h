#ifndef ESPCLAW_BOARD_CONFIG_H
#define ESPCLAW_BOARD_CONFIG_H

#include <stddef.h>

#include "espclaw/board_profile.h"

#define ESPCLAW_BOARD_VARIANT_MAX 32
#define ESPCLAW_BOARD_LABEL_MAX 64
#define ESPCLAW_BOARD_ALIAS_MAX 24
#define ESPCLAW_BOARD_PIN_COUNT_MAX 32
#define ESPCLAW_BOARD_I2C_BUS_COUNT_MAX 4
#define ESPCLAW_BOARD_UART_COUNT_MAX 4
#define ESPCLAW_BOARD_ADC_COUNT_MAX 8

typedef struct {
    char name[ESPCLAW_BOARD_ALIAS_MAX + 1];
    int pin;
} espclaw_board_pin_alias_t;

typedef struct {
    char name[ESPCLAW_BOARD_ALIAS_MAX + 1];
    int port;
    int sda_pin;
    int scl_pin;
    int frequency_hz;
} espclaw_board_i2c_bus_t;

typedef struct {
    char name[ESPCLAW_BOARD_ALIAS_MAX + 1];
    int port;
    int tx_pin;
    int rx_pin;
    int baud_rate;
} espclaw_board_uart_t;

typedef struct {
    char name[ESPCLAW_BOARD_ALIAS_MAX + 1];
    int unit;
    int channel;
} espclaw_board_adc_channel_t;

typedef struct {
    char variant_id[ESPCLAW_BOARD_VARIANT_MAX + 1];
    char display_name[ESPCLAW_BOARD_LABEL_MAX + 1];
    char source[16];
    espclaw_board_profile_id_t profile_id;
    size_t pin_count;
    size_t i2c_bus_count;
    size_t uart_count;
    size_t adc_count;
    espclaw_board_pin_alias_t pins[ESPCLAW_BOARD_PIN_COUNT_MAX];
    espclaw_board_i2c_bus_t i2c_buses[ESPCLAW_BOARD_I2C_BUS_COUNT_MAX];
    espclaw_board_uart_t uarts[ESPCLAW_BOARD_UART_COUNT_MAX];
    espclaw_board_adc_channel_t adc_channels[ESPCLAW_BOARD_ADC_COUNT_MAX];
} espclaw_board_descriptor_t;

size_t espclaw_board_preset_count(const espclaw_board_profile_t *profile);
int espclaw_board_preset_at(
    const espclaw_board_profile_t *profile,
    size_t index,
    espclaw_board_descriptor_t *descriptor
);
void espclaw_board_descriptor_default_for_profile(
    const espclaw_board_profile_t *profile,
    espclaw_board_descriptor_t *descriptor
);
int espclaw_board_descriptor_load(
    const char *workspace_root,
    const espclaw_board_profile_t *profile,
    espclaw_board_descriptor_t *descriptor
);
int espclaw_board_configure_current(
    const char *workspace_root,
    const espclaw_board_profile_t *profile
);
const espclaw_board_descriptor_t *espclaw_board_current(void);
int espclaw_board_resolve_pin_alias(const char *name, int *pin_out);
int espclaw_board_find_i2c_bus(const char *name, espclaw_board_i2c_bus_t *bus_out);
int espclaw_board_find_uart(const char *name, espclaw_board_uart_t *uart_out);
int espclaw_board_find_adc_channel(const char *name, espclaw_board_adc_channel_t *channel_out);
size_t espclaw_board_render_minimal_config_json(
    const espclaw_board_descriptor_t *descriptor,
    char *buffer,
    size_t buffer_size
);
int espclaw_board_write_variant_config(const char *workspace_root, const char *variant_id);

#endif
