/*
 * ESPClaw — Arduino .ino entry point
 *
 * Targets:
 *   - ESP32-S3 (full profile: PSRAM, camera optional)
 *   - ESP32 DevKitC V4 (balanced profile: no PSRAM, no camera)
 *
 * Build:
 *   arduino-cli compile --fqbn esp32:esp32:esp32s3 .
 *   arduino-cli compile --fqbn esp32:esp32:esp32    .
 *
 * Flash:
 *   arduino-cli upload --fqbn esp32:esp32:esp32s3 --port /dev/ttyUSB0 .
 *
 * OTA (after first flash):
 *   arduino-cli upload --fqbn esp32:esp32:esp32s3 --port net:<ip> .
 */

#ifdef ARDUINO
#include <Arduino.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#endif

#include "include/espclaw/admin_api.h"
#include "include/espclaw/admin_server.h"
#include "include/espclaw/admin_ui.h"
#include "include/espclaw/board_profile.h"
#include "include/espclaw/config_render.h"
#include "include/espclaw/log_buffer.h"
#include "include/espclaw/ota_manager.h"
#include "include/espclaw/ota_state.h"
#include "include/espclaw/runtime.h"
#include "include/espclaw/storage.h"

/* ------------------------------------------------------------------ */
/* Board profile selection                                             */
/* ------------------------------------------------------------------ */
/* ESP32-S3 gets the full profile (PSRAM, camera, larger budgets).    */
/* ESP32 DevKitC V4 and other classic ESP32 boards use the balanced   */
/* profile (ESP32CAM id re-used; no camera, no PSRAM assumed).        */
/* ------------------------------------------------------------------ */
#ifdef CONFIG_IDF_TARGET_ESP32S3
  #define ESPCLAW_ACTIVE_PROFILE ESPCLAW_BOARD_PROFILE_ESP32S3
#else
  #define ESPCLAW_ACTIVE_PROFILE ESPCLAW_BOARD_PROFILE_ESP32CAM
#endif

static const uint32_t ESPCLAW_OTA_CONFIRM_DELAY_MS = 15000;

static char s_default_config_buffer[2048];
static char s_status_buffer[512];

/* ------------------------------------------------------------------ */
/* Arduino setup() / loop()                                            */
/* ------------------------------------------------------------------ */

void setup()
{
    Serial.begin(115200);
    while (!Serial && millis() < 3000) {}  /* wait up to 3 s for USB CDC */

    /* ---- Core runtime init ---- */
    espclaw_log_buffer_init();
    espclaw_ota_manager_init();

    espclaw_runtime_status_t runtime_status;
    if (espclaw_runtime_start(ESPCLAW_ACTIVE_PROFILE, &runtime_status) != ESP_OK) {
        ESP_LOGE("espclaw", "Failed to start runtime");
        return;
    }

    /* ---- Admin HTTP server ---- */
    bool admin_started = (espclaw_admin_server_start() == ESP_OK);
    if (!admin_started) {
        ESP_LOGE("espclaw", "Failed to start admin server");
    }

    /* ---- Operator surfaces (UART console, Telegram) ---- */
    bool op_started = (espclaw_runtime_start_operator_surfaces() == ESP_OK);
    if (!op_started) {
        ESP_LOGE("espclaw", "Failed to start operator surfaces");
    }

    /* ---- OTA confirmation (marks current firmware as valid) ---- */
    if (admin_started) {
        char ota_msg[128];
        if (espclaw_ota_manager_schedule_confirm(ESPCLAW_OTA_CONFIRM_DELAY_MS, ota_msg, sizeof(ota_msg)) == ESP_OK) {
            ESP_LOGI("espclaw", "%s", ota_msg);
        } else {
            ESP_LOGW("espclaw", "%s", ota_msg);
        }
    } else {
        ESP_LOGW("espclaw", "Deferring OTA confirmation because admin server is unavailable");
    }

    /* ---- ArduinoOTA — network firmware upload via arduino-cli ---- */
#ifdef ARDUINO
    if (runtime_status.wifi_ready) {
        ArduinoOTA.setHostname("espclaw");
        ArduinoOTA.onStart([]() {
            ESP_LOGI("espclaw", "ArduinoOTA: starting update");
        });
        ArduinoOTA.onEnd([]() {
            ESP_LOGI("espclaw", "ArduinoOTA: update complete, rebooting");
        });
        ArduinoOTA.onError([](ota_error_t error) {
            ESP_LOGE("espclaw", "ArduinoOTA error %u", (unsigned)error);
        });
        ArduinoOTA.begin();
        ESP_LOGI("espclaw", "ArduinoOTA ready");

        /* ---- mDNS — reach device as espclaw.local ---- */
        if (MDNS.begin("espclaw")) {
            MDNS.addService("http", "tcp", 80);
            ESP_LOGI("espclaw", "mDNS started: espclaw.local");
        }
    }
#endif

    /* ---- Diagnostic boot log ---- */
    espclaw_ota_snapshot_t ota_snapshot;
    espclaw_ota_manager_snapshot(&ota_snapshot);

    size_t written = espclaw_render_default_config(
        &runtime_status.profile,
        s_default_config_buffer,
        sizeof(s_default_config_buffer)
    );
    size_t status_written = espclaw_render_admin_status_json(
        &runtime_status.profile,
        runtime_status.storage_backend,
        "openai_compat",
        "telegram",
        runtime_status.storage_ready,
        espclaw_runtime_get_yolo_mode(),
        &ota_snapshot.state,
        s_status_buffer,
        sizeof(s_status_buffer)
    );

    ESP_LOGI(
        "espclaw",
        "Booting ESPClaw profile=%s provisioning=%s storage_backend=%s "
        "storage=%d wifi=%d telegram=%d",
        runtime_status.profile.id,
        runtime_status.profile.provisioning,
        espclaw_storage_backend_name(runtime_status.storage_backend),
        runtime_status.storage_ready,
        runtime_status.wifi_ready,
        runtime_status.telegram_ready
    );
    ESP_LOGI("espclaw", "Operator surfaces started=%d", op_started);
    ESP_LOGI("espclaw", "Admin UI asset size=%u", (unsigned)espclaw_admin_ui_length());
    ESP_LOGI("espclaw", "Rendered default config bytes=%u", (unsigned)written);
    ESP_LOGI("espclaw", "Rendered admin status bytes=%u", (unsigned)status_written);
}

void loop()
{
    /* All real work runs in FreeRTOS tasks started by the runtime.   */
    /* ArduinoOTA polling must happen on the main loop task.          */
#ifdef ARDUINO
    ArduinoOTA.handle();
#endif
    delay(100);
}
