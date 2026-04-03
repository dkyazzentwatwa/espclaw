#include "espclaw/app_runtime.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "espclaw/board_config.h"
#include "espclaw/event_watch.h"
#include "espclaw/hardware.h"
#include "espclaw/task_runtime.h"
#include "espclaw/tool_catalog.h"
#include "espclaw/workspace.h"
#include "espclaw/web_tools.h"

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "nvs.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#elif defined(ESPCLAW_HOST_LUA)
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#endif

#define ESPCLAW_APP_LIST_BUFFER 512
#define ESPCLAW_APP_CACHE_NAMESPACE "espclaw_app"
#define ESPCLAW_APP_MANIFEST_KEY_PREFIX "a_"

static const char *DEFAULT_APP_PERMISSIONS = "fs.read";
static const char *DEFAULT_APP_TRIGGERS = "boot,telegram,manual";

#ifdef ESP_PLATFORM
static const char *TAG = "espclaw_apps";
#endif

typedef struct {
    const char *workspace_root;
    const espclaw_app_manifest_t *manifest;
    const char *app_id;
} espclaw_lua_context_t;

struct espclaw_app_vm {
#if defined(ESP_PLATFORM) || defined(ESPCLAW_HOST_LUA)
    lua_State *state;
    int module_ref;
#endif
    espclaw_app_manifest_t manifest;
    espclaw_lua_context_t context;
    char workspace_root[512];
    char script_path[512];
};

static uint32_t app_id_hash(const char *app_id)
{
    uint32_t hash = 2166136261u;
    const unsigned char *cursor = (const unsigned char *)app_id;

    while (cursor != NULL && *cursor != '\0') {
        hash ^= (uint32_t)(*cursor++);
        hash *= 16777619u;
    }

    return hash;
}

#ifdef ESP_PLATFORM
static void app_manifest_nvs_key(const char *app_id, char *buffer, size_t buffer_size)
{
    if (buffer == NULL || buffer_size == 0) {
        return;
    }
    snprintf(
        buffer,
        buffer_size,
        ESPCLAW_APP_MANIFEST_KEY_PREFIX "%08" PRIx32,
        app_id_hash(app_id != NULL ? app_id : "")
    );
}

static int app_manifest_cache_store_json(const char *app_id, const char *json)
{
    char key[15];
    nvs_handle_t handle;

    if (!espclaw_app_id_is_valid(app_id) || json == NULL) {
        return -1;
    }
    app_manifest_nvs_key(app_id, key, sizeof(key));
    if (nvs_open(ESPCLAW_APP_CACHE_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return -1;
    }
    if (nvs_set_str(handle, key, json) != ESP_OK || nvs_commit(handle) != ESP_OK) {
        nvs_close(handle);
        return -1;
    }
    nvs_close(handle);
    return 0;
}

static int app_manifest_cache_load_json(const char *app_id, char *buffer, size_t buffer_size)
{
    char key[15];
    nvs_handle_t handle;
    size_t required = buffer_size;

    if (!espclaw_app_id_is_valid(app_id) || buffer == NULL || buffer_size == 0) {
        return -1;
    }
    buffer[0] = '\0';
    app_manifest_nvs_key(app_id, key, sizeof(key));
    if (nvs_open(ESPCLAW_APP_CACHE_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return -1;
    }
    if (nvs_get_str(handle, key, buffer, &required) != ESP_OK) {
        nvs_close(handle);
        return -1;
    }
    nvs_close(handle);
    return 0;
}

static void app_manifest_cache_remove(const char *app_id)
{
    char key[15];
    nvs_handle_t handle;

    if (!espclaw_app_id_is_valid(app_id)) {
        return;
    }
    app_manifest_nvs_key(app_id, key, sizeof(key));
    if (nvs_open(ESPCLAW_APP_CACHE_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    nvs_erase_key(handle, key);
    nvs_commit(handle);
    nvs_close(handle);
}
#else
static int app_manifest_cache_store_json(const char *app_id, const char *json)
{
    (void)app_id;
    (void)json;
    return 0;
}

static int app_manifest_cache_load_json(const char *app_id, char *buffer, size_t buffer_size)
{
    (void)app_id;
    (void)buffer;
    (void)buffer_size;
    return -1;
}

static void app_manifest_cache_remove(const char *app_id)
{
    (void)app_id;
}
#endif

static bool copy_json_string_value(const char *start, char *buffer, size_t buffer_size)
{
    size_t index = 0;
    const char *cursor = start;

    if (start == NULL || buffer == NULL || buffer_size == 0 || *start != '"') {
        return false;
    }

    cursor++;
    while (*cursor != '\0' && *cursor != '"' && index + 1 < buffer_size) {
        if (*cursor == '\\' && cursor[1] != '\0') {
            cursor++;
            switch (*cursor) {
            case 'n':
                buffer[index++] = '\n';
                break;
            case 'r':
                buffer[index++] = '\r';
                break;
            case 't':
                buffer[index++] = '\t';
                break;
            default:
                buffer[index++] = *cursor;
                break;
            }
        } else {
            buffer[index++] = *cursor;
        }
        cursor++;
    }

    if (*cursor != '"') {
        return false;
    }

    buffer[index] = '\0';
    return true;
}

static const char *find_key(const char *json, const char *key)
{
    char pattern[64];

    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    return strstr(json, pattern);
}

static bool extract_string_after_key(const char *json, const char *key, char *buffer, size_t buffer_size)
{
    const char *key_start = find_key(json, key);
    const char *value_start = NULL;

    if (key_start == NULL) {
        return false;
    }

    value_start = strchr(key_start, ':');
    if (value_start == NULL) {
        return false;
    }
    value_start++;

    while (*value_start == ' ' || *value_start == '\n' || *value_start == '\r' || *value_start == '\t') {
        value_start++;
    }

    return copy_json_string_value(value_start, buffer, buffer_size);
}

static bool extract_string_array_after_key(
    const char *json,
    const char *key,
    char items[][ESPCLAW_APP_PERMISSION_NAME_MAX + 1],
    size_t max_items,
    size_t item_size,
    size_t *count_out
)
{
    const char *key_start = find_key(json, key);
    const char *cursor = NULL;
    size_t count = 0;

    if (count_out == NULL) {
        return false;
    }
    *count_out = 0;

    if (key_start == NULL) {
        return true;
    }

    cursor = strchr(key_start, '[');
    if (cursor == NULL) {
        return false;
    }
    cursor++;

    while (*cursor != '\0' && *cursor != ']') {
        while (*cursor == ' ' || *cursor == '\n' || *cursor == '\r' || *cursor == '\t' || *cursor == ',') {
            cursor++;
        }

        if (*cursor == ']') {
            break;
        }
        if (*cursor != '"' || count >= max_items) {
            return false;
        }

        if (!copy_json_string_value(cursor, items[count], item_size)) {
            return false;
        }

        cursor = strchr(cursor + 1, '"');
        if (cursor == NULL) {
            return false;
        }
        cursor++;
        count++;
    }

    *count_out = count;
    return true;
}

static bool extract_trigger_array_after_key(
    const char *json,
    const char *key,
    char items[][ESPCLAW_APP_TRIGGER_NAME_MAX + 1],
    size_t max_items,
    size_t item_size,
    size_t *count_out
)
{
    const char *key_start = find_key(json, key);
    const char *cursor = NULL;
    size_t count = 0;

    if (count_out == NULL) {
        return false;
    }
    *count_out = 0;

    if (key_start == NULL) {
        return true;
    }

    cursor = strchr(key_start, '[');
    if (cursor == NULL) {
        return false;
    }
    cursor++;

    while (*cursor != '\0' && *cursor != ']') {
        while (*cursor == ' ' || *cursor == '\n' || *cursor == '\r' || *cursor == '\t' || *cursor == ',') {
            cursor++;
        }

        if (*cursor == ']') {
            break;
        }
        if (*cursor != '"' || count >= max_items) {
            return false;
        }

        if (!copy_json_string_value(cursor, items[count], item_size)) {
            return false;
        }

        cursor = strchr(cursor + 1, '"');
        if (cursor == NULL) {
            return false;
        }
        cursor++;
        count++;
    }

    *count_out = count;
    return true;
}

static int ensure_directory(const char *path)
{
    if (mkdir(path, 0x1ED) == 0 || errno == EEXIST) {
        return 0;
    }
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

static int copy_file_contents(const char *source_path, const char *destination_path)
{
    FILE *source = NULL;
    FILE *destination = NULL;
    unsigned char chunk[2048];
    size_t bytes_read;

    if (source_path == NULL || destination_path == NULL || ensure_parent_directories(destination_path) != 0) {
        return -1;
    }
    source = fopen(source_path, "rb");
    if (source == NULL) {
        return -1;
    }
    destination = fopen(destination_path, "wb");
    if (destination == NULL) {
        fclose(source);
        return -1;
    }
    while ((bytes_read = fread(chunk, 1, sizeof(chunk), source)) > 0U) {
        if (fwrite(chunk, 1, bytes_read, destination) != bytes_read) {
            fclose(destination);
            fclose(source);
            return -1;
        }
    }
    if (ferror(source)) {
        fclose(destination);
        fclose(source);
        return -1;
    }
    fclose(destination);
    fclose(source);
    return 0;
}

static int remove_directory_tree(const char *path)
{
    DIR *dir = NULL;
    struct dirent *entry;

    if (path == NULL) {
        return -1;
    }

    dir = opendir(path);
    if (dir == NULL) {
        return errno == ENOENT ? 0 : -1;
    }

    while ((entry = readdir(dir)) != NULL) {
        char child_path[512];
        struct stat child_stat;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (snprintf(child_path, sizeof(child_path), "%s/%s", path, entry->d_name) >= (int)sizeof(child_path)) {
            closedir(dir);
            return -1;
        }
        if (stat(child_path, &child_stat) != 0) {
            closedir(dir);
            return -1;
        }

        if (S_ISDIR(child_stat.st_mode)) {
            if (remove_directory_tree(child_path) != 0) {
                closedir(dir);
                return -1;
            }
        } else if (unlink(child_path) != 0) {
            closedir(dir);
            return -1;
        }
    }

    closedir(dir);
    return rmdir(path) == 0 ? 0 : -1;
}

static bool entrypoint_is_safe(const char *entrypoint)
{
    return entrypoint != NULL &&
           entrypoint[0] != '\0' &&
           entrypoint[0] != '/' &&
           strstr(entrypoint, "..") == NULL;
}

static void normalize_permission_alias(char *value, size_t value_size);
static void normalize_trigger_alias(char *value, size_t value_size);

static size_t append_csv_items(
    char items[][ESPCLAW_APP_PERMISSION_NAME_MAX + 1],
    size_t max_items,
    size_t item_size,
    const char *csv
)
{
    char scratch[256];
    char *token = NULL;
    char *save_ptr = NULL;
    size_t count = 0;

    if (csv == NULL || csv[0] == '\0') {
        return 0;
    }

    snprintf(scratch, sizeof(scratch), "%s", csv);
    token = strtok_r(scratch, ",", &save_ptr);
    while (token != NULL && count < max_items) {
        size_t start = 0;
        size_t end = strlen(token);

        while (token[start] != '\0' && isspace((unsigned char)token[start])) {
            start++;
        }
        while (end > start && isspace((unsigned char)token[end - 1])) {
            end--;
        }

        if (end > start) {
            size_t length = end - start;
            if (length >= item_size) {
                length = item_size - 1;
            }
            memcpy(items[count], token + start, length);
            items[count][length] = '\0';
            normalize_permission_alias(items[count], item_size);
            count++;
        }

        token = strtok_r(NULL, ",", &save_ptr);
    }

    return count;
}

static void normalize_permission_alias(char *value, size_t value_size)
{
    size_t index = 0;

    if (value == NULL || value_size == 0) {
        return;
    }

    while (value[index] != '\0') {
        value[index] = (char)tolower((unsigned char)value[index]);
        index++;
    }

    if (strcmp(value, "pwm") == 0) {
        snprintf(value, value_size, "%s", "pwm.write");
    } else if (strcmp(value, "gpio") == 0) {
        snprintf(value, value_size, "%s", "gpio.write");
    } else if (strcmp(value, "fs") == 0 || strcmp(value, "filesystem") == 0) {
        snprintf(value, value_size, "%s", "fs.read");
    }
}

static size_t append_csv_triggers(
    char items[][ESPCLAW_APP_TRIGGER_NAME_MAX + 1],
    size_t max_items,
    size_t item_size,
    const char *csv
)
{
    char scratch[128];
    char *token = NULL;
    char *save_ptr = NULL;
    size_t count = 0;

    if (csv == NULL || csv[0] == '\0') {
        return 0;
    }

    snprintf(scratch, sizeof(scratch), "%s", csv);
    token = strtok_r(scratch, ",", &save_ptr);
    while (token != NULL && count < max_items) {
        size_t start = 0;
        size_t end = strlen(token);

        while (token[start] != '\0' && isspace((unsigned char)token[start])) {
            start++;
        }
        while (end > start && isspace((unsigned char)token[end - 1])) {
            end--;
        }

        if (end > start) {
            size_t length = end - start;
            if (length >= item_size) {
                length = item_size - 1;
            }
            memcpy(items[count], token + start, length);
            items[count][length] = '\0';
            normalize_trigger_alias(items[count], item_size);
            count++;
        }

        token = strtok_r(NULL, ",", &save_ptr);
    }

    return count;
}

static void normalize_trigger_alias(char *value, size_t value_size)
{
    size_t index = 0;

    if (value == NULL || value_size == 0) {
        return;
    }

    while (value[index] != '\0') {
        value[index] = (char)tolower((unsigned char)value[index]);
        index++;
    }

    if (strcmp(value, "run") == 0 || strcmp(value, "start") == 0 || strcmp(value, "execute") == 0) {
        snprintf(value, value_size, "%s", "manual");
    }
}

static int build_app_relative_path(
    const char *app_id,
    const char *suffix,
    char *buffer,
    size_t buffer_size
)
{
    if (!espclaw_app_id_is_valid(app_id) || suffix == NULL || buffer == NULL || buffer_size == 0) {
        return -1;
    }

    if (snprintf(buffer, buffer_size, "apps/%s/%s", app_id, suffix) >= (int)buffer_size) {
        return -1;
    }
    return 0;
}

static int build_app_absolute_path(
    const char *workspace_root,
    const char *app_id,
    const char *suffix,
    char *buffer,
    size_t buffer_size
)
{
    char relative_path[192];

    if (build_app_relative_path(app_id, suffix, relative_path, sizeof(relative_path)) != 0) {
        return -1;
    }

    return espclaw_workspace_resolve_path(workspace_root, relative_path, buffer, buffer_size);
}

bool espclaw_app_id_is_valid(const char *app_id)
{
    size_t index;

    if (app_id == NULL || app_id[0] == '\0' || strlen(app_id) > ESPCLAW_APP_ID_MAX) {
        return false;
    }

    for (index = 0; app_id[index] != '\0'; ++index) {
        unsigned char c = (unsigned char)app_id[index];

        if (!(isalnum(c) || c == '-' || c == '_')) {
            return false;
        }
    }

    return true;
}

int espclaw_app_parse_manifest_json(const char *json, espclaw_app_manifest_t *manifest)
{
    if (json == NULL || manifest == NULL) {
        return -1;
    }

    memset(manifest, 0, sizeof(*manifest));
    if (!extract_string_after_key(json, "id", manifest->app_id, sizeof(manifest->app_id)) ||
        !extract_string_after_key(json, "version", manifest->version, sizeof(manifest->version)) ||
        !extract_string_after_key(json, "title", manifest->title, sizeof(manifest->title)) ||
        !extract_string_after_key(json, "entrypoint", manifest->entrypoint, sizeof(manifest->entrypoint))) {
        return -1;
    }

    if (!extract_string_array_after_key(
            json,
            "permissions",
            manifest->permissions,
            ESPCLAW_APP_PERMISSION_MAX,
            sizeof(manifest->permissions[0]),
            &manifest->permission_count) ||
        !extract_trigger_array_after_key(
            json,
            "triggers",
            manifest->triggers,
            ESPCLAW_APP_TRIGGER_MAX,
            sizeof(manifest->triggers[0]),
            &manifest->trigger_count)) {
        return -1;
    }

    if (!espclaw_app_id_is_valid(manifest->app_id) || !entrypoint_is_safe(manifest->entrypoint)) {
        return -1;
    }

    if (manifest->permission_count == 0) {
        manifest->permission_count = append_csv_items(
            manifest->permissions,
            ESPCLAW_APP_PERMISSION_MAX,
            sizeof(manifest->permissions[0]),
            DEFAULT_APP_PERMISSIONS
        );
    }
    if (manifest->trigger_count == 0) {
        manifest->trigger_count = append_csv_triggers(
            manifest->triggers,
            ESPCLAW_APP_TRIGGER_MAX,
            sizeof(manifest->triggers[0]),
            DEFAULT_APP_TRIGGERS
        );
    }

    return 0;
}

int espclaw_app_load_manifest(
    const char *workspace_root,
    const char *app_id,
    espclaw_app_manifest_t *manifest
)
{
    char relative_path[192];
    char json[2048];

    if (manifest == NULL || !espclaw_app_id_is_valid(app_id)) {
        return -1;
    }

    if (app_manifest_cache_load_json(app_id, json, sizeof(json)) != 0) {
        if (workspace_root == NULL || build_app_relative_path(app_id, "app.json", relative_path, sizeof(relative_path)) != 0) {
            return -1;
        }
        if (espclaw_workspace_read_file(workspace_root, relative_path, json, sizeof(json)) != 0) {
            return -1;
        }
        (void)app_manifest_cache_store_json(app_id, json);
    }

    return espclaw_app_parse_manifest_json(json, manifest);
}

bool espclaw_app_has_permission(const espclaw_app_manifest_t *manifest, const char *permission)
{
    size_t index;

    if (manifest == NULL || permission == NULL) {
        return false;
    }

    for (index = 0; index < manifest->permission_count; ++index) {
        if (strcmp(manifest->permissions[index], permission) == 0) {
            return true;
        }
    }
    return false;
}

bool espclaw_app_supports_trigger(const espclaw_app_manifest_t *manifest, const char *trigger)
{
    size_t index;

    if (manifest == NULL || trigger == NULL) {
        return false;
    }

    for (index = 0; index < manifest->trigger_count; ++index) {
        if (strcmp(manifest->triggers[index], trigger) == 0) {
            return true;
        }
    }
    return false;
}

int espclaw_app_scaffold_lua(
    const char *workspace_root,
    const char *app_id,
    const char *title,
    const char *permissions_csv,
    const char *triggers_csv
)
{
    char app_dir[512];
    char manifest_relative_path[192];
    char script_relative_path[192];
    /* The default LLM-installable app permissions/triggers no longer fit in a
     * tiny manifest buffer on-device, especially once titles and app ids get
     * longer. Keep this comfortably above the current default manifest size so
     * app.install can scaffold directly on hardware. */
    char manifest_json[2048];
    char main_lua[1024];
    char permissions[ESPCLAW_APP_PERMISSION_MAX][ESPCLAW_APP_PERMISSION_NAME_MAX + 1] = {{0}};
    char triggers[ESPCLAW_APP_TRIGGER_MAX][ESPCLAW_APP_TRIGGER_NAME_MAX + 1] = {{0}};
    size_t permission_count;
    size_t trigger_count;
    size_t index;
    int written;

    if (workspace_root == NULL || title == NULL || !espclaw_app_id_is_valid(app_id)) {
        return -1;
    }

    if (espclaw_workspace_bootstrap(workspace_root) != 0) {
        return -1;
    }

    permission_count = append_csv_items(
        permissions,
        ESPCLAW_APP_PERMISSION_MAX,
        sizeof(permissions[0]),
        permissions_csv != NULL && permissions_csv[0] != '\0' ? permissions_csv : DEFAULT_APP_PERMISSIONS
    );
    trigger_count = append_csv_triggers(
        triggers,
        ESPCLAW_APP_TRIGGER_MAX,
        sizeof(triggers[0]),
        triggers_csv != NULL && triggers_csv[0] != '\0' ? triggers_csv : DEFAULT_APP_TRIGGERS
    );

    if (permission_count == 0 || trigger_count == 0 ||
        build_app_absolute_path(workspace_root, app_id, "", app_dir, sizeof(app_dir)) != 0 ||
        build_app_relative_path(app_id, "app.json", manifest_relative_path, sizeof(manifest_relative_path)) != 0 ||
        build_app_relative_path(app_id, "main.lua", script_relative_path, sizeof(script_relative_path)) != 0) {
        return -1;
    }

    if (ensure_directory(app_dir) != 0) {
        return -1;
    }

    written = snprintf(
        manifest_json,
        sizeof(manifest_json),
        "{\n"
        "  \"id\": \"%s\",\n"
        "  \"version\": \"0.1.0\",\n"
        "  \"title\": \"%s\",\n"
        "  \"entrypoint\": \"main.lua\",\n"
        "  \"permissions\": [",
        app_id,
        title
    );
    if (written < 0 || (size_t)written >= sizeof(manifest_json)) {
        return -1;
    }

    for (index = 0; index < permission_count; ++index) {
        written += snprintf(
            manifest_json + written,
            sizeof(manifest_json) - (size_t)written,
            "%s\"%s\"",
            index == 0 ? "" : ", ",
            permissions[index]
        );
        if (written < 0 || (size_t)written >= sizeof(manifest_json)) {
            return -1;
        }
    }

    written += snprintf(manifest_json + written, sizeof(manifest_json) - (size_t)written, "],\n  \"triggers\": [");
    if (written < 0 || (size_t)written >= sizeof(manifest_json)) {
        return -1;
    }

    for (index = 0; index < trigger_count; ++index) {
        written += snprintf(
            manifest_json + written,
            sizeof(manifest_json) - (size_t)written,
            "%s\"%s\"",
            index == 0 ? "" : ", ",
            triggers[index]
        );
        if (written < 0 || (size_t)written >= sizeof(manifest_json)) {
            return -1;
        }
    }

    written += snprintf(manifest_json + written, sizeof(manifest_json) - (size_t)written, "]\n}\n");
    if (written < 0 || (size_t)written >= sizeof(manifest_json)) {
        return -1;
    }

    written = snprintf(
        main_lua,
        sizeof(main_lua),
        "function handle(trigger, payload)\n"
        "  espclaw.log(\"%s trigger=\" .. trigger)\n"
        "  if trigger == \"boot\" then\n"
        "    return \"%s boot complete\"\n"
        "  end\n"
        "  if payload ~= nil and payload ~= \"\" then\n"
        "    return \"%s heard: \" .. payload\n"
        "  end\n"
        "  return \"%s ready\"\n"
        "end\n",
        app_id,
        app_id,
        app_id,
        app_id
    );
    if (written < 0 || (size_t)written >= sizeof(main_lua)) {
        return -1;
    }

    if (espclaw_workspace_write_file(workspace_root, manifest_relative_path, manifest_json) != 0 ||
        espclaw_workspace_write_file(workspace_root, script_relative_path, main_lua) != 0) {
        return -1;
    }
    (void)app_manifest_cache_store_json(app_id, manifest_json);

    return 0;
}

int espclaw_app_collect_ids(
    const char *workspace_root,
    char ids[][ESPCLAW_APP_ID_MAX + 1],
    size_t max_ids,
    size_t *count_out
)
{
    char apps_path[512];
    DIR *dir = NULL;
    struct dirent *entry;
    size_t count = 0;

    if (count_out == NULL) {
        return -1;
    }
    *count_out = 0;

    if (workspace_root == NULL || ids == NULL || max_ids == 0 ||
        espclaw_workspace_resolve_path(workspace_root, "apps", apps_path, sizeof(apps_path)) != 0) {
        return -1;
    }

    dir = opendir(apps_path);
    if (dir == NULL) {
        return -1;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (!espclaw_app_id_is_valid(entry->d_name)) {
            continue;
        }
        memcpy(ids[count], entry->d_name, sizeof(ids[count]));
        count++;
        if (count >= max_ids) {
            break;
        }
    }

    closedir(dir);
    *count_out = count;
    return 0;
}

int espclaw_app_read_source(
    const char *workspace_root,
    const char *app_id,
    char *buffer,
    size_t buffer_size
)
{
    espclaw_app_manifest_t manifest;
    char relative_path[192];

    if (workspace_root == NULL || buffer == NULL || buffer_size == 0) {
        return -1;
    }

    if (espclaw_app_load_manifest(workspace_root, app_id, &manifest) != 0 ||
        build_app_relative_path(app_id, manifest.entrypoint, relative_path, sizeof(relative_path)) != 0) {
        return -1;
    }

    return espclaw_workspace_read_file(workspace_root, relative_path, buffer, buffer_size);
}

int espclaw_app_update_source(
    const char *workspace_root,
    const char *app_id,
    const char *source
)
{
    espclaw_app_manifest_t manifest;
    char relative_path[192];

    if (workspace_root == NULL || source == NULL) {
        return -1;
    }

    if (espclaw_app_load_manifest(workspace_root, app_id, &manifest) != 0 ||
        build_app_relative_path(app_id, manifest.entrypoint, relative_path, sizeof(relative_path)) != 0) {
        return -1;
    }

    return espclaw_workspace_write_file(workspace_root, relative_path, source);
}

int espclaw_app_install_from_file(
    const char *workspace_root,
    const char *app_id,
    const char *title,
    const char *permissions_csv,
    const char *triggers_csv,
    const char *source_path
)
{
    espclaw_app_manifest_t manifest;
    char target_relative_path[192];
    char target_absolute_path[512];
    char source_absolute_path[512];
    char existing[64];
    bool app_exists;

    if (workspace_root == NULL || source_path == NULL || source_path[0] == '\0' || !espclaw_app_id_is_valid(app_id)) {
        return -1;
    }

    app_exists = espclaw_app_read_source(workspace_root, app_id, existing, sizeof(existing)) == 0;
    if (!app_exists &&
        espclaw_app_scaffold_lua(
            workspace_root,
            app_id,
            title != NULL && title[0] != '\0' ? title : app_id,
            permissions_csv,
            triggers_csv
        ) != 0) {
        return -1;
    }
    if (espclaw_app_load_manifest(workspace_root, app_id, &manifest) != 0 ||
        build_app_relative_path(app_id, manifest.entrypoint, target_relative_path, sizeof(target_relative_path)) != 0 ||
        espclaw_workspace_resolve_path(workspace_root, target_relative_path, target_absolute_path, sizeof(target_absolute_path)) != 0 ||
        espclaw_workspace_resolve_path(workspace_root, source_path, source_absolute_path, sizeof(source_absolute_path)) != 0) {
        return -1;
    }

    return copy_file_contents(source_absolute_path, target_absolute_path);
}

int espclaw_app_install_from_blob(
    const char *workspace_root,
    const char *app_id,
    const char *title,
    const char *permissions_csv,
    const char *triggers_csv,
    const char *blob_id
)
{
    espclaw_workspace_blob_status_t status;

    if (workspace_root == NULL || blob_id == NULL || blob_id[0] == '\0') {
        return -1;
    }
    if (espclaw_workspace_blob_status(workspace_root, blob_id, &status) != 0 ||
        status.stage != ESPCLAW_WORKSPACE_BLOB_STAGE_COMMITTED ||
        status.target_path[0] == '\0') {
        return -1;
    }
    return espclaw_app_install_from_file(
        workspace_root,
        app_id,
        title,
        permissions_csv,
        triggers_csv,
        status.target_path
    );
}

int espclaw_app_install_from_url(
    const char *workspace_root,
    const char *app_id,
    const char *title,
    const char *permissions_csv,
    const char *triggers_csv,
    const char *source_url
)
{
    char temp_relative[128];
    char temp_absolute[512];
    int result;

    if (workspace_root == NULL || source_url == NULL || source_url[0] == '\0' || !espclaw_app_id_is_valid(app_id)) {
        return -1;
    }
    if (snprintf(temp_relative, sizeof(temp_relative), "blobs/.staging/app_%s.lua", app_id) >= (int)sizeof(temp_relative) ||
        espclaw_workspace_bootstrap(workspace_root) != 0 ||
        espclaw_workspace_resolve_path(workspace_root, temp_relative, temp_absolute, sizeof(temp_absolute)) != 0 ||
        espclaw_web_download_to_file(source_url, temp_absolute) != 0) {
        return -1;
    }

    result = espclaw_app_install_from_file(
        workspace_root,
        app_id,
        title,
        permissions_csv,
        triggers_csv,
        temp_relative
    );
    unlink(temp_absolute);
    return result;
}

int espclaw_app_remove(
    const char *workspace_root,
    const char *app_id
)
{
    char app_dir[512];

    if (workspace_root == NULL || !espclaw_app_id_is_valid(app_id) ||
        build_app_absolute_path(workspace_root, app_id, "", app_dir, sizeof(app_dir)) != 0) {
        return -1;
    }

    app_manifest_cache_remove(app_id);
    return remove_directory_tree(app_dir);
}

#if defined(ESP_PLATFORM) || defined(ESPCLAW_HOST_LUA)
static espclaw_lua_context_t *lua_get_context(lua_State *state)
{
    return (espclaw_lua_context_t *)lua_touserdata(state, lua_upvalueindex(1));
}

static int lua_push_permission_error(lua_State *state, const char *permission)
{
    lua_pushnil(state);
    lua_pushfstring(state, "permission denied for %s", permission);
    return 2;
}

static int lua_resolve_pin_argument(lua_State *state, int index, int *pin_out)
{
    const char *alias = NULL;
    int pin = -1;

    if (pin_out == NULL) {
        return -1;
    }

    if (lua_isnumber(state, index)) {
        *pin_out = (int)lua_tointeger(state, index);
        return 0;
    }
    if (!lua_isstring(state, index)) {
        return -1;
    }

    alias = lua_tostring(state, index);
    if (alias == NULL || espclaw_board_resolve_pin_alias(alias, &pin) != 0) {
        return -1;
    }
    *pin_out = pin;
    return 0;
}

static void lua_push_byte_table(lua_State *state, const uint8_t *bytes, size_t length)
{
    size_t index;

    lua_newtable(state);
    for (index = 0; index < length; ++index) {
        lua_pushinteger(state, (lua_Integer)(index + 1));
        lua_pushinteger(state, (lua_Integer)bytes[index]);
        lua_settable(state, -3);
    }
}

static void lua_push_u16_table(lua_State *state, const uint16_t *values, size_t length)
{
    size_t index;

    lua_newtable(state);
    for (index = 0; index < length; ++index) {
        lua_pushinteger(state, (lua_Integer)(index + 1));
        lua_pushinteger(state, (lua_Integer)values[index]);
        lua_settable(state, -3);
    }
}

static int lua_read_bytes_argument(lua_State *state, int index, uint8_t *buffer, size_t max_length, size_t *length_out)
{
    size_t length = 0;
    size_t array_length = 0;

    if (lua_type(state, index) == LUA_TSTRING) {
        size_t string_length = 0;
        const char *string_data = lua_tolstring(state, index, &string_length);

        if (string_data == NULL || string_length == 0 || string_length > max_length) {
            return -1;
        }

        memcpy(buffer, string_data, string_length);
        *length_out = string_length;
        return 0;
    }

    if (!lua_istable(state, index)) {
        return -1;
    }

    array_length = (size_t)lua_rawlen(state, index);
    if (array_length == 0 || array_length > max_length) {
        return -1;
    }

    for (length = 0; length < array_length; ++length) {
        lua_geti(state, index, (lua_Integer)(length + 1));
        if (!lua_isnumber(state, -1)) {
            lua_pop(state, 1);
            return -1;
        }

        buffer[length] = (uint8_t)lua_tointeger(state, -1);
        lua_pop(state, 1);
    }

    *length_out = array_length;
    return 0;
}

static int lua_read_u16_array_argument(
    lua_State *state,
    int index,
    uint16_t *buffer,
    size_t max_length,
    size_t *length_out
)
{
    size_t length = 0;
    size_t array_length = 0;

    if (!lua_istable(state, index) || buffer == NULL || length_out == NULL) {
        return -1;
    }

    array_length = (size_t)lua_rawlen(state, index);
    if (array_length == 0 || array_length > max_length) {
        return -1;
    }

    for (length = 0; length < array_length; ++length) {
        lua_Integer value = 0;

        lua_geti(state, index, (lua_Integer)(length + 1));
        if (!lua_isnumber(state, -1)) {
            lua_pop(state, 1);
            return -1;
        }

        value = lua_tointeger(state, -1);
        if (value < 0 || value > 65535) {
            lua_pop(state, 1);
            return -1;
        }

        buffer[length] = (uint16_t)value;
        lua_pop(state, 1);
    }

    *length_out = array_length;
    return 0;
}

static int lua_espclaw_log(lua_State *state)
{
    const char *message = luaL_checkstring(state, 1);
    espclaw_lua_context_t *context = lua_get_context(state);

#ifdef ESP_PLATFORM
    ESP_LOGI(TAG, "[app:%s] %s", context != NULL ? context->app_id : "unknown", message);
#else
    fprintf(stderr, "[espclaw app:%s] %s\n", context != NULL ? context->app_id : "unknown", message);
#endif
    return 0;
}

static int lua_espclaw_has_permission(lua_State *state)
{
    espclaw_lua_context_t *context = lua_get_context(state);
    const char *permission = luaL_checkstring(state, 1);

    lua_pushboolean(state, context != NULL && espclaw_app_has_permission(context->manifest, permission));
    return 1;
}

static int lua_espclaw_read_file(lua_State *state)
{
    espclaw_lua_context_t *context = lua_get_context(state);
    const char *relative_path = luaL_checkstring(state, 1);
    char buffer[1024];

    if (context == NULL || !espclaw_app_has_permission(context->manifest, "fs.read")) {
        lua_pushnil(state);
        lua_pushstring(state, "permission denied for fs.read");
        return 2;
    }

    if (espclaw_workspace_read_file(context->workspace_root, relative_path, buffer, sizeof(buffer)) != 0) {
        lua_pushnil(state);
        lua_pushstring(state, "read failed");
        return 2;
    }

    lua_pushstring(state, buffer);
    return 1;
}

static int lua_espclaw_write_file(lua_State *state)
{
    espclaw_lua_context_t *context = lua_get_context(state);
    const char *relative_path = luaL_checkstring(state, 1);
    const char *content = luaL_checkstring(state, 2);

    if (context == NULL || !espclaw_app_has_permission(context->manifest, "fs.write")) {
        lua_pushnil(state);
        lua_pushstring(state, "permission denied for fs.write");
        return 2;
    }

    if (espclaw_workspace_write_file(context->workspace_root, relative_path, content) != 0) {
        lua_pushnil(state);
        lua_pushstring(state, "write failed");
        return 2;
    }

    lua_pushboolean(state, 1);
    return 1;
}

static int lua_espclaw_list_apps(lua_State *state)
{
    espclaw_lua_context_t *context = lua_get_context(state);
    char ids[32][ESPCLAW_APP_ID_MAX + 1];
    size_t count = 0;
    size_t index;

    if (context == NULL || espclaw_app_collect_ids(context->workspace_root, ids, 32, &count) != 0) {
        lua_newtable(state);
        return 1;
    }

    lua_newtable(state);
    for (index = 0; index < count; ++index) {
        lua_pushinteger(state, (lua_Integer)(index + 1));
        lua_pushstring(state, ids[index]);
        lua_settable(state, -3);
    }
    return 1;
}

static int lua_espclaw_board_variant(lua_State *state)
{
    const espclaw_board_descriptor_t *board = espclaw_board_current();

    if (board == NULL) {
        lua_pushnil(state);
        return 1;
    }

    lua_pushstring(state, board->variant_id);
    return 1;
}

static void lua_push_board_descriptor(lua_State *state, const espclaw_board_descriptor_t *board)
{
    size_t index;

    if (board == NULL) {
        lua_pushnil(state);
        return;
    }

    lua_newtable(state);
    lua_pushstring(state, board->variant_id);
    lua_setfield(state, -2, "variant");
    lua_pushstring(state, board->display_name);
    lua_setfield(state, -2, "display_name");
    lua_pushstring(state, board->source);
    lua_setfield(state, -2, "source");

    lua_newtable(state);
    for (index = 0; index < board->pin_count; ++index) {
        lua_newtable(state);
        lua_pushstring(state, board->pins[index].name);
        lua_setfield(state, -2, "name");
        lua_pushinteger(state, (lua_Integer)board->pins[index].pin);
        lua_setfield(state, -2, "pin");
        lua_seti(state, -2, (lua_Integer)(index + 1));
    }
    lua_setfield(state, -2, "pins");

    lua_newtable(state);
    for (index = 0; index < board->i2c_bus_count; ++index) {
        lua_newtable(state);
        lua_pushstring(state, board->i2c_buses[index].name);
        lua_setfield(state, -2, "name");
        lua_pushinteger(state, (lua_Integer)board->i2c_buses[index].port);
        lua_setfield(state, -2, "port");
        lua_pushinteger(state, (lua_Integer)board->i2c_buses[index].sda_pin);
        lua_setfield(state, -2, "sda");
        lua_pushinteger(state, (lua_Integer)board->i2c_buses[index].scl_pin);
        lua_setfield(state, -2, "scl");
        lua_pushinteger(state, (lua_Integer)board->i2c_buses[index].frequency_hz);
        lua_setfield(state, -2, "frequency_hz");
        lua_seti(state, -2, (lua_Integer)(index + 1));
    }
    lua_setfield(state, -2, "i2c_buses");

    lua_newtable(state);
    for (index = 0; index < board->uart_count; ++index) {
        lua_newtable(state);
        lua_pushstring(state, board->uarts[index].name);
        lua_setfield(state, -2, "name");
        lua_pushinteger(state, (lua_Integer)board->uarts[index].port);
        lua_setfield(state, -2, "port");
        lua_pushinteger(state, (lua_Integer)board->uarts[index].tx_pin);
        lua_setfield(state, -2, "tx");
        lua_pushinteger(state, (lua_Integer)board->uarts[index].rx_pin);
        lua_setfield(state, -2, "rx");
        lua_pushinteger(state, (lua_Integer)board->uarts[index].baud_rate);
        lua_setfield(state, -2, "baud_rate");
        lua_seti(state, -2, (lua_Integer)(index + 1));
    }
    lua_setfield(state, -2, "uarts");

    lua_newtable(state);
    for (index = 0; index < board->adc_count; ++index) {
        lua_newtable(state);
        lua_pushstring(state, board->adc_channels[index].name);
        lua_setfield(state, -2, "name");
        lua_pushinteger(state, (lua_Integer)board->adc_channels[index].unit);
        lua_setfield(state, -2, "unit");
        lua_pushinteger(state, (lua_Integer)board->adc_channels[index].channel);
        lua_setfield(state, -2, "channel");
        lua_seti(state, -2, (lua_Integer)(index + 1));
    }
    lua_setfield(state, -2, "adc_channels");
}

static int lua_espclaw_board_describe(lua_State *state)
{
    lua_push_board_descriptor(state, espclaw_board_current());
    return 1;
}

static int lua_espclaw_hardware_list(lua_State *state)
{
    static const char *CAPABILITIES[] = {
        "apps", "tasks", "events", "event_watches", "fs", "gpio", "pwm", "ppm", "buzzer",
        "adc", "i2c", "uart", "camera", "temperature", "imu", "pid", "control"
    };
    const espclaw_board_descriptor_t *board = espclaw_board_current();
    size_t index;

    lua_push_board_descriptor(state, board);
    if (lua_isnil(state, -1)) {
        return 1;
    }

    lua_newtable(state);
    for (index = 0; index < sizeof(CAPABILITIES) / sizeof(CAPABILITIES[0]); ++index) {
        lua_pushstring(state, CAPABILITIES[index]);
        lua_seti(state, -2, (lua_Integer)(index + 1));
    }
    lua_setfield(state, -2, "capabilities");
    return 1;
}

static int lua_espclaw_board_pin(lua_State *state)
{
    const char *name = luaL_checkstring(state, 1);
    int pin = -1;

    if (espclaw_board_resolve_pin_alias(name, &pin) != 0) {
        lua_pushnil(state);
        lua_pushfstring(state, "unknown board pin alias %s", name);
        return 2;
    }

    lua_pushinteger(state, (lua_Integer)pin);
    return 1;
}

static int lua_espclaw_board_i2c(lua_State *state)
{
    const char *name = luaL_optstring(state, 1, "default");
    espclaw_board_i2c_bus_t bus;

    if (espclaw_board_find_i2c_bus(name, &bus) != 0) {
        lua_pushnil(state);
        lua_pushfstring(state, "unknown board i2c bus %s", name);
        return 2;
    }

    lua_newtable(state);
    lua_pushinteger(state, (lua_Integer)bus.port);
    lua_setfield(state, -2, "port");
    lua_pushinteger(state, (lua_Integer)bus.sda_pin);
    lua_setfield(state, -2, "sda");
    lua_pushinteger(state, (lua_Integer)bus.scl_pin);
    lua_setfield(state, -2, "scl");
    lua_pushinteger(state, (lua_Integer)bus.frequency_hz);
    lua_setfield(state, -2, "frequency_hz");
    return 1;
}

static int lua_espclaw_board_uart(lua_State *state)
{
    const char *name = luaL_optstring(state, 1, "console");
    espclaw_board_uart_t uart;

    if (espclaw_board_find_uart(name, &uart) != 0) {
        lua_pushnil(state);
        lua_pushfstring(state, "unknown board uart %s", name);
        return 2;
    }

    lua_newtable(state);
    lua_pushinteger(state, (lua_Integer)uart.port);
    lua_setfield(state, -2, "port");
    lua_pushinteger(state, (lua_Integer)uart.tx_pin);
    lua_setfield(state, -2, "tx");
    lua_pushinteger(state, (lua_Integer)uart.rx_pin);
    lua_setfield(state, -2, "rx");
    lua_pushinteger(state, (lua_Integer)uart.baud_rate);
    lua_setfield(state, -2, "baud_rate");
    return 1;
}

static int lua_espclaw_board_adc(lua_State *state)
{
    const char *name = luaL_checkstring(state, 1);
    espclaw_board_adc_channel_t channel;

    if (espclaw_board_find_adc_channel(name, &channel) != 0) {
        lua_pushnil(state);
        lua_pushfstring(state, "unknown board adc channel %s", name);
        return 2;
    }

    lua_newtable(state);
    lua_pushinteger(state, (lua_Integer)channel.unit);
    lua_setfield(state, -2, "unit");
    lua_pushinteger(state, (lua_Integer)channel.channel);
    lua_setfield(state, -2, "channel");
    return 1;
}

static int lua_espclaw_gpio_read(lua_State *state)
{
    espclaw_lua_context_t *context = lua_get_context(state);
    int pin = -1;
    int level = 0;

    if (context == NULL || !espclaw_app_has_permission(context->manifest, "gpio.read")) {
        return lua_push_permission_error(state, "gpio.read");
    }
    if (lua_resolve_pin_argument(state, 1, &pin) != 0) {
        lua_pushnil(state);
        lua_pushstring(state, "invalid gpio pin or alias");
        return 2;
    }
    if (espclaw_hw_gpio_read(pin, &level) != 0) {
        lua_pushnil(state);
        lua_pushstring(state, "gpio read failed");
        return 2;
    }

    lua_pushinteger(state, (lua_Integer)level);
    return 1;
}

static int lua_espclaw_gpio_write(lua_State *state)
{
    espclaw_lua_context_t *context = lua_get_context(state);
    int pin = -1;
    int level = lua_toboolean(state, 2) ? 1 : 0;

    if (context == NULL || !espclaw_app_has_permission(context->manifest, "gpio.write")) {
        return lua_push_permission_error(state, "gpio.write");
    }
    if (lua_resolve_pin_argument(state, 1, &pin) != 0) {
        lua_pushnil(state);
        lua_pushstring(state, "invalid gpio pin or alias");
        return 2;
    }
    if (espclaw_hw_gpio_write(pin, level) != 0) {
        lua_pushnil(state);
        lua_pushstring(state, "gpio write failed");
        return 2;
    }

    lua_pushboolean(state, 1);
    return 1;
}

static int lua_espclaw_pwm_setup(lua_State *state)
{
    espclaw_lua_context_t *context = lua_get_context(state);
    int channel = (int)luaL_checkinteger(state, 1);
    int pin = -1;
    int frequency_hz = (int)luaL_checkinteger(state, 3);
    int resolution_bits = (int)luaL_optinteger(state, 4, 10);

    if (context == NULL || !espclaw_app_has_permission(context->manifest, "pwm.write")) {
        return lua_push_permission_error(state, "pwm.write");
    }
    if (lua_resolve_pin_argument(state, 2, &pin) != 0) {
        lua_pushnil(state);
        lua_pushstring(state, "invalid pwm pin or alias");
        return 2;
    }
    if (espclaw_hw_pwm_setup(channel, pin, frequency_hz, resolution_bits) != 0) {
        lua_pushnil(state);
        lua_pushstring(state, "pwm setup failed");
        return 2;
    }

    lua_pushboolean(state, 1);
    return 1;
}

static int lua_espclaw_pwm_write(lua_State *state)
{
    espclaw_lua_context_t *context = lua_get_context(state);
    int channel = (int)luaL_checkinteger(state, 1);
    int duty = (int)luaL_checkinteger(state, 2);

    if (context == NULL || !espclaw_app_has_permission(context->manifest, "pwm.write")) {
        return lua_push_permission_error(state, "pwm.write");
    }
    if (espclaw_hw_pwm_write(channel, duty) != 0) {
        lua_pushnil(state);
        lua_pushstring(state, "pwm write failed");
        return 2;
    }

    lua_pushboolean(state, 1);
    return 1;
}

static int lua_espclaw_pwm_write_us(lua_State *state)
{
    espclaw_lua_context_t *context = lua_get_context(state);
    int channel = (int)luaL_checkinteger(state, 1);
    int pulse_width_us = (int)luaL_checkinteger(state, 2);

    if (context == NULL || !espclaw_app_has_permission(context->manifest, "pwm.write")) {
        return lua_push_permission_error(state, "pwm.write");
    }
    if (espclaw_hw_pwm_write_us(channel, pulse_width_us) != 0) {
        lua_pushnil(state);
        lua_pushstring(state, "pwm microsecond write failed");
        return 2;
    }

    lua_pushboolean(state, 1);
    return 1;
}

static int lua_espclaw_pwm_stop(lua_State *state)
{
    espclaw_lua_context_t *context = lua_get_context(state);
    int channel = (int)luaL_checkinteger(state, 1);

    if (context == NULL || !espclaw_app_has_permission(context->manifest, "pwm.write")) {
        return lua_push_permission_error(state, "pwm.write");
    }
    if (espclaw_hw_pwm_stop(channel) != 0) {
        lua_pushnil(state);
        lua_pushstring(state, "pwm stop failed");
        return 2;
    }

    lua_pushboolean(state, 1);
    return 1;
}

static int lua_espclaw_pwm_state(lua_State *state)
{
    espclaw_lua_context_t *context = lua_get_context(state);
    int channel = (int)luaL_checkinteger(state, 1);
    espclaw_hw_pwm_state_t pwm_state;

    if (context == NULL || !espclaw_app_has_permission(context->manifest, "pwm.write")) {
        return lua_push_permission_error(state, "pwm.write");
    }
    if (espclaw_hw_pwm_state(channel, &pwm_state) != 0) {
        lua_pushnil(state);
        lua_pushstring(state, "pwm state unavailable");
        return 2;
    }

    lua_newtable(state);
    lua_pushboolean(state, pwm_state.configured);
    lua_setfield(state, -2, "configured");
    lua_pushinteger(state, (lua_Integer)pwm_state.pin);
    lua_setfield(state, -2, "pin");
    lua_pushinteger(state, (lua_Integer)pwm_state.frequency_hz);
    lua_setfield(state, -2, "frequency_hz");
    lua_pushinteger(state, (lua_Integer)pwm_state.resolution_bits);
    lua_setfield(state, -2, "resolution_bits");
    lua_pushinteger(state, (lua_Integer)pwm_state.duty);
    lua_setfield(state, -2, "duty");
    lua_pushinteger(state, (lua_Integer)pwm_state.pulse_width_us);
    lua_setfield(state, -2, "pulse_width_us");
    return 1;
}

static int lua_espclaw_servo_attach(lua_State *state)
{
    espclaw_lua_context_t *context = lua_get_context(state);
    int channel = (int)luaL_checkinteger(state, 1);
    int pin = -1;
    int frequency_hz = (int)luaL_optinteger(state, 3, 50);
    int resolution_bits = (int)luaL_optinteger(state, 4, 15);

    if (context == NULL || !espclaw_app_has_permission(context->manifest, "pwm.write")) {
        return lua_push_permission_error(state, "pwm.write");
    }
    if (lua_resolve_pin_argument(state, 2, &pin) != 0) {
        lua_pushnil(state);
        lua_pushstring(state, "invalid servo pin or alias");
        return 2;
    }
    if (espclaw_hw_pwm_setup(channel, pin, frequency_hz, resolution_bits) != 0) {
        lua_pushnil(state);
        lua_pushstring(state, "servo attach failed");
        return 2;
    }

    lua_pushboolean(state, 1);
    return 1;
}

static int lua_espclaw_servo_write_us(lua_State *state)
{
    return lua_espclaw_pwm_write_us(state);
}

static int lua_espclaw_servo_write_norm(lua_State *state)
{
    espclaw_lua_context_t *context = lua_get_context(state);
    int channel = (int)luaL_checkinteger(state, 1);
    double value = luaL_checknumber(state, 2);
    int min_us = (int)luaL_optinteger(state, 3, 1000);
    int max_us = (int)luaL_optinteger(state, 4, 2000);
    int pulse_width_us;

    if (context == NULL || !espclaw_app_has_permission(context->manifest, "pwm.write")) {
        return lua_push_permission_error(state, "pwm.write");
    }
    if (min_us <= 0 || max_us <= min_us || value < -1.0 || value > 1.0) {
        lua_pushnil(state);
        lua_pushstring(state, "servo norm parameters invalid");
        return 2;
    }

    pulse_width_us = min_us + (int)(((value + 1.0) * (double)(max_us - min_us)) / 2.0);
    if (espclaw_hw_pwm_write_us(channel, pulse_width_us) != 0) {
        lua_pushnil(state);
        lua_pushstring(state, "servo normalized write failed");
        return 2;
    }

    lua_pushinteger(state, (lua_Integer)pulse_width_us);
    return 1;
}

static int lua_espclaw_buzzer_tone(lua_State *state)
{
    espclaw_lua_context_t *context = lua_get_context(state);
    int channel = (int)luaL_checkinteger(state, 1);
    int pin = -1;
    int frequency_hz = (int)luaL_checkinteger(state, 3);
    int duration_ms = (int)luaL_checkinteger(state, 4);
    int duty_percent = (int)luaL_optinteger(state, 5, 50);

    if (context == NULL || !espclaw_app_has_permission(context->manifest, "buzzer.play")) {
        return lua_push_permission_error(state, "buzzer.play");
    }
    if (lua_resolve_pin_argument(state, 2, &pin) != 0) {
        lua_pushnil(state);
        lua_pushstring(state, "invalid buzzer pin or alias");
        return 2;
    }
    if (espclaw_hw_buzzer_tone(channel, pin, frequency_hz, duration_ms, duty_percent) != 0) {
        lua_pushnil(state);
        lua_pushstring(state, "buzzer tone failed");
        return 2;
    }

    lua_pushboolean(state, 1);
    return 1;
}

static int lua_espclaw_adc_read_raw(lua_State *state)
{
    espclaw_lua_context_t *context = lua_get_context(state);
    int unit = (int)luaL_checkinteger(state, 1);
    int channel = (int)luaL_checkinteger(state, 2);
    int raw = 0;

    if (context == NULL || !espclaw_app_has_permission(context->manifest, "adc.read")) {
        return lua_push_permission_error(state, "adc.read");
    }
    if (espclaw_hw_adc_read_raw(unit, channel, &raw) != 0) {
        lua_pushnil(state);
        lua_pushstring(state, "adc read failed");
        return 2;
    }

    lua_pushinteger(state, (lua_Integer)raw);
    return 1;
}

static int lua_espclaw_adc_read_mv(lua_State *state)
{
    espclaw_lua_context_t *context = lua_get_context(state);
    int unit = (int)luaL_checkinteger(state, 1);
    int channel = (int)luaL_checkinteger(state, 2);
    int millivolts = 0;

    if (context == NULL || !espclaw_app_has_permission(context->manifest, "adc.read")) {
        return lua_push_permission_error(state, "adc.read");
    }
    if (espclaw_hw_adc_read_mv(unit, channel, &millivolts) != 0) {
        lua_pushnil(state);
        lua_pushstring(state, "adc millivolt read failed");
        return 2;
    }

    lua_pushinteger(state, (lua_Integer)millivolts);
    return 1;
}

static int lua_espclaw_adc_read_named_mv(lua_State *state)
{
    espclaw_lua_context_t *context = lua_get_context(state);
    const char *name = luaL_checkstring(state, 1);
    espclaw_board_adc_channel_t channel;
    int millivolts = 0;

    if (context == NULL || !espclaw_app_has_permission(context->manifest, "adc.read")) {
        return lua_push_permission_error(state, "adc.read");
    }
    if (espclaw_board_find_adc_channel(name, &channel) != 0) {
        lua_pushnil(state);
        lua_pushstring(state, "unknown board adc channel");
        return 2;
    }
    if (espclaw_hw_adc_read_mv(channel.unit, channel.channel, &millivolts) != 0) {
        lua_pushnil(state);
        lua_pushstring(state, "adc millivolt read failed");
        return 2;
    }

    lua_pushinteger(state, (lua_Integer)millivolts);
    return 1;
}

static int lua_espclaw_i2c_begin(lua_State *state)
{
    espclaw_lua_context_t *context = lua_get_context(state);
    int port = (int)luaL_checkinteger(state, 1);
    int sda_pin = (int)luaL_checkinteger(state, 2);
    int scl_pin = (int)luaL_checkinteger(state, 3);
    int frequency_hz = (int)luaL_optinteger(state, 4, 400000);
    bool allowed = context != NULL &&
                   (espclaw_app_has_permission(context->manifest, "i2c.read") ||
                    espclaw_app_has_permission(context->manifest, "i2c.write") ||
                    espclaw_app_has_permission(context->manifest, "imu.read") ||
                    espclaw_app_has_permission(context->manifest, "temperature.read"));

    if (!allowed) {
        return lua_push_permission_error(state, "i2c.read/i2c.write/imu.read/temperature.read");
    }
    if (espclaw_hw_i2c_begin(port, sda_pin, scl_pin, frequency_hz) != 0) {
        lua_pushnil(state);
        lua_pushstring(state, "i2c begin failed");
        return 2;
    }

    lua_pushboolean(state, 1);
    return 1;
}

static int lua_espclaw_i2c_begin_board(lua_State *state)
{
    espclaw_lua_context_t *context = lua_get_context(state);
    const char *name = luaL_optstring(state, 1, "default");
    espclaw_board_i2c_bus_t bus;
    bool allowed = context != NULL &&
                   (espclaw_app_has_permission(context->manifest, "i2c.read") ||
                    espclaw_app_has_permission(context->manifest, "i2c.write") ||
                    espclaw_app_has_permission(context->manifest, "imu.read") ||
                    espclaw_app_has_permission(context->manifest, "temperature.read"));

    if (!allowed) {
        return lua_push_permission_error(state, "i2c.read/i2c.write/imu.read/temperature.read");
    }
    if (espclaw_board_find_i2c_bus(name, &bus) != 0) {
        lua_pushnil(state);
        lua_pushstring(state, "unknown board i2c bus");
        return 2;
    }
    if (espclaw_hw_i2c_begin(bus.port, bus.sda_pin, bus.scl_pin, bus.frequency_hz) != 0) {
        lua_pushnil(state);
        lua_pushstring(state, "i2c begin failed");
        return 2;
    }

    lua_pushboolean(state, 1);
    return 1;
}

static int lua_espclaw_i2c_scan(lua_State *state)
{
    espclaw_lua_context_t *context = lua_get_context(state);
    int port = (int)luaL_checkinteger(state, 1);
    uint8_t addresses[ESPCLAW_HW_I2C_SCAN_MAX];
    size_t count = 0;
    size_t index;

    if (context == NULL || !espclaw_app_has_permission(context->manifest, "i2c.read")) {
        return lua_push_permission_error(state, "i2c.read");
    }
    if (espclaw_hw_i2c_scan(port, addresses, sizeof(addresses), &count) != 0) {
        lua_pushnil(state);
        lua_pushstring(state, "i2c scan failed");
        return 2;
    }

    lua_newtable(state);
    for (index = 0; index < count; ++index) {
        lua_pushinteger(state, (lua_Integer)(index + 1));
        lua_pushinteger(state, (lua_Integer)addresses[index]);
        lua_settable(state, -3);
    }
    return 1;
}

static int lua_espclaw_i2c_read_reg(lua_State *state)
{
    espclaw_lua_context_t *context = lua_get_context(state);
    int port = (int)luaL_checkinteger(state, 1);
    int address = (int)luaL_checkinteger(state, 2);
    int reg = (int)luaL_checkinteger(state, 3);
    int length = (int)luaL_checkinteger(state, 4);
    uint8_t data[64];

    if (context == NULL || !espclaw_app_has_permission(context->manifest, "i2c.read")) {
        return lua_push_permission_error(state, "i2c.read");
    }
    if (length <= 0 || length > (int)sizeof(data) ||
        espclaw_hw_i2c_read_reg(port, (uint8_t)address, (uint8_t)reg, data, (size_t)length) != 0) {
        lua_pushnil(state);
        lua_pushstring(state, "i2c read failed");
        return 2;
    }

    lua_push_byte_table(state, data, (size_t)length);
    return 1;
}

static int lua_espclaw_i2c_write_reg(lua_State *state)
{
    espclaw_lua_context_t *context = lua_get_context(state);
    int port = (int)luaL_checkinteger(state, 1);
    int address = (int)luaL_checkinteger(state, 2);
    int reg = (int)luaL_checkinteger(state, 3);
    uint8_t data[64];
    size_t length = 0;

    if (context == NULL || !espclaw_app_has_permission(context->manifest, "i2c.write")) {
        return lua_push_permission_error(state, "i2c.write");
    }
    if (lua_read_bytes_argument(state, 4, data, sizeof(data), &length) != 0 ||
        espclaw_hw_i2c_write_reg(port, (uint8_t)address, (uint8_t)reg, data, length) != 0) {
        lua_pushnil(state);
        lua_pushstring(state, "i2c write failed");
        return 2;
    }

    lua_pushboolean(state, 1);
    return 1;
}

static int lua_espclaw_ppm_begin(lua_State *state)
{
    espclaw_lua_context_t *context = lua_get_context(state);
    int channel = (int)luaL_checkinteger(state, 1);
    int pin = -1;
    int frame_us = (int)luaL_optinteger(state, 3, 22500);
    int pulse_us = (int)luaL_optinteger(state, 4, 300);

    if (context == NULL || !espclaw_app_has_permission(context->manifest, "ppm.write")) {
        return lua_push_permission_error(state, "ppm.write");
    }
    if (lua_resolve_pin_argument(state, 2, &pin) != 0) {
        lua_pushnil(state);
        lua_pushstring(state, "invalid ppm pin or alias");
        return 2;
    }
    if (espclaw_hw_ppm_begin(channel, pin, frame_us, pulse_us) != 0) {
        lua_pushnil(state);
        lua_pushstring(state, "ppm begin failed");
        return 2;
    }

    lua_pushboolean(state, 1);
    return 1;
}

static int lua_espclaw_ppm_write(lua_State *state)
{
    espclaw_lua_context_t *context = lua_get_context(state);
    int channel = (int)luaL_checkinteger(state, 1);
    uint16_t outputs[ESPCLAW_HW_PPM_OUTPUT_MAX];
    size_t output_count = 0;

    if (context == NULL || !espclaw_app_has_permission(context->manifest, "ppm.write")) {
        return lua_push_permission_error(state, "ppm.write");
    }
    if (lua_read_u16_array_argument(state, 2, outputs, ESPCLAW_HW_PPM_OUTPUT_MAX, &output_count) != 0 ||
        espclaw_hw_ppm_write(channel, outputs, output_count) != 0) {
        lua_pushnil(state);
        lua_pushstring(state, "ppm write failed");
        return 2;
    }

    lua_pushboolean(state, 1);
    return 1;
}

static int lua_espclaw_ppm_state(lua_State *state)
{
    espclaw_lua_context_t *context = lua_get_context(state);
    int channel = (int)luaL_checkinteger(state, 1);
    espclaw_hw_ppm_state_t ppm_state;

    if (context == NULL || !espclaw_app_has_permission(context->manifest, "ppm.write")) {
        return lua_push_permission_error(state, "ppm.write");
    }
    if (espclaw_hw_ppm_state(channel, &ppm_state) != 0) {
        lua_pushnil(state);
        lua_pushstring(state, "ppm state unavailable");
        return 2;
    }

    lua_newtable(state);
    lua_pushboolean(state, ppm_state.configured);
    lua_setfield(state, -2, "configured");
    lua_pushinteger(state, (lua_Integer)ppm_state.pin);
    lua_setfield(state, -2, "pin");
    lua_pushinteger(state, (lua_Integer)ppm_state.frame_us);
    lua_setfield(state, -2, "frame_us");
    lua_pushinteger(state, (lua_Integer)ppm_state.pulse_us);
    lua_setfield(state, -2, "pulse_us");
    lua_pushinteger(state, (lua_Integer)ppm_state.output_count);
    lua_setfield(state, -2, "output_count");
    lua_push_u16_table(state, ppm_state.outputs, ppm_state.output_count);
    lua_setfield(state, -2, "outputs");
    return 1;
}

static int lua_espclaw_uart_read(lua_State *state)
{
    espclaw_lua_context_t *context = lua_get_context(state);
    int port = (int)luaL_checkinteger(state, 1);
    int length = (int)luaL_optinteger(state, 2, 256);
    uint8_t data[512];
    size_t bytes_read = 0;

    if (context == NULL || !espclaw_app_has_permission(context->manifest, "uart.read")) {
        return lua_push_permission_error(state, "uart.read");
    }
    if (length <= 0 || length > (int)sizeof(data) || espclaw_hw_uart_read(port, data, (size_t)length, &bytes_read) != 0) {
        lua_pushnil(state);
        lua_pushstring(state, "uart read failed");
        return 2;
    }

    lua_pushlstring(state, (const char *)data, bytes_read);
    return 1;
}

static int lua_espclaw_uart_write(lua_State *state)
{
    espclaw_lua_context_t *context = lua_get_context(state);
    int port = (int)luaL_checkinteger(state, 1);
    uint8_t data[512];
    size_t length = 0;
    size_t bytes_written = 0;

    if (context == NULL || !espclaw_app_has_permission(context->manifest, "uart.write")) {
        return lua_push_permission_error(state, "uart.write");
    }
    if (lua_read_bytes_argument(state, 2, data, sizeof(data), &length) != 0 ||
        espclaw_hw_uart_write(port, data, length, &bytes_written) != 0) {
        lua_pushnil(state);
        lua_pushstring(state, "uart write failed");
        return 2;
    }

    lua_pushinteger(state, (lua_Integer)bytes_written);
    return 1;
}

static int lua_espclaw_tmp102_c(lua_State *state)
{
    espclaw_lua_context_t *context = lua_get_context(state);
    int port = (int)luaL_checkinteger(state, 1);
    int address = (int)luaL_optinteger(state, 2, 0x48);
    double temperature_c = 0.0;

    if (context == NULL || !espclaw_app_has_permission(context->manifest, "temperature.read")) {
        return lua_push_permission_error(state, "temperature.read");
    }
    if (espclaw_hw_tmp102_read_c(port, (uint8_t)address, &temperature_c) != 0) {
        lua_pushnil(state);
        lua_pushstring(state, "temperature read failed");
        return 2;
    }

    lua_pushnumber(state, temperature_c);
    return 1;
}

static int lua_espclaw_mpu6050_begin(lua_State *state)
{
    espclaw_lua_context_t *context = lua_get_context(state);
    int port = (int)luaL_checkinteger(state, 1);
    int address = (int)luaL_optinteger(state, 2, 0x68);

    if (context == NULL || !espclaw_app_has_permission(context->manifest, "imu.read")) {
        return lua_push_permission_error(state, "imu.read");
    }
    if (espclaw_hw_mpu6050_begin(port, (uint8_t)address) != 0) {
        lua_pushnil(state);
        lua_pushstring(state, "imu begin failed");
        return 2;
    }

    lua_pushboolean(state, 1);
    return 1;
}

static int lua_espclaw_mpu6050_read(lua_State *state)
{
    espclaw_lua_context_t *context = lua_get_context(state);
    int port = (int)luaL_checkinteger(state, 1);
    int address = (int)luaL_optinteger(state, 2, 0x68);
    espclaw_hw_mpu6050_sample_t sample;

    if (context == NULL || !espclaw_app_has_permission(context->manifest, "imu.read")) {
        return lua_push_permission_error(state, "imu.read");
    }
    if (espclaw_hw_mpu6050_read(port, (uint8_t)address, &sample) != 0) {
        lua_pushnil(state);
        lua_pushstring(state, "imu read failed");
        return 2;
    }

    lua_newtable(state);
    lua_pushnumber(state, sample.accel_x_g);
    lua_setfield(state, -2, "accel_x_g");
    lua_pushnumber(state, sample.accel_y_g);
    lua_setfield(state, -2, "accel_y_g");
    lua_pushnumber(state, sample.accel_z_g);
    lua_setfield(state, -2, "accel_z_g");
    lua_pushnumber(state, sample.gyro_x_dps);
    lua_setfield(state, -2, "gyro_x_dps");
    lua_pushnumber(state, sample.gyro_y_dps);
    lua_setfield(state, -2, "gyro_y_dps");
    lua_pushnumber(state, sample.gyro_z_dps);
    lua_setfield(state, -2, "gyro_z_dps");
    lua_pushnumber(state, sample.temperature_c);
    lua_setfield(state, -2, "temperature_c");
    return 1;
}

static int lua_espclaw_pid_step(lua_State *state)
{
    double setpoint = luaL_checknumber(state, 1);
    double measurement = luaL_checknumber(state, 2);
    double integral = luaL_checknumber(state, 3);
    double previous_error = luaL_checknumber(state, 4);
    double kp = luaL_checknumber(state, 5);
    double ki = luaL_checknumber(state, 6);
    double kd = luaL_checknumber(state, 7);
    double dt_seconds = luaL_checknumber(state, 8);
    double output_min = luaL_optnumber(state, 9, -1000000.0);
    double output_max = luaL_optnumber(state, 10, 1000000.0);
    double next_integral = 0.0;
    double error = 0.0;
    double output = espclaw_hw_pid_step(
        setpoint,
        measurement,
        integral,
        previous_error,
        kp,
        ki,
        kd,
        dt_seconds,
        output_min,
        output_max,
        &next_integral,
        &error
    );

    lua_pushnumber(state, output);
    lua_pushnumber(state, next_integral);
    lua_pushnumber(state, error);
    return 3;
}

static int lua_espclaw_complementary_roll_pitch(lua_State *state)
{
    espclaw_hw_mpu6050_sample_t sample = {0};
    double previous_roll = luaL_checknumber(state, 2);
    double previous_pitch = luaL_checknumber(state, 3);
    double alpha = luaL_optnumber(state, 4, 0.98);
    double dt_seconds = luaL_optnumber(state, 5, 0.01);
    double roll = 0.0;
    double pitch = 0.0;

    luaL_checktype(state, 1, LUA_TTABLE);
    lua_getfield(state, 1, "accel_x_g");
    sample.accel_x_g = luaL_checknumber(state, -1);
    lua_pop(state, 1);
    lua_getfield(state, 1, "accel_y_g");
    sample.accel_y_g = luaL_checknumber(state, -1);
    lua_pop(state, 1);
    lua_getfield(state, 1, "accel_z_g");
    sample.accel_z_g = luaL_checknumber(state, -1);
    lua_pop(state, 1);
    lua_getfield(state, 1, "gyro_x_dps");
    sample.gyro_x_dps = luaL_checknumber(state, -1);
    lua_pop(state, 1);
    lua_getfield(state, 1, "gyro_y_dps");
    sample.gyro_y_dps = luaL_checknumber(state, -1);
    lua_pop(state, 1);
    lua_getfield(state, 1, "gyro_z_dps");
    sample.gyro_z_dps = luaL_optnumber(state, -1, 0.0);
    lua_pop(state, 1);

    if (espclaw_hw_complementary_roll_pitch(&sample, previous_roll, previous_pitch, alpha, dt_seconds, &roll, &pitch) != 0) {
        lua_pushnil(state);
        lua_pushstring(state, "attitude estimate failed");
        return 2;
    }

    lua_pushnumber(state, roll);
    lua_pushnumber(state, pitch);
    return 2;
}

static int lua_espclaw_mix_differential(lua_State *state)
{
    double throttle = luaL_checknumber(state, 1);
    double turn = luaL_checknumber(state, 2);
    double output_min = luaL_optnumber(state, 3, -1.0);
    double output_max = luaL_optnumber(state, 4, 1.0);
    double left = 0.0;
    double right = 0.0;

    if (espclaw_hw_mix_differential_drive(throttle, turn, output_min, output_max, &left, &right) != 0) {
        lua_pushnil(state);
        lua_pushstring(state, "differential mix failed");
        return 2;
    }

    lua_newtable(state);
    lua_pushnumber(state, left);
    lua_setfield(state, -2, "left");
    lua_pushnumber(state, right);
    lua_setfield(state, -2, "right");
    return 1;
}

static int lua_espclaw_mix_quad_x(lua_State *state)
{
    double throttle = luaL_checknumber(state, 1);
    double roll = luaL_checknumber(state, 2);
    double pitch = luaL_checknumber(state, 3);
    double yaw = luaL_checknumber(state, 4);
    double output_min = luaL_optnumber(state, 5, 0.0);
    double output_max = luaL_optnumber(state, 6, 1.0);
    double front_left = 0.0;
    double front_right = 0.0;
    double rear_right = 0.0;
    double rear_left = 0.0;

    if (espclaw_hw_mix_quad_x(
            throttle, roll, pitch, yaw, output_min, output_max,
            &front_left, &front_right, &rear_right, &rear_left) != 0) {
        lua_pushnil(state);
        lua_pushstring(state, "quad mix failed");
        return 2;
    }

    lua_newtable(state);
    lua_pushnumber(state, front_left);
    lua_setfield(state, -2, "front_left");
    lua_pushnumber(state, front_right);
    lua_setfield(state, -2, "front_right");
    lua_pushnumber(state, rear_right);
    lua_setfield(state, -2, "rear_right");
    lua_pushnumber(state, rear_left);
    lua_setfield(state, -2, "rear_left");
    return 1;
}

static int lua_espclaw_ticks_ms(lua_State *state)
{
    lua_pushinteger(state, (lua_Integer)espclaw_hw_ticks_ms());
    return 1;
}

static int lua_espclaw_sleep_ms(lua_State *state)
{
    int duration_ms = (int)luaL_checkinteger(state, 1);

    if (duration_ms < 0) {
        lua_pushnil(state);
        lua_pushstring(state, "sleep duration must be non-negative");
        return 2;
    }

    espclaw_hw_sleep_ms((uint32_t)duration_ms);
    lua_pushboolean(state, 1);
    return 1;
}

static int lua_espclaw_camera_capture(lua_State *state)
{
    espclaw_lua_context_t *context = lua_get_context(state);
    const char *filename = luaL_optstring(state, 1, "");
    espclaw_hw_camera_capture_t capture;

    if (context == NULL || !espclaw_app_has_permission(context->manifest, "camera.capture")) {
        return lua_push_permission_error(state, "camera.capture");
    }
    if (espclaw_hw_camera_capture(
            context->workspace_root,
            filename != NULL && filename[0] != '\0' ? filename : NULL,
            &capture) != 0) {
        lua_pushnil(state);
        lua_pushstring(state, capture.error[0] != '\0' ? capture.error : "camera capture failed");
        return 2;
    }

    lua_newtable(state);
    lua_pushstring(state, capture.relative_path);
    lua_setfield(state, -2, "path");
    lua_pushstring(state, capture.mime_type);
    lua_setfield(state, -2, "mime_type");
    lua_pushinteger(state, (lua_Integer)capture.bytes_written);
    lua_setfield(state, -2, "bytes");
    lua_pushinteger(state, (lua_Integer)capture.width);
    lua_setfield(state, -2, "width");
    lua_pushinteger(state, (lua_Integer)capture.height);
    lua_setfield(state, -2, "height");
    lua_pushboolean(state, capture.simulated);
    lua_setfield(state, -2, "simulated");
    return 1;
}

static int lua_espclaw_event_emit(lua_State *state)
{
    espclaw_lua_context_t *context = lua_get_context(state);
    const char *event_name = luaL_checkstring(state, 1);
    const char *payload = luaL_optstring(state, 2, "");
    char message[160];

    if (context == NULL || !espclaw_app_has_permission(context->manifest, "task.control")) {
        return lua_push_permission_error(state, "task.control");
    }
    if (espclaw_task_emit_event(event_name, payload, message, sizeof(message)) != 0) {
        lua_pushnil(state);
        lua_pushstring(state, message[0] != '\0' ? message : "event dispatch failed");
        return 2;
    }

    lua_pushboolean(state, 1);
    lua_pushstring(state, message);
    return 2;
}

static int lua_espclaw_event_watch_list(lua_State *state)
{
    char json[2048];

    if (espclaw_event_watch_render_json(json, sizeof(json)) != 0) {
        lua_pushnil(state);
        lua_pushstring(state, "failed to render watches");
        return 2;
    }

    lua_pushstring(state, json);
    return 1;
}

static int lua_espclaw_event_watch_uart(lua_State *state)
{
    espclaw_lua_context_t *context = lua_get_context(state);
    const char *watch_id = luaL_checkstring(state, 1);
    const char *event_name = luaL_optstring(state, 2, "uart");
    int port = (int)luaL_optinteger(state, 3, 0);
    char message[160];

    if (context == NULL || !espclaw_app_has_permission(context->manifest, "task.control")) {
        return lua_push_permission_error(state, "task.control");
    }
    if (espclaw_event_watch_add_uart(watch_id, event_name, port, message, sizeof(message)) != 0) {
        lua_pushnil(state);
        lua_pushstring(state, message[0] != '\0' ? message : "failed to add uart watch");
        return 2;
    }

    lua_pushboolean(state, 1);
    lua_pushstring(state, message);
    return 2;
}

static int lua_espclaw_event_watch_adc_threshold(lua_State *state)
{
    espclaw_lua_context_t *context = lua_get_context(state);
    const char *watch_id = luaL_checkstring(state, 1);
    int unit = (int)luaL_checkinteger(state, 2);
    int channel = (int)luaL_checkinteger(state, 3);
    int threshold = (int)luaL_checkinteger(state, 4);
    const char *event_name = luaL_optstring(state, 5, "sensor");
    int interval_ms = (int)luaL_optinteger(state, 6, 100);
    char message[160];

    if (context == NULL || !espclaw_app_has_permission(context->manifest, "task.control")) {
        return lua_push_permission_error(state, "task.control");
    }
    if (espclaw_event_watch_add_adc_threshold(
            watch_id,
            event_name,
            unit,
            channel,
            threshold,
            interval_ms > 0 ? (uint32_t)interval_ms : 100U,
            message,
            sizeof(message)) != 0) {
        lua_pushnil(state);
        lua_pushstring(state, message[0] != '\0' ? message : "failed to add adc watch");
        return 2;
    }

    lua_pushboolean(state, 1);
    lua_pushstring(state, message);
    return 2;
}

static int lua_espclaw_event_watch_remove(lua_State *state)
{
    espclaw_lua_context_t *context = lua_get_context(state);
    const char *watch_id = luaL_checkstring(state, 1);
    char message[160];

    if (context == NULL || !espclaw_app_has_permission(context->manifest, "task.control")) {
        return lua_push_permission_error(state, "task.control");
    }
    if (espclaw_event_watch_remove(watch_id, message, sizeof(message)) != 0) {
        lua_pushnil(state);
        lua_pushstring(state, message[0] != '\0' ? message : "failed to remove watch");
        return 2;
    }

    lua_pushboolean(state, 1);
    lua_pushstring(state, message);
    return 2;
}

static void register_lua_context_fn(lua_State *state, espclaw_lua_context_t *context, lua_CFunction fn, const char *name)
{
    lua_pushlightuserdata(state, context);
    lua_pushcclosure(state, fn, 1);
    lua_setfield(state, -2, name);
}

static void register_lua_table(lua_State *state, const char *name)
{
    lua_setfield(state, -2, name);
}

static void register_lua_bindings(lua_State *state, espclaw_lua_context_t *context)
{
    lua_newtable(state);

    register_lua_context_fn(state, context, lua_espclaw_log, "log");
    register_lua_context_fn(state, context, lua_espclaw_has_permission, "has_permission");
    register_lua_context_fn(state, context, lua_espclaw_read_file, "read_file");
    register_lua_context_fn(state, context, lua_espclaw_write_file, "write_file");
    register_lua_context_fn(state, context, lua_espclaw_list_apps, "list_apps");

    /* Keep file access namespaced for apps while preserving the older top-level helpers. */
    lua_newtable(state);
    register_lua_context_fn(state, context, lua_espclaw_read_file, "read");
    register_lua_context_fn(state, context, lua_espclaw_write_file, "write");
    register_lua_table(state, "fs");

    lua_newtable(state);
    lua_pushcfunction(state, lua_espclaw_board_variant);
    lua_setfield(state, -2, "variant");
    lua_pushcfunction(state, lua_espclaw_board_describe);
    lua_setfield(state, -2, "describe");
    lua_pushcfunction(state, lua_espclaw_board_pin);
    lua_setfield(state, -2, "pin");
    lua_pushcfunction(state, lua_espclaw_board_i2c);
    lua_setfield(state, -2, "i2c");
    lua_pushcfunction(state, lua_espclaw_board_uart);
    lua_setfield(state, -2, "uart");
    lua_pushcfunction(state, lua_espclaw_board_adc);
    lua_setfield(state, -2, "adc");
    register_lua_table(state, "board");

    lua_newtable(state);
    lua_pushcfunction(state, lua_espclaw_hardware_list);
    lua_setfield(state, -2, "list");
    register_lua_table(state, "hardware");

    lua_newtable(state);
    register_lua_context_fn(state, context, lua_espclaw_gpio_read, "read");
    register_lua_context_fn(state, context, lua_espclaw_gpio_write, "write");
    register_lua_table(state, "gpio");

    lua_newtable(state);
    register_lua_context_fn(state, context, lua_espclaw_pwm_setup, "setup");
    register_lua_context_fn(state, context, lua_espclaw_pwm_write, "write");
    register_lua_context_fn(state, context, lua_espclaw_pwm_write_us, "write_us");
    register_lua_context_fn(state, context, lua_espclaw_pwm_stop, "stop");
    register_lua_context_fn(state, context, lua_espclaw_pwm_state, "state");
    register_lua_table(state, "pwm");

    lua_newtable(state);
    register_lua_context_fn(state, context, lua_espclaw_servo_attach, "attach");
    register_lua_context_fn(state, context, lua_espclaw_servo_write_us, "write_us");
    register_lua_context_fn(state, context, lua_espclaw_servo_write_norm, "write_norm");
    register_lua_table(state, "servo");

    lua_newtable(state);
    register_lua_context_fn(state, context, lua_espclaw_adc_read_raw, "read_raw");
    register_lua_context_fn(state, context, lua_espclaw_adc_read_mv, "read_mv");
    register_lua_context_fn(state, context, lua_espclaw_adc_read_named_mv, "read_named_mv");
    register_lua_table(state, "adc");

    lua_newtable(state);
    register_lua_context_fn(state, context, lua_espclaw_i2c_begin, "begin");
    register_lua_context_fn(state, context, lua_espclaw_i2c_begin_board, "begin_board");
    register_lua_context_fn(state, context, lua_espclaw_i2c_scan, "scan");
    register_lua_context_fn(state, context, lua_espclaw_i2c_read_reg, "read_reg");
    register_lua_context_fn(state, context, lua_espclaw_i2c_write_reg, "write_reg");
    register_lua_table(state, "i2c");

    lua_newtable(state);
    register_lua_context_fn(state, context, lua_espclaw_ppm_begin, "begin");
    register_lua_context_fn(state, context, lua_espclaw_ppm_write, "write");
    register_lua_context_fn(state, context, lua_espclaw_ppm_state, "state");
    register_lua_table(state, "ppm");

    lua_newtable(state);
    register_lua_context_fn(state, context, lua_espclaw_uart_read, "read");
    register_lua_context_fn(state, context, lua_espclaw_uart_write, "write");
    register_lua_table(state, "uart");

    lua_newtable(state);
    register_lua_context_fn(state, context, lua_espclaw_camera_capture, "capture");
    register_lua_table(state, "camera");

    lua_newtable(state);
    register_lua_context_fn(state, context, lua_espclaw_tmp102_c, "tmp102_c");
    register_lua_table(state, "temperature");

    lua_newtable(state);
    register_lua_context_fn(state, context, lua_espclaw_mpu6050_begin, "mpu6050_begin");
    register_lua_context_fn(state, context, lua_espclaw_mpu6050_read, "mpu6050_read");
    lua_pushcfunction(state, lua_espclaw_complementary_roll_pitch);
    lua_setfield(state, -2, "complementary_roll_pitch");
    register_lua_table(state, "imu");

    lua_newtable(state);
    register_lua_context_fn(state, context, lua_espclaw_buzzer_tone, "tone");
    register_lua_table(state, "buzzer");

    lua_newtable(state);
    lua_pushcfunction(state, lua_espclaw_pid_step);
    lua_setfield(state, -2, "step");
    register_lua_table(state, "pid");

    lua_newtable(state);
    lua_pushcfunction(state, lua_espclaw_mix_differential);
    lua_setfield(state, -2, "mix_differential");
    lua_pushcfunction(state, lua_espclaw_mix_quad_x);
    lua_setfield(state, -2, "mix_quad_x");
    register_lua_table(state, "control");

    lua_newtable(state);
    lua_pushcfunction(state, lua_espclaw_ticks_ms);
    lua_setfield(state, -2, "ticks_ms");
    lua_pushcfunction(state, lua_espclaw_sleep_ms);
    lua_setfield(state, -2, "sleep_ms");
    register_lua_table(state, "time");

    lua_newtable(state);
    register_lua_context_fn(state, context, lua_espclaw_event_emit, "emit");
    register_lua_context_fn(state, context, lua_espclaw_event_watch_list, "list");
    register_lua_context_fn(state, context, lua_espclaw_event_watch_uart, "watch_uart");
    register_lua_context_fn(state, context, lua_espclaw_event_watch_adc_threshold, "watch_adc_threshold");
    register_lua_context_fn(state, context, lua_espclaw_event_watch_remove, "remove_watch");
    register_lua_table(state, "events");

    lua_setglobal(state, "espclaw");
}

static int configure_lua_package_path(
    lua_State *state,
    const char *workspace_root,
    const char *app_id,
    char *buffer,
    size_t buffer_size
)
{
    char package_path[1536];
    const char *existing_path = NULL;

    if (state == NULL || workspace_root == NULL || app_id == NULL) {
        return -1;
    }

    lua_getglobal(state, "package");
    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        if (buffer != NULL && buffer_size > 0) {
            snprintf(buffer, buffer_size, "lua package table is unavailable");
        }
        return -1;
    }

    lua_getfield(state, -1, "path");
    if (lua_isstring(state, -1)) {
        existing_path = lua_tostring(state, -1);
    }

    if (snprintf(
            package_path,
            sizeof(package_path),
            "%s/lib/?.lua;%s/lib/?/init.lua;%s/apps/%s/lib/?.lua;%s/apps/%s/lib/?/init.lua;%s/apps/%s/?.lua;%s",
            workspace_root,
            workspace_root,
            workspace_root,
            app_id,
            workspace_root,
            app_id,
            workspace_root,
            app_id,
            existing_path != NULL ? existing_path : ""
        ) >= (int)sizeof(package_path)) {
        lua_pop(state, 2);
        if (buffer != NULL && buffer_size > 0) {
            snprintf(buffer, buffer_size, "lua package.path exceeds buffer size");
        }
        return -1;
    }

    lua_pop(state, 1);
    lua_pushstring(state, package_path);
    lua_setfield(state, -2, "path");
    lua_pop(state, 1);
    return 0;
}

static int run_lua_handle(
    espclaw_app_vm_t *vm,
    const char *trigger,
    const char *payload,
    char *buffer,
    size_t buffer_size
)
{
    char handler_name[64];
    char module_handler_name[64];
    const char *result = NULL;

    if (vm == NULL || vm->state == NULL || trigger == NULL || buffer == NULL || buffer_size == 0) {
        return -1;
    }
    if (!espclaw_app_supports_trigger(&vm->manifest, trigger)) {
        snprintf(buffer, buffer_size, "app %s does not accept trigger %s", vm->manifest.app_id, trigger);
        return -1;
    }

    snprintf(handler_name, sizeof(handler_name), "on_");
    {
        size_t used = strlen(handler_name);
        size_t index;

        for (index = 0; trigger[index] != '\0' && used + 1 < sizeof(handler_name); ++index) {
            unsigned char c = (unsigned char)trigger[index];

            handler_name[used++] = (char)(isalnum(c) ? tolower(c) : '_');
            handler_name[used] = '\0';
        }
    }
    snprintf(module_handler_name, sizeof(module_handler_name), "%s", handler_name + 3);

    lua_getglobal(vm->state, handler_name);
    if (lua_isfunction(vm->state, -1)) {
        lua_pushstring(vm->state, payload != NULL ? payload : "");
        if (lua_pcall(vm->state, 1, 1, 0) != LUA_OK) {
            snprintf(buffer, buffer_size, "lua runtime failed: %s", lua_tostring(vm->state, -1));
            lua_pop(vm->state, 1);
            return -1;
        }
    } else {
        lua_pop(vm->state, 1);
        lua_getglobal(vm->state, "on_event");
        if (lua_isfunction(vm->state, -1)) {
            lua_pushstring(vm->state, trigger);
            lua_pushstring(vm->state, payload != NULL ? payload : "");
            if (lua_pcall(vm->state, 2, 1, 0) != LUA_OK) {
                snprintf(buffer, buffer_size, "lua runtime failed: %s", lua_tostring(vm->state, -1));
                lua_pop(vm->state, 1);
                return -1;
            }
        } else {
            lua_pop(vm->state, 1);
            lua_getglobal(vm->state, "handle");
            if (lua_isfunction(vm->state, -1)) {
                lua_pushstring(vm->state, trigger);
                lua_pushstring(vm->state, payload != NULL ? payload : "");
                if (lua_pcall(vm->state, 2, 1, 0) != LUA_OK) {
                    snprintf(buffer, buffer_size, "lua runtime failed: %s", lua_tostring(vm->state, -1));
                    lua_pop(vm->state, 1);
                    return -1;
                }
            } else if (vm->module_ref != LUA_NOREF && vm->module_ref != LUA_REFNIL) {
                lua_pop(vm->state, 1);
                lua_rawgeti(vm->state, LUA_REGISTRYINDEX, vm->module_ref);
                if (!lua_istable(vm->state, -1)) {
                    lua_pop(vm->state, 1);
                    snprintf(
                        buffer,
                        buffer_size,
                        "app %s missing %s(payload), on_event(trigger, payload), handle(trigger, payload), or module handlers",
                        vm->manifest.app_id,
                        handler_name
                    );
                    return -1;
                }

                lua_getfield(vm->state, -1, handler_name);
                if (lua_isfunction(vm->state, -1)) {
                    lua_remove(vm->state, -2);
                    lua_pushstring(vm->state, payload != NULL ? payload : "");
                    if (lua_pcall(vm->state, 1, 1, 0) != LUA_OK) {
                        snprintf(buffer, buffer_size, "lua runtime failed: %s", lua_tostring(vm->state, -1));
                        lua_pop(vm->state, 1);
                        return -1;
                    }
                } else {
                    lua_pop(vm->state, 1);
                    lua_getfield(vm->state, -1, module_handler_name);
                    if (lua_isfunction(vm->state, -1)) {
                        lua_remove(vm->state, -2);
                        lua_pushstring(vm->state, payload != NULL ? payload : "");
                        if (lua_pcall(vm->state, 1, 1, 0) != LUA_OK) {
                            snprintf(buffer, buffer_size, "lua runtime failed: %s", lua_tostring(vm->state, -1));
                            lua_pop(vm->state, 1);
                            return -1;
                        }
                    } else {
                        lua_pop(vm->state, 1);
                        lua_getfield(vm->state, -1, "on_event");
                        if (lua_isfunction(vm->state, -1)) {
                            lua_remove(vm->state, -2);
                            lua_pushstring(vm->state, trigger);
                            lua_pushstring(vm->state, payload != NULL ? payload : "");
                            if (lua_pcall(vm->state, 2, 1, 0) != LUA_OK) {
                                snprintf(buffer, buffer_size, "lua runtime failed: %s", lua_tostring(vm->state, -1));
                                lua_pop(vm->state, 1);
                                return -1;
                            }
                        } else {
                            lua_pop(vm->state, 1);
                            lua_getfield(vm->state, -1, "handle");
                            if (!lua_isfunction(vm->state, -1)) {
                                snprintf(
                                    buffer,
                                    buffer_size,
                                    "app %s missing %s(payload), on_event(trigger, payload), handle(trigger, payload), or module handlers",
                                    vm->manifest.app_id,
                                    handler_name
                                );
                                lua_pop(vm->state, 2);
                                return -1;
                            }

                            lua_remove(vm->state, -2);
                            lua_pushstring(vm->state, trigger);
                            lua_pushstring(vm->state, payload != NULL ? payload : "");
                            if (lua_pcall(vm->state, 2, 1, 0) != LUA_OK) {
                                snprintf(buffer, buffer_size, "lua runtime failed: %s", lua_tostring(vm->state, -1));
                                lua_pop(vm->state, 1);
                                return -1;
                            }
                        }
                    }
                }
            } else {
                snprintf(
                    buffer,
                    buffer_size,
                    "app %s missing %s(payload), on_event(trigger, payload), handle(trigger, payload), or module handlers",
                    vm->manifest.app_id,
                    handler_name
                );
                lua_pop(vm->state, 1);
                return -1;
            }
        }
    }

    if (lua_isstring(vm->state, -1)) {
        result = lua_tostring(vm->state, -1);
    }

    snprintf(buffer, buffer_size, "%s", result != NULL ? result : "");
    lua_pop(vm->state, 1);
    return 0;
}
#endif

int espclaw_app_vm_open(
    const char *workspace_root,
    const char *app_id,
    espclaw_app_vm_t **vm_out,
    char *buffer,
    size_t buffer_size
)
{
    espclaw_app_vm_t *vm = NULL;

    if (vm_out == NULL || workspace_root == NULL || app_id == NULL) {
        return -1;
    }
    *vm_out = NULL;
    if (buffer != NULL && buffer_size > 0) {
        buffer[0] = '\0';
    }

#if defined(ESP_PLATFORM) || defined(ESPCLAW_HOST_LUA)
    vm = (espclaw_app_vm_t *)calloc(1, sizeof(*vm));
    if (vm == NULL) {
        if (buffer != NULL && buffer_size > 0) {
            snprintf(buffer, buffer_size, "out of memory");
        }
        return -1;
    }
    if (espclaw_app_load_manifest(workspace_root, app_id, &vm->manifest) != 0 ||
        build_app_absolute_path(workspace_root, app_id, vm->manifest.entrypoint, vm->script_path, sizeof(vm->script_path)) != 0) {
        free(vm);
        if (buffer != NULL && buffer_size > 0) {
            snprintf(buffer, buffer_size, "failed to load app manifest");
        }
        return -1;
    }

    snprintf(vm->workspace_root, sizeof(vm->workspace_root), "%s", workspace_root);
    vm->context.workspace_root = vm->workspace_root;
    vm->context.manifest = &vm->manifest;
    vm->context.app_id = vm->manifest.app_id;
    vm->module_ref = LUA_NOREF;
    vm->state = luaL_newstate();
    if (vm->state == NULL) {
        free(vm);
        if (buffer != NULL && buffer_size > 0) {
            snprintf(buffer, buffer_size, "failed to create lua state");
        }
        return -1;
    }

    luaL_openlibs(vm->state);
    if (configure_lua_package_path(vm->state, vm->workspace_root, vm->manifest.app_id, buffer, buffer_size) != 0) {
        lua_close(vm->state);
        free(vm);
        return -1;
    }
    register_lua_bindings(vm->state, &vm->context);
    if (luaL_loadfile(vm->state, vm->script_path) != LUA_OK || lua_pcall(vm->state, 0, 1, 0) != LUA_OK) {
        if (buffer != NULL && buffer_size > 0) {
            snprintf(buffer, buffer_size, "lua load failed: %s", lua_tostring(vm->state, -1));
        }
        lua_close(vm->state);
        free(vm);
        return -1;
    }
    if (lua_istable(vm->state, -1)) {
        vm->module_ref = luaL_ref(vm->state, LUA_REGISTRYINDEX);
    } else {
        lua_pop(vm->state, 1);
    }

    *vm_out = vm;
    return 0;
#else
    (void)buffer;
    (void)buffer_size;
    return -1;
#endif
}

int espclaw_app_vm_step(
    espclaw_app_vm_t *vm,
    const char *trigger,
    const char *payload,
    char *buffer,
    size_t buffer_size
)
{
#if defined(ESP_PLATFORM) || defined(ESPCLAW_HOST_LUA)
    return run_lua_handle(vm, trigger, payload, buffer, buffer_size);
#else
    (void)vm;
    (void)trigger;
    (void)payload;
    if (buffer != NULL && buffer_size > 0) {
        snprintf(buffer, buffer_size, "lua runtime is only available in firmware builds");
    }
    return -1;
#endif
}

void espclaw_app_vm_close(espclaw_app_vm_t *vm)
{
    if (vm == NULL) {
        return;
    }

#if defined(ESP_PLATFORM) || defined(ESPCLAW_HOST_LUA)
    if (vm->state != NULL) {
        if (vm->module_ref != LUA_NOREF && vm->module_ref != LUA_REFNIL) {
            luaL_unref(vm->state, LUA_REGISTRYINDEX, vm->module_ref);
        }
        lua_close(vm->state);
    }
#endif
    free(vm);
}

int espclaw_app_run(
    const char *workspace_root,
    const char *app_id,
    const char *trigger,
    const char *payload,
    char *buffer,
    size_t buffer_size
)
{
    espclaw_app_vm_t *vm = NULL;
    int result;

    if (workspace_root == NULL || trigger == NULL || buffer == NULL || buffer_size == 0) {
        return -1;
    }

    buffer[0] = '\0';
    if (espclaw_app_vm_open(workspace_root, app_id, &vm, buffer, buffer_size) != 0) {
        return -1;
    }

    result = espclaw_app_vm_step(vm, trigger, payload, buffer, buffer_size);
    espclaw_app_vm_close(vm);
    return result;
}

int espclaw_app_run_boot_apps(
    const char *workspace_root,
    char *buffer,
    size_t buffer_size
)
{
    char ids[32][ESPCLAW_APP_ID_MAX + 1];
    char run_result[256];
    size_t count = 0;
    size_t index;
    size_t written = 0;

    if (workspace_root == NULL || buffer == NULL || buffer_size == 0) {
        return -1;
    }

    buffer[0] = '\0';
    if (espclaw_app_collect_ids(workspace_root, ids, 32, &count) != 0) {
        return -1;
    }

    for (index = 0; index < count; ++index) {
        espclaw_app_manifest_t manifest;

        if (espclaw_app_load_manifest(workspace_root, ids[index], &manifest) != 0 ||
            !espclaw_app_supports_trigger(&manifest, "boot")) {
            continue;
        }

        if (espclaw_app_run(workspace_root, ids[index], "boot", "", run_result, sizeof(run_result)) == 0) {
            written += (size_t)snprintf(
                buffer + written,
                buffer_size - written,
                "%s%s=%s",
                written == 0 ? "" : ";",
                ids[index],
                run_result
            );
        } else {
            written += (size_t)snprintf(
                buffer + written,
                buffer_size - written,
                "%s%s=error",
                written == 0 ? "" : ";",
                ids[index]
            );
        }

        if (written >= buffer_size) {
            buffer[buffer_size - 1] = '\0';
            break;
        }
    }

    return 0;
}
