#include "espclaw/log_buffer.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#endif

#define ESPCLAW_LOG_BUFFER_CAPACITY 16384U
#define ESPCLAW_LOG_BUFFER_LINE_MAX 512U

static char s_log_ring[ESPCLAW_LOG_BUFFER_CAPACITY];
static size_t s_log_start = 0;
static size_t s_log_size = 0;
static size_t s_log_dropped_bytes = 0;
static bool s_log_buffer_initialized = false;

#ifdef ESP_PLATFORM
static vprintf_like_t s_previous_logger = NULL;
static portMUX_TYPE s_log_ring_mux = portMUX_INITIALIZER_UNLOCKED;
#define ESPCLAW_LOG_BUFFER_LOCK() portENTER_CRITICAL(&s_log_ring_mux)
#define ESPCLAW_LOG_BUFFER_UNLOCK() portEXIT_CRITICAL(&s_log_ring_mux)
#elif defined(ESPCLAW_WASM)
#define ESPCLAW_LOG_BUFFER_LOCK() ((void)0)
#define ESPCLAW_LOG_BUFFER_UNLOCK() ((void)0)
#else
#include <pthread.h>
static pthread_mutex_t s_log_ring_mutex = PTHREAD_MUTEX_INITIALIZER;
#define ESPCLAW_LOG_BUFFER_LOCK() pthread_mutex_lock(&s_log_ring_mutex)
#define ESPCLAW_LOG_BUFFER_UNLOCK() pthread_mutex_unlock(&s_log_ring_mutex)
#endif

static size_t append_escaped_json(char *buffer, size_t buffer_size, size_t used, const char *value)
{
    const unsigned char *cursor = (const unsigned char *)(value != NULL ? value : "");

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
            if (*cursor < 0x20U) {
                if (used + 6 >= buffer_size) {
                    cursor++;
                    continue;
                }
                used += (size_t)snprintf(buffer + used, buffer_size - used, "\\u%04x", (unsigned int)*cursor);
            } else {
                buffer[used++] = (char)*cursor;
            }
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

static void log_buffer_append_bytes_locked(const char *data, size_t length)
{
    size_t index;

    if (data == NULL || length == 0) {
        return;
    }
    if (length >= ESPCLAW_LOG_BUFFER_CAPACITY) {
        s_log_dropped_bytes += length - ESPCLAW_LOG_BUFFER_CAPACITY;
        data += length - ESPCLAW_LOG_BUFFER_CAPACITY;
        length = ESPCLAW_LOG_BUFFER_CAPACITY;
        s_log_start = 0;
        s_log_size = 0;
    }
    while (s_log_size + length > ESPCLAW_LOG_BUFFER_CAPACITY) {
        s_log_start = (s_log_start + 1U) % ESPCLAW_LOG_BUFFER_CAPACITY;
        s_log_size--;
        s_log_dropped_bytes++;
    }
    for (index = 0; index < length; ++index) {
        size_t write_index = (s_log_start + s_log_size + index) % ESPCLAW_LOG_BUFFER_CAPACITY;
        s_log_ring[write_index] = data[index];
    }
    s_log_size += length;
}

#ifdef ESP_PLATFORM
static int log_buffer_vprintf(const char *format, va_list args)
{
    char line[ESPCLAW_LOG_BUFFER_LINE_MAX];
    va_list copy;
    int result = 0;
    int written;

    va_copy(copy, args);
    if (s_previous_logger != NULL) {
        result = s_previous_logger(format, args);
    } else {
        result = vprintf(format, args);
    }
    written = vsnprintf(line, sizeof(line), format, copy);
    va_end(copy);

    if (written > 0) {
        size_t stored = (size_t)written;

        if (stored >= sizeof(line)) {
            stored = sizeof(line) - 1U;
        }
        ESPCLAW_LOG_BUFFER_LOCK();
        log_buffer_append_bytes_locked(line, stored);
        ESPCLAW_LOG_BUFFER_UNLOCK();
    }
    return result;
}
#endif

void espclaw_log_buffer_init(void)
{
    if (s_log_buffer_initialized) {
        return;
    }

    ESPCLAW_LOG_BUFFER_LOCK();
    memset(s_log_ring, 0, sizeof(s_log_ring));
    s_log_start = 0;
    s_log_size = 0;
    s_log_dropped_bytes = 0;
    ESPCLAW_LOG_BUFFER_UNLOCK();

#ifdef ESP_PLATFORM
    s_previous_logger = esp_log_set_vprintf(log_buffer_vprintf);
#endif
    s_log_buffer_initialized = true;
}

void espclaw_log_buffer_reset(void)
{
    ESPCLAW_LOG_BUFFER_LOCK();
    memset(s_log_ring, 0, sizeof(s_log_ring));
    s_log_start = 0;
    s_log_size = 0;
    s_log_dropped_bytes = 0;
    ESPCLAW_LOG_BUFFER_UNLOCK();
}

void espclaw_log_buffer_append(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return;
    }

    ESPCLAW_LOG_BUFFER_LOCK();
    log_buffer_append_bytes_locked(text, strlen(text));
    ESPCLAW_LOG_BUFFER_UNLOCK();
}

size_t espclaw_log_buffer_capacity(void)
{
    return ESPCLAW_LOG_BUFFER_CAPACITY;
}

size_t espclaw_log_buffer_size(void)
{
    size_t size;

    ESPCLAW_LOG_BUFFER_LOCK();
    size = s_log_size;
    ESPCLAW_LOG_BUFFER_UNLOCK();
    return size;
}

size_t espclaw_log_buffer_dropped_bytes(void)
{
    size_t dropped;

    ESPCLAW_LOG_BUFFER_LOCK();
    dropped = s_log_dropped_bytes;
    ESPCLAW_LOG_BUFFER_UNLOCK();
    return dropped;
}

size_t espclaw_log_buffer_copy_tail(char *buffer, size_t buffer_size, size_t tail_bytes)
{
    size_t size;
    size_t start;
    size_t copy_index;

    if (buffer == NULL || buffer_size == 0) {
        return 0;
    }

    ESPCLAW_LOG_BUFFER_LOCK();
    size = s_log_size;
    if (tail_bytes > 0 && tail_bytes < size) {
        size = tail_bytes;
    }
    start = (s_log_start + s_log_size - size) % ESPCLAW_LOG_BUFFER_CAPACITY;
    for (copy_index = 0; copy_index < size && copy_index + 1U < buffer_size; ++copy_index) {
        buffer[copy_index] = s_log_ring[(start + copy_index) % ESPCLAW_LOG_BUFFER_CAPACITY];
    }
    ESPCLAW_LOG_BUFFER_UNLOCK();

    if (copy_index >= buffer_size) {
        copy_index = buffer_size - 1U;
    }
    buffer[copy_index] = '\0';
    return copy_index;
}

int espclaw_log_buffer_render_json(size_t tail_bytes, char *buffer, size_t buffer_size)
{
    char tail[4096];
    size_t used = 0;
    size_t stored_bytes;
    size_t dropped_bytes;

    if (buffer == NULL || buffer_size == 0) {
        return -1;
    }

    stored_bytes = espclaw_log_buffer_size();
    dropped_bytes = espclaw_log_buffer_dropped_bytes();
    espclaw_log_buffer_copy_tail(tail, sizeof(tail), tail_bytes);

    used += (size_t)snprintf(
        buffer + used,
        buffer_size - used,
        "{\"ok\":true,\"capacity_bytes\":%u,\"stored_bytes\":%u,\"dropped_bytes\":%u,\"tail\":",
        (unsigned int)ESPCLAW_LOG_BUFFER_CAPACITY,
        (unsigned int)stored_bytes,
        (unsigned int)dropped_bytes
    );
    used = append_escaped_json(buffer, buffer_size, used, tail);
    if (used < buffer_size) {
        used += (size_t)snprintf(buffer + used, buffer_size - used, "}");
    }
    return 0;
}
