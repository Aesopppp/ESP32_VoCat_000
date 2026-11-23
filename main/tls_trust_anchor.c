#include "tls_trust_anchor.h"

#include <stddef.h>

#ifdef DEVICE_PLATFORM_TLS_CA
extern const unsigned char _binary_platform_tls_ca_pem_start[];
extern const unsigned char _binary_platform_tls_ca_pem_end[];

const char *tls_trust_anchor_ca_pem(void)
{
    return (const char *)_binary_platform_tls_ca_pem_start;
}

size_t tls_trust_anchor_ca_pem_len(void)
{
    return (size_t)(_binary_platform_tls_ca_pem_end - _binary_platform_tls_ca_pem_start);
}
#else
const char *tls_trust_anchor_ca_pem(void) { return NULL; }
size_t tls_trust_anchor_ca_pem_len(void) { return 0; }
#endif
