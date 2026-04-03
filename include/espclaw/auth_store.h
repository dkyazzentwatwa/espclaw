#ifndef ESPCLAW_AUTH_STORE_H
#define ESPCLAW_AUTH_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ESPCLAW_AUTH_PROVIDER_ID_MAX 31
#define ESPCLAW_AUTH_MODEL_MAX 95
#define ESPCLAW_AUTH_URL_MAX 191
#define ESPCLAW_AUTH_TOKEN_MAX 2047
#define ESPCLAW_AUTH_ACCOUNT_ID_MAX 127
#define ESPCLAW_AUTH_SOURCE_MAX 31

typedef struct {
    bool configured;
    char provider_id[ESPCLAW_AUTH_PROVIDER_ID_MAX + 1];
    char model[ESPCLAW_AUTH_MODEL_MAX + 1];
    char base_url[ESPCLAW_AUTH_URL_MAX + 1];
    char access_token[ESPCLAW_AUTH_TOKEN_MAX + 1];
    char refresh_token[ESPCLAW_AUTH_TOKEN_MAX + 1];
    char account_id[ESPCLAW_AUTH_ACCOUNT_ID_MAX + 1];
    char source[ESPCLAW_AUTH_SOURCE_MAX + 1];
    int64_t expires_at;
} espclaw_auth_profile_t;

void espclaw_auth_profile_default(espclaw_auth_profile_t *profile);
bool espclaw_auth_profile_is_ready(const espclaw_auth_profile_t *profile);

int espclaw_auth_store_init(const char *workspace_root);
int espclaw_auth_store_load(espclaw_auth_profile_t *profile);
int espclaw_auth_store_save(const espclaw_auth_profile_t *profile);
int espclaw_auth_store_clear(void);
int espclaw_auth_store_import_json(
    const char *json,
    espclaw_auth_profile_t *profile,
    char *message,
    size_t message_size
);
int espclaw_auth_store_import_codex_cli(
    const char *codex_home,
    espclaw_auth_profile_t *profile,
    char *message,
    size_t message_size
);

#endif
