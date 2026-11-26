#include "device_handshake_cache.h"

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "psa/crypto.h"

static const char *TAG = "handshake_cache";
static const char *CACHE_KEY = "hs_cache_v1";
enum { HASH_BYTES = 32, HEADER_BYTES = HASH_BYTES + sizeof(uint32_t) };

/* The versioned NVS blob holds a digest, token TTL and these validated strings
 * in sequence. Store only used bytes to leave room for provisioning and MQTT. */
typedef struct { size_t offset, capacity; } cache_field_t;
#define FIELD(member) { offsetof(device_handshake_response_t, member), sizeof(((device_handshake_response_t *)0)->member) }
static const cache_field_t fields[] = {
    FIELD(server_time), FIELD(mqtt.endpoint), FIELD(mqtt.client_id), FIELD(mqtt.username),
    FIELD(mqtt.password), FIELD(mqtt.publish_topic), FIELD(mqtt.subscribe_topic),
    FIELD(websocket.url), FIELD(websocket.token), FIELD(firmware.version), FIELD(firmware.url),
};
#undef FIELD

/* Include the payload and the normalized identity. Separate fields with their
 * NUL terminators so that different values cannot form the same byte stream. */
static bool cache_digest(const device_handshake_config_t *cfg, const uint8_t *payload,
                         size_t bytes, uint8_t digest[HASH_BYTES])
{
    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    psa_status_t status = psa_crypto_init();
    if (status == PSA_SUCCESS) status = psa_hash_setup(&hash, PSA_ALG_SHA_256);
    const char *identity[] = { cfg->device_id, cfg->server_url, cfg->device_model, cfg->psk };
    for (size_t i = 0; i < sizeof(identity) / sizeof(identity[0]) && status == PSA_SUCCESS; ++i) {
        status = psa_hash_update(&hash, (const uint8_t *)identity[i], strlen(identity[i]) + 1);
    }
    if (status == PSA_SUCCESS) status = psa_hash_update(&hash, payload, bytes);
    size_t written = 0;
    if (status == PSA_SUCCESS) status = psa_hash_finish(&hash, digest, HASH_BYTES, &written);
    psa_hash_abort(&hash);
    return status == PSA_SUCCESS && written == HASH_BYTES;
}

bool device_handshake_cache_load(const device_handshake_config_t *cfg,
                                 device_handshake_response_t *out, int64_t *acquired_unix_ms)
{
    if (!cfg || !cfg->enabled || !out || !acquired_unix_ms) return false;
    device_handshake_wipe(out, sizeof(*out));
    *acquired_unix_ms = 0;
    nvs_handle_t handle;
    if (nvs_open("device_boot", NVS_READONLY, &handle) != ESP_OK) return false;
    size_t bytes = 0;
    uint8_t *record = NULL;
    if (nvs_get_blob(handle, CACHE_KEY, NULL, &bytes) == ESP_OK &&
        bytes > HEADER_BYTES && bytes <= HEADER_BYTES + sizeof(*out)) {
        record = device_handshake_alloc(bytes);
    }
    size_t length = bytes;
    bool ok = record && nvs_get_blob(handle, CACHE_KEY, record, &length) == ESP_OK && length == bytes;
    nvs_close(handle);
    uint8_t expected[HASH_BYTES];
    if (ok) ok = cache_digest(cfg, record + HASH_BYTES, bytes - HASH_BYTES, expected) &&
                 memcmp(expected, record, sizeof(expected)) == 0;
    if (ok) {
        memcpy(&out->websocket.expires_in, record + HASH_BYTES, sizeof(out->websocket.expires_in));
        const uint8_t *cursor = record + HEADER_BYTES, *end = record + bytes;
        for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
            const uint8_t *nul = memchr(cursor, 0, (size_t)(end - cursor));
            if (!nul || (size_t)(nul - cursor) >= fields[i].capacity) { ok = false; break; }
            memcpy((char *)out + fields[i].offset, cursor, (size_t)(nul - cursor) + 1);
            cursor = nul + 1;
        }
        ok = ok && cursor == end && out->websocket.expires_in &&
             out->websocket.url[0] && out->websocket.token[0] &&
             strcmp(cfg->device_id, out->mqtt.username) == 0 &&
             device_handshake_parse_time(out->server_time, &out->server_time_ms) &&
             out->server_time_ms > 0;
    }
    if (record) { device_handshake_wipe(record, bytes); free(record); }
    if (ok) *acquired_unix_ms = out->server_time_ms;
    else device_handshake_wipe(out, sizeof(*out));
    return ok;
}

void device_handshake_cache_save(const device_handshake_config_t *cfg,
                                 const device_handshake_response_t *response)
{
    if (!cfg || !cfg->enabled || !response || response->server_time_ms <= 0 ||
        !response->websocket.expires_in || strcmp(cfg->device_id, response->mqtt.username)) return;
    size_t bytes = HEADER_BYTES;
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        const char *value = (const char *)response + fields[i].offset;
        const char *nul = memchr(value, 0, fields[i].capacity);
        if (!nul) return;
        bytes += (size_t)(nul - value) + 1;
    }
    uint8_t *record = device_handshake_alloc(bytes);
    if (!record) return;
    memcpy(record + HASH_BYTES, &response->websocket.expires_in, sizeof(response->websocket.expires_in));
    uint8_t *cursor = record + HEADER_BYTES;
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        const char *value = (const char *)response + fields[i].offset;
        size_t len = strlen(value) + 1;
        memcpy(cursor, value, len);
        cursor += len;
    }
    esp_err_t err = ESP_FAIL;
    if (cache_digest(cfg, record + HASH_BYTES, bytes - HASH_BYTES, record)) {
        nvs_handle_t handle;
        err = nvs_open("device_boot", NVS_READWRITE, &handle);
        if (err == ESP_OK) {
            err = nvs_set_blob(handle, CACHE_KEY, record, bytes);
            if (err == ESP_OK) err = nvs_commit(handle);
            nvs_close(handle);
        }
    }
    device_handshake_wipe(record, bytes);
    free(record);
    if (err != ESP_OK) ESP_LOGW(TAG, "Saving credentials failed: %s", esp_err_to_name(err));
}

void device_handshake_cache_erase(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("device_boot", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_erase_key(handle, CACHE_KEY);
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND)
        ESP_LOGW(TAG, "Clearing credentials failed: %s", esp_err_to_name(err));
}
