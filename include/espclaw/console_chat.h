#ifndef ESPCLAW_CONSOLE_CHAT_H
#define ESPCLAW_CONSOLE_CHAT_H

#include <stdbool.h>
#include <stddef.h>

#include "espclaw/agent_loop.h"
#include "espclaw/runtime.h"

typedef struct {
    const espclaw_runtime_status_t *(*status)(void);
    esp_err_t (*wifi_scan)(espclaw_wifi_network_t *networks, size_t max_networks, size_t *count_out);
    esp_err_t (*wifi_join)(const char *ssid, const char *password, char *message, size_t message_size);
    esp_err_t (*telegram_get_config)(espclaw_telegram_config_t *config);
    esp_err_t (*telegram_set_config)(
        const espclaw_telegram_config_t *config,
        char *message,
        size_t message_size
    );
    esp_err_t (*factory_reset)(char *message, size_t message_size);
    void (*reboot)(void);
} espclaw_console_runtime_adapter_t;

void espclaw_console_set_runtime_adapter(const espclaw_console_runtime_adapter_t *adapter);

int espclaw_console_run(
    const char *workspace_root,
    const char *session_id,
    const char *input,
    bool allow_mutations,
    bool yolo_mode,
    espclaw_agent_run_result_t *result
);

#endif
