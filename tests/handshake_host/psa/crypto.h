#pragma once
#include <stddef.h>
#include <stdint.h>
#define PSA_SHA_TEST_BYTES 32
#define PSA_HASH_OPERATION_INIT { 0 }
enum { PSA_SUCCESS = 0, PSA_ALG_SHA_256 = 1 };
typedef int psa_status_t;
typedef struct { uint32_t digest; } psa_hash_operation_t;
psa_status_t psa_crypto_init(void);
psa_status_t psa_hash_setup(psa_hash_operation_t *hash, int algorithm);
psa_status_t psa_hash_update(psa_hash_operation_t *hash, const uint8_t *data, size_t size);
psa_status_t psa_hash_finish(psa_hash_operation_t *hash, uint8_t *out, size_t capacity, size_t *size);
psa_status_t psa_hash_abort(psa_hash_operation_t *hash);
