#include "latency_log.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_timer.h"
#include "esp_system.h"
#include "esp_random.h"

static char s_session_id[LAT_SESSION_ID_MAX] = "wake-pending";
static char s_room[LAT_ROOM_ID_MAX];
static int64_t s_anchor_utc_ms;
static uint32_t s_anchor_monotonic_ms;
static bool s_open;
static bool s_first_uplink_pending;

static void aligned_iso(char *buffer, size_t size, int64_t utc_ms)
{
    time_t seconds = (time_t)(utc_ms / 1000);
    struct tm utc;
    gmtime_r(&seconds, &utc);
    snprintf(buffer, size, "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ",
             utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
             utc.tm_hour, utc.tm_min, utc.tm_sec,
             (long long)(utc_ms % 1000));
}

void latency_log_begin(uint32_t monotonic_ms, uint32_t boot_seed)
{
    time_t now = time(NULL);
    snprintf(s_session_id, sizeof(s_session_id), "wake-%08lx-%08lx",
             (unsigned long)boot_seed, (unsigned long)monotonic_ms);
    s_room[0] = 0;
    s_anchor_utc_ms = (int64_t)now * 1000 + monotonic_ms % 1000;
    s_anchor_monotonic_ms = (int32_t)monotonic_ms;
    s_open = true;
    s_first_uplink_pending = false;
}

void latency_log_event(const char *event, uint32_t monotonic_ms)
{
    latency_log_event_detail(event, monotonic_ms, NULL, NULL);
}

void latency_log_event_detail(const char *event, uint32_t monotonic_ms,
                              const char *room, const char *detail)
{
    if (!event) return;
    if (!s_open) latency_log_begin(monotonic_ms, (uint32_t)esp_random());
    if (room && room[0]) {
        snprintf(s_room, sizeof(s_room), "%s", room);
    }
    char iso[32];
    aligned_iso(iso, sizeof(iso), s_anchor_utc_ms +
                (int32_t)(monotonic_ms - s_anchor_monotonic_ms));
    if (detail && detail[0]) {
        if (s_room[0])
            printf("%s LAT session=%s event=%s monotonic_ms=%lu room=%s detail=%s\n",
                   iso, s_session_id, event, (unsigned long)monotonic_ms, s_room, detail);
        else
            printf("%s LAT session=%s event=%s monotonic_ms=%lu detail=%s\n",
                   iso, s_session_id, event, (unsigned long)monotonic_ms, detail);
    } else if (s_room[0]) {
        printf("%s LAT session=%s event=%s monotonic_ms=%lu room=%s\n",
               iso, s_session_id, event, (unsigned long)monotonic_ms, s_room);
    } else {
        printf("%s LAT session=%s event=%s monotonic_ms=%lu\n",
               iso, s_session_id, event, (unsigned long)monotonic_ms);
    }
}

void latency_log_room(const char *room)
{
    if (!room || !room[0]) return;
    snprintf(s_room, sizeof(s_room), "%s", room);
}

void latency_log_add_offset(int32_t server_minus_device_ms)
{
    s_anchor_utc_ms += server_minus_device_ms;
}

void latency_log_aligned_iso(char *buffer, size_t size, uint32_t monotonic_ms)
{
    if (!buffer || size == 0) return;
    aligned_iso(buffer, size, s_anchor_utc_ms +
                (int32_t)(monotonic_ms - s_anchor_monotonic_ms));
}

const char *latency_log_session_id(void) { return s_session_id; }
bool latency_log_session_open(void) { return s_open; }
void latency_log_mark_upload_phase(void)
{
    if (s_open) s_first_uplink_pending = true;
}
bool latency_log_first_live_uplink_pending(void)
{
    bool pending = s_first_uplink_pending;
    s_first_uplink_pending = false;
    return pending;
}
