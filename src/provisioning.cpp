#include "espclaw/provisioning.h"

#include <stdio.h>
#include <string.h>

static const char *ESPCLAW_PROVISIONING_QR_HELPER_URL =
    "https://espressif.github.io/esp-jumpstart/qrcode.html";

static void copy_text(char *buffer, size_t buffer_size, const char *value)
{
    if (buffer == NULL || buffer_size == 0) {
        return;
    }
    snprintf(buffer, buffer_size, "%s", value != NULL ? value : "");
}

static size_t append_escaped_json(char *buffer, size_t buffer_size, size_t used, const char *value)
{
    const char *cursor = value != NULL ? value : "";

    if (buffer == NULL || buffer_size == 0 || used >= buffer_size) {
        return used;
    }

    used += (size_t)snprintf(buffer + used, buffer_size - used, "\"");
    while (*cursor != '\0' && used + 2 < buffer_size) {
        switch (*cursor) {
        case '\\':
        case '"':
            used += (size_t)snprintf(buffer + used, buffer_size - used, "\\%c", *cursor);
            break;
        case '\n':
            used += (size_t)snprintf(buffer + used, buffer_size - used, "\\n");
            break;
        case '\r':
            used += (size_t)snprintf(buffer + used, buffer_size - used, "\\r");
            break;
        case '\t':
            used += (size_t)snprintf(buffer + used, buffer_size - used, "\\t");
            break;
        default:
            buffer[used++] = *cursor;
            buffer[used] = '\0';
            break;
        }
        cursor++;
    }
    used += (size_t)snprintf(buffer + used, buffer_size - used, "\"");
    return used >= buffer_size ? buffer_size - 1 : used;
}

static bool is_unreserved(unsigned char value)
{
    return (value >= 'A' && value <= 'Z') ||
        (value >= 'a' && value <= 'z') ||
        (value >= '0' && value <= '9') ||
        value == '-' || value == '_' || value == '.' || value == '~';
}

static void url_encode(const char *source, char *buffer, size_t buffer_size)
{
    const unsigned char *cursor = (const unsigned char *)(source != NULL ? source : "");
    size_t used = 0;

    if (buffer == NULL || buffer_size == 0) {
        return;
    }

    buffer[0] = '\0';
    while (*cursor != '\0' && used + 4 < buffer_size) {
        if (is_unreserved(*cursor)) {
            buffer[used++] = (char)*cursor;
            buffer[used] = '\0';
        } else {
            used += (size_t)snprintf(buffer + used, buffer_size - used, "%%%02X", *cursor);
        }
        cursor++;
    }
}

void espclaw_provisioning_descriptor_init(espclaw_provisioning_descriptor_t *descriptor)
{
    if (descriptor == NULL) {
        return;
    }
    memset(descriptor, 0, sizeof(*descriptor));
}

int espclaw_provisioning_build_descriptor(
    bool active,
    const char *transport,
    const char *service_name,
    const char *username,
    const char *pop,
    const char *admin_url,
    espclaw_provisioning_descriptor_t *descriptor
)
{
    char encoded_payload[ESPCLAW_PROVISIONING_QR_PAYLOAD_MAX * 3];

    if (descriptor == NULL) {
        return -1;
    }

    espclaw_provisioning_descriptor_init(descriptor);
    descriptor->active = active;
    copy_text(descriptor->transport, sizeof(descriptor->transport), transport);
    copy_text(descriptor->service_name, sizeof(descriptor->service_name), service_name);
    copy_text(descriptor->username, sizeof(descriptor->username), username);
    copy_text(descriptor->pop, sizeof(descriptor->pop), pop);
    copy_text(descriptor->admin_url, sizeof(descriptor->admin_url), admin_url);

    if (!active) {
        return 0;
    }

    if (strcmp(descriptor->transport, "ble") == 0 && descriptor->service_name[0] != '\0') {
        size_t url_prefix_length = strlen(ESPCLAW_PROVISIONING_QR_HELPER_URL) + strlen("?data=");

        if (descriptor->pop[0] != '\0') {
            snprintf(
                descriptor->qr_payload,
                sizeof(descriptor->qr_payload),
                "{\"ver\":\"v1\",\"name\":\"%s\",\"pop\":\"%s\",\"transport\":\"ble\"}",
                descriptor->service_name,
                descriptor->pop
            );
        } else {
            snprintf(
                descriptor->qr_payload,
                sizeof(descriptor->qr_payload),
                "{\"ver\":\"v1\",\"name\":\"%s\",\"transport\":\"ble\"}",
                descriptor->service_name
            );
        }
        url_encode(descriptor->qr_payload, encoded_payload, sizeof(encoded_payload));
        if (url_prefix_length < sizeof(descriptor->qr_url) &&
            strlen(encoded_payload) < sizeof(descriptor->qr_url) - url_prefix_length) {
            copy_text(descriptor->qr_url, sizeof(descriptor->qr_url), ESPCLAW_PROVISIONING_QR_HELPER_URL);
            strncat(descriptor->qr_url, "?data=", sizeof(descriptor->qr_url) - strlen(descriptor->qr_url) - 1);
            strncat(descriptor->qr_url, encoded_payload, sizeof(descriptor->qr_url) - strlen(descriptor->qr_url) - 1);
        }
    }

    return 0;
}

size_t espclaw_provisioning_render_json(
    const espclaw_provisioning_descriptor_t *descriptor,
    char *buffer,
    size_t buffer_size
)
{
    size_t used = 0;
    espclaw_provisioning_descriptor_t empty;
    const espclaw_provisioning_descriptor_t *value = descriptor;

    if (buffer == NULL || buffer_size == 0) {
        return 0;
    }
    if (value == NULL) {
        espclaw_provisioning_descriptor_init(&empty);
        value = &empty;
    }

    used += (size_t)snprintf(
        buffer,
        buffer_size,
        "{\"ok\":true,\"active\":%s,\"transport\":",
        value->active ? "true" : "false"
    );
    used = append_escaped_json(buffer, buffer_size, used, value->transport);
    used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"service_name\":");
    used = append_escaped_json(buffer, buffer_size, used, value->service_name);
    used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"username\":");
    used = append_escaped_json(buffer, buffer_size, used, value->username);
    used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"pop\":");
    used = append_escaped_json(buffer, buffer_size, used, value->pop);
    used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"qr_payload\":");
    used = append_escaped_json(buffer, buffer_size, used, value->qr_payload);
    used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"qr_url\":");
    used = append_escaped_json(buffer, buffer_size, used, value->qr_url);
    used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"admin_url\":");
    used = append_escaped_json(buffer, buffer_size, used, value->admin_url);
    used += (size_t)snprintf(buffer + used, buffer_size - used, "}");
    return used >= buffer_size ? buffer_size - 1 : used;
}

const char *espclaw_provisioning_qr_helper_base_url(void)
{
    return ESPCLAW_PROVISIONING_QR_HELPER_URL;
}
