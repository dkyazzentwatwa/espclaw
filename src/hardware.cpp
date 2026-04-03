#include "espclaw/hardware.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_camera.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/ledc.h"
#include "driver/rmt.h"
#include "driver/uart.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#else
#include <fcntl.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include "espclaw/board_config.h"
#include "espclaw/board_profile.h"
#include "espclaw/workspace.h"

#define ESPCLAW_HW_GPIO_MAX 64
#define ESPCLAW_HW_ADC_UNIT_MAX 2
#define ESPCLAW_HW_ADC_CHANNEL_MAX 10
#define ESPCLAW_HW_DEFAULT_ADC_ATTEN ADC_ATTEN_DB_12
#define ESPCLAW_HW_DEFAULT_ADC_BITS ADC_BITWIDTH_DEFAULT
#define ESPCLAW_HW_DEFAULT_ADC_MAX_RAW 4095
#define ESPCLAW_HW_DEFAULT_ADC_MAX_MV 3300
#define ESPCLAW_HW_UART_BUFFER_MAX 2048
#define ESPCLAW_HW_TMP102_TEMP_REG 0x00
#define ESPCLAW_HW_MPU6050_TEMP_REG 0x3B
#define ESPCLAW_HW_MPU6050_PWR_MGMT_1 0x6B
#define ESPCLAW_HW_MPU6050_CONFIG 0x1A
#define ESPCLAW_HW_MPU6050_GYRO_CONFIG 0x1B
#define ESPCLAW_HW_MPU6050_ACCEL_CONFIG 0x1C
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
typedef struct {
    bool initialized;
    int sda_pin;
    int scl_pin;
    int frequency_hz;
} espclaw_hw_i2c_state_t;

typedef struct {
    size_t rx_length;
    size_t tx_length;
    size_t event_length;
#ifdef ESP_PLATFORM
    bool configured;
#else
    bool stdio_attached;
#endif
    uint8_t rx_buffer[ESPCLAW_HW_UART_BUFFER_MAX];
    uint8_t tx_buffer[ESPCLAW_HW_UART_BUFFER_MAX];
    uint8_t event_buffer[ESPCLAW_HW_UART_BUFFER_MAX];
} espclaw_hw_uart_state_t;

#ifndef ESP_PLATFORM
typedef struct {
    bool present;
    uint8_t value[ESPCLAW_HW_I2C_REGISTER_BYTES];
} espclaw_hw_i2c_device_t;
#endif

static espclaw_hw_pwm_state_t s_pwm_state[ESPCLAW_HW_PWM_CHANNEL_MAX];
static espclaw_hw_ppm_state_t s_ppm_state[ESPCLAW_HW_PPM_CHANNEL_MAX];
static espclaw_hw_i2c_state_t s_i2c_state[ESPCLAW_HW_I2C_PORT_MAX];
static espclaw_hw_uart_state_t s_uart_state[ESPCLAW_HW_UART_PORT_MAX];
static bool s_camera_initialized;

#ifdef ESP_PLATFORM
static const char *TAG = "espclaw_hw";
static adc_oneshot_unit_handle_t s_adc_units[ESPCLAW_HW_ADC_UNIT_MAX];
static bool s_adc_units_ready[ESPCLAW_HW_ADC_UNIT_MAX];
static bool s_adc_channel_configured[ESPCLAW_HW_ADC_UNIT_MAX][ESPCLAW_HW_ADC_CHANNEL_MAX];
static adc_cali_handle_t s_adc_cali[ESPCLAW_HW_ADC_UNIT_MAX][ESPCLAW_HW_ADC_CHANNEL_MAX];
static bool s_adc_cali_ready[ESPCLAW_HW_ADC_UNIT_MAX][ESPCLAW_HW_ADC_CHANNEL_MAX];
#else
static int s_gpio_level[ESPCLAW_HW_GPIO_MAX];
static int s_adc_raw[ESPCLAW_HW_ADC_UNIT_MAX][ESPCLAW_HW_ADC_CHANNEL_MAX];
static espclaw_hw_i2c_device_t s_i2c_devices[ESPCLAW_HW_I2C_PORT_MAX][ESPCLAW_HW_I2C_DEVICE_MAX];
static bool s_host_stdio_nonblocking_ready;
#endif
static bool s_camera_last_capture_ok;
static bool s_camera_last_simulated;
static size_t s_camera_last_width;
static size_t s_camera_last_height;
static size_t s_camera_last_bytes_written;
static char s_camera_last_relative_path[ESPCLAW_HW_CAMERA_PATH_MAX];
static char s_camera_last_error[ESPCLAW_HW_CAMERA_ERROR_MAX];

#ifndef ESP_PLATFORM
static const uint8_t ESPCLAW_SIM_CAMERA_JPEG[] = {
    0xff, 0xd8, 0xff, 0xdb, 0x00, 0x43, 0x00, 0x03, 0x02, 0x02, 0x02, 0x02, 0x02, 0x03, 0x02, 0x02,
    0x02, 0x03, 0x03, 0x03, 0x03, 0x04, 0x06, 0x04, 0x04, 0x04, 0x04, 0x04, 0x08, 0x06, 0x06, 0x05,
    0x06, 0x09, 0x08, 0x0a, 0x0a, 0x09, 0x08, 0x09, 0x09, 0x0a, 0x0c, 0x0f, 0x0c, 0x0a, 0x0b, 0x0e,
    0x0b, 0x09, 0x09, 0x0d, 0x11, 0x0d, 0x0e, 0x0f, 0x10, 0x10, 0x11, 0x10, 0x0a, 0x0c, 0x12, 0x13,
    0x12, 0x10, 0x13, 0x0f, 0x10, 0x10, 0x10, 0xff, 0xc0, 0x00, 0x11, 0x08, 0x00, 0x01, 0x00, 0x01,
    0x03, 0x01, 0x11, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11, 0x01, 0xff, 0xc4, 0x00, 0x14, 0x00, 0x01,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff,
    0xc4, 0x00, 0x14, 0x10, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xff, 0xda, 0x00, 0x0c, 0x03, 0x01, 0x00, 0x02, 0x11, 0x03, 0x11, 0x00, 0x3f, 0x00,
    0xff, 0xd9,
};
#endif

static bool pwm_channel_valid(int channel)
{
    return channel >= 0 && channel < ESPCLAW_HW_PWM_CHANNEL_MAX;
}

static bool ppm_channel_valid(int channel)
{
    return channel >= 0 && channel < ESPCLAW_HW_PPM_CHANNEL_MAX;
}

static bool gpio_pin_valid(int pin)
{
    return pin >= 0 && pin < ESPCLAW_HW_GPIO_MAX;
}

static bool i2c_port_valid(int port)
{
    return port >= 0 && port < ESPCLAW_HW_I2C_PORT_MAX;
}

static bool uart_port_valid(int port)
{
    return port >= 0 && port < ESPCLAW_HW_UART_PORT_MAX;
}

static void camera_reset_last_status(void)
{
    s_camera_last_capture_ok = false;
    s_camera_last_simulated = false;
    s_camera_last_width = 0U;
    s_camera_last_height = 0U;
    s_camera_last_bytes_written = 0U;
    s_camera_last_relative_path[0] = '\0';
    s_camera_last_error[0] = '\0';
}

static void camera_set_error(espclaw_hw_camera_capture_t *capture_out, const char *message)
{
    const char *value = message != NULL ? message : "camera capture failed";

    s_camera_last_capture_ok = false;
    s_camera_last_width = 0U;
    s_camera_last_height = 0U;
    s_camera_last_bytes_written = 0U;
    s_camera_last_relative_path[0] = '\0';
    snprintf(s_camera_last_error, sizeof(s_camera_last_error), "%s", value);
    if (capture_out != NULL) {
        snprintf(capture_out->error, sizeof(capture_out->error), "%s", value);
    }
}

static void camera_set_success(const espclaw_hw_camera_capture_t *capture)
{
    if (capture == NULL) {
        return;
    }

    s_camera_last_capture_ok = true;
    s_camera_last_simulated = capture->simulated;
    s_camera_last_width = capture->width;
    s_camera_last_height = capture->height;
    s_camera_last_bytes_written = capture->bytes_written;
    snprintf(s_camera_last_relative_path, sizeof(s_camera_last_relative_path), "%s", capture->relative_path);
    s_camera_last_error[0] = '\0';
}

static void camera_reset_runtime(void)
{
#ifdef ESP_PLATFORM
    if (s_camera_initialized) {
        esp_camera_deinit();
    }
#endif
    s_camera_initialized = false;
}

static const char *normalize_capture_filename(const char *filename)
{
    const char *normalized = filename != NULL ? filename : "";

    while (*normalized == '/') {
        normalized++;
    }
    if (strncmp(normalized, "media/", 6) == 0) {
        normalized += 6;
    }
    return normalized;
}

int espclaw_hw_apply_board_boot_defaults(void)
{
    const espclaw_board_descriptor_t *board = espclaw_board_current();
    int flash_led_pin;

    if (board == NULL) {
        return 0;
    }

    /* AI Thinker ESP32-CAM exposes the camera flash LED on GPIO4. Drive it low
       explicitly so board bring-up and SD probing do not leave it lit. */
    if (espclaw_board_resolve_pin_alias("flash_led", &flash_led_pin) == 0) {
        return espclaw_hw_gpio_write(flash_led_pin, 0);
    }

    return 0;
}

static size_t uart_buffer_append(uint8_t *buffer, size_t *length, const uint8_t *data, size_t data_length)
{
    size_t writable;

    if (buffer == NULL || length == NULL || data == NULL || data_length == 0) {
        return 0;
    }

    writable = ESPCLAW_HW_UART_BUFFER_MAX - *length;
    if (data_length < writable) {
        writable = data_length;
    }
    if (writable == 0) {
        return 0;
    }

    memcpy(buffer + *length, data, writable);
    *length += writable;
    return writable;
}

static size_t uart_buffer_take(uint8_t *buffer, size_t *length, uint8_t *data_out, size_t max_length)
{
    size_t readable;

    if (buffer == NULL || length == NULL || data_out == NULL || max_length == 0) {
        return 0;
    }

    readable = *length;
    if (readable > max_length) {
        readable = max_length;
    }
    if (readable == 0) {
        return 0;
    }

    memcpy(data_out, buffer, readable);
    if (*length > readable) {
        memmove(buffer, buffer + readable, *length - readable);
    }
    *length -= readable;
    return readable;
}

static int pwm_period_us(const espclaw_hw_pwm_state_t *state)
{
    if (state == NULL || state->frequency_hz <= 0) {
        return -1;
    }

    return 1000000 / state->frequency_hz;
}

static int pwm_duty_to_us(const espclaw_hw_pwm_state_t *state, int duty)
{
    int period_us;
    int max_duty;

    if (state == NULL || !state->configured) {
        return 0;
    }

    period_us = pwm_period_us(state);
    max_duty = (1 << state->resolution_bits) - 1;
    if (period_us <= 0 || max_duty <= 0) {
        return 0;
    }

    return (int)(((long long)duty * period_us + (max_duty / 2)) / max_duty);
}

static double clamp_double(double value, double minimum, double maximum)
{
    if (value < minimum) {
        return minimum;
    }
    if (value > maximum) {
        return maximum;
    }
    return value;
}

static int16_t read_be_i16(const uint8_t *data)
{
    return (int16_t)(((uint16_t)data[0] << 8) | data[1]);
}

#ifdef ESP_PLATFORM
static ledc_timer_t pwm_timer_for_channel(int channel)
{
    return (ledc_timer_t)channel;
}

static ledc_channel_t pwm_channel_to_ledc(int channel)
{
    return (ledc_channel_t)channel;
}

static ledc_mode_t pwm_mode(void)
{
    return LEDC_LOW_SPEED_MODE;
}

static rmt_channel_t ppm_channel_to_rmt(int channel)
{
    return channel == 0 ? RMT_CHANNEL_0 : RMT_CHANNEL_1;
}

static int ensure_adc_unit(int unit)
{
    adc_oneshot_unit_init_cfg_t cfg = {0};
    int index = unit - 1;

    if (index < 0 || index >= ESPCLAW_HW_ADC_UNIT_MAX) {
        return -1;
    }
    if (s_adc_units_ready[index]) {
        return 0;
    }

    cfg.unit_id = unit == 1 ? ADC_UNIT_1 : ADC_UNIT_2;
    if (adc_oneshot_new_unit(&cfg, &s_adc_units[index]) != ESP_OK) {
        return -1;
    }
    s_adc_units_ready[index] = true;
    return 0;
}

static int ensure_adc_channel(int unit, int channel)
{
    int index = unit - 1;
    adc_oneshot_chan_cfg_t cfg = {
        .atten = ESPCLAW_HW_DEFAULT_ADC_ATTEN,
        .bitwidth = ESPCLAW_HW_DEFAULT_ADC_BITS,
    };

    if (ensure_adc_unit(unit) != 0) {
        return -1;
    }
    if (!s_adc_channel_configured[index][channel]) {
        if (adc_oneshot_config_channel(s_adc_units[index], (adc_channel_t)channel, &cfg) != ESP_OK) {
            return -1;
        }
        s_adc_channel_configured[index][channel] = true;
    }
    if (!s_adc_cali_ready[index][channel]) {
#ifdef ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
        adc_cali_curve_fitting_config_t cali_cfg = {
            .unit_id = unit == 1 ? ADC_UNIT_1 : ADC_UNIT_2,
            .chan = (adc_channel_t)channel,
            .atten = ESPCLAW_HW_DEFAULT_ADC_ATTEN,
            .bitwidth = ESPCLAW_HW_DEFAULT_ADC_BITS,
        };

        if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_adc_cali[index][channel]) == ESP_OK) {
            s_adc_cali_ready[index][channel] = true;
        }
#elif defined(ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED)
        adc_cali_line_fitting_config_t cali_cfg = {
            .unit_id = unit == 1 ? ADC_UNIT_1 : ADC_UNIT_2,
            .atten = ESPCLAW_HW_DEFAULT_ADC_ATTEN,
            .bitwidth = ESPCLAW_HW_DEFAULT_ADC_BITS,
            .default_vref = 0,
        };

        if (adc_cali_create_scheme_line_fitting(&cali_cfg, &s_adc_cali[index][channel]) == ESP_OK) {
            s_adc_cali_ready[index][channel] = true;
        }
#endif
    }

    return 0;
}

static int ensure_uart_port(int port)
{
    uart_config_t config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    if (!uart_port_valid(port) || port != 0) {
        return -1;
    }
    if (s_uart_state[port].configured) {
        return 0;
    }
    if (uart_driver_install((uart_port_t)port, 1024, 1024, 0, NULL, 0) != ESP_OK) {
        return -1;
    }
    if (uart_param_config((uart_port_t)port, &config) != ESP_OK ||
        uart_set_pin((uart_port_t)port, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
        uart_driver_delete((uart_port_t)port);
        return -1;
    }

    s_uart_state[port].configured = true;
    return 0;
}

static void poll_uart_input(int port)
{
    uint8_t chunk[128];
    int bytes_read;

    if (!uart_port_valid(port) || !s_uart_state[port].configured) {
        return;
    }

    while (s_uart_state[port].rx_length < ESPCLAW_HW_UART_BUFFER_MAX ||
           s_uart_state[port].event_length < ESPCLAW_HW_UART_BUFFER_MAX) {
        bytes_read = uart_read_bytes((uart_port_t)port, chunk, sizeof(chunk), 0);
        if (bytes_read <= 0) {
            break;
        }
        (void)uart_buffer_append(s_uart_state[port].rx_buffer, &s_uart_state[port].rx_length, chunk, (size_t)bytes_read);
        (void)uart_buffer_append(s_uart_state[port].event_buffer, &s_uart_state[port].event_length, chunk, (size_t)bytes_read);
    }
}
#else
static void ensure_host_stdio_nonblocking(void)
{
    int flags;

    if (s_host_stdio_nonblocking_ready) {
        return;
    }

    flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags >= 0) {
        (void)fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    }
    s_host_stdio_nonblocking_ready = true;
}

static void host_uart_ingest_stdio(int port)
{
    uint8_t chunk[256];

    if (!uart_port_valid(port) || port != 0) {
        return;
    }

    ensure_host_stdio_nonblocking();
    while (s_uart_state[port].rx_length < ESPCLAW_HW_UART_BUFFER_MAX) {
        ssize_t bytes_read = read(STDIN_FILENO, chunk, sizeof(chunk));

        if (bytes_read > 0) {
            s_uart_state[port].stdio_attached = true;
            if (uart_buffer_append(s_uart_state[port].rx_buffer, &s_uart_state[port].rx_length, chunk, (size_t)bytes_read) < (size_t)bytes_read) {
                break;
            }
            (void)uart_buffer_append(s_uart_state[port].event_buffer, &s_uart_state[port].event_length, chunk, (size_t)bytes_read);
            continue;
        }
        if (bytes_read == 0 || errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        break;
    }
}
#endif

int espclaw_hw_gpio_write(int pin, int level)
{
    if (!gpio_pin_valid(pin)) {
        return -1;
    }

#ifdef ESP_PLATFORM
    gpio_config_t config = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    if (gpio_config(&config) != ESP_OK || gpio_set_level((gpio_num_t)pin, level != 0 ? 1 : 0) != ESP_OK) {
        return -1;
    }
#else
    s_gpio_level[pin] = level != 0 ? 1 : 0;
#endif

    return 0;
}

int espclaw_hw_gpio_read(int pin, int *level_out)
{
    if (!gpio_pin_valid(pin) || level_out == NULL) {
        return -1;
    }

#ifdef ESP_PLATFORM
    gpio_config_t config = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    if (gpio_config(&config) != ESP_OK) {
        return -1;
    }
    *level_out = gpio_get_level((gpio_num_t)pin);
#else
    *level_out = s_gpio_level[pin];
#endif

    return 0;
}

int espclaw_hw_pwm_setup(int channel, int pin, int frequency_hz, int resolution_bits)
{
    if (!pwm_channel_valid(channel) || !gpio_pin_valid(pin) || frequency_hz <= 0 || resolution_bits < 1 || resolution_bits > 15) {
        return -1;
    }

#ifdef ESP_PLATFORM
    ledc_timer_config_t timer_config = {
        .speed_mode = pwm_mode(),
        .timer_num = pwm_timer_for_channel(channel),
        .duty_resolution = (ledc_timer_bit_t)resolution_bits,
        .freq_hz = frequency_hz,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_channel_config_t channel_config = {
        .gpio_num = pin,
        .speed_mode = pwm_mode(),
        .channel = pwm_channel_to_ledc(channel),
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = pwm_timer_for_channel(channel),
        .duty = 0,
        .hpoint = 0,
    };

    if (ledc_timer_config(&timer_config) != ESP_OK || ledc_channel_config(&channel_config) != ESP_OK) {
        return -1;
    }
#endif

    s_pwm_state[channel].configured = true;
    s_pwm_state[channel].pin = pin;
    s_pwm_state[channel].frequency_hz = frequency_hz;
    s_pwm_state[channel].resolution_bits = resolution_bits;
    s_pwm_state[channel].duty = 0;
    s_pwm_state[channel].pulse_width_us = 0;
    return 0;
}

int espclaw_hw_pwm_write(int channel, int duty)
{
    int max_duty;

    if (!pwm_channel_valid(channel) || !s_pwm_state[channel].configured) {
        return -1;
    }

    max_duty = (1 << s_pwm_state[channel].resolution_bits) - 1;
    if (duty < 0 || duty > max_duty) {
        return -1;
    }

#ifdef ESP_PLATFORM
    if (ledc_set_duty(pwm_mode(), pwm_channel_to_ledc(channel), duty) != ESP_OK ||
        ledc_update_duty(pwm_mode(), pwm_channel_to_ledc(channel)) != ESP_OK) {
        return -1;
    }
#endif

    s_pwm_state[channel].duty = duty;
    s_pwm_state[channel].pulse_width_us = pwm_duty_to_us(&s_pwm_state[channel], duty);
    return 0;
}

int espclaw_hw_pwm_write_us(int channel, int pulse_width_us)
{
    int period_us;
    int max_duty;
    int duty;

    if (!pwm_channel_valid(channel) || !s_pwm_state[channel].configured || pulse_width_us < 0) {
        return -1;
    }

    period_us = pwm_period_us(&s_pwm_state[channel]);
    max_duty = (1 << s_pwm_state[channel].resolution_bits) - 1;
    if (period_us <= 0 || max_duty <= 0 || pulse_width_us > period_us) {
        return -1;
    }

    duty = (int)(((long long)pulse_width_us * max_duty + (period_us / 2)) / period_us);
    if (espclaw_hw_pwm_write(channel, duty) != 0) {
        return -1;
    }

    s_pwm_state[channel].pulse_width_us = pulse_width_us;
    return 0;
}

int espclaw_hw_pwm_stop(int channel)
{
    if (!pwm_channel_valid(channel) || !s_pwm_state[channel].configured) {
        return -1;
    }

#ifdef ESP_PLATFORM
    if (ledc_stop(pwm_mode(), pwm_channel_to_ledc(channel), 0) != ESP_OK) {
        return -1;
    }
#endif

    s_pwm_state[channel].configured = false;
    s_pwm_state[channel].duty = 0;
    s_pwm_state[channel].pulse_width_us = 0;
    return 0;
}

int espclaw_hw_pwm_state(int channel, espclaw_hw_pwm_state_t *state_out)
{
    if (!pwm_channel_valid(channel) || state_out == NULL) {
        return -1;
    }

    *state_out = s_pwm_state[channel];
    return 0;
}

int espclaw_hw_buzzer_tone(int channel, int pin, int frequency_hz, int duration_ms, int duty_percent)
{
    int duty;
    int max_duty;

    if (duration_ms < 0 || duty_percent < 0 || duty_percent > 100) {
        return -1;
    }
    if (espclaw_hw_pwm_setup(channel, pin, frequency_hz, 10) != 0) {
        return -1;
    }

    max_duty = (1 << 10) - 1;
    duty = (max_duty * duty_percent) / 100;
    if (espclaw_hw_pwm_write(channel, duty) != 0) {
        return -1;
    }

    if (duration_ms > 0) {
        espclaw_hw_sleep_ms((uint32_t)duration_ms);
        return espclaw_hw_pwm_stop(channel);
    }

    return 0;
}

int espclaw_hw_adc_read_raw(int unit, int channel, int *raw_out)
{
    if (raw_out == NULL || channel < 0 || channel >= ESPCLAW_HW_ADC_CHANNEL_MAX || unit < 1 || unit > ESPCLAW_HW_ADC_UNIT_MAX) {
        return -1;
    }

#ifdef ESP_PLATFORM
    int raw = 0;

    if (ensure_adc_channel(unit, channel) != 0) {
        return -1;
    }
    if (adc_oneshot_read(s_adc_units[unit - 1], (adc_channel_t)channel, &raw) != ESP_OK) {
        return -1;
    }
    *raw_out = raw;
#else
    *raw_out = s_adc_raw[unit - 1][channel];
#endif

    return 0;
}

int espclaw_hw_adc_read_mv(int unit, int channel, int *millivolts_out)
{
    int raw = 0;

    if (millivolts_out == NULL || espclaw_hw_adc_read_raw(unit, channel, &raw) != 0) {
        return -1;
    }

#ifdef ESP_PLATFORM
    if (s_adc_cali_ready[unit - 1][channel]) {
        int millivolts = 0;

        if (adc_cali_raw_to_voltage(s_adc_cali[unit - 1][channel], raw, &millivolts) == ESP_OK) {
            *millivolts_out = millivolts;
            return 0;
        }
    }
#endif

    *millivolts_out = (raw * ESPCLAW_HW_DEFAULT_ADC_MAX_MV) / ESPCLAW_HW_DEFAULT_ADC_MAX_RAW;
    return 0;
}

int espclaw_hw_uart_read(int port, uint8_t *data, size_t max_length, size_t *length_out)
{
    size_t length = 0;

    if (!uart_port_valid(port) || data == NULL || length_out == NULL || max_length == 0) {
        return -1;
    }

#ifdef ESP_PLATFORM
    if (ensure_uart_port(port) != 0) {
        return -1;
    }
    poll_uart_input(port);
    length = uart_buffer_take(s_uart_state[port].rx_buffer, &s_uart_state[port].rx_length, data, max_length);
#else
    host_uart_ingest_stdio(port);
    length = uart_buffer_take(s_uart_state[port].rx_buffer, &s_uart_state[port].rx_length, data, max_length);
#endif

    *length_out = length;
    return 0;
}

int espclaw_hw_uart_write(int port, const uint8_t *data, size_t length, size_t *written_out)
{
    if (!uart_port_valid(port) || data == NULL || written_out == NULL) {
        return -1;
    }

#ifdef ESP_PLATFORM
    if (ensure_uart_port(port) != 0) {
        return -1;
    }
    if (uart_write_bytes((uart_port_t)port, (const char *)data, length) < 0) {
        return -1;
    }
#else
    if (port == 0 && length > 0) {
        (void)fwrite(data, 1, length, stdout);
        fflush(stdout);
        s_uart_state[port].stdio_attached = true;
    }
#endif

    (void)uart_buffer_append(s_uart_state[port].tx_buffer, &s_uart_state[port].tx_length, data, length);
    *written_out = length;
    return 0;
}

int espclaw_hw_uart_take_event_data(int port, uint8_t *data, size_t max_length, size_t *length_out)
{
    if (!uart_port_valid(port) || data == NULL || length_out == NULL || max_length == 0) {
        return -1;
    }

#ifdef ESP_PLATFORM
    if (ensure_uart_port(port) != 0) {
        return -1;
    }
    poll_uart_input(port);
#else
    host_uart_ingest_stdio(port);
#endif

    *length_out = uart_buffer_take(s_uart_state[port].event_buffer, &s_uart_state[port].event_length, data, max_length);
    return 0;
}

int espclaw_hw_i2c_begin(int port, int sda_pin, int scl_pin, int frequency_hz)
{
    if (!i2c_port_valid(port) || !gpio_pin_valid(sda_pin) || !gpio_pin_valid(scl_pin) || frequency_hz <= 0) {
        return -1;
    }

#ifdef ESP_PLATFORM
    i2c_config_t cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = sda_pin,
        .scl_io_num = scl_pin,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = (uint32_t)frequency_hz,
        .clk_flags = 0,
    };

    if (s_i2c_state[port].initialized) {
        if (s_i2c_state[port].sda_pin == sda_pin &&
            s_i2c_state[port].scl_pin == scl_pin &&
            s_i2c_state[port].frequency_hz == frequency_hz) {
            return 0;
        }
        i2c_driver_delete((i2c_port_t)port);
    }

    if (i2c_param_config((i2c_port_t)port, &cfg) != ESP_OK ||
        i2c_driver_install((i2c_port_t)port, cfg.mode, 0, 0, 0) != ESP_OK) {
        return -1;
    }
#endif

    s_i2c_state[port].initialized = true;
    s_i2c_state[port].sda_pin = sda_pin;
    s_i2c_state[port].scl_pin = scl_pin;
    s_i2c_state[port].frequency_hz = frequency_hz;
    return 0;
}

int espclaw_hw_i2c_scan(int port, uint8_t *addresses, size_t max_addresses, size_t *count_out)
{
    size_t count = 0;

    if (!i2c_port_valid(port) || addresses == NULL || count_out == NULL) {
        return -1;
    }

#ifdef ESP_PLATFORM
    uint8_t address;

    if (!s_i2c_state[port].initialized) {
        return -1;
    }

    for (address = 1; address < 127 && count < max_addresses; ++address) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        esp_err_t result;

        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (address << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        result = i2c_master_cmd_begin((i2c_port_t)port, cmd, pdMS_TO_TICKS(20));
        i2c_cmd_link_delete(cmd);
        if (result == ESP_OK) {
            addresses[count++] = address;
        }
    }
#else
    int address;

    for (address = 1; address < ESPCLAW_HW_I2C_DEVICE_MAX && count < max_addresses; ++address) {
        if (s_i2c_devices[port][address].present) {
            addresses[count++] = (uint8_t)address;
        }
    }
#endif

    *count_out = count;
    return 0;
}

int espclaw_hw_i2c_read_reg(int port, uint8_t address, uint8_t reg, uint8_t *data, size_t length)
{
    if (!i2c_port_valid(port) || data == NULL || length == 0 || length > ESPCLAW_HW_I2C_REGISTER_BYTES) {
        return -1;
    }

#ifdef ESP_PLATFORM
    if (!s_i2c_state[port].initialized) {
        return -1;
    }
    if (i2c_master_write_read_device((i2c_port_t)port, address, &reg, 1, data, length, pdMS_TO_TICKS(50)) != ESP_OK) {
        return -1;
    }
#else
    if (!s_i2c_devices[port][address].present || reg + length > ESPCLAW_HW_I2C_REGISTER_BYTES) {
        return -1;
    }
    memcpy(data, &s_i2c_devices[port][address].value[reg], length);
#endif

    return 0;
}

int espclaw_hw_tmp102_read_c(int port, uint8_t address, double *temperature_c_out)
{
    uint8_t data[2];
    int16_t raw;

    if (temperature_c_out == NULL || espclaw_hw_i2c_read_reg(port, address, ESPCLAW_HW_TMP102_TEMP_REG, data, sizeof(data)) != 0) {
        return -1;
    }

    raw = (int16_t)(((uint16_t)data[0] << 8) | data[1]);
    raw >>= 4;
    if ((raw & 0x0800) != 0) {
        raw |= (int16_t)0xF000;
    }

    *temperature_c_out = (double)raw * 0.0625;
    return 0;
}

int espclaw_hw_mpu6050_begin(int port, uint8_t address)
{
    static const uint8_t wake[] = {0x00};
    static const uint8_t config[] = {0x03};
    static const uint8_t gyro_range[] = {0x00};
    static const uint8_t accel_range[] = {0x00};

    if (espclaw_hw_i2c_write_reg(port, address, ESPCLAW_HW_MPU6050_PWR_MGMT_1, wake, sizeof(wake)) != 0) {
        return -1;
    }
    if (espclaw_hw_i2c_write_reg(port, address, ESPCLAW_HW_MPU6050_CONFIG, config, sizeof(config)) != 0) {
        return -1;
    }
    if (espclaw_hw_i2c_write_reg(port, address, ESPCLAW_HW_MPU6050_GYRO_CONFIG, gyro_range, sizeof(gyro_range)) != 0) {
        return -1;
    }
    if (espclaw_hw_i2c_write_reg(port, address, ESPCLAW_HW_MPU6050_ACCEL_CONFIG, accel_range, sizeof(accel_range)) != 0) {
        return -1;
    }

    return 0;
}

int espclaw_hw_mpu6050_read(int port, uint8_t address, espclaw_hw_mpu6050_sample_t *sample_out)
{
    uint8_t data[14];
    int16_t accel_x_raw;
    int16_t accel_y_raw;
    int16_t accel_z_raw;
    int16_t temp_raw;
    int16_t gyro_x_raw;
    int16_t gyro_y_raw;
    int16_t gyro_z_raw;

    if (sample_out == NULL ||
        espclaw_hw_i2c_read_reg(port, address, ESPCLAW_HW_MPU6050_TEMP_REG, data, sizeof(data)) != 0) {
        return -1;
    }

    accel_x_raw = read_be_i16(&data[0]);
    accel_y_raw = read_be_i16(&data[2]);
    accel_z_raw = read_be_i16(&data[4]);
    temp_raw = read_be_i16(&data[6]);
    gyro_x_raw = read_be_i16(&data[8]);
    gyro_y_raw = read_be_i16(&data[10]);
    gyro_z_raw = read_be_i16(&data[12]);

    sample_out->accel_x_g = (double)accel_x_raw / 16384.0;
    sample_out->accel_y_g = (double)accel_y_raw / 16384.0;
    sample_out->accel_z_g = (double)accel_z_raw / 16384.0;
    sample_out->gyro_x_dps = (double)gyro_x_raw / 131.0;
    sample_out->gyro_y_dps = (double)gyro_y_raw / 131.0;
    sample_out->gyro_z_dps = (double)gyro_z_raw / 131.0;
    sample_out->temperature_c = 36.53 + ((double)temp_raw / 340.0);
    return 0;
}

int espclaw_hw_i2c_write_reg(int port, uint8_t address, uint8_t reg, const uint8_t *data, size_t length)
{
    if (!i2c_port_valid(port) || data == NULL || length == 0 || length > ESPCLAW_HW_I2C_REGISTER_BYTES - 1) {
        return -1;
    }

#ifdef ESP_PLATFORM
    uint8_t buffer[ESPCLAW_HW_I2C_REGISTER_BYTES];

    if (!s_i2c_state[port].initialized || reg + length > ESPCLAW_HW_I2C_REGISTER_BYTES) {
        return -1;
    }

    buffer[0] = reg;
    memcpy(&buffer[1], data, length);
    if (i2c_master_write_to_device((i2c_port_t)port, address, buffer, length + 1, pdMS_TO_TICKS(50)) != ESP_OK) {
        return -1;
    }
#else
    if (reg + length > ESPCLAW_HW_I2C_REGISTER_BYTES) {
        return -1;
    }
    s_i2c_devices[port][address].present = true;
    memcpy(&s_i2c_devices[port][address].value[reg], data, length);
#endif

    return 0;
}

int espclaw_hw_ppm_begin(int channel, int pin, int frame_us, int pulse_us)
{
    if (!ppm_channel_valid(channel) || !gpio_pin_valid(pin) || frame_us <= 0 || pulse_us <= 0 || pulse_us >= frame_us) {
        return -1;
    }

    if (s_ppm_state[channel].configured &&
        s_ppm_state[channel].pin == pin &&
        s_ppm_state[channel].frame_us == frame_us &&
        s_ppm_state[channel].pulse_us == pulse_us) {
        return 0;
    }

#ifdef ESP_PLATFORM
    rmt_config_t config = {
        .rmt_mode = RMT_MODE_TX,
        .channel = ppm_channel_to_rmt(channel),
        .gpio_num = pin,
        .clk_div = 80,
        .mem_block_num = 1,
        .tx_config = {
            .loop_en = false,
            .carrier_en = false,
            .idle_output_en = true,
            .idle_level = RMT_IDLE_LEVEL_HIGH,
        },
    };

    if (s_ppm_state[channel].configured) {
        rmt_driver_uninstall(config.channel);
    }
    if (rmt_config(&config) != ESP_OK || rmt_driver_install(config.channel, 0, 0) != ESP_OK) {
        return -1;
    }
#endif

    memset(&s_ppm_state[channel], 0, sizeof(s_ppm_state[channel]));
    s_ppm_state[channel].configured = true;
    s_ppm_state[channel].pin = pin;
    s_ppm_state[channel].frame_us = frame_us;
    s_ppm_state[channel].pulse_us = pulse_us;
    return 0;
}

int espclaw_hw_ppm_write(int channel, const uint16_t *outputs, size_t output_count)
{
    size_t index;
    int consumed_us = 0;
    int remaining_us;

    if (!ppm_channel_valid(channel) || !s_ppm_state[channel].configured || outputs == NULL ||
        output_count == 0 || output_count > ESPCLAW_HW_PPM_OUTPUT_MAX) {
        return -1;
    }

    for (index = 0; index < output_count; ++index) {
        if (outputs[index] <= (uint16_t)s_ppm_state[channel].pulse_us) {
            return -1;
        }
        consumed_us += outputs[index];
    }

    remaining_us = s_ppm_state[channel].frame_us - consumed_us;
    if (remaining_us <= s_ppm_state[channel].pulse_us) {
        return -1;
    }

#ifdef ESP_PLATFORM
    rmt_item32_t items[ESPCLAW_HW_PPM_OUTPUT_MAX + 1];

    for (index = 0; index < output_count; ++index) {
        items[index].level0 = 0;
        items[index].duration0 = (uint16_t)s_ppm_state[channel].pulse_us;
        items[index].level1 = 1;
        items[index].duration1 = (uint16_t)(outputs[index] - s_ppm_state[channel].pulse_us);
    }
    items[output_count].level0 = 0;
    items[output_count].duration0 = (uint16_t)s_ppm_state[channel].pulse_us;
    items[output_count].level1 = 1;
    items[output_count].duration1 = (uint16_t)(remaining_us - s_ppm_state[channel].pulse_us);

    if (rmt_write_items(ppm_channel_to_rmt(channel), items, output_count + 1, true) != ESP_OK) {
        return -1;
    }
#endif

    memset(s_ppm_state[channel].outputs, 0, sizeof(s_ppm_state[channel].outputs));
    memcpy(s_ppm_state[channel].outputs, outputs, output_count * sizeof(outputs[0]));
    s_ppm_state[channel].output_count = output_count;
    return 0;
}

int espclaw_hw_ppm_state(int channel, espclaw_hw_ppm_state_t *state_out)
{
    if (!ppm_channel_valid(channel) || state_out == NULL) {
        return -1;
    }

    *state_out = s_ppm_state[channel];
    return 0;
}

double espclaw_hw_pid_step(
    double setpoint,
    double measurement,
    double integral,
    double previous_error,
    double kp,
    double ki,
    double kd,
    double dt_seconds,
    double output_min,
    double output_max,
    double *integral_out,
    double *error_out
)
{
    double error = setpoint - measurement;
    double derivative = 0.0;
    double output;

    if (dt_seconds > 0.0) {
        integral += error * dt_seconds;
        derivative = (error - previous_error) / dt_seconds;
    }

    output = (kp * error) + (ki * integral) + (kd * derivative);
    if (output < output_min) {
        output = output_min;
    }
    if (output > output_max) {
        output = output_max;
    }

    if (integral_out != NULL) {
        *integral_out = integral;
    }
    if (error_out != NULL) {
        *error_out = error;
    }

    return output;
}

int espclaw_hw_complementary_roll_pitch(
    const espclaw_hw_mpu6050_sample_t *sample,
    double previous_roll_deg,
    double previous_pitch_deg,
    double alpha,
    double dt_seconds,
    double *roll_deg_out,
    double *pitch_deg_out
)
{
    double accel_roll;
    double accel_pitch;

    if (sample == NULL || roll_deg_out == NULL || pitch_deg_out == NULL || dt_seconds < 0.0) {
        return -1;
    }

    alpha = clamp_double(alpha, 0.0, 1.0);
    accel_roll = atan2(sample->accel_y_g, sample->accel_z_g) * (180.0 / M_PI);
    accel_pitch = atan2(-sample->accel_x_g, sqrt((sample->accel_y_g * sample->accel_y_g) +
                                                  (sample->accel_z_g * sample->accel_z_g))) * (180.0 / M_PI);

    *roll_deg_out = (alpha * (previous_roll_deg + (sample->gyro_x_dps * dt_seconds))) +
                    ((1.0 - alpha) * accel_roll);
    *pitch_deg_out = (alpha * (previous_pitch_deg + (sample->gyro_y_dps * dt_seconds))) +
                     ((1.0 - alpha) * accel_pitch);
    return 0;
}

int espclaw_hw_mix_differential_drive(
    double throttle,
    double turn,
    double output_min,
    double output_max,
    double *left_out,
    double *right_out
)
{
    if (left_out == NULL || right_out == NULL || output_min > output_max) {
        return -1;
    }

    *left_out = clamp_double(throttle + turn, output_min, output_max);
    *right_out = clamp_double(throttle - turn, output_min, output_max);
    return 0;
}

int espclaw_hw_mix_quad_x(
    double throttle,
    double roll,
    double pitch,
    double yaw,
    double output_min,
    double output_max,
    double *front_left_out,
    double *front_right_out,
    double *rear_right_out,
    double *rear_left_out
)
{
    if (front_left_out == NULL || front_right_out == NULL || rear_right_out == NULL || rear_left_out == NULL ||
        output_min > output_max) {
        return -1;
    }

    *front_left_out = clamp_double(throttle + pitch + roll - yaw, output_min, output_max);
    *front_right_out = clamp_double(throttle + pitch - roll + yaw, output_min, output_max);
    *rear_right_out = clamp_double(throttle - pitch - roll - yaw, output_min, output_max);
    *rear_left_out = clamp_double(throttle - pitch + roll + yaw, output_min, output_max);
    return 0;
}

static int write_binary_file(const char *path, const uint8_t *data, size_t length)
{
    FILE *file;

    if (path == NULL || data == NULL) {
        return -1;
    }

    file = fopen(path, "wb");
    if (file == NULL) {
        return -1;
    }
    if (fwrite(data, 1, length, file) != length) {
        fclose(file);
        return -1;
    }
    fclose(file);
    return 0;
}

#ifdef ESP_PLATFORM
static framesize_t camera_framesize_for_profile(const espclaw_board_profile_t *profile)
{
    if (profile == NULL) {
        return FRAMESIZE_VGA;
    }
    if (profile->default_capture_width >= 1280 || profile->default_capture_height >= 720) {
        return FRAMESIZE_HD;
    }
    if (profile->default_capture_width >= 800 || profile->default_capture_height >= 600) {
        return FRAMESIZE_SVGA;
    }
    return FRAMESIZE_VGA;
}

static int camera_init_for_board(const espclaw_board_descriptor_t *board, const espclaw_board_profile_t *profile)
{
    camera_config_t config;
    framesize_t attempts[4];
    size_t attempt_count = 0;
    size_t index;
    esp_err_t err;

    if (board == NULL || profile == NULL) {
        return -1;
    }
    if (s_camera_initialized) {
        return 0;
    }
    if (board->profile_id != ESPCLAW_BOARD_PROFILE_ESP32CAM ||
        strcmp(board->variant_id, "ai_thinker_esp32cam") != 0) {
        ESP_LOGW(TAG, "camera init unsupported on board variant '%s'", board->variant_id);
        snprintf(s_camera_last_error, sizeof(s_camera_last_error), "camera unsupported on board '%s'", board->variant_id);
        return -1;
    }

    memset(&config, 0, sizeof(config));
    /* Reserve LEDC channel/timer 1 for camera XCLK so user PWM apps can
       continue using channel 0 without breaking camera initialization. */
    config.ledc_channel = LEDC_CHANNEL_1;
    config.ledc_timer = LEDC_TIMER_1;
    config.pin_d0 = 5;
    config.pin_d1 = 18;
    config.pin_d2 = 19;
    config.pin_d3 = 21;
    config.pin_d4 = 36;
    config.pin_d5 = 39;
    config.pin_d6 = 34;
    config.pin_d7 = 35;
    config.pin_xclk = 0;
    config.pin_pclk = 22;
    config.pin_vsync = 25;
    config.pin_href = 23;
    config.pin_sccb_sda = 26;
    config.pin_sccb_scl = 27;
    config.pin_pwdn = 32;
    config.pin_reset = -1;
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size = camera_framesize_for_profile(profile);
    config.jpeg_quality = 12;
    config.fb_count = 1;
    config.grab_mode = CAMERA_GRAB_LATEST;
    config.fb_location = CAMERA_FB_IN_PSRAM;

    attempts[attempt_count++] = config.frame_size;
    if (config.frame_size != FRAMESIZE_VGA) {
        attempts[attempt_count++] = FRAMESIZE_VGA;
    }
    if (config.frame_size != FRAMESIZE_CIF && FRAMESIZE_VGA != FRAMESIZE_CIF) {
        attempts[attempt_count++] = FRAMESIZE_CIF;
    }
    if (config.frame_size != FRAMESIZE_QVGA &&
        FRAMESIZE_VGA != FRAMESIZE_QVGA &&
        FRAMESIZE_CIF != FRAMESIZE_QVGA) {
        attempts[attempt_count++] = FRAMESIZE_QVGA;
    }

    for (index = 0; index < attempt_count; ++index) {
        config.frame_size = attempts[index];
        err = esp_camera_init(&config);
        if (err == ESP_OK) {
            s_camera_initialized = true;
            ESP_LOGI(TAG, "camera initialized for board=%s framesize=%d", board->variant_id, (int)config.frame_size);
            return 0;
        }
        ESP_LOGW(TAG, "esp_camera_init failed for framesize=%d: %s", (int)config.frame_size, esp_err_to_name(err));
        snprintf(
            s_camera_last_error,
            sizeof(s_camera_last_error),
            "esp_camera_init failed: %s (framesize=%d)",
            esp_err_to_name(err),
            (int)config.frame_size
        );
        camera_reset_runtime();
    }

    return -1;
}
#endif

int espclaw_hw_camera_capture(
    const char *workspace_root,
    const char *filename,
    espclaw_hw_camera_capture_t *capture_out
)
{
    char relative_path[ESPCLAW_HW_CAMERA_PATH_MAX];
    char absolute_path[512];
    const espclaw_board_descriptor_t *board = espclaw_board_current();
    espclaw_board_profile_t profile;
    uint64_t now_ms;

    if (workspace_root == NULL || capture_out == NULL || board == NULL) {
        if (capture_out != NULL) {
            camera_set_error(capture_out, "camera capture requires board and workspace context");
        }
        return -1;
    }

    memset(capture_out, 0, sizeof(*capture_out));
    camera_reset_last_status();
    profile = espclaw_board_profile_for(board->profile_id);
    if (!profile.has_camera) {
        camera_set_error(capture_out, "camera is not supported by the active board profile");
        return -1;
    }

    now_ms = espclaw_hw_ticks_ms();
    if (filename != NULL && normalize_capture_filename(filename)[0] != '\0') {
        snprintf(relative_path, sizeof(relative_path), "media/%s", normalize_capture_filename(filename));
    } else {
        snprintf(relative_path, sizeof(relative_path), "media/capture_%llu.jpg", (unsigned long long)now_ms);
    }
    if (espclaw_workspace_resolve_path(workspace_root, relative_path, absolute_path, sizeof(absolute_path)) != 0) {
        camera_set_error(capture_out, "failed to resolve capture path inside workspace");
        return -1;
    }

#ifdef ESP_PLATFORM
    camera_fb_t *frame;

    if (camera_init_for_board(board, &profile) != 0) {
        camera_set_error(capture_out, s_camera_last_error[0] != '\0' ? s_camera_last_error : "camera initialization failed");
        return -1;
    }

    frame = esp_camera_fb_get();
    if (frame == NULL) {
        ESP_LOGE(TAG, "camera frame capture failed");
        camera_reset_runtime();
        camera_set_error(capture_out, "camera frame capture failed");
        return -1;
    }
    if (frame->format != PIXFORMAT_JPEG || frame->buf == NULL || frame->len == 0) {
        ESP_LOGE(TAG, "camera frame is not a valid JPEG");
        esp_camera_fb_return(frame);
        camera_reset_runtime();
        camera_set_error(capture_out, "camera frame is not a valid JPEG");
        return -1;
    }
    if (write_binary_file(absolute_path, frame->buf, frame->len) != 0) {
        ESP_LOGE(TAG, "failed to persist camera frame to %s", absolute_path);
        esp_camera_fb_return(frame);
        camera_reset_runtime();
        camera_set_error(capture_out, "failed to persist captured JPEG to the workspace");
        return -1;
    }

    capture_out->ok = true;
    capture_out->simulated = false;
    capture_out->width = frame->width;
    capture_out->height = frame->height;
    capture_out->bytes_written = frame->len;
    snprintf(capture_out->relative_path, sizeof(capture_out->relative_path), "%s", relative_path);
    snprintf(capture_out->mime_type, sizeof(capture_out->mime_type), "image/jpeg");
    capture_out->error[0] = '\0';
    esp_camera_fb_return(frame);
    (void)espclaw_hw_apply_board_boot_defaults();
    camera_set_success(capture_out);
    return 0;
#else
    if (write_binary_file(absolute_path, ESPCLAW_SIM_CAMERA_JPEG, sizeof(ESPCLAW_SIM_CAMERA_JPEG)) != 0) {
        camera_set_error(capture_out, "failed to write simulated camera JPEG");
        return -1;
    }
    capture_out->ok = true;
    capture_out->simulated = true;
    capture_out->width = 1;
    capture_out->height = 1;
    capture_out->bytes_written = sizeof(ESPCLAW_SIM_CAMERA_JPEG);
    snprintf(capture_out->relative_path, sizeof(capture_out->relative_path), "%s", relative_path);
    snprintf(capture_out->mime_type, sizeof(capture_out->mime_type), "image/jpeg");
    capture_out->error[0] = '\0';
    camera_set_success(capture_out);
    return 0;
#endif
}

int espclaw_hw_camera_status(espclaw_hw_camera_status_t *status_out)
{
    const espclaw_board_descriptor_t *board = espclaw_board_current();
    espclaw_board_profile_t profile;

    if (status_out == NULL) {
        return -1;
    }

    memset(status_out, 0, sizeof(*status_out));
    if (board == NULL) {
        snprintf(status_out->last_error, sizeof(status_out->last_error), "board descriptor is unavailable");
        return -1;
    }

    profile = espclaw_board_profile_for(board->profile_id);
    status_out->supported = profile.has_camera;
    status_out->initialized = s_camera_initialized;
    status_out->simulated = s_camera_last_simulated;
    status_out->last_capture_ok = s_camera_last_capture_ok;
    status_out->last_width = s_camera_last_width;
    status_out->last_height = s_camera_last_height;
    status_out->last_bytes_written = s_camera_last_bytes_written;
    snprintf(status_out->board_variant, sizeof(status_out->board_variant), "%s", board->variant_id);
    snprintf(status_out->last_relative_path, sizeof(status_out->last_relative_path), "%s", s_camera_last_relative_path);
    snprintf(status_out->last_error, sizeof(status_out->last_error), "%s", s_camera_last_error);
    return 0;
}

uint64_t espclaw_hw_ticks_ms(void)
{
#ifdef ESP_PLATFORM
    return (uint64_t)(esp_timer_get_time() / 1000ULL);
#else
    struct timeval now;

    gettimeofday(&now, NULL);
    return ((uint64_t)now.tv_sec * 1000ULL) + ((uint64_t)now.tv_usec / 1000ULL);
#endif
}

void espclaw_hw_sleep_ms(uint32_t duration_ms)
{
#ifdef ESP_PLATFORM
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
#else
    usleep(duration_ms * 1000U);
#endif
}

#ifndef ESP_PLATFORM
void espclaw_hw_sim_reset(void)
{
    memset(s_pwm_state, 0, sizeof(s_pwm_state));
    memset(s_ppm_state, 0, sizeof(s_ppm_state));
    memset(s_i2c_state, 0, sizeof(s_i2c_state));
    memset(s_uart_state, 0, sizeof(s_uart_state));
    memset(s_gpio_level, 0, sizeof(s_gpio_level));
    memset(s_adc_raw, 0, sizeof(s_adc_raw));
    memset(s_i2c_devices, 0, sizeof(s_i2c_devices));
    s_host_stdio_nonblocking_ready = false;
}

void espclaw_hw_sim_set_gpio_input(int pin, int level)
{
    if (gpio_pin_valid(pin)) {
        s_gpio_level[pin] = level != 0 ? 1 : 0;
    }
}

void espclaw_hw_sim_set_adc_raw(int unit, int channel, int raw)
{
    if (unit >= 1 && unit <= ESPCLAW_HW_ADC_UNIT_MAX && channel >= 0 && channel < ESPCLAW_HW_ADC_CHANNEL_MAX) {
        s_adc_raw[unit - 1][channel] = raw;
    }
}

void espclaw_hw_sim_set_i2c_reg(int port, uint8_t address, uint8_t reg, const uint8_t *data, size_t length)
{
    if (!i2c_port_valid(port) || data == NULL || reg + length > ESPCLAW_HW_I2C_REGISTER_BYTES) {
        return;
    }

    s_i2c_devices[port][address].present = true;
    memcpy(&s_i2c_devices[port][address].value[reg], data, length);
}

void espclaw_hw_sim_uart_feed_input(int port, const uint8_t *data, size_t length)
{
    if (!uart_port_valid(port) || data == NULL || length == 0) {
        return;
    }

    (void)uart_buffer_append(s_uart_state[port].rx_buffer, &s_uart_state[port].rx_length, data, length);
    (void)uart_buffer_append(s_uart_state[port].event_buffer, &s_uart_state[port].event_length, data, length);
}

int espclaw_hw_sim_uart_take_output(int port, uint8_t *data, size_t max_length, size_t *length_out)
{
    if (!uart_port_valid(port) || data == NULL || length_out == NULL || max_length == 0) {
        return -1;
    }

    *length_out = uart_buffer_take(s_uart_state[port].tx_buffer, &s_uart_state[port].tx_length, data, max_length);
    return 0;
}
#endif
