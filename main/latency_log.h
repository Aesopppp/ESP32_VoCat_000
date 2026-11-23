#ifndef LATENCY_LOG_H
#define LATENCY_LOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LAT_SESSION_ID_MAX 32
#define LAT_ROOM_ID_MAX 64

void latency_log_begin(uint32_t monotonic_ms, uint32_t boot_seed);
void latency_log_event(const char *event, uint32_t monotonic_ms);
void latency_log_event_detail(const char *event, uint32_t monotonic_ms,
                              const char *room, const char *detail);
void latency_log_room(const char *room);
void latency_log_add_offset(int32_t server_minus_device_ms);
void latency_log_aligned_iso(char *buffer, size_t size, uint32_t monotonic_ms);
const char *latency_log_session_id(void);
bool latency_log_session_open(void);
void latency_log_mark_upload_phase(void);
bool latency_log_first_live_uplink_pending(void);

#ifdef __cplusplus
}
#endif

#endif
