#include "espclaw/runtime.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_event.h"
#include "esp_crt_bundle.h"
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#ifdef ESP_PLATFORM
#ifndef ARDUINO
#include "freertos/idf_additions.h"
#endif
#endif
#include "freertos/semphr.h"
#include "freertos/task.h"

#ifdef ARDUINO
/* ── Arduino: WiFiManager captive-portal provisioning ─────────────────────── */
#include <Arduino.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiManager.h>
/* esp_http_client is still needed for telegram_send_photo multipart upload
 * which uses the streaming write API.  The Arduino ESP32 core ships it. */
#include "esp_http_client.h"
#define ESPCLAW_BLE_PROVISIONING_AVAILABLE 0
#else
/* ── ESP-IDF: native WiFi + provisioning stack ────────────────────────────── */
#include "esp_http_client.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "wifi_provisioning/manager.h"
#if defined(CONFIG_BT_ENABLED) && CONFIG_BT_ENABLED
#include "wifi_provisioning/scheme_ble.h"
#define ESPCLAW_BLE_PROVISIONING_AVAILABLE 1
#else
#define ESPCLAW_BLE_PROVISIONING_AVAILABLE 0
#endif
#endif /* ARDUINO */

#include "espclaw/admin_api.h"
#include "espclaw/admin_server.h"
#include "espclaw/agent_loop.h"
#include "espclaw/console_chat.h"
#include "espclaw/admin_ops.h"
#include "espclaw/app_runtime.h"
#include "espclaw/auth_store.h"
#include "espclaw/behavior_runtime.h"
#include "espclaw/board_config.h"
#include "espclaw/event_watch.h"
#include "espclaw/hardware.h"
#include "espclaw/board_profile.h"
#include "espclaw/session_store.h"
#include "espclaw/storage.h"
#include "espclaw/system_monitor.h"
#include "espclaw/task_policy.h"
#include "espclaw/telegram_protocol.h"
#include "espclaw/workspace.h"

static const char *TAG = "espclaw_runtime";
static const char *ESPCLAW_PROVISIONING_POP = "espclaw-pass";
static const char *ESPCLAW_TELEGRAM_NAMESPACE = "espclaw_tg";
static const char *ESPCLAW_OPERATOR_NAMESPACE = "espclaw_op";

#ifndef CONFIG_ESPCLAW_TELEGRAM_BOT_TOKEN
#define CONFIG_ESPCLAW_TELEGRAM_BOT_TOKEN ""
#endif

#ifndef CONFIG_ESPCLAW_TELEGRAM_POLL_INTERVAL_SECONDS
#define CONFIG_ESPCLAW_TELEGRAM_POLL_INTERVAL_SECONDS 5
#endif

#ifndef CONFIG_ESPCLAW_ENABLE_SD_BOOTSTRAP
#define CONFIG_ESPCLAW_ENABLE_SD_BOOTSTRAP 1
#endif

#ifndef CONFIG_ESPCLAW_ENABLE_WIFI_PROVISIONING
#define CONFIG_ESPCLAW_ENABLE_WIFI_PROVISIONING 1
#endif

#ifndef CONFIG_ESPCLAW_ENABLE_TELEGRAM
#define CONFIG_ESPCLAW_ENABLE_TELEGRAM 1
#endif

enum {
    ESPCLAW_WIFI_CONNECTED_BIT = BIT0
};

static EventGroupHandle_t s_runtime_event_group;
static espclaw_runtime_status_t s_runtime_status;
static long s_telegram_offset;
static espclaw_telegram_config_t s_telegram_config;
static TaskHandle_t s_operator_worker_handle;
static SemaphoreHandle_t s_operator_request_mutex;
static bool s_operator_worker_started;
static bool s_telegram_task_started;
static bool s_uart_console_started;
static bool s_softap_onboarding_active;
static bool s_ble_provisioning_active;
static bool s_sta_connect_enabled;
static bool s_time_sync_started;
static bool s_time_sync_completed;
static esp_sntp_config_t s_sntp_config;
static bool s_operator_yolo_mode;
static esp_err_t maybe_start_telegram(void);
static esp_err_t maybe_start_operator_worker(void);
esp_err_t espclaw_runtime_start_operator_surfaces(void);

#ifdef ESP_PLATFORM
#define ESPCLAW_BEHAVIOR_AUTOSTART_DELAY_MS 8000
#define ESPCLAW_BEHAVIOR_AUTOSTART_STACK_WORDS 6144
#if CONFIG_ESPCLAW_BOARD_PROFILE_ESP32CAM
#define ESPCLAW_TELEGRAM_STACK_BYTES 8192
#else
#define ESPCLAW_TELEGRAM_STACK_BYTES 16384
#endif
#if CONFIG_ESPCLAW_BOARD_PROFILE_ESP32CAM
#define ESPCLAW_OPERATOR_AGENT_STACK_BYTES 20480
#else
#define ESPCLAW_OPERATOR_AGENT_STACK_BYTES 32768
#endif
#define ESPCLAW_TELEGRAM_URL_BYTES 512
#define ESPCLAW_TELEGRAM_RESPONSE_BYTES 8192
#define ESPCLAW_TELEGRAM_REPLY_BYTES 768
#define ESPCLAW_TELEGRAM_CONTEXT_SLOTS 4
#define ESPCLAW_TELEGRAM_CONTEXT_MESSAGES 6
#if CONFIG_ESPCLAW_BOARD_PROFILE_ESP32CAM
#define ESPCLAW_UART_CONSOLE_STACK_BYTES 6144
#else
#define ESPCLAW_UART_CONSOLE_STACK_BYTES 8192
#endif
#endif

typedef struct {
    bool active;
    char chat_id[32];
    espclaw_agent_history_message_t messages[ESPCLAW_TELEGRAM_CONTEXT_MESSAGES];
    size_t count;
} espclaw_telegram_chat_context_t;

static espclaw_telegram_chat_context_t s_telegram_contexts[ESPCLAW_TELEGRAM_CONTEXT_SLOTS];

typedef enum {
    ESPCLAW_OPERATOR_REQUEST_NONE = 0,
    ESPCLAW_OPERATOR_REQUEST_UART_CONSOLE,
    ESPCLAW_OPERATOR_REQUEST_TELEGRAM_AGENT
} espclaw_operator_request_kind_t;

typedef struct {
    espclaw_operator_request_kind_t kind;
    const char *workspace_root;
    const char *session_id;
    const char *input;
    bool allow_mutations;
    bool yolo_mode;
    espclaw_agent_run_result_t *result;
    const espclaw_agent_history_message_t *history;
    size_t history_count;
    int status;
    TaskHandle_t requester_task;
} espclaw_operator_request_t;

static espclaw_operator_request_t s_operator_request;

#ifdef ESP_PLATFORM
#if CONFIG_ESPCLAW_BOARD_PROFILE_ESP32CAM
#define ESPCLAW_STACK_WORDS(bytes) (((bytes) + sizeof(StackType_t) - 1U) / sizeof(StackType_t))
static DRAM_ATTR StaticTask_t s_operator_worker_task_buffer;
static DRAM_ATTR StackType_t s_operator_worker_stack[ESPCLAW_STACK_WORDS(ESPCLAW_OPERATOR_AGENT_STACK_BYTES)];
static DRAM_ATTR StaticTask_t s_uart_console_task_buffer;
static DRAM_ATTR StackType_t s_uart_console_stack[ESPCLAW_STACK_WORDS(ESPCLAW_UART_CONSOLE_STACK_BYTES)];
static DRAM_ATTR StaticTask_t s_telegram_task_buffer;
static DRAM_ATTR StackType_t s_telegram_task_stack[ESPCLAW_STACK_WORDS(ESPCLAW_TELEGRAM_STACK_BYTES)];
#endif
#endif

typedef struct {
#ifdef ESP_PLATFORM
    StackType_t *stack_buffer;
    StaticTask_t *task_buffer;
#endif
} espclaw_runtime_task_reservation_t;

static espclaw_runtime_task_reservation_t reserve_runtime_task_storage(const char *name)
{
    espclaw_runtime_task_reservation_t reservation = {0};

#ifdef ESP_PLATFORM
#if CONFIG_ESPCLAW_BOARD_PROFILE_ESP32CAM
    if (name != NULL && strcmp(name, "espclaw_op") == 0) {
        reservation.stack_buffer = s_operator_worker_stack;
        reservation.task_buffer = &s_operator_worker_task_buffer;
    } else if (name != NULL && strcmp(name, "espclaw_uart") == 0) {
        reservation.stack_buffer = s_uart_console_stack;
        reservation.task_buffer = &s_uart_console_task_buffer;
    } else if (name != NULL && strcmp(name, "espclaw_tg") == 0) {
        reservation.stack_buffer = s_telegram_task_stack;
        reservation.task_buffer = &s_telegram_task_buffer;
    }
#endif
#else
    (void)name;
#endif
    return reservation;
}

static BaseType_t create_runtime_task(
    TaskFunction_t task,
    const char *name,
    uint32_t stack_bytes,
    void *parameter,
    UBaseType_t priority,
    TaskHandle_t *handle_out,
    BaseType_t core_id
)
{
#ifdef ESP_PLATFORM
    espclaw_runtime_task_reservation_t reservation = reserve_runtime_task_storage(name);

    if (reservation.stack_buffer != NULL && reservation.task_buffer != NULL) {
        TaskHandle_t handle = xTaskCreateStaticPinnedToCore(
            task,
            name,
            stack_bytes,
            parameter,
            priority,
            reservation.stack_buffer,
            reservation.task_buffer,
            core_id
        );

        if (handle_out != NULL) {
            *handle_out = handle;
        }
        return handle != NULL ? pdPASS : errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY;
    }
#endif
    return xTaskCreatePinnedToCore(
        task,
        name,
        stack_bytes,
        parameter,
        priority,
        handle_out,
        core_id
    );
}

static void capture_operator_bench_metrics_before(espclaw_operator_bench_metrics_t *metrics)
{
    if (metrics == NULL) {
        return;
    }
#ifdef ESP_PLATFORM
    metrics->free_internal_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    metrics->largest_internal_before = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    metrics->free_internal_before = 0U;
    metrics->largest_internal_before = 0U;
#endif
}

static void capture_operator_bench_metrics_after(espclaw_operator_bench_metrics_t *metrics)
{
    if (metrics == NULL) {
        return;
    }
#ifdef ESP_PLATFORM
    metrics->free_internal_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    metrics->largest_internal_after = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    metrics->free_internal_after = 0U;
    metrics->largest_internal_after = 0U;
#endif
}

static void *runtime_external_malloc(size_t size)
{
#ifdef ESP_PLATFORM
#if defined(CONFIG_SPIRAM_USE_MALLOC)
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (ptr != NULL) {
        return ptr;
    }
#endif
#endif
    return malloc(size);
}

static void *runtime_external_calloc(size_t count, size_t size)
{
#ifdef ESP_PLATFORM
#if defined(CONFIG_SPIRAM_USE_MALLOC)
    void *ptr = heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (ptr != NULL) {
        return ptr;
    }
#endif
#endif
    return calloc(count, size);
}

static esp_err_t load_operator_config_from_nvs(bool *yolo_mode)
{
    if (yolo_mode == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *yolo_mode = true;
#ifdef ARDUINO
    Preferences prefs;
    if (prefs.begin(ESPCLAW_OPERATOR_NAMESPACE, true)) {
        *yolo_mode = prefs.getUChar("yolo", 1U) != 0U;
        prefs.end();
    }
    return ESP_OK;
#else
    nvs_handle_t handle;
    uint8_t enabled = 1U;

    if (nvs_open(ESPCLAW_OPERATOR_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return ESP_OK;
    }
    if (nvs_get_u8(handle, "yolo", &enabled) == ESP_OK) {
        *yolo_mode = enabled != 0U;
    }
    nvs_close(handle);
    return ESP_OK;
#endif
}

static esp_err_t save_operator_config_to_nvs(bool yolo_mode)
{
#ifdef ARDUINO
    Preferences prefs;
    if (!prefs.begin(ESPCLAW_OPERATOR_NAMESPACE, false)) {
        return ESP_FAIL;
    }
    prefs.putUChar("yolo", yolo_mode ? 1U : 0U);
    prefs.end();
    return ESP_OK;
#else
    nvs_handle_t handle;
    esp_err_t err;

    err = nvs_open(ESPCLAW_OPERATOR_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    if (nvs_set_u8(handle, "yolo", yolo_mode ? 1U : 0U) != ESP_OK ||
        nvs_commit(handle) != ESP_OK) {
        nvs_close(handle);
        return ESP_FAIL;
    }
    nvs_close(handle);
    return ESP_OK;
#endif
}

static bool should_skip_boot_automation_for_reset_reason(esp_reset_reason_t reason)
{
    return reason == ESP_RST_PANIC ||
           reason == ESP_RST_INT_WDT ||
           reason == ESP_RST_TASK_WDT ||
           reason == ESP_RST_WDT ||
           reason == ESP_RST_BROWNOUT;
}

static void trim_trailing_whitespace(char *text)
{
    size_t length;

    if (text == NULL) {
        return;
    }
    length = strlen(text);
    while (length > 0 && isspace((unsigned char)text[length - 1])) {
        text[--length] = '\0';
    }
}

static espclaw_telegram_chat_context_t *telegram_context_for_chat(const char *chat_id, bool create_if_missing)
{
    size_t index;
    espclaw_telegram_chat_context_t *empty_slot = NULL;

    if (chat_id == NULL || chat_id[0] == '\0') {
        return NULL;
    }
    for (index = 0; index < ESPCLAW_TELEGRAM_CONTEXT_SLOTS; ++index) {
        if (s_telegram_contexts[index].active &&
            strcmp(s_telegram_contexts[index].chat_id, chat_id) == 0) {
            return &s_telegram_contexts[index];
        }
        if (empty_slot == NULL && !s_telegram_contexts[index].active) {
            empty_slot = &s_telegram_contexts[index];
        }
    }
    if (!create_if_missing) {
        return NULL;
    }
    if (empty_slot == NULL) {
        empty_slot = &s_telegram_contexts[0];
    }
    memset(empty_slot, 0, sizeof(*empty_slot));
    empty_slot->active = true;
    snprintf(empty_slot->chat_id, sizeof(empty_slot->chat_id), "%s", chat_id);
    return empty_slot;
}

static void telegram_context_append_message(
    espclaw_telegram_chat_context_t *context,
    const char *role,
    const char *content
)
{
    size_t index;

    if (context == NULL || role == NULL || content == NULL || role[0] == '\0' || content[0] == '\0') {
        return;
    }
    if (context->count >= ESPCLAW_TELEGRAM_CONTEXT_MESSAGES) {
        for (index = 1; index < context->count; ++index) {
            context->messages[index - 1] = context->messages[index];
        }
        context->count = ESPCLAW_TELEGRAM_CONTEXT_MESSAGES - 1U;
    }
    snprintf(context->messages[context->count].role, sizeof(context->messages[context->count].role), "%s", role);
    snprintf(context->messages[context->count].content, sizeof(context->messages[context->count].content), "%s", content);
    context->count++;
}

static void copy_text(char *buffer, size_t buffer_size, const char *value)
{
    if (buffer == NULL || buffer_size == 0) {
        return;
    }
    snprintf(buffer, buffer_size, "%s", value != NULL ? value : "");
}

static void telegram_token_hint(const char *token, char *buffer, size_t buffer_size)
{
    size_t length;

    if (buffer == NULL || buffer_size == 0) {
        return;
    }
    buffer[0] = '\0';
    if (token == NULL || token[0] == '\0') {
        return;
    }
    length = strlen(token);
    if (length <= 8U) {
        snprintf(buffer, buffer_size, "configured");
        return;
    }
    snprintf(buffer, buffer_size, "%.4s...%.4s", token, token + length - 4U);
}

static void telegram_config_defaults(espclaw_telegram_config_t *config)
{
    if (config == NULL) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->enabled = CONFIG_ESPCLAW_ENABLE_TELEGRAM != 0;
    config->poll_interval_seconds = CONFIG_ESPCLAW_TELEGRAM_POLL_INTERVAL_SECONDS > 0
        ? (uint32_t)CONFIG_ESPCLAW_TELEGRAM_POLL_INTERVAL_SECONDS
        : 5U;
    copy_text(config->bot_token, sizeof(config->bot_token), CONFIG_ESPCLAW_TELEGRAM_BOT_TOKEN);
    config->configured = config->bot_token[0] != '\0';
    config->ready = false;
    telegram_token_hint(config->bot_token, config->token_hint, sizeof(config->token_hint));
}

static void refresh_telegram_config_derived_fields(espclaw_telegram_config_t *config)
{
    if (config == NULL) {
        return;
    }
    if (config->poll_interval_seconds == 0U) {
        config->poll_interval_seconds = 5U;
    }
    config->configured = config->bot_token[0] != '\0';
    telegram_token_hint(config->bot_token, config->token_hint, sizeof(config->token_hint));
}

static void telegram_config_to_runtime_snapshot(const espclaw_telegram_config_t *source, espclaw_telegram_config_t *target)
{
    if (target == NULL) {
        return;
    }
    if (source == NULL) {
        telegram_config_defaults(target);
        return;
    }
    *target = *source;
    refresh_telegram_config_derived_fields(target);
    target->ready = s_runtime_status.telegram_ready;
}

static esp_err_t load_telegram_config_from_nvs(espclaw_telegram_config_t *config)
{
    espclaw_telegram_config_t loaded;

    telegram_config_defaults(&loaded);
#ifdef ARDUINO
    Preferences prefs;
    if (prefs.begin(ESPCLAW_TELEGRAM_NAMESPACE, true)) {
        loaded.enabled = prefs.getUChar("enabled", loaded.enabled ? 1U : 0U) != 0U;
        loaded.poll_interval_seconds = prefs.getUInt("poll", loaded.poll_interval_seconds);
        String token = prefs.getString("token", "");
        if (token.length() > 0) {
            snprintf(loaded.bot_token, sizeof(loaded.bot_token), "%s", token.c_str());
        }
        prefs.end();
    }
#else
    nvs_handle_t handle;
    uint8_t enabled = 0;
    size_t required = 0;

    if (nvs_open(ESPCLAW_TELEGRAM_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        if (config != NULL) {
            *config = loaded;
        }
        return ESP_OK;
    }
    if (nvs_get_u8(handle, "enabled", &enabled) == ESP_OK) {
        loaded.enabled = enabled != 0U;
    }
    nvs_get_u32(handle, "poll", &loaded.poll_interval_seconds);
    required = sizeof(loaded.bot_token);
    if (nvs_get_str(handle, "token", loaded.bot_token, &required) != ESP_OK) {
        loaded.bot_token[0] = '\0';
    }
    nvs_close(handle);
#endif
    refresh_telegram_config_derived_fields(&loaded);
    if (config != NULL) {
        *config = loaded;
    }
    return ESP_OK;
}

static esp_err_t save_telegram_config_to_nvs(const espclaw_telegram_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
#ifdef ARDUINO
    Preferences prefs;
    if (!prefs.begin(ESPCLAW_TELEGRAM_NAMESPACE, false)) {
        return ESP_FAIL;
    }
    prefs.putUChar("enabled", config->enabled ? 1U : 0U);
    prefs.putUInt("poll", config->poll_interval_seconds);
    prefs.putString("token", config->bot_token);
    prefs.end();
    return ESP_OK;
#else
    nvs_handle_t handle;

    if (nvs_open(ESPCLAW_TELEGRAM_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return ESP_FAIL;
    }
    if (nvs_set_u8(handle, "enabled", config->enabled ? 1U : 0U) != ESP_OK ||
        nvs_set_u32(handle, "poll", config->poll_interval_seconds) != ESP_OK ||
        nvs_set_str(handle, "token", config->bot_token) != ESP_OK ||
        nvs_commit(handle) != ESP_OK) {
        nvs_close(handle);
        return ESP_FAIL;
    }
    nvs_close(handle);
    return ESP_OK;
#endif
}

bool espclaw_runtime_should_defer_wifi_boot(const espclaw_board_profile_t *profile, bool storage_ready)
{
    if (profile == NULL) {
        return false;
    }

    /* ESP32-CAM boards often brown out when failed SD bootstrap is followed
     * immediately by STA Wi-Fi start and full RF calibration. */
    return profile->profile_id == ESPCLAW_BOARD_PROFILE_ESP32CAM && !storage_ready;
}

bool espclaw_runtime_should_force_softap_only_boot(
    const espclaw_board_profile_t *profile,
    bool storage_ready,
    bool has_saved_wifi_credentials
)
{
    return espclaw_runtime_should_defer_wifi_boot(profile, storage_ready) && !has_saved_wifi_credentials;
}

static bool system_time_sane(void)
{
    time_t now = 0;

    time(&now);
    return now >= 1704067200; /* 2024-01-01 UTC */
}

static void maybe_start_time_sync(void)
{
    if (s_time_sync_started) {
        return;
    }
    if (!s_runtime_status.wifi_ready) {
        ESP_LOGW(TAG, "Deferring SNTP start until Wi-Fi is ready");
        return;
    }

    s_time_sync_completed = false;
#ifdef ARDUINO
    /* Arduino: configTime(gmtOffset_sec, daylightOffset_sec, server) */
    configTime(0, 0, "time.google.com");
    s_time_sync_started = true;
    ESP_LOGI(TAG, "Starting SNTP time sync (Arduino configTime)");
#else
    const esp_sntp_config_t default_config = ESP_NETIF_SNTP_DEFAULT_CONFIG("time.google.com");

    s_sntp_config = default_config;
    s_sntp_config.sync_cb = NULL;
    if (esp_netif_sntp_init(&s_sntp_config) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to start SNTP time sync");
        return;
    }
    s_time_sync_started = true;
    ESP_LOGI(TAG, "Starting SNTP time sync");
#endif
}

static esp_err_t build_runtime_provisioning_descriptor(espclaw_provisioning_descriptor_t *descriptor)
{
    const char *transport = "";
    const char *admin_url = "";
    const char *pop = "";

    if (descriptor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_runtime_status.provisioning_active) {
        if (s_ble_provisioning_active) {
            transport = "ble";
            pop = ESPCLAW_PROVISIONING_POP;
        } else if (s_softap_onboarding_active) {
            transport = "softap";
            admin_url = "http://192.168.4.1/";
        } else if (s_runtime_status.profile.provisioning != NULL) {
            transport = s_runtime_status.profile.provisioning;
        }
    }

    if (espclaw_provisioning_build_descriptor(
            s_runtime_status.provisioning_active,
            transport,
            s_runtime_status.onboarding_ssid,
            "",
            pop,
            admin_url,
            descriptor) != 0) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

static void clear_onboarding_state(void)
{
    s_softap_onboarding_active = false;
    s_ble_provisioning_active = false;
    s_runtime_status.provisioning_active = false;
    s_runtime_status.onboarding_ssid[0] = '\0';
}

static void refresh_wifi_ssid(void)
{
    s_runtime_status.wifi_ssid[0] = '\0';
#ifdef ARDUINO
    String ssid = WiFi.SSID();
    snprintf(s_runtime_status.wifi_ssid, sizeof(s_runtime_status.wifi_ssid), "%s", ssid.c_str());
#else
    wifi_config_t wifi_cfg = {0};

    if (esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg) != ESP_OK) {
        return;
    }
    snprintf(s_runtime_status.wifi_ssid, sizeof(s_runtime_status.wifi_ssid), "%s", (const char *)wifi_cfg.sta.ssid);
#endif
}

static bool has_saved_sta_credentials(void)
{
#ifdef ARDUINO
    /* WiFiManager persists credentials in NVS; WiFi.SSID() returns the stored SSID. */
    return WiFi.SSID().length() > 0;
#else
    wifi_config_t wifi_cfg = {0};

    if (esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg) != ESP_OK) {
        return false;
    }
    return wifi_cfg.sta.ssid[0] != '\0';
#endif
}

#ifndef ARDUINO
/* IDF BLE provisioning callback — not used in the Arduino build (WiFiManager used instead). */
static void provisioning_event_cb(void *user_data, wifi_prov_cb_event_t event, void *event_data)
{
    (void)user_data;
    (void)event_data;

    switch (event) {
    case WIFI_PROV_START:
        s_ble_provisioning_active = true;
        s_runtime_status.provisioning_active = true;
        break;
    case WIFI_PROV_CRED_SUCCESS:
        ESP_LOGI(TAG, "Provisioning credentials accepted");
        refresh_wifi_ssid();
        break;
    case WIFI_PROV_END:
        clear_onboarding_state();
        wifi_prov_mgr_deinit();
        if (espclaw_admin_server_start() != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start admin server after provisioning");
        }
        if (espclaw_runtime_start_operator_surfaces() != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start operator surfaces after provisioning");
        }
        break;
    default:
        break;
    }
}
#endif /* !ARDUINO */

static esp_err_t read_http_response(esp_http_client_handle_t client, char *buffer, size_t buffer_size)
{
    int total_read = 0;

    if (buffer == NULL || buffer_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    while (total_read < (int)buffer_size - 1) {
        int read_now = esp_http_client_read(client, buffer + total_read, (int)buffer_size - 1 - total_read);
        if (read_now < 0) {
            return ESP_FAIL;
        }
        if (read_now == 0) {
            break;
        }
        total_read += read_now;
    }

    buffer[total_read] = '\0';
    return ESP_OK;
}

#ifdef ESP_PLATFORM
static void delayed_behavior_autostart_task(void *arg)
{
    char behavior_log[512];
    EventBits_t bits = 0;

    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(ESPCLAW_BEHAVIOR_AUTOSTART_DELAY_MS));
    if (s_runtime_event_group != NULL) {
        bits = xEventGroupWaitBits(
            s_runtime_event_group,
            ESPCLAW_WIFI_CONNECTED_BIT,
            pdFALSE,
            pdTRUE,
            pdMS_TO_TICKS(1000)
        );
    }
    if (!s_runtime_status.storage_ready || s_runtime_status.workspace_root[0] == '\0') {
        ESP_LOGW(TAG, "Skipping delayed behavior autostart because workspace storage is unavailable");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(
        TAG,
        "Running delayed behavior autostart (wifi_ready=%d, bits=0x%02x)",
        s_runtime_status.wifi_ready ? 1 : 0,
        (unsigned int)bits
    );

    if (espclaw_behavior_start_autostart(s_runtime_status.workspace_root, behavior_log, sizeof(behavior_log)) == 0 &&
        behavior_log[0] != '\0') {
        ESP_LOGI(TAG, "Autostart behaviors: %s", behavior_log);
    }
    vTaskDelete(NULL);
}

static esp_err_t maybe_start_delayed_behavior_autostart(void)
{
    int core = espclaw_task_policy_core_for(ESPCLAW_TASK_KIND_ADMIN);

    if (xTaskCreatePinnedToCore(
            delayed_behavior_autostart_task,
            "espclaw_autostart",
            ESPCLAW_BEHAVIOR_AUTOSTART_STACK_WORDS,
            NULL,
            4,
            NULL,
            core >= 0 ? core : tskNO_AFFINITY) != pdPASS) {
        return ESP_FAIL;
    }
    return ESP_OK;
}
#endif

#ifndef ARDUINO
/* IDF WiFi event handler — replaced by WiFi.onEvent() lambda in the Arduino build. */
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (s_sta_connect_enabled) {
            esp_wifi_connect();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_runtime_status.wifi_ready = false;
        xEventGroupClearBits(s_runtime_event_group, ESPCLAW_WIFI_CONNECTED_BIT);
        if (s_sta_connect_enabled) {
            esp_wifi_connect();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        refresh_wifi_ssid();
        s_runtime_status.wifi_ready = true;
        xEventGroupSetBits(s_runtime_event_group, ESPCLAW_WIFI_CONNECTED_BIT);
        if (s_softap_onboarding_active) {
            ESP_LOGI(TAG, "Wi-Fi joined, disabling onboarding SoftAP");
            clear_onboarding_state();
            if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) {
                ESP_LOGW(TAG, "Failed to disable SoftAP after onboarding");
            }
        }
    }
}
#endif /* !ARDUINO */

static esp_err_t init_nvs_flash_store(void)
{
#ifdef ARDUINO
    /* Arduino ESP32 core initialises NVS flash in its startup code (before setup()).
     * Calling nvs_flash_init() again here is harmless but unnecessary. */
    return ESP_OK;
#else
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
#endif
}

static esp_err_t mount_workspace_storage(const espclaw_board_profile_t *profile)
{
    espclaw_storage_mount_t mount;

    if (profile == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (espclaw_storage_mount_workspace(profile, &mount) != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Workspace mount failed for backend %s",
            espclaw_storage_backend_name(espclaw_storage_backend_for_profile(profile))
        );
        return ESP_FAIL;
    }

    s_runtime_status.storage_backend = mount.backend;
    s_runtime_status.storage_total_bytes = mount.total_bytes;
    s_runtime_status.storage_used_bytes = mount.used_bytes;
    snprintf(s_runtime_status.workspace_root, sizeof(s_runtime_status.workspace_root), "%s", mount.workspace_root);
    s_runtime_status.storage_ready = true;
    ESP_LOGI(
        TAG,
        "Workspace ready backend=%s root=%s",
        espclaw_storage_backend_name(s_runtime_status.storage_backend),
        s_runtime_status.workspace_root
    );
    return ESP_OK;
}

static esp_err_t init_wifi_stack(void)
{
#ifdef ARDUINO
    /* Arduino ESP32 core already initialises the netif stack and event loop.
     * Register a WiFi event listener to update runtime status flags. */
    WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
        switch (event) {
        case ARDUINO_EVENT_WIFI_STA_CONNECTED:
            break;
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            refresh_wifi_ssid();
            s_runtime_status.wifi_ready = true;
            if (s_runtime_event_group != NULL) {
                xEventGroupSetBits(s_runtime_event_group, ESPCLAW_WIFI_CONNECTED_BIT);
            }
            maybe_start_time_sync();
            ESP_LOGI(TAG, "Wi-Fi connected SSID=%s IP=%s",
                     s_runtime_status.wifi_ssid,
                     WiFi.localIP().toString().c_str());
            break;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            s_runtime_status.wifi_ready = false;
            if (s_runtime_event_group != NULL) {
                xEventGroupClearBits(s_runtime_event_group, ESPCLAW_WIFI_CONNECTED_BIT);
            }
            break;
        default:
            break;
        }
    });
    return ESP_OK;
#else
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));
    return ESP_OK;
#endif
}

static esp_err_t start_softap_onboarding(bool force_ap_only)
{
#ifdef ARDUINO
    /* In the Arduino build, WiFiManager handles both station mode (saved credentials)
     * and AP captive-portal mode (no credentials).  This function is kept for
     * source compatibility but the real work is done by start_wifi_provisioning(). */
    (void)force_ap_only;
    return ESP_OK;
#else
    wifi_config_t sta_cfg = {0};
    wifi_config_t ap_cfg = {0};
    uint8_t mac[6] = {0};
    char service_name[32];

    if (!force_ap_only &&
        esp_wifi_get_config(WIFI_IF_STA, &sta_cfg) == ESP_OK &&
        sta_cfg.sta.ssid[0] != '\0') {
        ESP_LOGI(TAG, "Wi-Fi already configured, starting station mode");
        s_sta_connect_enabled = true;
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
        refresh_wifi_ssid();
        clear_onboarding_state();
        return ESP_OK;
    }

    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(service_name, sizeof(service_name), "ESPClaw-%02X%02X%02X", mac[3], mac[4], mac[5]);

    snprintf((char *)ap_cfg.ap.ssid, sizeof(ap_cfg.ap.ssid), "%s", service_name);
    ap_cfg.ap.ssid_len = (uint8_t)strlen((const char *)ap_cfg.ap.ssid);
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    ap_cfg.ap.ssid_hidden = 0;
    ap_cfg.ap.pmf_cfg.required = false;

    s_sta_connect_enabled = false;
    s_softap_onboarding_active = true;
    s_runtime_status.provisioning_active = true;
    snprintf(s_runtime_status.onboarding_ssid, sizeof(s_runtime_status.onboarding_ssid), "%s", service_name);

    ESP_ERROR_CHECK(esp_wifi_set_mode(force_ap_only ? WIFI_MODE_AP : WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Starting onboarding SoftAP %s%s", service_name, force_ap_only ? " (AP-only safe-start)" : "");
    ESP_LOGI(TAG, "Admin UI available at http://192.168.4.1/");
    return ESP_OK;
#endif
}

static esp_err_t start_wifi_provisioning(const espclaw_board_profile_t *profile)
{
#ifdef ARDUINO
    /* ── WiFiManager captive-portal provisioning ──────────────────────────────
     * On first boot (no saved credentials) WiFiManager starts an open AP named
     * "ESPClaw-XXXXXX".  The user connects, the portal opens, they enter Wi-Fi
     * credentials, and the device reboots into station mode automatically.
     * On subsequent boots it connects silently with the stored credentials.
     * --------------------------------------------------------------------- */
    uint8_t mac[6] = {0};
    char service_name[32];

    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(service_name, sizeof(service_name), "ESPClaw-%02X%02X%02X", mac[3], mac[4], mac[5]);
    snprintf(s_runtime_status.onboarding_ssid, sizeof(s_runtime_status.onboarding_ssid), "%s", service_name);

    WiFiManager wm;
    wm.setConfigPortalTimeout(180);  /* 3-minute portal timeout → reboot */
    wm.setConnectTimeout(30);
    wm.setDebugOutput(false);

    /* Callback: portal opened (no saved credentials) */
    wm.setAPCallback([](WiFiManager *mgr) {
        s_softap_onboarding_active = true;
        s_runtime_status.provisioning_active = true;
        s_runtime_status.profile.provisioning = "wifimanager";
        ESP_LOGI(TAG, "WiFiManager portal started: SSID=%s  URL=http://192.168.4.1/",
                 mgr->getConfigPortalSSID().c_str());
    });

    /* Callback: successfully saved new credentials */
    wm.setSaveConfigCallback([]() {
        clear_onboarding_state();
        ESP_LOGI(TAG, "WiFiManager: new credentials saved");
    });

    if (!wm.autoConnect(service_name)) {
        ESP_LOGW(TAG, "WiFiManager timed out, rebooting");
        esp_restart();
        return ESP_FAIL;
    }

    /* Connected (either from saved credentials or after portal) */
    clear_onboarding_state();
    s_sta_connect_enabled = true;
    refresh_wifi_ssid();
    s_runtime_status.wifi_ready = true;
    if (s_runtime_event_group != NULL) {
        xEventGroupSetBits(s_runtime_event_group, ESPCLAW_WIFI_CONNECTED_BIT);
    }
    maybe_start_time_sync();
    ESP_LOGI(TAG, "Wi-Fi connected SSID=%s IP=%s",
             s_runtime_status.wifi_ssid, WiFi.localIP().toString().c_str());
    return ESP_OK;

#else /* !ARDUINO — original ESP-IDF provisioning stack ─────────────────── */

#if CONFIG_ESPCLAW_ENABLE_WIFI_PROVISIONING
    wifi_prov_mgr_config_t config = {0};
    bool provisioned = false;
    const bool wants_ble = profile != NULL && profile->supports_ble_provisioning;
    const bool use_ble = wants_ble && ESPCLAW_BLE_PROVISIONING_AVAILABLE;

    if (use_ble) {
#if ESPCLAW_BLE_PROVISIONING_AVAILABLE
        config.scheme = wifi_prov_scheme_ble;
        config.scheme_event_handler = (wifi_prov_event_handler_t)WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM;
#endif
    } else {
        if (wants_ble) {
            ESP_LOGW(TAG, "BLE provisioning requested by board profile but BT is disabled in sdkconfig, using ESPClaw SoftAP onboarding");
            s_runtime_status.profile.provisioning = "softap";
        }
        return start_softap_onboarding(false);
    }
    config.app_event_handler.event_cb = provisioning_event_cb;
    config.app_event_handler.user_data = NULL;
    config.wifi_prov_conn_cfg.wifi_conn_attempts = 0;

    ESP_ERROR_CHECK(wifi_prov_mgr_init(config));
    ESP_ERROR_CHECK(wifi_prov_mgr_is_provisioned(&provisioned));

    if (!provisioned) {
        char service_name[32];
        espclaw_provisioning_descriptor_t descriptor;
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(service_name, sizeof(service_name), "ESPClaw-%02X%02X%02X", mac[3], mac[4], mac[5]);
        ESP_LOGI(TAG, "Starting provisioning service %s", service_name);
        snprintf(s_runtime_status.onboarding_ssid, sizeof(s_runtime_status.onboarding_ssid), "%s", service_name);
        if (espclaw_provisioning_build_descriptor(
                true,
                "ble",
                service_name,
                "",
                ESPCLAW_PROVISIONING_POP,
                "",
                &descriptor) == 0) {
            ESP_LOGI(TAG, "BLE provisioning PoP: %s", descriptor.pop);
            if (descriptor.qr_url[0] != '\0') {
                ESP_LOGI(TAG, "BLE provisioning helper URL: %s", descriptor.qr_url);
            }
        }
        ESP_ERROR_CHECK(wifi_prov_mgr_start_provisioning(
            WIFI_PROV_SECURITY_1,
            ESPCLAW_PROVISIONING_POP,
            service_name,
            NULL
        ));
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Wi-Fi already provisioned, starting station mode");
    wifi_prov_mgr_deinit();
    s_sta_connect_enabled = true;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    refresh_wifi_ssid();
    clear_onboarding_state();
    return ESP_OK;
#else
    (void)profile;
    return ESP_ERR_NOT_SUPPORTED;
#endif /* CONFIG_ESPCLAW_ENABLE_WIFI_PROVISIONING */
#endif /* !ARDUINO */
}

esp_err_t espclaw_runtime_wifi_scan(
    espclaw_wifi_network_t *networks,
    size_t max_networks,
    size_t *count_out
)
{
    if (count_out == NULL || networks == NULL || max_networks == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *count_out = 0;

#ifdef ARDUINO
    int found = WiFi.scanNetworks();
    if (found < 0) {
        return ESP_FAIL;
    }
    for (int i = 0; i < found && (size_t)i < max_networks; ++i) {
        snprintf(networks[i].ssid, sizeof(networks[i].ssid), "%s", WiFi.SSID(i).c_str());
        networks[i].rssi    = WiFi.RSSI(i);
        networks[i].channel = WiFi.channel(i);
        networks[i].secure  = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
        (*count_out)++;
    }
    WiFi.scanDelete();
    return ESP_OK;
#else
    uint16_t ap_count = 0;
    uint16_t desired = 0;
    wifi_scan_config_t scan_config = {0};
    wifi_ap_record_t records[16];
    uint16_t record_count = sizeof(records) / sizeof(records[0]);
    uint16_t index;

    if (esp_wifi_scan_start(&scan_config, true) != ESP_OK) {
        return ESP_FAIL;
    }
    if (esp_wifi_scan_get_ap_num(&ap_count) != ESP_OK) {
        return ESP_FAIL;
    }
    desired = ap_count < record_count ? ap_count : record_count;
    if (desired == 0) {
        return ESP_OK;
    }
    if (esp_wifi_scan_get_ap_records(&desired, records) != ESP_OK) {
        return ESP_FAIL;
    }
    for (index = 0; index < desired && index < max_networks; ++index) {
        snprintf(networks[index].ssid, sizeof(networks[index].ssid), "%s", (const char *)records[index].ssid);
        networks[index].rssi    = records[index].rssi;
        networks[index].channel = records[index].primary;
        networks[index].secure  = records[index].authmode != WIFI_AUTH_OPEN;
        (*count_out)++;
    }
    return ESP_OK;
#endif
}

esp_err_t espclaw_runtime_wifi_join(
    const char *ssid,
    const char *password,
    char *message,
    size_t message_size
)
{
    if (message != NULL && message_size > 0) {
        message[0] = '\0';
    }
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

#ifdef ARDUINO
    /* Reconnect using Arduino WiFi API; WiFiManager has already stored credentials. */
    WiFi.begin(ssid, password != NULL ? password : "");
    snprintf(s_runtime_status.wifi_ssid, sizeof(s_runtime_status.wifi_ssid), "%s", ssid);
    if (message != NULL && message_size > 0) {
        snprintf(message, message_size, "connecting to %s", ssid);
    }
    return ESP_OK;
#else
    wifi_config_t wifi_cfg = {0};

    snprintf((char *)wifi_cfg.sta.ssid, sizeof(wifi_cfg.sta.ssid), "%s", ssid);
    snprintf((char *)wifi_cfg.sta.password, sizeof(wifi_cfg.sta.password), "%s", password != NULL ? password : "");
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wifi_cfg.sta.pmf_cfg.capable = true;
    wifi_cfg.sta.pmf_cfg.required = false;

    if (s_runtime_status.provisioning_active) {
        if (s_ble_provisioning_active) {
            if (wifi_prov_mgr_configure_sta(&wifi_cfg) != ESP_OK) {
                if (message != NULL && message_size > 0) {
                    snprintf(message, message_size, "failed to submit Wi-Fi credentials");
                }
                return ESP_FAIL;
            }
            if (message != NULL && message_size > 0) {
                snprintf(message, message_size, "submitted credentials for %s", ssid);
            }
            snprintf(s_runtime_status.wifi_ssid, sizeof(s_runtime_status.wifi_ssid), "%s", ssid);
            return ESP_OK;
        }
    }

    s_sta_connect_enabled = true;
    if (esp_wifi_set_mode(s_softap_onboarding_active ? WIFI_MODE_APSTA : WIFI_MODE_STA) != ESP_OK ||
        esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg) != ESP_OK ||
        esp_wifi_connect() != ESP_OK) {
        if (message != NULL && message_size > 0) {
            snprintf(message, message_size, "failed to connect to %s", ssid);
        }
        return ESP_FAIL;
    }

    if (message != NULL && message_size > 0) {
        snprintf(message, message_size, "connecting to %s", ssid);
    }
    snprintf(s_runtime_status.wifi_ssid, sizeof(s_runtime_status.wifi_ssid), "%s", ssid);
    return ESP_OK;
#endif
}

static esp_err_t telegram_send_message(const char *token, const char *chat_id, const char *text)
{
    char url[256];
    char payload[1152];

    if (token == NULL || chat_id == NULL || text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage", token);
    espclaw_telegram_build_send_message_payload(chat_id, text, payload, sizeof(payload));

#ifdef ARDUINO
    HTTPClient http;
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(15000);
    int code = http.POST(payload);
    http.end();
    return (code >= 200 && code < 300) ? ESP_OK : ESP_FAIL;
#else
    esp_http_client_config_t config = {0};
    esp_http_client_handle_t client;

    config.url = url;
    config.transport_type = HTTP_TRANSPORT_OVER_SSL;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.timeout_ms = 15000;
    client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_FAIL;
    }
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, payload, (int)strlen(payload));
    if (esp_http_client_perform(client) != ESP_OK) {
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    if (esp_http_client_get_status_code(client) < 200 || esp_http_client_get_status_code(client) >= 300) {
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    esp_http_client_cleanup(client);
    return ESP_OK;
#endif
}

static esp_err_t http_client_write_all(esp_http_client_handle_t client, const uint8_t *data, size_t length)
{
    size_t written = 0;

    if (client == NULL || (data == NULL && length > 0U)) {
        return ESP_ERR_INVALID_ARG;
    }

    while (written < length) {
        int result = esp_http_client_write(client, (const char *)data + written, (int)(length - written));
        if (result <= 0) {
            return ESP_FAIL;
        }
        written += (size_t)result;
    }

    return ESP_OK;
}

static esp_err_t telegram_send_chat_action(const char *token, const char *chat_id, const char *action)
{
    char url[256];
    char payload[256];

    if (token == NULL || chat_id == NULL || action == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendChatAction", token);
    snprintf(payload, sizeof(payload), "{\"chat_id\":\"%s\",\"action\":\"%s\"}", chat_id, action);

#ifdef ARDUINO
    HTTPClient http;
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(15000);
    int code = http.POST(payload);
    http.end();
    return (code >= 200 && code < 300) ? ESP_OK : ESP_FAIL;
#else
    esp_http_client_config_t config = {0};
    esp_http_client_handle_t client;

    config.url = url;
    config.transport_type = HTTP_TRANSPORT_OVER_SSL;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.timeout_ms = 15000;
    client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_FAIL;
    }
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, payload, (int)strlen(payload));
    if (esp_http_client_perform(client) != ESP_OK) {
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    if (esp_http_client_get_status_code(client) < 200 || esp_http_client_get_status_code(client) >= 300) {
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    esp_http_client_cleanup(client);
    return ESP_OK;
#endif
}

static esp_err_t telegram_send_photo(
    const char *workspace_root,
    const char *token,
    const char *chat_id,
    const char *relative_path,
    const char *caption
)
{
    static const char *boundary = "----espclawTelegramBoundary7MA4YWxkTrZu0gW";
    char url[256];
    char absolute_path[512];
    char prelude[384];
    char caption_part[256];
    char file_header[384];
    char closing[96];
    uint8_t *file_buffer = NULL;
    const char *filename;
    FILE *handle = NULL;
    long file_size = 0;
    size_t closing_len;
    size_t content_length;
    esp_http_client_config_t config = {0};
    esp_http_client_handle_t client = NULL;
    esp_err_t status = ESP_FAIL;

    if (workspace_root == NULL || token == NULL || chat_id == NULL || relative_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (espclaw_workspace_resolve_path(workspace_root, relative_path, absolute_path, sizeof(absolute_path)) != 0) {
        return ESP_FAIL;
    }

    handle = fopen(absolute_path, "rb");
    if (handle == NULL) {
        return ESP_FAIL;
    }
    if (fseek(handle, 0, SEEK_END) != 0) {
        fclose(handle);
        return ESP_FAIL;
    }
    file_size = ftell(handle);
    if (file_size <= 0 || fseek(handle, 0, SEEK_SET) != 0) {
        fclose(handle);
        return ESP_FAIL;
    }

    filename = strrchr(relative_path, '/');
    filename = filename != NULL ? filename + 1 : relative_path;
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendPhoto", token);
    snprintf(
        prelude,
        sizeof(prelude),
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n"
        "%s\r\n",
        boundary,
        chat_id
    );
    caption_part[0] = '\0';
    if (caption != NULL && caption[0] != '\0') {
        snprintf(
            caption_part,
            sizeof(caption_part),
            "--%s\r\n"
            "Content-Disposition: form-data; name=\"caption\"\r\n\r\n"
            "%s\r\n",
            boundary,
            caption
        );
    }
    snprintf(
        file_header,
        sizeof(file_header),
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"photo\"; filename=\"%s\"\r\n"
        "Content-Type: image/jpeg\r\n\r\n",
        boundary,
        filename
    );
    snprintf(closing, sizeof(closing), "\r\n--%s--\r\n", boundary);
    closing_len = strlen(closing);
    content_length = strlen(prelude) + strlen(caption_part) + strlen(file_header) + (size_t)file_size + closing_len;

    config.url = url;
    config.transport_type = HTTP_TRANSPORT_OVER_SSL;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.timeout_ms = 30000;
    client = esp_http_client_init(&config);
    if (client == NULL) {
        fclose(handle);
        return ESP_FAIL;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    {
        char content_type[128];
        snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", boundary);
        esp_http_client_set_header(client, "Content-Type", content_type);
    }

    if (esp_http_client_open(client, (int)content_length) != ESP_OK) {
        goto cleanup;
    }
    if (http_client_write_all(client, (const uint8_t *)prelude, strlen(prelude)) != ESP_OK ||
        http_client_write_all(client, (const uint8_t *)caption_part, strlen(caption_part)) != ESP_OK ||
        http_client_write_all(client, (const uint8_t *)file_header, strlen(file_header)) != ESP_OK) {
        goto cleanup;
    }

    file_buffer = runtime_external_malloc(1024);
    if (file_buffer == NULL) {
        goto cleanup;
    }
    while (!feof(handle)) {
        size_t bytes_read = fread(file_buffer, 1, 1024, handle);
        if (bytes_read > 0U && http_client_write_all(client, file_buffer, bytes_read) != ESP_OK) {
            goto cleanup;
        }
        if (ferror(handle)) {
            goto cleanup;
        }
    }
    if (http_client_write_all(client, (const uint8_t *)closing, closing_len) != ESP_OK) {
        goto cleanup;
    }
    if (esp_http_client_fetch_headers(client) < 0) {
        goto cleanup;
    }
    if (esp_http_client_get_status_code(client) >= 200 && esp_http_client_get_status_code(client) < 300) {
        status = ESP_OK;
    }

cleanup:
    if (handle != NULL) {
        fclose(handle);
    }
    free(file_buffer);
    if (client != NULL) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    return status;
}

esp_err_t espclaw_runtime_get_telegram_config(espclaw_telegram_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    telegram_config_to_runtime_snapshot(&s_telegram_config, config);
    return ESP_OK;
}

bool espclaw_runtime_get_yolo_mode(void)
{
    return s_operator_yolo_mode;
}

esp_err_t espclaw_runtime_set_yolo_mode(bool enabled, char *message, size_t message_size)
{
    if (save_operator_config_to_nvs(enabled) != ESP_OK) {
        if (message != NULL && message_size > 0U) {
            snprintf(message, message_size, "failed to save YOLO mode");
        }
        return ESP_FAIL;
    }
    s_operator_yolo_mode = enabled;
    if (message != NULL && message_size > 0U) {
        snprintf(message, message_size, "YOLO mode %s.", enabled ? "enabled" : "disabled");
    }
    return ESP_OK;
}

esp_err_t espclaw_runtime_set_telegram_config(
    const espclaw_telegram_config_t *config,
    char *message,
    size_t message_size
)
{
#if CONFIG_ESPCLAW_ENABLE_TELEGRAM
    espclaw_telegram_config_t next;

    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    next = *config;
    refresh_telegram_config_derived_fields(&next);
    if (save_telegram_config_to_nvs(&next) != ESP_OK) {
        if (message != NULL && message_size > 0U) {
            snprintf(message, message_size, "failed to save Telegram config");
        }
        return ESP_FAIL;
    }
    s_telegram_config = next;
    if (!s_telegram_config.enabled || !s_telegram_config.configured) {
        s_runtime_status.telegram_ready = false;
    } else {
        maybe_start_telegram();
    }
    if (message != NULL && message_size > 0U) {
        if (!s_telegram_config.enabled) {
            snprintf(message, message_size, "Telegram polling disabled.");
        } else if (!s_telegram_config.configured) {
            snprintf(message, message_size, "Telegram token cleared. Polling idle until a token is saved.");
        } else {
            snprintf(
                message,
                message_size,
                "Telegram config saved (%s, %lu s poll interval).",
                s_telegram_config.token_hint[0] != '\0' ? s_telegram_config.token_hint : "configured",
                (unsigned long)s_telegram_config.poll_interval_seconds
            );
        }
    }
    return ESP_OK;
#else
    (void)config;
    if (message != NULL && message_size > 0U) {
        snprintf(message, message_size, "Telegram support is disabled in this firmware build.");
    }
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t espclaw_runtime_factory_reset(char *message, size_t message_size)
{
    if (espclaw_auth_store_clear() != 0) {
        if (message != NULL && message_size > 0) {
            snprintf(message, message_size, "failed to clear stored provider auth");
        }
        return ESP_FAIL;
    }

    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_restore();
    clear_onboarding_state();
    s_runtime_status.wifi_ready = false;
    s_runtime_status.wifi_ssid[0] = '\0';

    if (message != NULL && message_size > 0) {
        snprintf(message, message_size, "Cleared Wi-Fi credentials and auth store. Rebooting. SD workspace is preserved.");
    }
    esp_restart();
    return ESP_OK;
}

void espclaw_runtime_reboot(void)
{
    esp_restart();
}

static void uart_console_write_raw(const char *text)
{
    size_t written = 0;

    if (text == NULL || text[0] == '\0') {
        return;
    }
    espclaw_hw_uart_write(0, (const uint8_t *)text, strlen(text), &written);
}

static void uart_console_write_text(const char *text)
{
    size_t written = 0;
    const char *cursor = text;
    const char *chunk_start = text;

    if (text == NULL || text[0] == '\0') {
        return;
    }

    while (*cursor != '\0') {
        if (*cursor == '\n' && (cursor == text || cursor[-1] != '\r')) {
            if (cursor > chunk_start) {
                espclaw_hw_uart_write(0, (const uint8_t *)chunk_start, (size_t)(cursor - chunk_start), &written);
            }
            espclaw_hw_uart_write(0, (const uint8_t *)"\r\n", 2, &written);
            cursor++;
            chunk_start = cursor;
            continue;
        }
        cursor++;
    }

    if (cursor > chunk_start) {
        espclaw_hw_uart_write(0, (const uint8_t *)chunk_start, (size_t)(cursor - chunk_start), &written);
    }
}

static void uart_console_prompt(void)
{
    uart_console_write_text("\r\nespclaw> ");
}

static int execute_operator_request(espclaw_operator_request_t *request)
{
    if (request == NULL || request->result == NULL) {
        return -1;
    }

    switch (request->kind) {
        case ESPCLAW_OPERATOR_REQUEST_UART_CONSOLE:
            return espclaw_console_run(
                request->workspace_root,
                request->session_id,
                request->input,
                request->allow_mutations,
                request->yolo_mode,
                request->result
            );
        case ESPCLAW_OPERATOR_REQUEST_TELEGRAM_AGENT:
            return espclaw_agent_loop_run_stateless_with_history(
                request->workspace_root,
                request->session_id,
                request->input,
                request->allow_mutations,
                request->yolo_mode,
                request->history,
                request->history_count,
                request->result
            );
        case ESPCLAW_OPERATOR_REQUEST_NONE:
        default:
            return -1;
    }
}

static esp_err_t submit_operator_request(
    espclaw_operator_request_kind_t kind,
    const char *workspace_root,
    const char *session_id,
    const char *input,
    bool allow_mutations,
    bool yolo_mode,
    const espclaw_agent_history_message_t *history,
    size_t history_count,
    espclaw_agent_run_result_t *result
)
{
    if (!s_operator_worker_started || s_operator_worker_handle == NULL || s_operator_request_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (session_id == NULL || input == NULL || result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_operator_request_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }

    memset(&s_operator_request, 0, sizeof(s_operator_request));
    s_operator_request.kind = kind;
    s_operator_request.workspace_root = workspace_root;
    s_operator_request.session_id = session_id;
    s_operator_request.input = input;
    s_operator_request.allow_mutations = allow_mutations;
    s_operator_request.yolo_mode = yolo_mode;
    s_operator_request.result = result;
    s_operator_request.history = history;
    s_operator_request.history_count = history_count;
    s_operator_request.requester_task = xTaskGetCurrentTaskHandle();
    xTaskNotifyGive(s_operator_worker_handle);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    xSemaphoreGive(s_operator_request_mutex);
    return s_operator_request.status == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t espclaw_runtime_bench_operator_turn(
    espclaw_operator_surface_t surface,
    const char *session_id,
    const char *input,
    bool allow_mutations,
    bool yolo_mode,
    espclaw_agent_run_result_t *result,
    espclaw_operator_bench_metrics_t *metrics
)
{
    const espclaw_runtime_status_t *status = espclaw_runtime_status();
    const char *workspace_root = NULL;
    esp_err_t err = ESP_FAIL;

    if (session_id == NULL || input == NULL || result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(result, 0, sizeof(*result));
    if (metrics != NULL) {
        memset(metrics, 0, sizeof(*metrics));
    }
    capture_operator_bench_metrics_before(metrics);

    if (status != NULL && status->storage_ready) {
        workspace_root = status->workspace_root;
    }

    switch (surface) {
        case ESPCLAW_OPERATOR_SURFACE_WEB:
            err = espclaw_console_run(
                      workspace_root,
                      session_id,
                      input,
                      allow_mutations,
                      yolo_mode,
                      result
                  ) == 0
                ? ESP_OK
                : ESP_FAIL;
            break;
        case ESPCLAW_OPERATOR_SURFACE_UART:
            err = submit_operator_request(
                ESPCLAW_OPERATOR_REQUEST_UART_CONSOLE,
                workspace_root,
                session_id,
                input,
                allow_mutations,
                yolo_mode,
                NULL,
                0U,
                result
            );
            break;
        case ESPCLAW_OPERATOR_SURFACE_TELEGRAM:
            err = submit_operator_request(
                ESPCLAW_OPERATOR_REQUEST_TELEGRAM_AGENT,
                workspace_root,
                session_id,
                input,
                allow_mutations,
                yolo_mode,
                NULL,
                0U,
                result
            );
            break;
        default:
            err = ESP_ERR_INVALID_ARG;
            break;
    }

    capture_operator_bench_metrics_after(metrics);
    return err;
}

static void operator_worker_task(void *arg)
{
    (void)arg;

    while (true) {
        TaskHandle_t requester = NULL;

        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        requester = s_operator_request.requester_task;
        s_operator_request.status = execute_operator_request(&s_operator_request);
        s_operator_request.kind = ESPCLAW_OPERATOR_REQUEST_NONE;
        if (requester != NULL) {
            xTaskNotifyGive(requester);
        }
    }
}

static void uart_console_task(void *arg)
{
    char line[768];
    espclaw_agent_run_result_t *result = NULL;
    size_t line_length = 0;

    (void)arg;
    result = calloc(1, sizeof(*result));
    if (result == NULL) {
        free(result);
        uart_console_write_text("\r\nESPClaw serial chat unavailable: out of memory.");
        vTaskDelete(NULL);
        return;
    }
    memset(line, 0, sizeof(line));
    uart_console_write_text("\r\nESPClaw serial chat ready. Use /help for commands.");
    uart_console_prompt();

    while (true) {
        uint8_t chunk[64];
        size_t chunk_length = 0;

        if (espclaw_hw_uart_read(0, chunk, sizeof(chunk), &chunk_length) == 0 && chunk_length > 0) {
            for (size_t index = 0; index < chunk_length; ++index) {
                char c = (char)chunk[index];

                if (c == '\r' || c == '\n') {
                    if (line_length == 0) {
                        continue;
                    }
                    line[line_length] = '\0';
                    trim_trailing_whitespace(line);
                    if (line[0] != '\0') {
                        uart_console_write_raw("\r\n");
                        memset(result, 0, sizeof(*result));
                        if (submit_operator_request(
                                ESPCLAW_OPERATOR_REQUEST_UART_CONSOLE,
                                s_runtime_status.storage_ready ? s_runtime_status.workspace_root : NULL,
                                "uart0",
                                line,
                                true,
                                s_operator_yolo_mode,
                                NULL,
                                0U,
                                result) != ESP_OK) {
                            copy_text(
                                result->final_text,
                                sizeof(result->final_text),
                                "Operator worker unavailable."
                            );
                        }
                        uart_console_write_text("\r\n");
                        uart_console_write_text(result->final_text);
                    }
                    line_length = 0;
                    line[0] = '\0';
                    uart_console_prompt();
                } else if ((c == '\b' || c == 0x7F) && line_length > 0) {
                    line[--line_length] = '\0';
                    uart_console_write_text("\b \b");
                } else if (isprint((unsigned char)c) && line_length + 1 < sizeof(line)) {
                    size_t written = 0;

                    line[line_length++] = c;
                    line[line_length] = '\0';
                    espclaw_hw_uart_write(0, (const uint8_t *)&c, 1, &written);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void build_status_reply(char *buffer, size_t buffer_size)
{
    espclaw_ota_state_t ota_state = espclaw_ota_state_init();
    char admin_status[384];
    const char *provider_id = "not configured";
#ifdef ESP_PLATFORM
    espclaw_auth_profile_t *profile = (espclaw_auth_profile_t *)calloc(1, sizeof(*profile));

    if (profile != NULL) {
        espclaw_auth_profile_default(profile);
        espclaw_auth_store_load(profile);
        if (espclaw_auth_profile_is_ready(profile)) {
            provider_id = profile->provider_id;
        }
    }
#else
    espclaw_auth_profile_t profile_storage;
    espclaw_auth_profile_t *profile = &profile_storage;

    espclaw_auth_profile_default(profile);
    espclaw_auth_store_load(profile);
    if (espclaw_auth_profile_is_ready(profile)) {
        provider_id = profile->provider_id;
    }
#endif

    espclaw_render_admin_status_json(
        &s_runtime_status.profile,
        s_runtime_status.storage_backend,
        provider_id,
        "telegram",
        s_runtime_status.storage_ready,
        s_operator_yolo_mode,
        &ota_state,
        admin_status,
        sizeof(admin_status)
    );
    snprintf(buffer, buffer_size, "ESPClaw status: %s", admin_status);

#ifdef ESP_PLATFORM
    free(profile);
#endif
}

static void build_apps_list_reply(char *buffer, size_t buffer_size)
{
    char ids[32][ESPCLAW_APP_ID_MAX + 1];
    size_t count = 0;
    size_t index;
    size_t written;

    if (!s_runtime_status.storage_ready ||
        espclaw_app_collect_ids(s_runtime_status.workspace_root, ids, 32, &count) != 0 ||
        count == 0) {
        snprintf(buffer, buffer_size, "No apps installed.");
        return;
    }

    written = (size_t)snprintf(buffer, buffer_size, "Installed apps:");
    for (index = 0; index < count && written < buffer_size; ++index) {
        espclaw_app_manifest_t manifest;

        if (espclaw_app_load_manifest(s_runtime_status.workspace_root, ids[index], &manifest) == 0) {
            written += (size_t)snprintf(
                buffer + written,
                buffer_size - written,
                "%s%s(%s)",
                index == 0 ? " " : ", ",
                manifest.app_id,
                manifest.version
            );
        } else {
            written += (size_t)snprintf(buffer + written, buffer_size - written, "%s%s", index == 0 ? " " : ", ", ids[index]);
        }
    }
}

static bool parse_app_command(
    const char *message,
    char *app_id,
    size_t app_id_size,
    char *payload,
    size_t payload_size
)
{
    const char *cursor = NULL;
    const char *payload_start = NULL;
    size_t app_id_length = 0;

    if (message == NULL || app_id == NULL || payload == NULL || strncmp(message, "/app ", 5) != 0) {
        return false;
    }

    cursor = message + 5;
    while (*cursor == ' ') {
        cursor++;
    }
    if (*cursor == '\0') {
        return false;
    }

    payload_start = strchr(cursor, ' ');
    app_id_length = payload_start == NULL ? strlen(cursor) : (size_t)(payload_start - cursor);
    if (app_id_length == 0 || app_id_length >= app_id_size) {
        return false;
    }

    memcpy(app_id, cursor, app_id_length);
    app_id[app_id_length] = '\0';

    if (payload_start == NULL) {
        payload[0] = '\0';
    } else {
        while (*payload_start == ' ') {
            payload_start++;
        }
        snprintf(payload, payload_size, "%s", payload_start);
    }

    return espclaw_app_id_is_valid(app_id);
}

static bool parse_new_app_command(
    const char *message,
    char *app_id,
    size_t app_id_size
)
{
    const char *cursor = NULL;

    if (message == NULL || app_id == NULL || strncmp(message, "/newapp ", 8) != 0) {
        return false;
    }

    cursor = message + 8;
    while (*cursor == ' ') {
        cursor++;
    }
    if (*cursor == '\0' || strlen(cursor) >= app_id_size) {
        return false;
    }

    snprintf(app_id, app_id_size, "%s", cursor);
    return espclaw_app_id_is_valid(app_id);
}

static bool parse_remove_app_command(
    const char *message,
    char *app_id,
    size_t app_id_size
)
{
    const char *cursor = NULL;

    if (message == NULL || app_id == NULL || strncmp(message, "/rmapp ", 7) != 0) {
        return false;
    }

    cursor = message + 7;
    while (*cursor == ' ') {
        cursor++;
    }
    if (*cursor == '\0' || strlen(cursor) >= app_id_size) {
        return false;
    }

    snprintf(app_id, app_id_size, "%s", cursor);
    return espclaw_app_id_is_valid(app_id);
}

static void telegram_polling_task(void *arg)
{
    char *url;
    char *response;
    char *reply;
    espclaw_agent_run_result_t *run_result;

    (void)arg;
    /* Keep Telegram worker buffers out of internal RAM so the ESP32 camera can
       still reserve a contiguous DMA block when /camera is handled later. */
    url = runtime_external_malloc(ESPCLAW_TELEGRAM_URL_BYTES);
    response = runtime_external_malloc(ESPCLAW_TELEGRAM_RESPONSE_BYTES);
    reply = runtime_external_malloc(ESPCLAW_TELEGRAM_REPLY_BYTES);
    run_result = runtime_external_calloc(1, sizeof(*run_result));
    if (url == NULL || response == NULL || reply == NULL || run_result == NULL) {
        ESP_LOGE(TAG, "Telegram polling task failed to allocate working buffers");
        free(url);
        free(response);
        free(reply);
        free(run_result);
        s_runtime_status.telegram_ready = false;
        s_telegram_task_started = false;
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        espclaw_telegram_config_t config;
        EventBits_t bits = xEventGroupWaitBits(
            s_runtime_event_group,
            ESPCLAW_WIFI_CONNECTED_BIT,
            pdFALSE,
            pdTRUE,
            pdMS_TO_TICKS(5000)
        );

        if ((bits & ESPCLAW_WIFI_CONNECTED_BIT) == 0) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        telegram_config_to_runtime_snapshot(&s_telegram_config, &config);
        if (!config.enabled) {
            s_runtime_status.telegram_ready = false;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (!config.configured) {
            s_runtime_status.telegram_ready = false;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        {
            esp_http_client_config_t client_config = {0};
            esp_http_client_handle_t client;
            espclaw_telegram_update_t update;
            espclaw_telegram_chat_context_t *chat_context = NULL;

            snprintf(
                url,
                ESPCLAW_TELEGRAM_URL_BYTES,
                "https://api.telegram.org/bot%s/getUpdates?timeout=5&offset=%ld",
                config.bot_token,
                s_telegram_offset
            );

            client_config.url = url;
            client_config.transport_type = HTTP_TRANSPORT_OVER_SSL;
            client_config.crt_bundle_attach = esp_crt_bundle_attach;
            client_config.timeout_ms = 15000;
            client = esp_http_client_init(&client_config);
            if (client == NULL) {
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }

            esp_http_client_set_method(client, HTTP_METHOD_GET);
            if (esp_http_client_open(client, 0) == ESP_OK &&
                esp_http_client_fetch_headers(client) >= 0 &&
                esp_http_client_get_status_code(client) == 200 &&
                read_http_response(client, response, ESPCLAW_TELEGRAM_RESPONSE_BYTES) == ESP_OK &&
                espclaw_telegram_extract_update(response, &update)) {
                bool append_exchange = true;

                s_telegram_offset = update.update_id + 1;
                chat_context = telegram_context_for_chat(update.chat_id, true);
                telegram_send_chat_action(config.bot_token, update.chat_id, "typing");

                if (strcmp(update.text, "/camera") == 0 || strcmp(update.text, "/photo") == 0) {
                    espclaw_hw_camera_capture_t capture;

                    append_exchange = false;
                    telegram_send_chat_action(config.bot_token, update.chat_id, "upload_photo");
                    if (!s_runtime_status.storage_ready) {
                        snprintf(reply, ESPCLAW_TELEGRAM_REPLY_BYTES, "Workspace storage is not available.");
                    } else if (espclaw_hw_camera_capture(
                                   s_runtime_status.workspace_root,
                                   "telegram_capture.jpg",
                                   &capture) != 0) {
                        snprintf(reply, ESPCLAW_TELEGRAM_REPLY_BYTES, "Camera capture failed: %s", capture.error);
                    } else if (telegram_send_photo(
                                   s_runtime_status.workspace_root,
                                   config.bot_token,
                                   update.chat_id,
                                   capture.relative_path,
                                   "ESPClaw camera capture") == ESP_OK) {
                        snprintf(reply, ESPCLAW_TELEGRAM_REPLY_BYTES, "Sent camera capture: %s", capture.relative_path);
                        s_runtime_status.telegram_ready = true;
                    } else {
                        snprintf(reply, ESPCLAW_TELEGRAM_REPLY_BYTES, "Captured image but failed to upload it to Telegram.");
                    }
                } else if (strcmp(update.text, "/status") == 0) {
                    build_status_reply(reply, ESPCLAW_TELEGRAM_REPLY_BYTES);
                } else if (strcmp(update.text, "/apps") == 0) {
                    build_apps_list_reply(reply, ESPCLAW_TELEGRAM_REPLY_BYTES);
                } else if (strncmp(update.text, "/newapp ", 8) == 0) {
                    char app_id[ESPCLAW_APP_ID_MAX + 1];

                    if (!s_runtime_status.storage_ready) {
                        snprintf(reply, ESPCLAW_TELEGRAM_REPLY_BYTES, "Workspace storage is not available.");
                    } else if (!parse_new_app_command(update.text, app_id, sizeof(app_id))) {
                        snprintf(reply, ESPCLAW_TELEGRAM_REPLY_BYTES, "Usage: /newapp <app_id>");
                    } else {
                        if (espclaw_admin_scaffold_default_app(s_runtime_status.workspace_root, app_id) == 0) {
                            snprintf(reply, ESPCLAW_TELEGRAM_REPLY_BYTES, "Created app %s. Run it with /app %s", app_id, app_id);
                        } else {
                            snprintf(reply, ESPCLAW_TELEGRAM_REPLY_BYTES, "Failed to create app %s", app_id);
                        }
                    }
                } else if (strncmp(update.text, "/app ", 5) == 0) {
                    char app_id[ESPCLAW_APP_ID_MAX + 1];
                    char app_payload[256];

                    if (!s_runtime_status.storage_ready) {
                        snprintf(reply, ESPCLAW_TELEGRAM_REPLY_BYTES, "Workspace storage is not available.");
                    } else if (!parse_app_command(update.text, app_id, sizeof(app_id), app_payload, sizeof(app_payload))) {
                        snprintf(reply, ESPCLAW_TELEGRAM_REPLY_BYTES, "Usage: /app <app_id> [payload]");
                    } else if (espclaw_app_run(
                                   s_runtime_status.workspace_root,
                                   app_id,
                                   "telegram",
                                   app_payload,
                                   reply,
                                   ESPCLAW_TELEGRAM_REPLY_BYTES) != 0 && reply[0] == '\0') {
                        snprintf(reply, ESPCLAW_TELEGRAM_REPLY_BYTES, "Failed to run app %s", app_id);
                    }
                } else if (strncmp(update.text, "/rmapp ", 7) == 0) {
                    char app_id[ESPCLAW_APP_ID_MAX + 1];

                    if (!s_runtime_status.storage_ready) {
                        snprintf(reply, ESPCLAW_TELEGRAM_REPLY_BYTES, "Workspace storage is not available.");
                    } else if (!parse_remove_app_command(update.text, app_id, sizeof(app_id))) {
                        snprintf(reply, ESPCLAW_TELEGRAM_REPLY_BYTES, "Usage: /rmapp <app_id>");
                    } else if (espclaw_app_remove(s_runtime_status.workspace_root, app_id) == 0) {
                        snprintf(reply, ESPCLAW_TELEGRAM_REPLY_BYTES, "Removed app %s", app_id);
                    } else {
                        snprintf(reply, ESPCLAW_TELEGRAM_REPLY_BYTES, "Failed to remove app %s", app_id);
                    }
                } else {
                    append_exchange = false;
                    if (!s_runtime_status.storage_ready) {
                        snprintf(reply, ESPCLAW_TELEGRAM_REPLY_BYTES, "Workspace storage is not available.");
                    } else {
                        memset(run_result, 0, sizeof(*run_result));
                    }
                    if (s_runtime_status.storage_ready && (submit_operator_request(
                                   ESPCLAW_OPERATOR_REQUEST_TELEGRAM_AGENT,
                                   s_runtime_status.workspace_root,
                                   update.chat_id,
                                   update.text,
                                   s_operator_yolo_mode,
                                   s_operator_yolo_mode,
                                   chat_context != NULL ? chat_context->messages : NULL,
                                   chat_context != NULL ? chat_context->count : 0U,
                                   run_result) == ESP_OK || run_result->ok)) {
                        copy_text(reply, ESPCLAW_TELEGRAM_REPLY_BYTES, run_result->final_text);
                    } else if (s_runtime_status.storage_ready) {
                        copy_text(reply, ESPCLAW_TELEGRAM_REPLY_BYTES, run_result->final_text);
                    }
                }

                if (s_runtime_status.storage_ready && append_exchange) {
                    espclaw_session_append_message(
                        s_runtime_status.workspace_root,
                        update.chat_id,
                        "user",
                        update.text
                    );
                    espclaw_session_append_message(
                        s_runtime_status.workspace_root,
                        update.chat_id,
                        "assistant",
                        reply
                    );
                }

                telegram_context_append_message(chat_context, "user", update.text);
                telegram_context_append_message(chat_context, "assistant", reply);

                if (telegram_send_message(config.bot_token, update.chat_id, reply) == ESP_OK) {
                    s_runtime_status.telegram_ready = true;
                }
            }

            esp_http_client_close(client);
            esp_http_client_cleanup(client);
        }

        vTaskDelay(pdMS_TO_TICKS(config.poll_interval_seconds * 1000U));
    }
}

static esp_err_t maybe_start_telegram(void)
{
#if CONFIG_ESPCLAW_ENABLE_TELEGRAM
    int core = espclaw_task_policy_core_for(ESPCLAW_TASK_KIND_TELEGRAM);
    BaseType_t created;
    size_t free_internal;
    size_t largest_internal;

    if (s_telegram_task_started) {
        return ESP_OK;
    }
    if (!s_telegram_config.enabled) {
        ESP_LOGI(TAG, "Telegram polling disabled.");
        return ESP_OK;
    } else if (!s_telegram_config.configured) {
        ESP_LOGI(TAG, "Telegram polling idle: empty bot token");
        return ESP_OK;
    }

    free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_LOGI(
        TAG,
        "Starting telegram task stack=%u core=%d free_internal=%u largest_internal=%u",
        (unsigned int)ESPCLAW_TELEGRAM_STACK_BYTES,
        core,
        (unsigned int)free_internal,
        (unsigned int)largest_internal
    );
    created = create_runtime_task(
        telegram_polling_task,
        "espclaw_tg",
        ESPCLAW_TELEGRAM_STACK_BYTES,
        NULL,
        5,
        NULL,
        core >= 0 ? core : tskNO_AFFINITY
    );
    if (created != pdPASS) {
        ESP_LOGE(
            TAG,
            "Failed to start telegram task stack=%u core=%d free_internal=%u largest_internal=%u",
            (unsigned int)ESPCLAW_TELEGRAM_STACK_BYTES,
            core,
            (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
            (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
        );
        return ESP_FAIL;
    }
    s_telegram_task_started = true;
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static esp_err_t maybe_start_operator_worker(void)
{
    int core = espclaw_task_policy_core_for(ESPCLAW_TASK_KIND_CONSOLE);
    BaseType_t created;
    size_t free_internal;
    size_t largest_internal;

    if (s_operator_worker_started) {
        return ESP_OK;
    }
    if (s_operator_request_mutex == NULL) {
        s_operator_request_mutex = xSemaphoreCreateMutex();
        if (s_operator_request_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to allocate operator worker request mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_LOGI(
        TAG,
        "Starting operator worker stack=%u core=%d free_internal=%u largest_internal=%u",
        (unsigned int)ESPCLAW_OPERATOR_AGENT_STACK_BYTES,
        core,
        (unsigned int)free_internal,
        (unsigned int)largest_internal
    );
    created = create_runtime_task(
        operator_worker_task,
        "espclaw_op",
        ESPCLAW_OPERATOR_AGENT_STACK_BYTES,
        NULL,
        4,
        &s_operator_worker_handle,
        core >= 0 ? core : tskNO_AFFINITY
    );
    if (created != pdPASS) {
        ESP_LOGE(
            TAG,
            "Failed to start operator worker stack=%u core=%d free_internal=%u largest_internal=%u",
            (unsigned int)ESPCLAW_OPERATOR_AGENT_STACK_BYTES,
            core,
            (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
            (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
        );
        return ESP_FAIL;
    }
    s_operator_worker_started = true;
    return ESP_OK;
}

static esp_err_t maybe_start_uart_console(void)
{
    int core = espclaw_task_policy_core_for(ESPCLAW_TASK_KIND_CONSOLE);
    BaseType_t created;
    size_t free_internal;
    size_t largest_internal;

    if (s_uart_console_started) {
        return ESP_OK;
    }
    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_log_level_set("esp_netif_handlers", ESP_LOG_WARN);
    free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_LOGI(
        TAG,
        "Starting uart console task stack=%u core=%d free_internal=%u largest_internal=%u",
        (unsigned int)ESPCLAW_UART_CONSOLE_STACK_BYTES,
        core,
        (unsigned int)free_internal,
        (unsigned int)largest_internal
    );
    created = create_runtime_task(
        uart_console_task,
        "espclaw_uart",
        ESPCLAW_UART_CONSOLE_STACK_BYTES,
        NULL,
        4,
        NULL,
        core >= 0 ? core : tskNO_AFFINITY
    );
    if (created != pdPASS) {
        ESP_LOGE(
            TAG,
            "Failed to start uart console task stack=%u core=%d free_internal=%u largest_internal=%u",
            (unsigned int)ESPCLAW_UART_CONSOLE_STACK_BYTES,
            core,
            (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
            (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
        );
        return ESP_FAIL;
    }
    s_uart_console_started = true;
    return ESP_OK;
}

esp_err_t espclaw_runtime_start_operator_surfaces(void)
{
    esp_err_t err = maybe_start_operator_worker();

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Operator worker startup failed err=0x%x", (unsigned int)err);
        return err;
    }
    err = maybe_start_uart_console();

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART operator surface startup failed err=0x%x", (unsigned int)err);
        return err;
    }
    err = maybe_start_telegram();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Telegram operator surface startup failed err=0x%x", (unsigned int)err);
        return err;
    }
    return ESP_OK;
}

esp_err_t espclaw_runtime_start(espclaw_board_profile_id_t profile_id, espclaw_runtime_status_t *status)
{
    bool saved_wifi_credentials = false;

    memset(&s_runtime_status, 0, sizeof(s_runtime_status));
    s_softap_onboarding_active = false;
    s_ble_provisioning_active = false;
    s_sta_connect_enabled = false;
    s_time_sync_started = false;
    s_time_sync_completed = false;
    s_telegram_task_started = false;
    s_uart_console_started = false;
    s_operator_yolo_mode = true;
    memset(&s_sntp_config, 0, sizeof(s_sntp_config));
    telegram_config_defaults(&s_telegram_config);
    s_runtime_status.profile = espclaw_board_profile_for(profile_id);
    espclaw_task_policy_select(&s_runtime_status.profile);
    espclaw_board_configure_current(NULL, &s_runtime_status.profile);
    if (espclaw_hw_apply_board_boot_defaults() != 0) {
        ESP_LOGW(TAG, "Failed to apply board boot defaults");
    }
    if (espclaw_system_monitor_init(&s_runtime_status.profile) != 0) {
        return ESP_FAIL;
    }
    if (espclaw_event_watch_runtime_start() != 0) {
        return ESP_FAIL;
    }

    if (s_runtime_event_group == NULL) {
        s_runtime_event_group = xEventGroupCreate();
        if (s_runtime_event_group == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_ERROR_CHECK(init_nvs_flash_store());
    load_telegram_config_from_nvs(&s_telegram_config);
    load_operator_config_from_nvs(&s_operator_yolo_mode);
    ESP_ERROR_CHECK(init_wifi_stack());
    saved_wifi_credentials = has_saved_sta_credentials();

    if (mount_workspace_storage(&s_runtime_status.profile) != ESP_OK) {
        ESP_LOGW(TAG, "Continuing without workspace storage");
    } else {
        char boot_log[512];

        espclaw_board_configure_current(s_runtime_status.workspace_root, &s_runtime_status.profile);
        if (espclaw_hw_apply_board_boot_defaults() != 0) {
            ESP_LOGW(TAG, "Failed to apply workspace board boot defaults");
        }
        espclaw_auth_store_init(s_runtime_status.workspace_root);
        if (should_skip_boot_automation_for_reset_reason(esp_reset_reason())) {
            ESP_LOGW(
                TAG,
                "Skipping boot apps after reset reason %d to avoid a crash loop",
                (int)esp_reset_reason()
            );
        } else if (espclaw_app_run_boot_apps(s_runtime_status.workspace_root, boot_log, sizeof(boot_log)) == 0 &&
                   boot_log[0] != '\0') {
            ESP_LOGI(TAG, "Boot apps: %s", boot_log);
        }
        if (should_skip_boot_automation_for_reset_reason(esp_reset_reason())) {
            ESP_LOGW(
                TAG,
                "Skipping autostart behaviors after reset reason %d to avoid a crash loop",
                (int)esp_reset_reason()
            );
        } else {
#ifdef ESP_PLATFORM
            if (maybe_start_delayed_behavior_autostart() != ESP_OK) {
                ESP_LOGW(TAG, "Failed to schedule delayed behavior autostart");
            } else {
                ESP_LOGI(TAG, "Scheduled delayed behavior autostart");
            }
#endif
        }
    }

    if (espclaw_runtime_should_defer_wifi_boot(&s_runtime_status.profile, s_runtime_status.storage_ready)) {
        s_runtime_status.wifi_boot_deferred = true;
        if (saved_wifi_credentials) {
            ESP_LOGW(
                TAG,
                "ESP32-CAM safe-start active: workspace storage is unavailable, but saved Wi-Fi credentials exist. Preferring station boot over AP-only onboarding."
            );
            if (start_softap_onboarding(false) != ESP_OK) {
                ESP_LOGW(TAG, "Failed to start station boot with saved Wi-Fi credentials during safe-start");
            }
        } else if (espclaw_runtime_should_force_softap_only_boot(
                       &s_runtime_status.profile,
                       s_runtime_status.storage_ready,
                       saved_wifi_credentials)) {
            ESP_LOGW(
                TAG,
                "ESP32-CAM safe-start active: workspace storage is unavailable, deferring Wi-Fi boot. Check SD card media/wiring and board power, then reboot."
            );
            if (start_softap_onboarding(true) != ESP_OK) {
                ESP_LOGW(TAG, "Failed to start AP-only onboarding during safe-start");
            }
        }
    } else {
        ESP_ERROR_CHECK(start_wifi_provisioning(&s_runtime_status.profile));
    }

    if (status != NULL) {
        *status = s_runtime_status;
    }
    return ESP_OK;
}

const espclaw_runtime_status_t *espclaw_runtime_status(void)
{
    return &s_runtime_status;
}

bool espclaw_runtime_time_is_sane(void)
{
    return system_time_sane();
}

bool espclaw_runtime_wait_for_time_sync(uint32_t timeout_ms)
{
    if (system_time_sane() && s_time_sync_completed) {
        return true;
    }
    if (!s_time_sync_started) {
        maybe_start_time_sync();
    }
    if (!s_time_sync_started) {
        return false;
    }
    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout_ms)) != ESP_OK) {
        return system_time_sane() && s_time_sync_completed;
    }
    s_time_sync_completed = system_time_sane();
    return system_time_sane();
}

esp_err_t espclaw_runtime_get_provisioning_descriptor(espclaw_provisioning_descriptor_t *descriptor)
{
    return build_runtime_provisioning_descriptor(descriptor);
}
