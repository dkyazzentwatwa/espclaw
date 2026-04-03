#include "espclaw/workspace.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
static const char *TAG = "espclaw_workspace";
#endif

static const espclaw_workspace_file_t WORKSPACE_FILES[] = {
    {
        "AGENTS.md",
        "# Agent Instructions\n\n"
        "You are ESPClaw, an embedded AI assistant.\n"
        "Prefer concise, hardware-aware actions and keep the user informed.\n",
    },
    {
        "IDENTITY.md",
        "# Identity\n\n"
        "ESPClaw is an ESP32-native assistant with SD-backed workspace files and local hardware tools.\n",
    },
    {
        "USER.md",
        "# User Preferences\n\n"
        "- Capture stable preferences here.\n"
        "- Prefer durable notes over transient chat history.\n",
    },
    {
        "HEARTBEAT.md",
        "# Heartbeat\n\n"
        "- Check pending maintenance tasks.\n"
        "- Report only when there is meaningful work to do.\n",
    },
    {
        "memory/MEMORY.md",
        "# Memory\n\n"
        "Long-term user and system memory belongs here.\n",
    },
    {
        "config/board.json",
        "{\n"
        "  \"variant\": \"auto\",\n"
        "  \"pins\": {\n"
        "    \"buzzer\": -1,\n"
        "    \"power_ctl\": -1,\n"
        "    \"battery_adc_pin\": -1\n"
        "  },\n"
        "  \"i2c\": {\n"
        "    \"default\": {\n"
        "      \"port\": 0,\n"
        "      \"sda\": -1,\n"
        "      \"scl\": -1,\n"
        "      \"frequency_hz\": 400000\n"
        "    }\n"
        "  },\n"
        "  \"uart\": {\n"
        "    \"console\": {\n"
        "      \"port\": 0,\n"
        "      \"tx\": -1,\n"
        "      \"rx\": -1,\n"
        "      \"baud_rate\": 115200\n"
        "    }\n"
        "  },\n"
        "  \"adc\": {\n"
        "    \"battery\": {\n"
        "      \"unit\": 1,\n"
        "      \"channel\": -1\n"
        "    }\n"
        "  }\n"
        "}\n",
    },
};

size_t espclaw_workspace_file_count(void)
{
    return sizeof(WORKSPACE_FILES) / sizeof(WORKSPACE_FILES[0]);
}

const espclaw_workspace_file_t *espclaw_workspace_file_at(size_t index)
{
    if (index >= espclaw_workspace_file_count()) {
        return NULL;
    }
    return &WORKSPACE_FILES[index];
}

const espclaw_workspace_file_t *espclaw_find_workspace_file(const char *relative_path)
{
    size_t index;

    if (relative_path == NULL) {
        return NULL;
    }

    for (index = 0; index < espclaw_workspace_file_count(); ++index) {
        if (strcmp(WORKSPACE_FILES[index].relative_path, relative_path) == 0) {
            return &WORKSPACE_FILES[index];
        }
    }

    return NULL;
}

bool espclaw_workspace_is_control_file(const char *relative_path)
{
    return espclaw_find_workspace_file(relative_path) != NULL;
}

int espclaw_workspace_resolve_path(
    const char *workspace_root,
    const char *relative_path,
    char *buffer,
    size_t buffer_size
)
{
    int written;

    if (workspace_root == NULL || relative_path == NULL || buffer == NULL || buffer_size == 0) {
        return -1;
    }

    if (relative_path[0] == '/' || strstr(relative_path, "..") != NULL) {
        return -1;
    }

    written = snprintf(buffer, buffer_size, "%s/%s", workspace_root, relative_path);
    if (written < 0 || (size_t)written >= buffer_size) {
        return -1;
    }

    return 0;
}

static int ensure_directory(const char *path)
{
    if (mkdir(path, 0x1ED) == 0 || errno == EEXIST) {
        return 0;
    }
#ifdef ESP_PLATFORM
    ESP_LOGE(TAG, "mkdir failed for %s errno=%d", path, errno);
#endif
    return -1;
}

static int ensure_parent_directories(const char *path)
{
    char buffer[512];
    char *cursor;

    if (path == NULL || snprintf(buffer, sizeof(buffer), "%s", path) >= (int)sizeof(buffer)) {
        return -1;
    }
    for (cursor = buffer + 1; *cursor != '\0'; ++cursor) {
        if (*cursor != '/') {
            continue;
        }
        *cursor = '\0';
        if (ensure_directory(buffer) != 0) {
            return -1;
        }
        *cursor = '/';
    }
    return 0;
}

static bool blob_id_valid(const char *blob_id)
{
    size_t index;

    if (blob_id == NULL || blob_id[0] == '\0') {
        return false;
    }
    for (index = 0; blob_id[index] != '\0'; ++index) {
        unsigned char c = (unsigned char)blob_id[index];

        if (!(isalnum(c) || c == '_' || c == '-')) {
            return false;
        }
    }
    return index < 65U;
}

static int build_blob_relative_path(const char *blob_id, const char *suffix, bool staging, char *buffer, size_t buffer_size)
{
    if (!blob_id_valid(blob_id) || suffix == NULL || buffer == NULL || buffer_size == 0) {
        return -1;
    }
    return snprintf(
               buffer,
               buffer_size,
               staging ? "blobs/.staging/%s%s" : "blobs/%s%s",
               blob_id,
               suffix
           ) >= (int)buffer_size
               ? -1
               : 0;
}

static void trim_line(char *line)
{
    size_t length;

    if (line == NULL) {
        return;
    }
    length = strlen(line);
    while (length > 0U && (line[length - 1U] == '\n' || line[length - 1U] == '\r')) {
        line[--length] = '\0';
    }
}

static void copy_text(char *buffer, size_t buffer_size, const char *value)
{
    if (buffer == NULL || buffer_size == 0U) {
        return;
    }
    if (value == NULL) {
        buffer[0] = '\0';
        return;
    }
    snprintf(buffer, buffer_size, "%.*s", (int)(buffer_size - 1U), value);
}

static int write_blob_meta(
    const char *workspace_root,
    const char *blob_id,
    bool staging,
    const char *target_path,
    const char *content_type,
    size_t bytes_written
)
{
    char relative_path[160];
    char absolute_path[512];
    FILE *handle;

    if (build_blob_relative_path(blob_id, ".meta", staging, relative_path, sizeof(relative_path)) != 0 ||
        espclaw_workspace_resolve_path(workspace_root, relative_path, absolute_path, sizeof(absolute_path)) != 0 ||
        ensure_parent_directories(absolute_path) != 0) {
        return -1;
    }

    handle = fopen(absolute_path, "w");
    if (handle == NULL) {
        return -1;
    }
    fprintf(handle, "blob_id=%s\n", blob_id);
    fprintf(handle, "target_path=%s\n", target_path != NULL ? target_path : "");
    fprintf(handle, "content_type=%s\n", content_type != NULL ? content_type : "");
    fprintf(handle, "bytes_written=%lu\n", (unsigned long)bytes_written);
    fclose(handle);
    return 0;
}

static int read_blob_meta(
    const char *workspace_root,
    const char *blob_id,
    bool staging,
    espclaw_workspace_blob_status_t *status_out
)
{
    char relative_path[160];
    char absolute_path[512];
    FILE *handle;
    char line[384];

    if (status_out == NULL ||
        build_blob_relative_path(blob_id, ".meta", staging, relative_path, sizeof(relative_path)) != 0 ||
        espclaw_workspace_resolve_path(workspace_root, relative_path, absolute_path, sizeof(absolute_path)) != 0) {
        return -1;
    }

    handle = fopen(absolute_path, "r");
    if (handle == NULL) {
        return -1;
    }

    memset(status_out, 0, sizeof(*status_out));
    snprintf(status_out->blob_id, sizeof(status_out->blob_id), "%s", blob_id);
    status_out->stage = staging ? ESPCLAW_WORKSPACE_BLOB_STAGE_OPEN : ESPCLAW_WORKSPACE_BLOB_STAGE_COMMITTED;
    while (fgets(line, sizeof(line), handle) != NULL) {
        trim_line(line);
        if (strncmp(line, "target_path=", 12) == 0) {
            copy_text(status_out->target_path, sizeof(status_out->target_path), line + 12);
        } else if (strncmp(line, "content_type=", 13) == 0) {
            copy_text(status_out->content_type, sizeof(status_out->content_type), line + 13);
        } else if (strncmp(line, "bytes_written=", 14) == 0) {
            status_out->bytes_written = (size_t)strtoul(line + 14, NULL, 10);
        }
    }
    fclose(handle);
    return 0;
}

static int stat_blob_data_size(const char *workspace_root, const char *blob_id, bool staging, size_t *bytes_out)
{
    char relative_path[160];
    char absolute_path[512];
    struct stat file_stat;

    if (bytes_out == NULL ||
        build_blob_relative_path(blob_id, ".part", staging, relative_path, sizeof(relative_path)) != 0 ||
        espclaw_workspace_resolve_path(workspace_root, relative_path, absolute_path, sizeof(absolute_path)) != 0 ||
        stat(absolute_path, &file_stat) != 0) {
        return -1;
    }

    *bytes_out = (size_t)file_stat.st_size;
    return 0;
}

static int bootstrap_file(const char *workspace_root, const espclaw_workspace_file_t *file)
{
    char path[512];
    FILE *handle;

    if (workspace_root == NULL || file == NULL) {
        return -1;
    }

    if (espclaw_workspace_resolve_path(workspace_root, file->relative_path, path, sizeof(path)) != 0) {
        return -1;
    }

    handle = fopen(path, "r");
    if (handle != NULL) {
        fclose(handle);
        return 0;
    }

    handle = fopen(path, "w");
    if (handle == NULL) {
#ifdef ESP_PLATFORM
        ESP_LOGE(TAG, "fopen write failed for %s errno=%d", path, errno);
#endif
        return -1;
    }

    fputs(file->default_content, handle);
    fclose(handle);
    return 0;
}

int espclaw_workspace_bootstrap(const char *workspace_root)
{
    char path[512];
    size_t index;

    if (workspace_root == NULL || workspace_root[0] == '\0') {
        return -1;
    }

    if (ensure_directory(workspace_root) != 0) {
        return -1;
    }

    if (espclaw_workspace_resolve_path(workspace_root, "memory", path, sizeof(path)) != 0 || ensure_directory(path) != 0) {
        return -1;
    }
    if (espclaw_workspace_resolve_path(workspace_root, "sessions", path, sizeof(path)) != 0 || ensure_directory(path) != 0) {
        return -1;
    }
    if (espclaw_workspace_resolve_path(workspace_root, "media", path, sizeof(path)) != 0 || ensure_directory(path) != 0) {
        return -1;
    }
    if (espclaw_workspace_resolve_path(workspace_root, "config", path, sizeof(path)) != 0 || ensure_directory(path) != 0) {
        return -1;
    }
    if (espclaw_workspace_resolve_path(workspace_root, "apps", path, sizeof(path)) != 0 || ensure_directory(path) != 0) {
        return -1;
    }
    if (espclaw_workspace_resolve_path(workspace_root, "components", path, sizeof(path)) != 0 || ensure_directory(path) != 0) {
        return -1;
    }
    if (espclaw_workspace_resolve_path(workspace_root, "lib", path, sizeof(path)) != 0 || ensure_directory(path) != 0) {
        return -1;
    }
    if (espclaw_workspace_resolve_path(workspace_root, "blobs", path, sizeof(path)) != 0 || ensure_directory(path) != 0) {
        return -1;
    }
    if (espclaw_workspace_resolve_path(workspace_root, "blobs/.staging", path, sizeof(path)) != 0 || ensure_directory(path) != 0) {
        return -1;
    }
    if (espclaw_workspace_resolve_path(workspace_root, "behaviors", path, sizeof(path)) != 0 || ensure_directory(path) != 0) {
        return -1;
    }

    for (index = 0; index < espclaw_workspace_file_count(); ++index) {
        if (bootstrap_file(workspace_root, &WORKSPACE_FILES[index]) != 0) {
            return -1;
        }
    }

    return 0;
}

int espclaw_workspace_read_file(
    const char *workspace_root,
    const char *relative_path,
    char *buffer,
    size_t buffer_size
)
{
    char path[512];
    FILE *handle;
    size_t bytes_read;

    if (workspace_root == NULL || relative_path == NULL || buffer == NULL || buffer_size == 0) {
        return -1;
    }

    if (espclaw_workspace_resolve_path(workspace_root, relative_path, path, sizeof(path)) != 0) {
        return -1;
    }

    handle = fopen(path, "r");
    if (handle == NULL) {
        return -1;
    }

    bytes_read = fread(buffer, 1, buffer_size - 1, handle);
    buffer[bytes_read] = '\0';
    fclose(handle);
    return 0;
}

int espclaw_workspace_write_file(
    const char *workspace_root,
    const char *relative_path,
    const char *content
)
{
    char path[512];
    FILE *handle;

    if (workspace_root == NULL || relative_path == NULL || content == NULL) {
        return -1;
    }

    if (espclaw_workspace_resolve_path(workspace_root, relative_path, path, sizeof(path)) != 0) {
        return -1;
    }

    if (ensure_parent_directories(path) != 0) {
        return -1;
    }

    handle = fopen(path, "w");
    if (handle == NULL) {
        return -1;
    }

    fputs(content, handle);
    fclose(handle);
    return 0;
}

int espclaw_workspace_blob_begin(
    const char *workspace_root,
    const char *blob_id,
    const char *target_path,
    const char *content_type
)
{
    char data_relative[160];
    char data_absolute[512];
    char meta_relative[160];
    char meta_absolute[512];
    char target_absolute[512];
    const char *effective_target = target_path;
    char default_target[160];
    FILE *handle;

    if (workspace_root == NULL || !blob_id_valid(blob_id) || espclaw_workspace_bootstrap(workspace_root) != 0) {
        return -1;
    }
    if (effective_target == NULL || effective_target[0] == '\0') {
        if (snprintf(default_target, sizeof(default_target), "blobs/%s.bin", blob_id) >= (int)sizeof(default_target)) {
            return -1;
        }
        effective_target = default_target;
    }
    if (espclaw_workspace_resolve_path(workspace_root, effective_target, target_absolute, sizeof(target_absolute)) != 0 ||
        build_blob_relative_path(blob_id, ".part", true, data_relative, sizeof(data_relative)) != 0 ||
        espclaw_workspace_resolve_path(workspace_root, data_relative, data_absolute, sizeof(data_absolute)) != 0 ||
        build_blob_relative_path(blob_id, ".meta", true, meta_relative, sizeof(meta_relative)) != 0 ||
        espclaw_workspace_resolve_path(workspace_root, meta_relative, meta_absolute, sizeof(meta_absolute)) != 0 ||
        ensure_parent_directories(data_absolute) != 0) {
        return -1;
    }

    handle = fopen(data_absolute, "wb");
    if (handle == NULL) {
        return -1;
    }
    fclose(handle);
    unlink(meta_absolute);
    return write_blob_meta(workspace_root, blob_id, true, effective_target, content_type, 0U);
}

int espclaw_workspace_blob_append(
    const char *workspace_root,
    const char *blob_id,
    const void *data,
    size_t data_size,
    size_t *bytes_written_out
)
{
    char data_relative[160];
    char data_absolute[512];
    FILE *handle;
    espclaw_workspace_blob_status_t status;
    size_t actual_bytes = 0;

    if (workspace_root == NULL || !blob_id_valid(blob_id) || data == NULL || data_size == 0U) {
        return -1;
    }
    if (read_blob_meta(workspace_root, blob_id, true, &status) != 0 ||
        build_blob_relative_path(blob_id, ".part", true, data_relative, sizeof(data_relative)) != 0 ||
        espclaw_workspace_resolve_path(workspace_root, data_relative, data_absolute, sizeof(data_absolute)) != 0) {
        return -1;
    }

    handle = fopen(data_absolute, "ab");
    if (handle == NULL) {
        return -1;
    }
    actual_bytes = fwrite(data, 1, data_size, handle);
    fclose(handle);
    if (actual_bytes != data_size) {
        return -1;
    }
    status.bytes_written += actual_bytes;
    if (write_blob_meta(workspace_root, blob_id, true, status.target_path, status.content_type, status.bytes_written) != 0) {
        return -1;
    }
    if (bytes_written_out != NULL) {
        *bytes_written_out = status.bytes_written;
    }
    return 0;
}

int espclaw_workspace_blob_commit(
    const char *workspace_root,
    const char *blob_id,
    char *committed_path,
    size_t committed_path_size,
    size_t *bytes_written_out
)
{
    char data_relative[160];
    char data_absolute[512];
    char target_absolute[512];
    char staging_meta_relative[160];
    char staging_meta_absolute[512];
    espclaw_workspace_blob_status_t status;
    size_t actual_bytes = 0;

    if (workspace_root == NULL || !blob_id_valid(blob_id) || read_blob_meta(workspace_root, blob_id, true, &status) != 0) {
        return -1;
    }
    if (stat_blob_data_size(workspace_root, blob_id, true, &actual_bytes) == 0) {
        status.bytes_written = actual_bytes;
    }
    if (build_blob_relative_path(blob_id, ".part", true, data_relative, sizeof(data_relative)) != 0 ||
        espclaw_workspace_resolve_path(workspace_root, data_relative, data_absolute, sizeof(data_absolute)) != 0 ||
        espclaw_workspace_resolve_path(workspace_root, status.target_path, target_absolute, sizeof(target_absolute)) != 0 ||
        ensure_parent_directories(target_absolute) != 0) {
        return -1;
    }
    unlink(target_absolute);
    if (rename(data_absolute, target_absolute) != 0) {
        return -1;
    }
    if (write_blob_meta(workspace_root, blob_id, false, status.target_path, status.content_type, status.bytes_written) != 0) {
        return -1;
    }
    if (build_blob_relative_path(blob_id, ".meta", true, staging_meta_relative, sizeof(staging_meta_relative)) == 0 &&
        espclaw_workspace_resolve_path(workspace_root, staging_meta_relative, staging_meta_absolute, sizeof(staging_meta_absolute)) == 0) {
        unlink(staging_meta_absolute);
    }
    if (committed_path != NULL && committed_path_size > 0U) {
        snprintf(committed_path, committed_path_size, "%s", status.target_path);
    }
    if (bytes_written_out != NULL) {
        *bytes_written_out = status.bytes_written;
    }
    return 0;
}

int espclaw_workspace_blob_status(
    const char *workspace_root,
    const char *blob_id,
    espclaw_workspace_blob_status_t *status_out
)
{
    size_t actual_bytes = 0;

    if (workspace_root == NULL || !blob_id_valid(blob_id) || status_out == NULL) {
        return -1;
    }
    if (read_blob_meta(workspace_root, blob_id, false, status_out) == 0) {
        char absolute_path[512];
        struct stat file_stat;

        if (espclaw_workspace_resolve_path(workspace_root, status_out->target_path, absolute_path, sizeof(absolute_path)) == 0 &&
            stat(absolute_path, &file_stat) == 0) {
            status_out->bytes_written = (size_t)file_stat.st_size;
        }
        return 0;
    }
    if (read_blob_meta(workspace_root, blob_id, true, status_out) == 0) {
        if (stat_blob_data_size(workspace_root, blob_id, true, &actual_bytes) == 0) {
            status_out->bytes_written = actual_bytes;
        }
        return 0;
    }
    memset(status_out, 0, sizeof(*status_out));
    snprintf(status_out->blob_id, sizeof(status_out->blob_id), "%s", blob_id);
    status_out->stage = ESPCLAW_WORKSPACE_BLOB_STAGE_NONE;
    return -1;
}
