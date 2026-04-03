#include "espclaw/context_runtime.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "espclaw/workspace.h"

#define ESPCLAW_CONTEXT_DEFAULT_CHUNK_BYTES 2048U
#define ESPCLAW_CONTEXT_MAX_CHUNK_BYTES 8192U
#define ESPCLAW_CONTEXT_DEFAULT_SEARCH_LIMIT 3U
#define ESPCLAW_CONTEXT_MAX_SEARCH_LIMIT 8U
#define ESPCLAW_CONTEXT_MAX_TERMS 8U
#define ESPCLAW_CONTEXT_DEFAULT_SELECT_BYTES 2048U
#define ESPCLAW_CONTEXT_MAX_SELECT_BYTES 6144U
#define ESPCLAW_CONTEXT_DEFAULT_SUMMARY_BYTES 768U
#define ESPCLAW_CONTEXT_MAX_SUMMARY_BYTES 2048U
#define ESPCLAW_CONTEXT_MAX_SUMMARY_LINES 8U

typedef struct {
    size_t index;
    size_t start;
    size_t length;
    size_t score;
    char *text;
} espclaw_context_search_hit_t;

typedef struct {
    char *text;
    size_t score;
} espclaw_context_summary_line_t;

static bool context_path_valid(const char *relative_path)
{
    size_t index;

    if (relative_path == NULL || relative_path[0] == '\0' || relative_path[0] == '/') {
        return false;
    }
    if (strstr(relative_path, "..") != NULL) {
        return false;
    }
    for (index = 0; relative_path[index] != '\0'; ++index) {
        unsigned char c = (unsigned char)relative_path[index];

        if (!(isalnum(c) || c == '_' || c == '-' || c == '.' || c == '/')) {
            return false;
        }
    }
    return true;
}

static size_t normalize_chunk_bytes(size_t chunk_bytes)
{
    if (chunk_bytes == 0U) {
        return ESPCLAW_CONTEXT_DEFAULT_CHUNK_BYTES;
    }
    if (chunk_bytes > ESPCLAW_CONTEXT_MAX_CHUNK_BYTES) {
        return ESPCLAW_CONTEXT_MAX_CHUNK_BYTES;
    }
    return chunk_bytes;
}

static size_t normalize_search_limit(size_t limit)
{
    if (limit == 0U) {
        return ESPCLAW_CONTEXT_DEFAULT_SEARCH_LIMIT;
    }
    if (limit > ESPCLAW_CONTEXT_MAX_SEARCH_LIMIT) {
        return ESPCLAW_CONTEXT_MAX_SEARCH_LIMIT;
    }
    return limit;
}

static size_t normalize_select_bytes(size_t output_bytes)
{
    if (output_bytes == 0U) {
        return ESPCLAW_CONTEXT_DEFAULT_SELECT_BYTES;
    }
    if (output_bytes > ESPCLAW_CONTEXT_MAX_SELECT_BYTES) {
        return ESPCLAW_CONTEXT_MAX_SELECT_BYTES;
    }
    return output_bytes;
}

static size_t normalize_summary_bytes(size_t summary_bytes)
{
    if (summary_bytes == 0U) {
        return ESPCLAW_CONTEXT_DEFAULT_SUMMARY_BYTES;
    }
    if (summary_bytes > ESPCLAW_CONTEXT_MAX_SUMMARY_BYTES) {
        return ESPCLAW_CONTEXT_MAX_SUMMARY_BYTES;
    }
    return summary_bytes;
}

static size_t append_json_escaped(char *buffer, size_t buffer_size, size_t used, const char *text)
{
    const unsigned char *cursor = (const unsigned char *)(text != NULL ? text : "");

    if (buffer == NULL || buffer_size == 0 || used >= buffer_size) {
        return used;
    }
    buffer[used++] = '"';
    while (*cursor != '\0' && used + 2 < buffer_size) {
        if (*cursor == '\\' || *cursor == '"') {
            buffer[used++] = '\\';
            buffer[used++] = (char)*cursor;
        } else if (*cursor == '\n') {
            buffer[used++] = '\\';
            buffer[used++] = 'n';
        } else if (*cursor == '\r') {
            buffer[used++] = '\\';
            buffer[used++] = 'r';
        } else if (*cursor == '\t') {
            buffer[used++] = '\\';
            buffer[used++] = 't';
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
    return used;
}

static int open_context_file(
    const char *workspace_root,
    const char *relative_path,
    FILE **handle_out,
    size_t *total_bytes_out
)
{
    char absolute_path[512];
    struct stat file_stat;
    FILE *handle;

    if (handle_out == NULL || total_bytes_out == NULL || workspace_root == NULL || !context_path_valid(relative_path) ||
        espclaw_workspace_resolve_path(workspace_root, relative_path, absolute_path, sizeof(absolute_path)) != 0 ||
        stat(absolute_path, &file_stat) != 0) {
        return -1;
    }
    handle = fopen(absolute_path, "rb");
    if (handle == NULL) {
        return -1;
    }
    *handle_out = handle;
    *total_bytes_out = (size_t)file_stat.st_size;
    return 0;
}

static size_t chunk_count_for_size(size_t total_bytes, size_t chunk_bytes)
{
    if (chunk_bytes == 0U) {
        return 0U;
    }
    if (total_bytes == 0U) {
        return 1U;
    }
    return (total_bytes + chunk_bytes - 1U) / chunk_bytes;
}

static int load_chunk_text(
    const char *workspace_root,
    const char *relative_path,
    size_t chunk_index,
    size_t chunk_bytes,
    char *text_out,
    size_t text_out_size,
    size_t *total_bytes_out,
    size_t *chunk_count_out,
    size_t *chunk_start_out,
    size_t *chunk_length_out
)
{
    FILE *handle = NULL;
    size_t total_bytes = 0;
    size_t chunk_count;
    size_t offset;
    size_t bytes_to_read;
    size_t bytes_read;

    if (text_out == NULL || text_out_size == 0 || total_bytes_out == NULL || chunk_count_out == NULL ||
        chunk_start_out == NULL || chunk_length_out == NULL) {
        return -1;
    }
    text_out[0] = '\0';
    chunk_bytes = normalize_chunk_bytes(chunk_bytes);

    if (open_context_file(workspace_root, relative_path, &handle, &total_bytes) != 0) {
        return -1;
    }

    chunk_count = chunk_count_for_size(total_bytes, chunk_bytes);
    if (chunk_index >= chunk_count) {
        fclose(handle);
        return -1;
    }

    offset = chunk_index * chunk_bytes;
    bytes_to_read = total_bytes > offset ? total_bytes - offset : 0U;
    if (bytes_to_read > chunk_bytes) {
        bytes_to_read = chunk_bytes;
    }
    if (bytes_to_read >= text_out_size) {
        bytes_to_read = text_out_size - 1U;
    }

    if (fseek(handle, (long)offset, SEEK_SET) != 0) {
        fclose(handle);
        return -1;
    }
    bytes_read = fread(text_out, 1U, bytes_to_read, handle);
    fclose(handle);
    text_out[bytes_read] = '\0';

    *total_bytes_out = total_bytes;
    *chunk_count_out = chunk_count;
    *chunk_start_out = offset;
    *chunk_length_out = bytes_read;
    return 0;
}

static void lower_in_place(char *text)
{
    size_t index;

    if (text == NULL) {
        return;
    }
    for (index = 0; text[index] != '\0'; ++index) {
        text[index] = (char)tolower((unsigned char)text[index]);
    }
}

static size_t split_query_terms(const char *query, char terms[][32], size_t max_terms)
{
    char scratch[ESPCLAW_CONTEXT_QUERY_MAX + 1];
    char *token;
    char *save_ptr = NULL;
    size_t count = 0;

    if (query == NULL || query[0] == '\0' || max_terms == 0U) {
        return 0U;
    }
    snprintf(scratch, sizeof(scratch), "%s", query);
    lower_in_place(scratch);
    token = strtok_r(scratch, " \t\r\n", &save_ptr);
    while (token != NULL && count < max_terms) {
        if (token[0] != '\0') {
            snprintf(terms[count], 32U, "%s", token);
            count++;
        }
        token = strtok_r(NULL, " \t\r\n", &save_ptr);
    }
    return count;
}

static size_t count_term_hits(const char *haystack_lower, const char *needle_lower)
{
    const char *cursor = haystack_lower;
    size_t count = 0U;
    size_t needle_length;

    if (haystack_lower == NULL || needle_lower == NULL || needle_lower[0] == '\0') {
        return 0U;
    }
    needle_length = strlen(needle_lower);
    while ((cursor = strstr(cursor, needle_lower)) != NULL) {
        count++;
        cursor += needle_length;
    }
    return count;
}

static size_t score_text_against_terms(const char *text, char terms[][32], size_t term_count)
{
    char *lower = NULL;
    size_t score = 0U;
    size_t term_index;
    size_t length;

    if (text == NULL || term_count == 0U) {
        return 0U;
    }
    length = strlen(text);
    lower = (char *)calloc(length + 1U, 1U);
    if (lower == NULL) {
        return 0U;
    }
    memcpy(lower, text, length + 1U);
    lower_in_place(lower);
    for (term_index = 0; term_index < term_count; ++term_index) {
        score += count_term_hits(lower, terms[term_index]);
    }
    free(lower);
    return score;
}

static void maybe_record_search_hit(
    espclaw_context_search_hit_t *hits,
    size_t max_hits,
    size_t chunk_index,
    size_t chunk_start,
    const char *text,
    size_t text_length,
    size_t score
)
{
    size_t slot = max_hits;
    size_t worst_score = (size_t)-1;
    size_t index;

    if (hits == NULL || max_hits == 0U || text == NULL || score == 0U) {
        return;
    }
    for (index = 0; index < max_hits; ++index) {
        if (hits[index].text == NULL) {
            slot = index;
            break;
        }
        if (hits[index].score < worst_score) {
            worst_score = hits[index].score;
            slot = index;
        }
    }
    if (slot >= max_hits) {
        return;
    }
    if (hits[slot].text != NULL && hits[slot].score >= score) {
        return;
    }
    if (hits[slot].text != NULL) {
        free(hits[slot].text);
        hits[slot].text = NULL;
    }
    hits[slot].text = (char *)calloc(text_length + 1U, 1U);
    if (hits[slot].text == NULL) {
        return;
    }
    memcpy(hits[slot].text, text, text_length);
    hits[slot].text[text_length] = '\0';
    hits[slot].index = chunk_index;
    hits[slot].start = chunk_start;
    hits[slot].length = text_length;
    hits[slot].score = score;
}

static int compare_hits_desc(const void *lhs, const void *rhs)
{
    const espclaw_context_search_hit_t *a = (const espclaw_context_search_hit_t *)lhs;
    const espclaw_context_search_hit_t *b = (const espclaw_context_search_hit_t *)rhs;

    if (a->text == NULL && b->text == NULL) {
        return 0;
    }
    if (a->text == NULL) {
        return 1;
    }
    if (b->text == NULL) {
        return -1;
    }
    if (a->score < b->score) {
        return 1;
    }
    if (a->score > b->score) {
        return -1;
    }
    if (a->index > b->index) {
        return 1;
    }
    if (a->index < b->index) {
        return -1;
    }
    return 0;
}

static void free_search_hits(espclaw_context_search_hit_t *hits, size_t max_hits)
{
    size_t index;

    if (hits == NULL) {
        return;
    }
    for (index = 0; index < max_hits; ++index) {
        free(hits[index].text);
        hits[index].text = NULL;
    }
}

static int collect_search_hits(
    const char *workspace_root,
    const char *relative_path,
    const char *query,
    size_t chunk_bytes,
    size_t limit,
    espclaw_context_search_hit_t *hits,
    size_t max_hits,
    size_t *total_bytes_out,
    size_t *chunk_count_out
)
{
    FILE *handle = NULL;
    size_t total_bytes = 0;
    size_t chunk_count;
    size_t chunk_index;
    char terms[ESPCLAW_CONTEXT_MAX_TERMS][32];
    size_t term_count;
    char *text = NULL;
    char *lower = NULL;
    int rc = -1;

    if (hits == NULL || max_hits == 0U || total_bytes_out == NULL || chunk_count_out == NULL ||
        query == NULL || query[0] == '\0') {
        return -1;
    }
    memset(hits, 0, sizeof(*hits) * max_hits);
    chunk_bytes = normalize_chunk_bytes(chunk_bytes);
    limit = normalize_search_limit(limit);
    if (limit > max_hits) {
        limit = max_hits;
    }
    term_count = split_query_terms(query, terms, ESPCLAW_CONTEXT_MAX_TERMS);
    if (term_count == 0U) {
        return -1;
    }
    if (open_context_file(workspace_root, relative_path, &handle, &total_bytes) != 0) {
        return -1;
    }
    fclose(handle);
    chunk_count = chunk_count_for_size(total_bytes, chunk_bytes);

    text = (char *)calloc(chunk_bytes + 1U, 1U);
    lower = (char *)calloc(chunk_bytes + 1U, 1U);
    if (text == NULL || lower == NULL) {
        goto cleanup;
    }

    for (chunk_index = 0; chunk_index < chunk_count; ++chunk_index) {
        size_t loaded_total = 0;
        size_t loaded_chunk_count = 0;
        size_t chunk_start = 0;
        size_t chunk_length = 0;
        size_t score = 0U;
        size_t term_index;

        if (load_chunk_text(
                workspace_root,
                relative_path,
                chunk_index,
                chunk_bytes,
                text,
                chunk_bytes + 1U,
                &loaded_total,
                &loaded_chunk_count,
                &chunk_start,
                &chunk_length) != 0) {
            continue;
        }
        memcpy(lower, text, chunk_length + 1U);
        lower_in_place(lower);
        for (term_index = 0; term_index < term_count; ++term_index) {
            score += count_term_hits(lower, terms[term_index]);
        }
        maybe_record_search_hit(hits, limit, chunk_index, chunk_start, text, chunk_length, score);
    }

    qsort(hits, limit, sizeof(hits[0]), compare_hits_desc);
    *total_bytes_out = total_bytes;
    *chunk_count_out = chunk_count;
    rc = 0;

cleanup:
    free(lower);
    free(text);
    return rc;
}

static void trim_segment_bounds(const char *text, size_t *start_io, size_t *end_io)
{
    size_t start = start_io != NULL ? *start_io : 0U;
    size_t end = end_io != NULL ? *end_io : 0U;

    while (start < end && isspace((unsigned char)text[start])) {
        start++;
    }
    while (end > start && isspace((unsigned char)text[end - 1U])) {
        end--;
    }
    if (start_io != NULL) {
        *start_io = start;
    }
    if (end_io != NULL) {
        *end_io = end;
    }
}

static void maybe_record_summary_line(
    espclaw_context_summary_line_t *lines,
    size_t max_lines,
    const char *text,
    size_t score
)
{
    size_t slot = max_lines;
    size_t worst_score = (size_t)-1;
    size_t index;
    size_t length;

    if (lines == NULL || max_lines == 0U || text == NULL || text[0] == '\0') {
        return;
    }

    for (index = 0; index < max_lines; ++index) {
        if (lines[index].text == NULL) {
            slot = index;
            break;
        }
        if (strcmp(lines[index].text, text) == 0) {
            if (score > lines[index].score) {
                lines[index].score = score;
            }
            return;
        }
        if (lines[index].score < worst_score) {
            worst_score = lines[index].score;
            slot = index;
        }
    }
    if (slot >= max_lines) {
        return;
    }
    if (lines[slot].text != NULL && lines[slot].score >= score) {
        return;
    }
    free(lines[slot].text);
    lines[slot].text = NULL;
    length = strlen(text);
    lines[slot].text = (char *)calloc(length + 1U, 1U);
    if (lines[slot].text == NULL) {
        return;
    }
    memcpy(lines[slot].text, text, length + 1U);
    lines[slot].score = score;
}

static int compare_summary_lines_desc(const void *lhs, const void *rhs)
{
    const espclaw_context_summary_line_t *a = (const espclaw_context_summary_line_t *)lhs;
    const espclaw_context_summary_line_t *b = (const espclaw_context_summary_line_t *)rhs;

    if (a->text == NULL && b->text == NULL) {
        return 0;
    }
    if (a->text == NULL) {
        return 1;
    }
    if (b->text == NULL) {
        return -1;
    }
    if (a->score < b->score) {
        return 1;
    }
    if (a->score > b->score) {
        return -1;
    }
    return strcmp(a->text, b->text);
}

int espclaw_context_render_chunks_json(
    const char *workspace_root,
    const char *relative_path,
    size_t chunk_bytes,
    char *buffer,
    size_t buffer_size
)
{
    FILE *handle = NULL;
    size_t total_bytes = 0;
    size_t chunk_count;
    size_t used = 0;

    if (buffer == NULL || buffer_size == 0) {
        return -1;
    }
    buffer[0] = '\0';
    chunk_bytes = normalize_chunk_bytes(chunk_bytes);
    if (open_context_file(workspace_root, relative_path, &handle, &total_bytes) != 0) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"context file not found\"}");
        return -1;
    }
    fclose(handle);
    chunk_count = chunk_count_for_size(total_bytes, chunk_bytes);

    used += (size_t)snprintf(buffer + used, buffer_size - used, "{\"ok\":true,\"path\":");
    used = append_json_escaped(buffer, buffer_size, used, relative_path);
    used += (size_t)snprintf(
        buffer + used,
        buffer_size - used,
        ",\"chunk_bytes\":%lu,\"total_bytes\":%lu,\"chunk_count\":%lu}",
        (unsigned long)chunk_bytes,
        (unsigned long)total_bytes,
        (unsigned long)chunk_count
    );
    return 0;
}

int espclaw_context_render_chunk_json(
    const char *workspace_root,
    const char *relative_path,
    size_t chunk_index,
    size_t chunk_bytes,
    char *buffer,
    size_t buffer_size
)
{
    char *text = NULL;
    size_t total_bytes = 0;
    size_t chunk_count = 0;
    size_t chunk_start = 0;
    size_t chunk_length = 0;
    size_t used = 0;
    int rc = -1;

    if (buffer == NULL || buffer_size == 0) {
        return -1;
    }
    buffer[0] = '\0';
    chunk_bytes = normalize_chunk_bytes(chunk_bytes);
    text = (char *)calloc(chunk_bytes + 1U, 1U);
    if (text == NULL) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"out of memory\"}");
        return -1;
    }
    if (load_chunk_text(
            workspace_root,
            relative_path,
            chunk_index,
            chunk_bytes,
            text,
            chunk_bytes + 1U,
            &total_bytes,
            &chunk_count,
            &chunk_start,
            &chunk_length) != 0) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"context chunk not found\"}");
        goto cleanup;
    }

    used += (size_t)snprintf(buffer + used, buffer_size - used, "{\"ok\":true,\"path\":");
    used = append_json_escaped(buffer, buffer_size, used, relative_path);
    used += (size_t)snprintf(
        buffer + used,
        buffer_size - used,
        ",\"chunk_index\":%lu,\"chunk_bytes\":%lu,\"chunk_start\":%lu,\"chunk_length\":%lu,\"chunk_count\":%lu,\"total_bytes\":%lu,\"text\":",
        (unsigned long)chunk_index,
        (unsigned long)chunk_bytes,
        (unsigned long)chunk_start,
        (unsigned long)chunk_length,
        (unsigned long)chunk_count,
        (unsigned long)total_bytes
    );
    used = append_json_escaped(buffer, buffer_size, used, text);
    used += (size_t)snprintf(buffer + used, buffer_size - used, "}");
    rc = 0;

cleanup:
    free(text);
    return rc;
}

int espclaw_context_search_json(
    const char *workspace_root,
    const char *relative_path,
    const char *query,
    size_t chunk_bytes,
    size_t limit,
    char *buffer,
    size_t buffer_size
)
{
    espclaw_context_search_hit_t hits[ESPCLAW_CONTEXT_MAX_SEARCH_LIMIT];
    size_t total_bytes = 0;
    size_t chunk_count = 0;
    size_t used = 0;
    size_t index;
    int rc = -1;

    if (buffer == NULL || buffer_size == 0 || query == NULL || query[0] == '\0') {
        return -1;
    }
    buffer[0] = '\0';
    chunk_bytes = normalize_chunk_bytes(chunk_bytes);
    limit = normalize_search_limit(limit);
    if (collect_search_hits(
            workspace_root,
            relative_path,
            query,
            chunk_bytes,
            limit,
            hits,
            ESPCLAW_CONTEXT_MAX_SEARCH_LIMIT,
            &total_bytes,
            &chunk_count) != 0) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"context search failed\"}");
        return -1;
    }
    used += (size_t)snprintf(buffer + used, buffer_size - used, "{\"ok\":true,\"path\":");
    used = append_json_escaped(buffer, buffer_size, used, relative_path);
    used += (size_t)snprintf(
        buffer + used,
        buffer_size - used,
        ",\"query\":"
    );
    used = append_json_escaped(buffer, buffer_size, used, query);
    used += (size_t)snprintf(
        buffer + used,
        buffer_size - used,
        ",\"chunk_bytes\":%lu,\"total_bytes\":%lu,\"chunk_count\":%lu,\"results\":[",
        (unsigned long)chunk_bytes,
        (unsigned long)total_bytes,
        (unsigned long)chunk_count
    );
    for (index = 0; index < limit && hits[index].text != NULL && used + 64 < buffer_size; ++index) {
        used += (size_t)snprintf(
            buffer + used,
            buffer_size - used,
            "%s{\"chunk_index\":%lu,\"chunk_start\":%lu,\"chunk_length\":%lu,\"score\":%lu,\"text\":",
            index == 0 ? "" : ",",
            (unsigned long)hits[index].index,
            (unsigned long)hits[index].start,
            (unsigned long)hits[index].length,
            (unsigned long)hits[index].score
        );
        used = append_json_escaped(buffer, buffer_size, used, hits[index].text);
        used += (size_t)snprintf(buffer + used, buffer_size - used, "}");
    }
    used += (size_t)snprintf(buffer + used, buffer_size - used, "]}");
    rc = 0;

    free_search_hits(hits, ESPCLAW_CONTEXT_MAX_SEARCH_LIMIT);
    return rc;
}

int espclaw_context_select_json(
    const char *workspace_root,
    const char *relative_path,
    const char *query,
    size_t chunk_bytes,
    size_t limit,
    size_t output_bytes,
    char *buffer,
    size_t buffer_size
)
{
    espclaw_context_search_hit_t hits[ESPCLAW_CONTEXT_MAX_SEARCH_LIMIT];
    char selected_text[ESPCLAW_CONTEXT_MAX_SELECT_BYTES + 1];
    size_t total_bytes = 0;
    size_t chunk_count = 0;
    size_t used = 0;
    size_t selected_bytes = 0;
    size_t index;
    int rc = -1;

    if (buffer == NULL || buffer_size == 0 || query == NULL || query[0] == '\0') {
        return -1;
    }
    buffer[0] = '\0';
    chunk_bytes = normalize_chunk_bytes(chunk_bytes);
    limit = normalize_search_limit(limit);
    output_bytes = normalize_select_bytes(output_bytes);
    selected_text[0] = '\0';
    if (collect_search_hits(
            workspace_root,
            relative_path,
            query,
            chunk_bytes,
            limit,
            hits,
            ESPCLAW_CONTEXT_MAX_SEARCH_LIMIT,
            &total_bytes,
            &chunk_count) != 0) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"context selection failed\"}");
        return -1;
    }

    used += (size_t)snprintf(buffer + used, buffer_size - used, "{\"ok\":true,\"path\":");
    used = append_json_escaped(buffer, buffer_size, used, relative_path);
    used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"query\":");
    used = append_json_escaped(buffer, buffer_size, used, query);
    used += (size_t)snprintf(
        buffer + used,
        buffer_size - used,
        ",\"chunk_bytes\":%lu,\"total_bytes\":%lu,\"chunk_count\":%lu,\"output_bytes\":%lu,\"selected_chunks\":[",
        (unsigned long)chunk_bytes,
        (unsigned long)total_bytes,
        (unsigned long)chunk_count,
        (unsigned long)output_bytes
    );
    for (index = 0; index < limit && hits[index].text != NULL && used + 64 < buffer_size; ++index) {
        used += (size_t)snprintf(
            buffer + used,
            buffer_size - used,
            "%s{\"chunk_index\":%lu,\"chunk_start\":%lu,\"chunk_length\":%lu,\"score\":%lu}",
            index == 0 ? "" : ",",
            (unsigned long)hits[index].index,
            (unsigned long)hits[index].start,
            (unsigned long)hits[index].length,
            (unsigned long)hits[index].score
        );
    }
    for (index = 0; index < limit && hits[index].text != NULL && selected_bytes + 16U < output_bytes; ++index) {
        char label[64];
        size_t label_length;
        size_t text_length = strlen(hits[index].text);
        size_t copy_length;
        size_t selected_used = strlen(selected_text);

        snprintf(
            label,
            sizeof(label),
            "%s[chunk %lu score=%lu]\\n",
            index == 0 ? "" : "\\n",
            (unsigned long)hits[index].index,
            (unsigned long)hits[index].score
        );
        label_length = strlen(label);
        if (selected_bytes + label_length >= output_bytes) {
            break;
        }
        copy_length = text_length;
        if (selected_bytes + label_length + copy_length > output_bytes) {
            copy_length = output_bytes - selected_bytes - label_length;
        }
        if (copy_length > 0U) {
            snprintf(selected_text + selected_used, sizeof(selected_text) - selected_used, "%s", label);
            selected_bytes += label_length;
            {
                size_t remaining_capacity;

                selected_used = strlen(selected_text);
                remaining_capacity = sizeof(selected_text) - selected_used;
                if (copy_length >= remaining_capacity) {
                    copy_length = remaining_capacity - 1U;
                }
                memcpy(selected_text + selected_used, hits[index].text, copy_length);
                selected_text[selected_used + copy_length] = '\0';
                selected_bytes += copy_length;
                if (copy_length < text_length && selected_bytes + 3U <= output_bytes) {
                    strncat(selected_text, "...", sizeof(selected_text) - strlen(selected_text) - 1U);
                    selected_bytes += 3U;
                }
            }
        }
    }
    used += (size_t)snprintf(buffer + used, buffer_size - used, "],\"selected_text\":");
    used = append_json_escaped(buffer, buffer_size, used, selected_text);
    used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"selected_bytes\":%lu}", (unsigned long)selected_bytes);
    rc = 0;

    free_search_hits(hits, ESPCLAW_CONTEXT_MAX_SEARCH_LIMIT);
    return rc;
}

int espclaw_context_summarize_json(
    const char *workspace_root,
    const char *relative_path,
    const char *query,
    size_t chunk_bytes,
    size_t limit,
    size_t summary_bytes,
    char *buffer,
    size_t buffer_size
)
{
    espclaw_context_search_hit_t hits[ESPCLAW_CONTEXT_MAX_SEARCH_LIMIT];
    espclaw_context_summary_line_t lines[ESPCLAW_CONTEXT_MAX_SUMMARY_LINES];
    char terms[ESPCLAW_CONTEXT_MAX_TERMS][32];
    size_t term_count;
    size_t total_bytes = 0;
    size_t chunk_count = 0;
    size_t used = 0;
    size_t summary_used = 0;
    size_t hit_index;
    size_t line_index;
    int rc = -1;

    if (buffer == NULL || buffer_size == 0 || query == NULL || query[0] == '\0') {
        return -1;
    }
    buffer[0] = '\0';
    memset(lines, 0, sizeof(lines));
    chunk_bytes = normalize_chunk_bytes(chunk_bytes);
    limit = normalize_search_limit(limit);
    summary_bytes = normalize_summary_bytes(summary_bytes);
    term_count = split_query_terms(query, terms, ESPCLAW_CONTEXT_MAX_TERMS);
    if (term_count == 0U) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"missing query\"}");
        return -1;
    }
    if (collect_search_hits(
            workspace_root,
            relative_path,
            query,
            chunk_bytes,
            limit,
            hits,
            ESPCLAW_CONTEXT_MAX_SEARCH_LIMIT,
            &total_bytes,
            &chunk_count) != 0) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"context summary failed\"}");
        return -1;
    }

    for (hit_index = 0; hit_index < limit && hits[hit_index].text != NULL; ++hit_index) {
        const char *cursor = hits[hit_index].text;

        while (*cursor != '\0') {
            const char *segment_start = cursor;
            const char *segment_end = cursor;
            size_t start_index;
            size_t end_index;
            char line[256];
            size_t line_length;
            size_t score;

            while (*segment_end != '\0' && *segment_end != '\n' && *segment_end != '.') {
                segment_end++;
            }
            start_index = 0U;
            end_index = (size_t)(segment_end - segment_start);
            trim_segment_bounds(segment_start, &start_index, &end_index);
            line_length = end_index > start_index ? end_index - start_index : 0U;
            if (line_length > 0U) {
                if (line_length >= sizeof(line)) {
                    line_length = sizeof(line) - 1U;
                }
                memcpy(line, segment_start + start_index, line_length);
                line[line_length] = '\0';
                score = score_text_against_terms(line, terms, term_count);
                if (score == 0U && hit_index == 0U) {
                    score = 1U;
                }
                if (score > 0U) {
                    maybe_record_summary_line(lines, ESPCLAW_CONTEXT_MAX_SUMMARY_LINES, line, score);
                }
            }
            cursor = *segment_end == '\0' ? segment_end : segment_end + 1;
        }
    }

    qsort(lines, ESPCLAW_CONTEXT_MAX_SUMMARY_LINES, sizeof(lines[0]), compare_summary_lines_desc);
    used += (size_t)snprintf(buffer + used, buffer_size - used, "{\"ok\":true,\"path\":");
    used = append_json_escaped(buffer, buffer_size, used, relative_path);
    used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"query\":");
    used = append_json_escaped(buffer, buffer_size, used, query);
    used += (size_t)snprintf(
        buffer + used,
        buffer_size - used,
        ",\"chunk_bytes\":%lu,\"total_bytes\":%lu,\"chunk_count\":%lu,\"summary_bytes\":%lu,\"summary\":",
        (unsigned long)chunk_bytes,
        (unsigned long)total_bytes,
        (unsigned long)chunk_count,
        (unsigned long)summary_bytes
    );
    if (used + 2 < buffer_size) {
        buffer[used++] = '"';
        buffer[used] = '\0';
    }
    for (line_index = 0; line_index < ESPCLAW_CONTEXT_MAX_SUMMARY_LINES && lines[line_index].text != NULL && used + 8 < buffer_size;
         ++line_index) {
        size_t line_length = strlen(lines[line_index].text);
        size_t remaining = summary_used < summary_bytes ? summary_bytes - summary_used : 0U;
        size_t cursor;

        if (remaining <= 4U) {
            break;
        }
        if (line_index > 0U && used + 2 < buffer_size) {
            buffer[used++] = '\\';
            buffer[used++] = 'n';
            summary_used += 2U;
        }
        if (used + 2 < buffer_size) {
            buffer[used++] = '-';
            buffer[used++] = ' ';
            summary_used += 2U;
        }
        if (line_length + 2U > remaining) {
            line_length = remaining - 4U;
        }
        for (cursor = 0; cursor < line_length && used + 2 < buffer_size; ++cursor) {
            unsigned char c = (unsigned char)lines[line_index].text[cursor];

            if (c == '\\' || c == '"') {
                buffer[used++] = '\\';
                buffer[used++] = (char)c;
                summary_used += 2U;
            } else if (c == '\n' || c == '\r' || c == '\t') {
                continue;
            } else {
                buffer[used++] = (char)c;
                summary_used++;
            }
        }
    }
    if (used < buffer_size) {
        buffer[used++] = '"';
    }
    used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"selected_chunks\":[");
    for (hit_index = 0; hit_index < limit && hits[hit_index].text != NULL && used + 32 < buffer_size; ++hit_index) {
        used += (size_t)snprintf(
            buffer + used,
            buffer_size - used,
            "%s%lu",
            hit_index == 0 ? "" : ",",
            (unsigned long)hits[hit_index].index
        );
    }
    used += (size_t)snprintf(buffer + used, buffer_size - used, "]}");
    rc = 0;

    for (line_index = 0; line_index < ESPCLAW_CONTEXT_MAX_SUMMARY_LINES; ++line_index) {
        free(lines[line_index].text);
    }
    free_search_hits(hits, ESPCLAW_CONTEXT_MAX_SEARCH_LIMIT);
    return rc;
}
