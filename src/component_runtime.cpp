#include "espclaw/component_runtime.h"

#include <stdbool.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "espclaw/workspace.h"
#include "espclaw/web_tools.h"

static bool component_id_valid(const char *value)
{
    size_t index;

    if (value == NULL || value[0] == '\0') {
        return false;
    }
    for (index = 0; value[index] != '\0'; ++index) {
        if (!(isalnum((unsigned char)value[index]) || value[index] == '_' || value[index] == '-')) {
            return false;
        }
    }
    return index <= ESPCLAW_COMPONENT_ID_MAX;
}

static bool module_name_valid(const char *value)
{
    size_t index;
    char previous = '\0';

    if (value == NULL || value[0] == '\0') {
        return false;
    }
    for (index = 0; value[index] != '\0'; ++index) {
        char c = value[index];

        if (!(isalnum((unsigned char)c) || c == '_' || c == '.')) {
            return false;
        }
        if (c == '.' && (previous == '\0' || previous == '.')) {
            return false;
        }
        previous = c;
    }
    return previous != '.' && index <= ESPCLAW_COMPONENT_MODULE_MAX;
}

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
        }
        buffer[index++] = *cursor++;
    }
    if (*cursor != '"') {
        return false;
    }
    buffer[index] = '\0';
    return true;
}

static const char *find_key(const char *json, const char *key)
{
    char pattern[48];

    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    return strstr(json, pattern);
}

static bool extract_string_after_key(const char *json, const char *key, char *buffer, size_t buffer_size)
{
    const char *key_start = find_key(json, key);
    const char *value_start;

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

static bool extract_string_after_any_key(
    const char *json,
    const char *const *keys,
    size_t key_count,
    char *buffer,
    size_t buffer_size
)
{
    size_t index;

    if (keys == NULL) {
        return false;
    }
    for (index = 0; index < key_count; ++index) {
        if (extract_string_after_key(json, keys[index], buffer, buffer_size)) {
            return true;
        }
    }
    return false;
}

static bool extract_string_array_after_key(
    const char *json,
    const char *key,
    char values[][ESPCLAW_COMPONENT_URL_MAX + 1],
    size_t max_values,
    size_t *count_out
)
{
    const char *key_start = find_key(json, key);
    const char *cursor;
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
        if (*cursor != '"' || count >= max_values) {
            return false;
        }
        if (!copy_json_string_value(cursor, values[count], ESPCLAW_COMPONENT_URL_MAX + 1U)) {
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
    return (mkdir(path, 0755) == 0 || errno == EEXIST) ? 0 : -1;
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

static int write_text_file(const char *path, const char *content)
{
    FILE *handle;

    if (ensure_parent_directories(path) != 0) {
        return -1;
    }
    handle = fopen(path, "w");
    if (handle == NULL) {
        return -1;
    }
    fputs(content != NULL ? content : "", handle);
    fclose(handle);
    return 0;
}

static int write_component_manifest(
    const char *path,
    const char *component_id,
    const char *effective_version,
    const char *effective_title,
    const char *module_name,
    const char *effective_summary,
    const char *lib_relative,
    const char *manifest_url,
    const char *source_url,
    const char *docs_url,
    const char *checksum,
    char dependencies[][ESPCLAW_COMPONENT_URL_MAX + 1],
    size_t dependency_count
)
{
    char manifest_json[2048];
    size_t used = 0;
    size_t index;

    if (path == NULL) {
        return -1;
    }
    used += (size_t)snprintf(
        manifest_json + used,
        sizeof(manifest_json) - used,
        "{\n"
        "  \"id\": \"%s\",\n"
        "  \"version\": \"%s\",\n"
        "  \"title\": \"%s\",\n"
        "  \"module\": \"%s\",\n"
        "  \"summary\": \"%s\",\n"
        "  \"source\": \"module.lua\",\n"
        "  \"lib_path\": \"%s\",\n"
        "  \"manifest_url\": \"%s\",\n"
        "  \"source_url\": \"%s\",\n"
        "  \"docs_url\": \"%s\",\n"
        "  \"checksum\": \"%s\",\n"
        "  \"dependencies\": [",
        component_id,
        effective_version,
        effective_title,
        module_name,
        effective_summary,
        lib_relative,
        manifest_url != NULL ? manifest_url : "",
        source_url != NULL ? source_url : "",
        docs_url != NULL ? docs_url : "",
        checksum != NULL ? checksum : ""
    );
    for (index = 0; index < dependency_count && used + 8 < sizeof(manifest_json); ++index) {
        used += (size_t)snprintf(
            manifest_json + used,
            sizeof(manifest_json) - used,
            "%s\"%s\"",
            index == 0 ? "" : ", ",
            dependencies[index]
        );
    }
    used += (size_t)snprintf(manifest_json + used, sizeof(manifest_json) - used, "]\n}\n");
    return write_text_file(path, manifest_json);
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

static int build_component_relative_path(const char *component_id, const char *leaf, char *buffer, size_t buffer_size)
{
    if (!component_id_valid(component_id) || leaf == NULL || buffer == NULL || buffer_size == 0) {
        return -1;
    }
    return snprintf(buffer, buffer_size, "components/%s/%s", component_id, leaf) >= (int)buffer_size ? -1 : 0;
}

static int module_name_to_relative_path(const char *module_name, char *buffer, size_t buffer_size)
{
    size_t used = 0;
    size_t index;

    if (!module_name_valid(module_name) || buffer == NULL || buffer_size == 0) {
        return -1;
    }
    used += (size_t)snprintf(buffer + used, buffer_size - used, "lib/");
    for (index = 0; module_name[index] != '\0' && used + 6 < buffer_size; ++index) {
        buffer[used++] = module_name[index] == '.' ? '/' : module_name[index];
    }
    used += (size_t)snprintf(buffer + used, buffer_size - used, ".lua");
    return used < buffer_size ? 0 : -1;
}

static void append_json_escaped(char *buffer, size_t buffer_size, size_t *used_io, const char *text)
{
    size_t used = used_io != NULL ? *used_io : 0;
    const unsigned char *cursor = (const unsigned char *)(text != NULL ? text : "");

    if (buffer == NULL || buffer_size == 0 || used >= buffer_size) {
        return;
    }
    buffer[used++] = '"';
    while (*cursor != '\0' && used + 2 < buffer_size) {
        if (*cursor == '\\' || *cursor == '"') {
            buffer[used++] = '\\';
            buffer[used++] = (char)*cursor;
        } else if (*cursor == '\n') {
            buffer[used++] = '\\';
            buffer[used++] = 'n';
        } else {
            buffer[used++] = (char)*cursor;
        }
        cursor++;
    }
    if (used < buffer_size) {
        buffer[used++] = '"';
    }
    if (used < buffer_size) {
        buffer[used] = '\0';
    } else {
        buffer[buffer_size - 1] = '\0';
    }
    if (used_io != NULL) {
        *used_io = used;
    }
}

int espclaw_component_install(
    const char *workspace_root,
    const char *component_id,
    const char *title,
    const char *module_name,
    const char *summary,
    const char *version,
    const char *source
)
{
    char manifest_relative[192];
    char source_relative[192];
    char lib_relative[192];
    char manifest_path[512];
    char source_path[512];
    char lib_path[512];
    const char *effective_title = title != NULL && title[0] != '\0' ? title : component_id;
    const char *effective_summary = summary != NULL ? summary : "";
    const char *effective_version = version != NULL && version[0] != '\0' ? version : "0.1.0";
    char dependencies[ESPCLAW_COMPONENT_DEPENDENCY_MAX][ESPCLAW_COMPONENT_URL_MAX + 1] = {{0}};

    if (workspace_root == NULL || source == NULL || !component_id_valid(component_id) || !module_name_valid(module_name)) {
        return -1;
    }
    if (espclaw_workspace_bootstrap(workspace_root) != 0 ||
        build_component_relative_path(component_id, "component.json", manifest_relative, sizeof(manifest_relative)) != 0 ||
        build_component_relative_path(component_id, "module.lua", source_relative, sizeof(source_relative)) != 0 ||
        module_name_to_relative_path(module_name, lib_relative, sizeof(lib_relative)) != 0 ||
        espclaw_workspace_resolve_path(workspace_root, manifest_relative, manifest_path, sizeof(manifest_path)) != 0 ||
        espclaw_workspace_resolve_path(workspace_root, source_relative, source_path, sizeof(source_path)) != 0 ||
        espclaw_workspace_resolve_path(workspace_root, lib_relative, lib_path, sizeof(lib_path)) != 0) {
        return -1;
    }

    if (write_component_manifest(
            manifest_path,
            component_id,
            effective_version,
            effective_title,
            module_name,
            effective_summary,
            lib_relative,
            "",
            "",
            "",
            "",
            dependencies,
            0U) != 0 ||
        write_text_file(source_path, source) != 0 ||
        write_text_file(lib_path, source) != 0) {
        return -1;
    }
    return 0;
}

int espclaw_component_install_from_file(
    const char *workspace_root,
    const char *component_id,
    const char *title,
    const char *module_name,
    const char *summary,
    const char *version,
    const char *source_path
)
{
    char manifest_relative[192];
    char component_source_relative[192];
    char lib_relative[192];
    char manifest_path[512];
    char component_source_path[512];
    char lib_path[512];
    char input_path[512];
    const char *effective_title = title != NULL && title[0] != '\0' ? title : component_id;
    const char *effective_summary = summary != NULL ? summary : "";
    const char *effective_version = version != NULL && version[0] != '\0' ? version : "0.1.0";
    char dependencies[ESPCLAW_COMPONENT_DEPENDENCY_MAX][ESPCLAW_COMPONENT_URL_MAX + 1] = {{0}};

    if (workspace_root == NULL || source_path == NULL || source_path[0] == '\0' ||
        !component_id_valid(component_id) || !module_name_valid(module_name)) {
        return -1;
    }
    if (espclaw_workspace_bootstrap(workspace_root) != 0 ||
        build_component_relative_path(component_id, "component.json", manifest_relative, sizeof(manifest_relative)) != 0 ||
        build_component_relative_path(component_id, "module.lua", component_source_relative, sizeof(component_source_relative)) != 0 ||
        module_name_to_relative_path(module_name, lib_relative, sizeof(lib_relative)) != 0 ||
        espclaw_workspace_resolve_path(workspace_root, source_path, input_path, sizeof(input_path)) != 0 ||
        espclaw_workspace_resolve_path(workspace_root, manifest_relative, manifest_path, sizeof(manifest_path)) != 0 ||
        espclaw_workspace_resolve_path(workspace_root, component_source_relative, component_source_path, sizeof(component_source_path)) != 0 ||
        espclaw_workspace_resolve_path(workspace_root, lib_relative, lib_path, sizeof(lib_path)) != 0) {
        return -1;
    }

    if (write_component_manifest(
            manifest_path,
            component_id,
            effective_version,
            effective_title,
            module_name,
            effective_summary,
            lib_relative,
            "",
            "",
            "",
            "",
            dependencies,
            0U) != 0 ||
        copy_file_contents(input_path, component_source_path) != 0 ||
        copy_file_contents(input_path, lib_path) != 0) {
        return -1;
    }
    return 0;
}

int espclaw_component_install_from_blob(
    const char *workspace_root,
    const char *component_id,
    const char *title,
    const char *module_name,
    const char *summary,
    const char *version,
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
    return espclaw_component_install_from_file(
        workspace_root,
        component_id,
        title,
        module_name,
        summary,
        version,
        status.target_path
    );
}

int espclaw_component_install_from_url(
    const char *workspace_root,
    const char *component_id,
    const char *title,
    const char *module_name,
    const char *summary,
    const char *version,
    const char *source_url
)
{
    char temp_relative[128];
    char temp_absolute[512];
    int result;

    if (workspace_root == NULL || source_url == NULL || source_url[0] == '\0' || !component_id_valid(component_id)) {
        return -1;
    }
    if (snprintf(temp_relative, sizeof(temp_relative), "blobs/.staging/component_%s.lua", component_id) >= (int)sizeof(temp_relative) ||
        espclaw_workspace_bootstrap(workspace_root) != 0 ||
        espclaw_workspace_resolve_path(workspace_root, temp_relative, temp_absolute, sizeof(temp_absolute)) != 0 ||
        espclaw_web_download_to_file(source_url, temp_absolute) != 0) {
        return -1;
    }

    result = espclaw_component_install_from_file(
        workspace_root,
        component_id,
        title,
        module_name,
        summary,
        version,
        temp_relative
    );
    unlink(temp_absolute);
    return result;
}

static int espclaw_component_install_from_manifest_depth(
    const char *workspace_root,
    const char *manifest_url,
    size_t depth
)
{
    char temp_relative[160];
    char temp_absolute[512];
    char manifest_json[4096];
    char component_id[ESPCLAW_COMPONENT_ID_MAX + 1];
    char title[ESPCLAW_COMPONENT_TITLE_MAX + 1];
    char module_name[ESPCLAW_COMPONENT_MODULE_MAX + 1];
    char summary[ESPCLAW_COMPONENT_SUMMARY_MAX + 1];
    char version[ESPCLAW_COMPONENT_VERSION_MAX + 1];
    char source_url[ESPCLAW_COMPONENT_URL_MAX + 1];
    char docs_url[ESPCLAW_COMPONENT_URL_MAX + 1];
    char checksum[ESPCLAW_COMPONENT_CHECKSUM_MAX + 1];
    char dependencies[ESPCLAW_COMPONENT_DEPENDENCY_MAX][ESPCLAW_COMPONENT_URL_MAX + 1];
    char manifest_path[512];
    char lib_relative[192];
    char source_relative[192];
    size_t dependency_count = 0;
    size_t dependency_index;
    int result = -1;

    if (workspace_root == NULL || manifest_url == NULL || manifest_url[0] == '\0' || depth > ESPCLAW_COMPONENT_DEPENDENCY_MAX) {
        return -1;
    }
    if (snprintf(temp_relative, sizeof(temp_relative), "blobs/.staging/component_manifest_%lu.json", (unsigned long)depth) >= (int)sizeof(temp_relative) ||
        espclaw_workspace_bootstrap(workspace_root) != 0 ||
        espclaw_workspace_resolve_path(workspace_root, temp_relative, temp_absolute, sizeof(temp_absolute)) != 0 ||
        espclaw_web_download_to_file(manifest_url, temp_absolute) != 0 ||
        espclaw_workspace_read_file(workspace_root, temp_relative, manifest_json, sizeof(manifest_json)) != 0) {
        unlink(temp_absolute);
        return -1;
    }

    title[0] = '\0';
    summary[0] = '\0';
    version[0] = '\0';
    docs_url[0] = '\0';
    checksum[0] = '\0';
    if (!extract_string_after_any_key(manifest_json, (const char *[]){"id", "component_id"}, 2, component_id, sizeof(component_id)) ||
        !extract_string_after_key(manifest_json, "module", module_name, sizeof(module_name)) ||
        !extract_string_after_key(manifest_json, "source_url", source_url, sizeof(source_url))) {
        unlink(temp_absolute);
        return -1;
    }
    (void)extract_string_after_key(manifest_json, "title", title, sizeof(title));
    (void)extract_string_after_key(manifest_json, "summary", summary, sizeof(summary));
    (void)extract_string_after_key(manifest_json, "version", version, sizeof(version));
    (void)extract_string_after_key(manifest_json, "docs_url", docs_url, sizeof(docs_url));
    (void)extract_string_after_key(manifest_json, "checksum", checksum, sizeof(checksum));
    if (!extract_string_array_after_key(manifest_json, "dependencies", dependencies, ESPCLAW_COMPONENT_DEPENDENCY_MAX, &dependency_count)) {
        unlink(temp_absolute);
        return -1;
    }

    for (dependency_index = 0; dependency_index < dependency_count; ++dependency_index) {
        if (espclaw_component_install_from_manifest_depth(workspace_root, dependencies[dependency_index], depth + 1U) != 0) {
            unlink(temp_absolute);
            return -1;
        }
    }

    if (espclaw_component_install_from_url(workspace_root, component_id, title, module_name, summary, version, source_url) != 0 ||
        build_component_relative_path(component_id, "component.json", source_relative, sizeof(source_relative)) != 0 ||
        module_name_to_relative_path(module_name, lib_relative, sizeof(lib_relative)) != 0) {
        unlink(temp_absolute);
        return -1;
    }
    if (espclaw_workspace_resolve_path(workspace_root, source_relative, manifest_path, sizeof(manifest_path)) != 0) {
        unlink(temp_absolute);
        return -1;
    }
    result = write_component_manifest(
        manifest_path,
        component_id,
        version[0] != '\0' ? version : "0.1.0",
        title[0] != '\0' ? title : component_id,
        module_name,
        summary,
        module_name_to_relative_path(module_name, lib_relative, sizeof(lib_relative)) == 0 ? lib_relative : "",
        manifest_url,
        source_url,
        docs_url,
        checksum,
        dependencies,
        dependency_count
    );
    unlink(temp_absolute);
    return result;
}

int espclaw_component_install_from_manifest(
    const char *workspace_root,
    const char *manifest_url
)
{
    return espclaw_component_install_from_manifest_depth(workspace_root, manifest_url, 0U);
}

int espclaw_component_collect_ids(
    const char *workspace_root,
    char ids[][ESPCLAW_COMPONENT_ID_MAX + 1],
    size_t max_ids,
    size_t *count_out
)
{
    char components_path[512];
    DIR *dir;
    struct dirent *entry;
    size_t count = 0;

    if (workspace_root == NULL || ids == NULL || max_ids == 0 || count_out == NULL ||
        espclaw_workspace_resolve_path(workspace_root, "components", components_path, sizeof(components_path)) != 0) {
        return -1;
    }
    *count_out = 0;
    dir = opendir(components_path);
    if (dir == NULL) {
        return -1;
    }
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (!component_id_valid(entry->d_name)) {
            continue;
        }
        memcpy(ids[count], entry->d_name, ESPCLAW_COMPONENT_ID_MAX);
        ids[count][ESPCLAW_COMPONENT_ID_MAX] = '\0';
        count++;
        if (count >= max_ids) {
            break;
        }
    }
    closedir(dir);
    *count_out = count;
    return 0;
}

int espclaw_component_load_manifest(
    const char *workspace_root,
    const char *component_id,
    espclaw_component_manifest_t *manifest
)
{
    char relative_path[192];
    char json[1024];

    if (workspace_root == NULL || manifest == NULL || !component_id_valid(component_id) ||
        build_component_relative_path(component_id, "component.json", relative_path, sizeof(relative_path)) != 0 ||
        espclaw_workspace_read_file(workspace_root, relative_path, json, sizeof(json)) != 0) {
        return -1;
    }

    memset(manifest, 0, sizeof(*manifest));
    if (!extract_string_after_key(json, "id", manifest->component_id, sizeof(manifest->component_id)) ||
        !extract_string_after_key(json, "version", manifest->version, sizeof(manifest->version)) ||
        !extract_string_after_key(json, "title", manifest->title, sizeof(manifest->title)) ||
        !extract_string_after_key(json, "module", manifest->module, sizeof(manifest->module))) {
        return -1;
    }
    (void)extract_string_after_key(json, "summary", manifest->summary, sizeof(manifest->summary));
    (void)extract_string_after_key(json, "manifest_url", manifest->manifest_url, sizeof(manifest->manifest_url));
    (void)extract_string_after_key(json, "source_url", manifest->source_url, sizeof(manifest->source_url));
    (void)extract_string_after_key(json, "docs_url", manifest->docs_url, sizeof(manifest->docs_url));
    (void)extract_string_after_key(json, "checksum", manifest->checksum, sizeof(manifest->checksum));
    (void)extract_string_array_after_key(
        json,
        "dependencies",
        manifest->dependencies,
        ESPCLAW_COMPONENT_DEPENDENCY_MAX,
        &manifest->dependency_count
    );
    return 0;
}

int espclaw_component_read_source(
    const char *workspace_root,
    const char *component_id,
    char *buffer,
    size_t buffer_size
)
{
    char relative_path[192];

    if (workspace_root == NULL || buffer == NULL || buffer_size == 0 ||
        build_component_relative_path(component_id, "module.lua", relative_path, sizeof(relative_path)) != 0) {
        return -1;
    }
    return espclaw_workspace_read_file(workspace_root, relative_path, buffer, buffer_size);
}

int espclaw_component_remove(const char *workspace_root, const char *component_id)
{
    espclaw_component_manifest_t manifest;
    char lib_relative[192];
    char path[512];

    if (workspace_root == NULL || !component_id_valid(component_id) ||
        espclaw_component_load_manifest(workspace_root, component_id, &manifest) != 0 ||
        module_name_to_relative_path(manifest.module, lib_relative, sizeof(lib_relative)) != 0) {
        return -1;
    }

    if (espclaw_workspace_resolve_path(workspace_root, lib_relative, path, sizeof(path)) == 0) {
        unlink(path);
    }
    if (espclaw_workspace_resolve_path(workspace_root, "components", path, sizeof(path)) == 0) {
        char component_dir[512];

        if (snprintf(component_dir, sizeof(component_dir), "%s/%s", path, component_id) < (int)sizeof(component_dir)) {
            char file_path[512];

            if (snprintf(file_path, sizeof(file_path), "%s/component.json", component_dir) < (int)sizeof(file_path)) {
                unlink(file_path);
            }
            if (snprintf(file_path, sizeof(file_path), "%s/module.lua", component_dir) < (int)sizeof(file_path)) {
                unlink(file_path);
            }
            rmdir(component_dir);
        }
    }
    return 0;
}

int espclaw_render_components_json(const char *workspace_root, char *buffer, size_t buffer_size)
{
    char ids[32][ESPCLAW_COMPONENT_ID_MAX + 1];
    size_t count = 0;
    size_t index;
    size_t used = 0;
    bool first = true;

    if (buffer == NULL || buffer_size == 0) {
        return -1;
    }
    used += (size_t)snprintf(buffer + used, buffer_size - used, "{\"components\":[");
    if (workspace_root != NULL && espclaw_component_collect_ids(workspace_root, ids, 32, &count) == 0) {
        for (index = 0; index < count && used + 32 < buffer_size; ++index) {
            espclaw_component_manifest_t manifest;

            if (espclaw_component_load_manifest(workspace_root, ids[index], &manifest) != 0) {
                continue;
            }
            used += (size_t)snprintf(buffer + used, buffer_size - used, "%s{\"id\":", first ? "" : ",");
            append_json_escaped(buffer, buffer_size, &used, manifest.component_id);
            used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"component_id\":");
            append_json_escaped(buffer, buffer_size, &used, manifest.component_id);
            used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"module\":");
            append_json_escaped(buffer, buffer_size, &used, manifest.module);
            used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"title\":");
            append_json_escaped(buffer, buffer_size, &used, manifest.title);
            used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"version\":");
            append_json_escaped(buffer, buffer_size, &used, manifest.version);
            used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"summary\":");
            append_json_escaped(buffer, buffer_size, &used, manifest.summary);
            used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"manifest_url\":");
            append_json_escaped(buffer, buffer_size, &used, manifest.manifest_url);
            used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"source_url\":");
            append_json_escaped(buffer, buffer_size, &used, manifest.source_url);
            used += (size_t)snprintf(buffer + used, buffer_size - used, "}");
            first = false;
        }
    }
    used += (size_t)snprintf(buffer + used, buffer_size - used, "]}");
    return 0;
}

int espclaw_render_component_detail_json(
    const char *workspace_root,
    const char *component_id,
    char *buffer,
    size_t buffer_size
)
{
    espclaw_component_manifest_t manifest;
    char source[4096];
    size_t used = 0;

    if (workspace_root == NULL || component_id == NULL || buffer == NULL || buffer_size == 0) {
        return -1;
    }
    if (espclaw_component_load_manifest(workspace_root, component_id, &manifest) != 0 ||
        espclaw_component_read_source(workspace_root, component_id, source, sizeof(source)) != 0) {
        return -1;
    }

    used += (size_t)snprintf(buffer + used, buffer_size - used, "{\"id\":");
    append_json_escaped(buffer, buffer_size, &used, manifest.component_id);
    used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"component_id\":");
    append_json_escaped(buffer, buffer_size, &used, manifest.component_id);
    used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"module\":");
    append_json_escaped(buffer, buffer_size, &used, manifest.module);
    used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"title\":");
    append_json_escaped(buffer, buffer_size, &used, manifest.title);
    used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"version\":");
    append_json_escaped(buffer, buffer_size, &used, manifest.version);
    used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"summary\":");
    append_json_escaped(buffer, buffer_size, &used, manifest.summary);
    used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"manifest_url\":");
    append_json_escaped(buffer, buffer_size, &used, manifest.manifest_url);
    used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"source_url\":");
    append_json_escaped(buffer, buffer_size, &used, manifest.source_url);
    used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"docs_url\":");
    append_json_escaped(buffer, buffer_size, &used, manifest.docs_url);
    used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"checksum\":");
    append_json_escaped(buffer, buffer_size, &used, manifest.checksum);
    used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"dependencies\":[");
    for (size_t index = 0; index < manifest.dependency_count && used + 8 < buffer_size; ++index) {
        if (index > 0U) {
            used += (size_t)snprintf(buffer + used, buffer_size - used, ",");
        }
        append_json_escaped(buffer, buffer_size, &used, manifest.dependencies[index]);
    }
    used += (size_t)snprintf(buffer + used, buffer_size - used, "]");
    used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"source\":");
    append_json_escaped(buffer, buffer_size, &used, source);
    used += (size_t)snprintf(buffer + used, buffer_size - used, "}");
    return 0;
}
