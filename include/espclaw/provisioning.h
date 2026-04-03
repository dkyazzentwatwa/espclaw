#ifndef ESPCLAW_PROVISIONING_H
#define ESPCLAW_PROVISIONING_H

#include <stdbool.h>
#include <stddef.h>

#define ESPCLAW_PROVISIONING_TRANSPORT_MAX 16
#define ESPCLAW_PROVISIONING_SERVICE_NAME_MAX 64
#define ESPCLAW_PROVISIONING_USERNAME_MAX 64
#define ESPCLAW_PROVISIONING_POP_MAX 64
#define ESPCLAW_PROVISIONING_QR_PAYLOAD_MAX 256
#define ESPCLAW_PROVISIONING_QR_URL_MAX 512
#define ESPCLAW_PROVISIONING_ADMIN_URL_MAX 128

typedef struct {
    bool active;
    char transport[ESPCLAW_PROVISIONING_TRANSPORT_MAX];
    char service_name[ESPCLAW_PROVISIONING_SERVICE_NAME_MAX];
    char username[ESPCLAW_PROVISIONING_USERNAME_MAX];
    char pop[ESPCLAW_PROVISIONING_POP_MAX];
    char qr_payload[ESPCLAW_PROVISIONING_QR_PAYLOAD_MAX];
    char qr_url[ESPCLAW_PROVISIONING_QR_URL_MAX];
    char admin_url[ESPCLAW_PROVISIONING_ADMIN_URL_MAX];
} espclaw_provisioning_descriptor_t;

void espclaw_provisioning_descriptor_init(espclaw_provisioning_descriptor_t *descriptor);
int espclaw_provisioning_build_descriptor(
    bool active,
    const char *transport,
    const char *service_name,
    const char *username,
    const char *pop,
    const char *admin_url,
    espclaw_provisioning_descriptor_t *descriptor
);
size_t espclaw_provisioning_render_json(
    const espclaw_provisioning_descriptor_t *descriptor,
    char *buffer,
    size_t buffer_size
);
const char *espclaw_provisioning_qr_helper_base_url(void);

#endif
