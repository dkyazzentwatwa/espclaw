#include "espclaw/system_monitor.h"

#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_freertos_hooks.h"
#ifndef ARDUINO
/* esp_private headers are internal IDF headers; Arduino uses getCpuFrequencyMhz() instead. */
#include "esp_private/esp_clk.h"
#endif
#include "esp_heap_caps.h"
#include "esp_image_format.h"
#include "esp_flash.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#endif

typedef struct {
    bool initialized;
    unsigned int cpu_cores;
    unsigned long long last_sample_us;
    unsigned long idle_count[ESPCLAW_SYSTEM_MONITOR_MAX_CORES];
    unsigned long last_idle_count[ESPCLAW_SYSTEM_MONITOR_MAX_CORES];
    unsigned long idle_baseline[ESPCLAW_SYSTEM_MONITOR_MAX_CORES];
} espclaw_system_monitor_state_t;

static espclaw_system_monitor_state_t s_monitor_state;

#ifdef ESP_PLATFORM
static IRAM_ATTR bool idle_hook_cpu0(void)
{
    s_monitor_state.idle_count[0]++;
    return false;
}

static IRAM_ATTR bool idle_hook_cpu1(void)
{
    s_monitor_state.idle_count[1]++;
    return false;
}
#endif

int espclaw_system_monitor_init(const espclaw_board_profile_t *profile)
{
    unsigned int cpu_cores = profile != NULL ? (unsigned int)profile->cpu_cores : 1U;

    memset(&s_monitor_state, 0, sizeof(s_monitor_state));
    if (cpu_cores == 0U) {
        cpu_cores = 1U;
    }
    if (cpu_cores > ESPCLAW_SYSTEM_MONITOR_MAX_CORES) {
        cpu_cores = ESPCLAW_SYSTEM_MONITOR_MAX_CORES;
    }
    s_monitor_state.cpu_cores = cpu_cores;

#ifdef ESP_PLATFORM
    if (esp_register_freertos_idle_hook_for_cpu(idle_hook_cpu0, 0) != ESP_OK) {
        return -1;
    }
    if (cpu_cores > 1U && esp_register_freertos_idle_hook_for_cpu(idle_hook_cpu1, 1) != ESP_OK) {
        return -1;
    }
#endif

    s_monitor_state.initialized = true;
    return 0;
}

int espclaw_system_monitor_snapshot(
    const espclaw_board_profile_t *profile,
    size_t workspace_total_bytes,
    size_t workspace_used_bytes,
    espclaw_system_monitor_snapshot_t *snapshot
)
{
    unsigned int index;
    unsigned int cpu_cores = profile != NULL ? (unsigned int)profile->cpu_cores : 1U;

    if (snapshot == NULL) {
        return -1;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    if (cpu_cores == 0U) {
        cpu_cores = 1U;
    }
    if (cpu_cores > ESPCLAW_SYSTEM_MONITOR_MAX_CORES) {
        cpu_cores = ESPCLAW_SYSTEM_MONITOR_MAX_CORES;
    }

    snapshot->available = true;
    snapshot->memory_class = profile != NULL ? profile->runtime_budget.memory_class : "unknown";
    snapshot->cpu_cores = cpu_cores;
    snapshot->dual_core = cpu_cores > 1U;
    snapshot->workspace_total_bytes = workspace_total_bytes;
    snapshot->workspace_used_bytes = workspace_used_bytes;
    if (profile != NULL) {
        snapshot->agent_estimated_heap_bytes = profile->runtime_budget.agent_estimated_heap_bytes;
        snapshot->recommended_free_heap_bytes = profile->runtime_budget.recommended_free_heap_bytes;
        snapshot->agent_request_buffer_bytes = profile->runtime_budget.agent_request_buffer_max;
        snapshot->agent_response_buffer_bytes = profile->runtime_budget.agent_response_buffer_max;
        snapshot->agent_codex_items_bytes = profile->runtime_budget.agent_codex_items_max;
        snapshot->agent_instructions_bytes = profile->runtime_budget.agent_instructions_max;
        snapshot->agent_tool_result_bytes = profile->runtime_budget.agent_tool_result_max;
        snapshot->agent_image_data_bytes = profile->runtime_budget.agent_image_data_max;
        snapshot->agent_history_slots = profile->runtime_budget.agent_history_max;
    }

#ifdef ESP_PLATFORM
    {
        uint32_t flash_size = 0;
        const esp_partition_t *running = esp_ota_get_running_partition();
        esp_image_metadata_t metadata;
        unsigned long long now_us = (unsigned long long)esp_timer_get_time();

#ifdef ARDUINO
        snapshot->cpu_mhz = (unsigned int)getCpuFrequencyMhz();
#else
        snapshot->cpu_mhz = (unsigned int)(esp_clk_cpu_freq() / 1000000U);
#endif
        snapshot->ram_total_bytes = heap_caps_get_total_size(MALLOC_CAP_8BIT);
        snapshot->ram_free_bytes = heap_caps_get_free_size(MALLOC_CAP_8BIT);
        snapshot->ram_min_free_bytes = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
        snapshot->ram_largest_free_block_bytes = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
            snapshot->flash_chip_size_bytes = flash_size;
        }
        if (running != NULL) {
            esp_partition_pos_t pos = {
                .offset = running->address,
                .size = running->size,
            };

            snapshot->app_partition_size_bytes = running->size;
            memset(&metadata, 0, sizeof(metadata));
            if (esp_image_get_metadata(&pos, &metadata) == ESP_OK) {
                snapshot->app_image_size_bytes = metadata.image_len;
            }
        }

        if (s_monitor_state.initialized) {
            if (s_monitor_state.last_sample_us == 0ULL) {
                s_monitor_state.last_sample_us = now_us;
                for (index = 0; index < cpu_cores; ++index) {
                    s_monitor_state.last_idle_count[index] = s_monitor_state.idle_count[index];
                    s_monitor_state.idle_baseline[index] = 0;
                    snapshot->cpu_load_percent[index] = 0U;
                }
            } else {
                for (index = 0; index < cpu_cores; ++index) {
                    unsigned long delta_idle = s_monitor_state.idle_count[index] - s_monitor_state.last_idle_count[index];
                    unsigned long baseline = s_monitor_state.idle_baseline[index];

                    if (delta_idle > baseline) {
                        baseline = delta_idle;
                    } else if (baseline > 0UL) {
                        baseline = (baseline * 31UL + delta_idle) / 32UL;
                    }
                    s_monitor_state.idle_baseline[index] = baseline;
                    s_monitor_state.last_idle_count[index] = s_monitor_state.idle_count[index];

                    if (baseline == 0UL || delta_idle >= baseline) {
                        snapshot->cpu_load_percent[index] = 0U;
                    } else {
                        /* Keep CPU load estimation lightweight by deriving it from idle hook deltas. */
                        unsigned long busy = ((baseline - delta_idle) * 100UL) / baseline;
                        if (busy > 100UL) {
                            busy = 100UL;
                        }
                        snapshot->cpu_load_percent[index] = (unsigned int)busy;
                    }
                }
                s_monitor_state.last_sample_us = now_us;
            }
        }
    }
#else
    snapshot->cpu_mhz = 160U;
    snapshot->ram_total_bytes = 256U * 1024U;
    snapshot->ram_free_bytes = 192U * 1024U;
    snapshot->ram_min_free_bytes = 192U * 1024U;
    snapshot->ram_largest_free_block_bytes = 128U * 1024U;
    snapshot->flash_chip_size_bytes = 4U * 1024U * 1024U;
    snapshot->app_partition_size_bytes = 0;
    snapshot->app_image_size_bytes = 0;
    for (index = 0; index < cpu_cores; ++index) {
        snapshot->cpu_load_percent[index] = 0U;
    }
#endif

    return 0;
}
