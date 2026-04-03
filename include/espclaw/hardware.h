#ifndef ESPCLAW_HARDWARE_H
#define ESPCLAW_HARDWARE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ESPCLAW_HW_PWM_CHANNEL_MAX 4
#define ESPCLAW_HW_PPM_CHANNEL_MAX 2
#define ESPCLAW_HW_PPM_OUTPUT_MAX 8
#define ESPCLAW_HW_I2C_PORT_MAX 2
#define ESPCLAW_HW_I2C_DEVICE_MAX 128
#define ESPCLAW_HW_I2C_REGISTER_BYTES 256
#define ESPCLAW_HW_I2C_SCAN_MAX 126
#define ESPCLAW_HW_UART_PORT_MAX 3
#define ESPCLAW_HW_CAMERA_PATH_MAX 128
#define ESPCLAW_HW_CAMERA_ERROR_MAX 128

typedef struct {
    bool configured;
    int pin;
    int frequency_hz;
    int resolution_bits;
    int duty;
    int pulse_width_us;
} espclaw_hw_pwm_state_t;

typedef struct {
    bool configured;
    int pin;
    int frame_us;
    int pulse_us;
    size_t output_count;
    uint16_t outputs[ESPCLAW_HW_PPM_OUTPUT_MAX];
} espclaw_hw_ppm_state_t;

typedef struct {
    double accel_x_g;
    double accel_y_g;
    double accel_z_g;
    double gyro_x_dps;
    double gyro_y_dps;
    double gyro_z_dps;
    double temperature_c;
} espclaw_hw_mpu6050_sample_t;

typedef struct {
    bool ok;
    bool simulated;
    size_t width;
    size_t height;
    size_t bytes_written;
    char relative_path[ESPCLAW_HW_CAMERA_PATH_MAX];
    char mime_type[24];
    char error[ESPCLAW_HW_CAMERA_ERROR_MAX];
} espclaw_hw_camera_capture_t;

typedef struct {
    bool supported;
    bool initialized;
    bool simulated;
    bool last_capture_ok;
    size_t last_width;
    size_t last_height;
    size_t last_bytes_written;
    char board_variant[64];
    char last_relative_path[ESPCLAW_HW_CAMERA_PATH_MAX];
    char last_error[ESPCLAW_HW_CAMERA_ERROR_MAX];
} espclaw_hw_camera_status_t;

int espclaw_hw_apply_board_boot_defaults(void);

int espclaw_hw_gpio_write(int pin, int level);
int espclaw_hw_gpio_read(int pin, int *level_out);

int espclaw_hw_pwm_setup(int channel, int pin, int frequency_hz, int resolution_bits);
int espclaw_hw_pwm_write(int channel, int duty);
int espclaw_hw_pwm_write_us(int channel, int pulse_width_us);
int espclaw_hw_pwm_stop(int channel);
int espclaw_hw_pwm_state(int channel, espclaw_hw_pwm_state_t *state_out);
int espclaw_hw_buzzer_tone(int channel, int pin, int frequency_hz, int duration_ms, int duty_percent);

int espclaw_hw_adc_read_raw(int unit, int channel, int *raw_out);
int espclaw_hw_adc_read_mv(int unit, int channel, int *millivolts_out);

int espclaw_hw_uart_read(int port, uint8_t *data, size_t max_length, size_t *length_out);
int espclaw_hw_uart_write(int port, const uint8_t *data, size_t length, size_t *written_out);
int espclaw_hw_uart_take_event_data(int port, uint8_t *data, size_t max_length, size_t *length_out);

int espclaw_hw_i2c_begin(int port, int sda_pin, int scl_pin, int frequency_hz);
int espclaw_hw_i2c_scan(int port, uint8_t *addresses, size_t max_addresses, size_t *count_out);
int espclaw_hw_i2c_read_reg(int port, uint8_t address, uint8_t reg, uint8_t *data, size_t length);
int espclaw_hw_i2c_write_reg(int port, uint8_t address, uint8_t reg, const uint8_t *data, size_t length);
int espclaw_hw_tmp102_read_c(int port, uint8_t address, double *temperature_c_out);
int espclaw_hw_mpu6050_begin(int port, uint8_t address);
int espclaw_hw_mpu6050_read(int port, uint8_t address, espclaw_hw_mpu6050_sample_t *sample_out);

int espclaw_hw_ppm_begin(int channel, int pin, int frame_us, int pulse_us);
int espclaw_hw_ppm_write(int channel, const uint16_t *outputs, size_t output_count);
int espclaw_hw_ppm_state(int channel, espclaw_hw_ppm_state_t *state_out);

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
);
int espclaw_hw_complementary_roll_pitch(
    const espclaw_hw_mpu6050_sample_t *sample,
    double previous_roll_deg,
    double previous_pitch_deg,
    double alpha,
    double dt_seconds,
    double *roll_deg_out,
    double *pitch_deg_out
);
int espclaw_hw_mix_differential_drive(
    double throttle,
    double turn,
    double output_min,
    double output_max,
    double *left_out,
    double *right_out
);
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
);

int espclaw_hw_camera_capture(
    const char *workspace_root,
    const char *filename,
    espclaw_hw_camera_capture_t *capture_out
);
int espclaw_hw_camera_status(espclaw_hw_camera_status_t *status_out);

uint64_t espclaw_hw_ticks_ms(void);
void espclaw_hw_sleep_ms(uint32_t duration_ms);

#ifndef ESP_PLATFORM
void espclaw_hw_sim_reset(void);
void espclaw_hw_sim_set_gpio_input(int pin, int level);
void espclaw_hw_sim_set_adc_raw(int unit, int channel, int raw);
void espclaw_hw_sim_set_i2c_reg(int port, uint8_t address, uint8_t reg, const uint8_t *data, size_t length);
void espclaw_hw_sim_uart_feed_input(int port, const uint8_t *data, size_t length);
int espclaw_hw_sim_uart_take_output(int port, uint8_t *data, size_t max_length, size_t *length_out);
#endif

#endif
