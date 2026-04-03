#include "espclaw/lua_api_registry.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    const char *text;
} lua_rule_t;

static const lua_rule_t LUA_RULES[] = {
    {"Expose one of these entrypoints: function handle(trigger, payload), function on_<trigger>(payload), or function on_event(trigger, payload)."},
    {"Use only built-in espclaw.* APIs and workspace modules that already exist."},
    {"Prefer shared reusable drivers, filters, and adapters as components under /workspace/components plus require()-able modules under /workspace/lib."},
    {"Do not execute hardware side effects at load time; do the work inside a handler."},
    {"Do not assume external Lua modules like cjson are present; build JSON with string.format unless a module already exists in the workspace."},
    {"If the user asks you to create, install, or update an app, call app.install before claiming success."},
    {"If the user needs reusable code shared by multiple apps, call component.install first, then app.install for the apps that require it."},
    {"Call lua_api.list when exact signatures matter before installing a Lua app."},
};

static const espclaw_lua_api_descriptor_t LUA_APIS[] = {
    {"core", "espclaw.log", "espclaw.log(message)", "Write a message to the device log."},
    {"core", "espclaw.has_permission", "espclaw.has_permission(permission)", "Check whether the current app manifest grants a permission string."},
    {"core", "espclaw.list_apps", "espclaw.list_apps()", "List installed app ids."},
    {"fs", "espclaw.fs.read", "espclaw.fs.read(path)", "Read a workspace file."},
    {"fs", "espclaw.fs.write", "espclaw.fs.write(path, content)", "Write a workspace file."},
    {"board", "espclaw.board.variant", "espclaw.board.variant()", "Return the current board variant id."},
    {"board", "espclaw.board.pin", "espclaw.board.pin(alias)", "Resolve a named pin alias to a GPIO number."},
    {"board", "espclaw.board.i2c", "espclaw.board.i2c(name)", "Resolve a named I2C bus descriptor."},
    {"board", "espclaw.board.uart", "espclaw.board.uart(name)", "Resolve a named UART descriptor."},
    {"board", "espclaw.board.adc", "espclaw.board.adc(name)", "Resolve a named ADC channel descriptor."},
    {"hardware", "espclaw.hardware.list", "espclaw.hardware.list()", "Return the live board capabilities and named buses."},
    {"gpio", "espclaw.gpio.read", "espclaw.gpio.read(pin)", "Read a GPIO value."},
    {"gpio", "espclaw.gpio.write", "espclaw.gpio.write(pin, value)", "Write a GPIO value."},
    {"pwm", "espclaw.pwm.setup", "espclaw.pwm.setup(channel, pin, frequency_hz, resolution_bits)", "Configure a PWM channel on a pin."},
    {"pwm", "espclaw.pwm.write", "espclaw.pwm.write(channel, duty)", "Write an integer PWM duty value."},
    {"pwm", "espclaw.pwm.write_us", "espclaw.pwm.write_us(channel, pulse_us)", "Write a servo-style pulse width in microseconds."},
    {"pwm", "espclaw.pwm.stop", "espclaw.pwm.stop(channel)", "Stop a PWM channel."},
    {"pwm", "espclaw.pwm.state", "espclaw.pwm.state(channel)", "Inspect the current PWM channel state."},
    {"servo", "espclaw.servo.attach", "espclaw.servo.attach(channel, pin, min_us, max_us)", "Attach a servo/ESC output to a PWM channel."},
    {"servo", "espclaw.servo.write_us", "espclaw.servo.write_us(channel, pulse_us)", "Write a servo/ESC pulse width."},
    {"servo", "espclaw.servo.write_norm", "espclaw.servo.write_norm(channel, normalized)", "Write a normalized servo/ESC output from -1..1."},
    {"adc", "espclaw.adc.read_raw", "espclaw.adc.read_raw(channel)", "Read a raw ADC sample."},
    {"adc", "espclaw.adc.read_mv", "espclaw.adc.read_mv(channel)", "Read an ADC channel in millivolts."},
    {"adc", "espclaw.adc.read_named_mv", "espclaw.adc.read_named_mv(name)", "Read a named ADC channel in millivolts."},
    {"i2c", "espclaw.i2c.begin", "espclaw.i2c.begin(port, sda, scl, frequency_hz)", "Initialize an explicit I2C bus."},
    {"i2c", "espclaw.i2c.begin_board", "espclaw.i2c.begin_board(name)", "Initialize a named board I2C bus."},
    {"i2c", "espclaw.i2c.scan", "espclaw.i2c.scan(port)", "Scan an I2C bus for devices."},
    {"i2c", "espclaw.i2c.read_reg", "espclaw.i2c.read_reg(port, address, reg, length)", "Read bytes from an I2C register."},
    {"i2c", "espclaw.i2c.write_reg", "espclaw.i2c.write_reg(port, address, reg, bytes)", "Write bytes to an I2C register."},
    {"ppm", "espclaw.ppm.begin", "espclaw.ppm.begin(channel, pin)", "Attach a PPM output to a pin."},
    {"ppm", "espclaw.ppm.write", "espclaw.ppm.write(channel, value_us)", "Write a PPM pulse value."},
    {"ppm", "espclaw.ppm.state", "espclaw.ppm.state(channel)", "Inspect a PPM output state."},
    {"uart", "espclaw.uart.read", "espclaw.uart.read(port, length)", "Read bytes from a UART."},
    {"uart", "espclaw.uart.write", "espclaw.uart.write(port, data)", "Write bytes to a UART."},
    {"camera", "espclaw.camera.capture", "espclaw.camera.capture(filename)", "Capture a JPEG to the workspace media directory."},
    {"temperature", "espclaw.temperature.tmp102_c", "espclaw.temperature.tmp102_c(port, address)", "Read a TMP102 temperature sensor in Celsius."},
    {"imu", "espclaw.imu.mpu6050_begin", "espclaw.imu.mpu6050_begin(port, address)", "Initialize an MPU6050 IMU."},
    {"imu", "espclaw.imu.mpu6050_read", "espclaw.imu.mpu6050_read(port, address)", "Read accel/gyro data from an MPU6050."},
    {"imu", "espclaw.imu.complementary_roll_pitch", "espclaw.imu.complementary_roll_pitch(ax, ay, az, gx_dps, gy_dps, dt_seconds, alpha, prev_roll_deg, prev_pitch_deg)", "Estimate roll and pitch from accel and gyro inputs."},
    {"buzzer", "espclaw.buzzer.tone", "espclaw.buzzer.tone(pin, frequency_hz, duration_ms)", "Play a tone on a buzzer-capable pin."},
    {"pid", "espclaw.pid.step", "espclaw.pid.step(setpoint, measurement, dt_seconds, kp, ki, kd, integral, prev_error)", "Compute a PID step and updated controller state."},
    {"control", "espclaw.control.mix_differential", "espclaw.control.mix_differential(throttle, steering)", "Mix rover differential drive outputs."},
    {"control", "espclaw.control.mix_quad_x", "espclaw.control.mix_quad_x(throttle, roll, pitch, yaw)", "Mix quad-X motor outputs."},
    {"time", "espclaw.time.ticks_ms", "espclaw.time.ticks_ms()", "Return the current monotonic tick count in milliseconds."},
    {"time", "espclaw.time.sleep_ms", "espclaw.time.sleep_ms(ms)", "Sleep the current Lua task for a number of milliseconds."},
    {"events", "espclaw.events.emit", "espclaw.events.emit(name, payload)", "Emit a local event to event-driven tasks."},
    {"events", "espclaw.events.list", "espclaw.events.list()", "List active event watches."},
    {"events", "espclaw.events.watch_uart", "espclaw.events.watch_uart(watch_id, event_name, port, interval_ms)", "Create a UART-backed event watch."},
    {"events", "espclaw.events.watch_adc_threshold", "espclaw.events.watch_adc_threshold(watch_id, event_name, unit, channel, threshold, interval_ms)", "Create an ADC-threshold event watch."},
    {"events", "espclaw.events.remove_watch", "espclaw.events.remove_watch(watch_id)", "Remove an event watch."},
};

size_t espclaw_lua_api_count(void)
{
    return sizeof(LUA_APIS) / sizeof(LUA_APIS[0]);
}

const espclaw_lua_api_descriptor_t *espclaw_lua_api_at(size_t index)
{
    if (index >= espclaw_lua_api_count()) {
        return NULL;
    }
    return &LUA_APIS[index];
}

static size_t append_json_escaped(char *buffer, size_t buffer_size, size_t used, const char *text)
{
    const unsigned char *cursor = (const unsigned char *)(text != NULL ? text : "");

    if (used >= buffer_size) {
        return used;
    }
    buffer[used++] = '"';
    while (*cursor != '\0' && used + 2 < buffer_size) {
        switch (*cursor) {
            case '\\':
            case '"':
                buffer[used++] = '\\';
                buffer[used++] = (char)*cursor;
                break;
            case '\n':
                buffer[used++] = '\\';
                buffer[used++] = 'n';
                break;
            case '\r':
                buffer[used++] = '\\';
                buffer[used++] = 'r';
                break;
            case '\t':
                buffer[used++] = '\\';
                buffer[used++] = 't';
                break;
            default:
                buffer[used++] = (char)*cursor;
                break;
        }
        cursor++;
    }
    if (used < buffer_size) {
        buffer[used++] = '"';
    }
    if (used < buffer_size) {
        buffer[used] = '\0';
    } else if (buffer_size > 0) {
        buffer[buffer_size - 1] = '\0';
    }
    return used;
}

size_t espclaw_render_lua_api_json(char *buffer, size_t buffer_size)
{
    size_t used = 0;
    size_t index;

    if (buffer == NULL || buffer_size == 0) {
        return 0;
    }

    used += (size_t)snprintf(buffer + used, buffer_size - used, "{\"rules\":[");
    for (index = 0; index < sizeof(LUA_RULES) / sizeof(LUA_RULES[0]) && used + 8 < buffer_size; ++index) {
        used += (size_t)snprintf(buffer + used, buffer_size - used, "%s", index == 0 ? "" : ",");
        used = append_json_escaped(buffer, buffer_size, used, LUA_RULES[index].text);
    }

    used += (size_t)snprintf(buffer + used, buffer_size - used, "],\"apis\":[");
    for (index = 0; index < espclaw_lua_api_count() && used + 32 < buffer_size; ++index) {
        const espclaw_lua_api_descriptor_t *api = espclaw_lua_api_at(index);

        used += (size_t)snprintf(buffer + used, buffer_size - used, "%s{\"category\":", index == 0 ? "" : ",");
        used = append_json_escaped(buffer, buffer_size, used, api->category);
        used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"name\":");
        used = append_json_escaped(buffer, buffer_size, used, api->name);
        used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"signature\":");
        used = append_json_escaped(buffer, buffer_size, used, api->signature);
        used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"summary\":");
        used = append_json_escaped(buffer, buffer_size, used, api->summary);
        used += (size_t)snprintf(buffer + used, buffer_size - used, "}");
    }
    used += (size_t)snprintf(buffer + used, buffer_size - used, "]}");
    return used;
}

size_t espclaw_render_lua_api_markdown(char *buffer, size_t buffer_size)
{
    size_t used = 0;
    size_t index;
    const char *current_category = NULL;

    if (buffer == NULL || buffer_size == 0) {
        return 0;
    }

    used += (size_t)snprintf(buffer + used, buffer_size - used, "# Lua API Reference\n\n");
    used += (size_t)snprintf(buffer + used, buffer_size - used, "This document is generated from the runtime Lua API registry.\n\n");
    used += (size_t)snprintf(buffer + used, buffer_size - used, "## Rules\n\n");
    for (index = 0; index < sizeof(LUA_RULES) / sizeof(LUA_RULES[0]) && used + 8 < buffer_size; ++index) {
        used += (size_t)snprintf(buffer + used, buffer_size - used, "- %s\n", LUA_RULES[index].text);
    }
    used += (size_t)snprintf(buffer + used, buffer_size - used, "\n## Functions\n");

    for (index = 0; index < espclaw_lua_api_count() && used + 32 < buffer_size; ++index) {
        const espclaw_lua_api_descriptor_t *api = espclaw_lua_api_at(index);

        if (current_category == NULL || strcmp(current_category, api->category) != 0) {
            current_category = api->category;
            used += (size_t)snprintf(buffer + used, buffer_size - used, "\n### %s\n", current_category);
        }
        used += (size_t)snprintf(
            buffer + used,
            buffer_size - used,
            "- `%s`\n  %s\n",
            api->signature,
            api->summary
        );
    }

    return used;
}

size_t espclaw_render_lua_api_prompt_snapshot(char *buffer, size_t buffer_size)
{
    size_t used = 0;
    size_t index;
    const char *current_category = NULL;

    if (buffer == NULL || buffer_size == 0) {
        return 0;
    }

    used += (size_t)snprintf(buffer + used, buffer_size - used, "# Lua App Contract\n");
    for (index = 0; index < sizeof(LUA_RULES) / sizeof(LUA_RULES[0]) && used + 8 < buffer_size; ++index) {
        used += (size_t)snprintf(buffer + used, buffer_size - used, "- %s\n", LUA_RULES[index].text);
    }
    used += (size_t)snprintf(buffer + used, buffer_size - used, "\n# Lua API Snapshot\n");

    for (index = 0; index < espclaw_lua_api_count() && used + 32 < buffer_size; ++index) {
        const espclaw_lua_api_descriptor_t *api = espclaw_lua_api_at(index);

        if (current_category == NULL || strcmp(current_category, api->category) != 0) {
            current_category = api->category;
            used += (size_t)snprintf(buffer + used, buffer_size - used, "- %s:\n", current_category);
        }
        used += (size_t)snprintf(buffer + used, buffer_size - used, "  - `%s`\n", api->signature);
    }

    return used;
}

size_t espclaw_render_component_architecture_prompt_snapshot(char *buffer, size_t buffer_size)
{
    size_t used = 0;

    if (buffer == NULL || buffer_size == 0) {
        return 0;
    }

    used += (size_t)snprintf(buffer + used, buffer_size - used, "# Component Guidance\n");
    used += (size_t)snprintf(buffer + used, buffer_size - used, "- A component is reusable Lua code plus metadata under /workspace/components/<component_id>/.\n");
    used += (size_t)snprintf(buffer + used, buffer_size - used, "- Components are published into /workspace/lib so apps can require(module_name).\n");
    used += (size_t)snprintf(buffer + used, buffer_size - used, "- Prefer component.install for shared low-level sensor drivers, filters, and control helpers.\n");
    used += (size_t)snprintf(buffer + used, buffer_size - used, "- Prefer app.install for product logic and user-facing features.\n");
    used += (size_t)snprintf(buffer + used, buffer_size - used, "- Use task.start for temporary schedules, behavior.register for persisted schedules, and events only for decoupled producers or consumers.\n");
    return used;
}
