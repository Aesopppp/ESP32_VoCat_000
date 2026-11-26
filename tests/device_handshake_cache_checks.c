/* Exercise the actual record format and identity checks with fake NVS and a
 * deterministic digest driver. Firmware builds link IDF's PSA SHA-256. */
#include "../main/device_handshake_cache.h"
#include "nvs.h"
#include "psa/crypto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(test) do { ++checks; if (!(test)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #test); exit(1); } } while (0)
static uint8_t stored[8192];
static size_t stored_size;
static bool force_write_failure;

esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *handle)
{
    *handle = 1;
    return strcmp(ns, "device_boot") == 0 ? ESP_OK : ESP_FAIL;
}
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *size)
{
    if (!stored_size || strcmp(key, "hs_cache_v1")) return ESP_ERR_NVS_NOT_FOUND;
    if (out && *size < stored_size) return ESP_FAIL;
    if (out) memcpy(out, stored, stored_size);
    *size = stored_size;
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t size)
{
    if (force_write_failure || size > sizeof(stored) || strcmp(key, "hs_cache_v1")) return ESP_FAIL;
    memcpy(stored, data, size);
    stored_size = size;
    return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    if (strcmp(key, "hs_cache_v1")) return ESP_ERR_NVS_NOT_FOUND;
    stored_size = 0;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle) { return ESP_OK; }
void nvs_close(nvs_handle_t handle) { }

psa_status_t psa_crypto_init(void) { return PSA_SUCCESS; }
psa_status_t psa_hash_setup(psa_hash_operation_t *hash, int algorithm)
{
    hash->digest = 2166136261u;
    return algorithm == PSA_ALG_SHA_256 ? PSA_SUCCESS : -1;
}
psa_status_t psa_hash_update(psa_hash_operation_t *hash, const uint8_t *data, size_t size)
{
    for (size_t i = 0; i < size; ++i) hash->digest = (hash->digest ^ data[i]) * 16777619u;
    return PSA_SUCCESS;
}
psa_status_t psa_hash_finish(psa_hash_operation_t *hash, uint8_t *out, size_t capacity, size_t *size)
{
    if (capacity < PSA_SHA_TEST_BYTES) return -1;
    memset(out, 0, PSA_SHA_TEST_BYTES);
    memcpy(out, &hash->digest, sizeof(hash->digest));
    *size = PSA_SHA_TEST_BYTES;
    return PSA_SUCCESS;
}
psa_status_t psa_hash_abort(psa_hash_operation_t *hash) { return PSA_SUCCESS; }

int main(void)
{
    device_handshake_config_t cfg = {.enabled = true, .device_id = "dev-fixture", .psk = "psk-a",
                                      .server_url = "https://platform-a", .device_model = "esp32s3"};
    device_handshake_response_t original = {
        .server_time = "2026-11-26T01:00:00.231Z",
        .mqtt = { .endpoint = "mqtts://fixture:8883", .username = "dev-fixture", .client_id = "id",
                  .password = "pw", .publish_topic = "devices/dev-fixture/status",
                  .subscribe_topic = "devices/dev-fixture/task" },
        .websocket = {.url = "wss://fixture", .token = "test-only-token", .expires_in = 3600},
        .firmware = {.version = "1", .url = "https://firmware"},
    }, loaded;
    int64_t acquired_ms;
    CHECK(device_handshake_parse_time(original.server_time, &original.server_time_ms));
    CHECK(!device_handshake_cache_load(&cfg, &loaded, &acquired_ms));
    device_handshake_cache_save(&cfg, &original);
    CHECK(stored_size > 0 && stored_size < 256);
    CHECK(device_handshake_cache_load(&cfg, &loaded, &acquired_ms));
    CHECK(acquired_ms == original.server_time_ms && !memcmp(&original, &loaded, sizeof(loaded)));
    device_handshake_config_t changed = cfg;
    strcpy(changed.psk, "psk-b");
    CHECK(!device_handshake_cache_load(&changed, &loaded, &acquired_ms) && acquired_ms == 0);
    changed = cfg;
    strcpy(changed.server_url, "https://platform-b");
    CHECK(!device_handshake_cache_load(&changed, &loaded, &acquired_ms));
    changed = cfg;
    strcpy(changed.device_id, "dev-b");
    CHECK(!device_handshake_cache_load(&changed, &loaded, &acquired_ms));
    stored[stored_size - 2] ^= 1;
    CHECK(!device_handshake_cache_load(&cfg, &loaded, &acquired_ms) && !loaded.websocket.token[0]);
    stored[stored_size - 2] ^= 1;
    --stored_size;
    CHECK(!device_handshake_cache_load(&cfg, &loaded, &acquired_ms));
    ++stored_size;
    force_write_failure = true;
    strcpy(original.websocket.token, "replacement-token");
    device_handshake_cache_save(&cfg, &original);
    CHECK(device_handshake_cache_load(&cfg, &loaded, &acquired_ms));
    CHECK(!strcmp(loaded.websocket.token, "test-only-token"));
    force_write_failure = false;
    memset(original.websocket.token, 't', sizeof(original.websocket.token)-1);
    device_handshake_cache_save(&cfg, &original);
    CHECK(device_handshake_cache_load(&cfg, &loaded, &acquired_ms));
    CHECK(!strcmp(loaded.websocket.token, original.websocket.token));
    device_handshake_cache_erase();
    CHECK(!device_handshake_cache_load(&cfg, &loaded, &acquired_ms));
    printf("handshake cache: %u checks passed\n", checks);
    return 0;
}
