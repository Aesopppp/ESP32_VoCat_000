#pragma once

#include "device_handshake.h"

/* Save only validated HTTP results. The caller checks TTL and wall clock at use. */
bool device_handshake_cache_load(const device_handshake_config_t *cfg,
                                 device_handshake_response_t *out, int64_t *acquired_unix_ms);
void device_handshake_cache_save(const device_handshake_config_t *cfg,
                                 const device_handshake_response_t *response);
void device_handshake_cache_erase(void);
