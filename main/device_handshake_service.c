#include "device_handshake_service.h"
#include "device_handshake_cache.h"
#include "device_handshake_http.h"
#include "wifi_service.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "tls_trust_anchor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"

/* 沿用本工程的本地私有头文件约定；此文件被 Git 忽略。
 * NVS device_boot/config 整条记录优先，绝不混用两套身份的字段。 */
#if __has_include("device_handshake_local_config.h")
#include "device_handshake_local_config.h"
#endif
#ifndef DEVICE_HANDSHAKE_LOCAL_JSON
#define DEVICE_HANDSHAKE_LOCAL_JSON "{\"enabled\":false}"
#endif
#ifndef DEVICE_HANDSHAKE_WS_REFRESH_MARGIN_SECONDS
#define DEVICE_HANDSHAKE_WS_REFRESH_MARGIN_SECONDS 300u
#endif
#ifndef DEVICE_HANDSHAKE_LOG_WS_TOKEN
#define DEVICE_HANDSHAKE_LOG_WS_TOKEN 0
#endif
_Static_assert(DEVICE_HANDSHAKE_WS_REFRESH_MARGIN_SECONDS > 0 &&
               DEVICE_HANDSHAKE_WS_REFRESH_MARGIN_SECONDS <= UINT32_MAX,
               "WebSocket refresh margin must be a positive number of seconds");

static const char *TAG = "device_handshake";
static device_handshake_config_t *s_config;
static device_handshake_schedule_t s_schedule;
static bool s_initialized, s_pending, s_disabled;
static bool s_ws_waiting_for_credentials;
static QueueHandle_t s_jobs, s_results;
static esp_event_handler_instance_t s_wifi_handler, s_ip_handler;
/* 回调只改变 epoch，不触碰主任务的状态或做网络 I/O。
 * 即使主任务来不及处理一次很短的断线再重连，旧 HTTP 结果也会失效。 */
static atomic_uint s_network_epoch, s_cancel_generation;
static unsigned s_seen_epoch;
static unsigned s_mqtt_epoch;
/* A usable snapshot can cover the background HTTP refresh after GOT_IP. */
#define HANDSHAKE_CACHE_REUSE_MS 120000
#define HANDSHAKE_BACKGROUND_DELAY_MS 6000
static int64_t s_reuse_until_ms;

typedef struct {
    unsigned network_epoch;
    uint32_t generation;
    int64_t acquired_ms; /* 请求开始的单调时间；不是可被校时修改的系统时间。 */
    int64_t cached_server_ms; /* Nonzero only for a snapshot loaded from flash. */
    device_handshake_config_t config; /* 请求拥有独立副本，refresh 可安全替换主配置。 */
    device_handshake_result_t result;
} handshake_attempt_t;

static handshake_attempt_t *s_snapshot;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static int64_t snapshot_remaining_ms(void)
{
    const uint32_t ttl = s_snapshot->result.response.websocket.expires_in;
    if (!s_snapshot->cached_server_ms)
        return device_handshake_token_remaining_ms(ttl, s_snapshot->acquired_ms, now_ms());
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) return 0;
    const int64_t wall_ms = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    if (wall_ms < 1700000000000LL) {
        /* Power loss can reset the RTC to 1970. Allow one short boot window
         * for gateway validation while a real HTTP request obtains fresh credentials. */
        if (now_ms() - s_snapshot->acquired_ms >= HANDSHAKE_CACHE_REUSE_MS) return 0;
        return device_handshake_token_remaining_ms(ttl, s_snapshot->acquired_ms, now_ms());
    }
    return device_handshake_token_remaining_ms(ttl, s_snapshot->cached_server_ms, wall_ms);
}

static void release_attempt(handshake_attempt_t *attempt)
{
    if (!attempt) return;
    device_handshake_wipe(attempt, sizeof(*attempt));
    free(attempt);
}

static void clear_snapshot(void)
{
    release_attempt(s_snapshot);
    s_snapshot = NULL;
}

static void on_network_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if ((base == WIFI_EVENT && (id == WIFI_EVENT_STA_DISCONNECTED || id == WIFI_EVENT_STA_STOP)) ||
        (base == IP_EVENT && (id == IP_EVENT_STA_GOT_IP || id == IP_EVENT_STA_LOST_IP)))
        atomic_fetch_add(&s_network_epoch, 1);
}

/* 独立 namespace 是平台出厂身份；不读写 korvo_cfg/device_id（旧 YMOS 身份）。
 * NVS 错误必须显式失败，不能默默退回固件内的另一台设备凭据。 */
static esp_err_t load_config(device_handshake_config_t *config)
{
    char *json = device_handshake_alloc(DEVICE_HANDSHAKE_CONFIG_MAX + 1);
    if (!json) return ESP_ERR_NO_MEM;
    const char *reason = "nvs_read_failed";
    const char *source = "NVS";
    nvs_handle_t handle = 0;
    size_t len = 0;
    esp_err_t err = nvs_open("device_boot", NVS_READONLY, &handle);
    if (err == ESP_OK) {
        err = nvs_get_str(handle, "config", NULL, &len);
        if (err == ESP_OK) {
            if (len < 2 || len > DEVICE_HANDSHAKE_CONFIG_MAX + 1) err = ESP_ERR_INVALID_SIZE;
            else err = nvs_get_str(handle, "config", json, &len);
        }
        nvs_close(handle);
    }
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        source = "local default";
        len = sizeof(DEVICE_HANDSHAKE_LOCAL_JSON);
        if (len > DEVICE_HANDSHAKE_CONFIG_MAX + 1) err = ESP_ERR_INVALID_SIZE;
        else { memcpy(json, DEVICE_HANDSHAKE_LOCAL_JSON, len); err = ESP_OK; }
    }
    if (err == ESP_OK && (len < 2 || json[len - 1] != 0 || strlen(json) + 1 != len))
        err = ESP_ERR_INVALID_SIZE;
    if (err == ESP_OK && !device_handshake_parse_config(json, len - 1, config, &reason))
        err = ESP_ERR_INVALID_ARG;
    device_handshake_wipe(json, DEVICE_HANDSHAKE_CONFIG_MAX + 1);
    free(json);
    if (err != ESP_OK) ESP_LOGE(TAG, "Configuration blocked: %s (%s)", reason, esp_err_to_name(err));
    else ESP_LOGI(TAG, "%s via %s", config->enabled ? "Configured; waiting for Wi-Fi" : "Disabled", source);
    return err;
}

typedef struct {
    handshake_attempt_t *attempt;
    esp_http_client_handle_t client;
    int64_t init_ms, open_ms, write_ms, headers_ms;
} handshake_http_context_t;

static bool http_cancelled(void *context)
{
    handshake_attempt_t *attempt = ((handshake_http_context_t *)context)->attempt;
    return attempt->network_epoch != atomic_load(&s_network_epoch) ||
           attempt->generation != atomic_load(&s_cancel_generation);
}

static int64_t http_now_ms(void *context) { (void)context; return now_ms(); }

static bool http_begin(void *context, const char *url, const device_handshake_header_t *headers,
                       size_t count, const char *body, int *status, int64_t *content_length)
{
    handshake_http_context_t *http = context;
    int64_t started = now_ms();
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 5000,
        .disable_auto_redirect = true,
        .keep_alive_enable = false,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
#ifdef DEVICE_PLATFORM_TLS_CA
        .crt_bundle_attach = NULL,
        .cert_pem = tls_trust_anchor_ca_pem(),
#else
        .crt_bundle_attach = esp_crt_bundle_attach,
#endif
    };
    http->client = esp_http_client_init(&config);
    bool configured = http->client != NULL;
    for (size_t i = 0; configured && i < count; ++i)
        configured = esp_http_client_set_header(http->client, headers[i].name, headers[i].value) == ESP_OK;
    http->init_ms = now_ms() - started;
    if (!configured || http_cancelled(context)) return false;
    started = now_ms();
    esp_err_t opened = esp_http_client_open(http->client, (int)strlen(body));
    http->open_ms = now_ms() - started;
    if (opened != ESP_OK) return false;
    size_t sent = 0, body_len = strlen(body);
    while (sent < body_len) {
        if (http_cancelled(context)) return false;
        started = now_ms();
        int n = esp_http_client_write(http->client, body + sent, (int)(body_len - sent));
        http->write_ms += now_ms() - started;
        if (n <= 0 || (size_t)n > body_len - sent) return false;
        sent += (size_t)n;
    }
    if (http_cancelled(context)) return false;
    started = now_ms();
    int64_t length = esp_http_client_fetch_headers(http->client);
    http->headers_ms = now_ms() - started;
    if (length < 0) return false;
    *status = esp_http_client_get_status_code(http->client);
    /* chunked/关闭连接定界时长度未知；不能将 fetch_headers 的 0 当作空 body。 */
    *content_length = esp_http_client_is_chunked_response(http->client) ? -1 :
                      esp_http_client_get_content_length(http->client);
    return *status >= 100;
}

static int http_read(void *context, char *buffer, size_t size)
{
    return esp_http_client_read(((handshake_http_context_t *)context)->client, buffer, (int)size);
}

static bool http_complete(void *context)
{
    return esp_http_client_is_complete_data_received(((handshake_http_context_t *)context)->client);
}

static void http_close(void *context)
{
    handshake_http_context_t *http = context;
    if (http->client) esp_http_client_cleanup(http->client);
    http->client = NULL;
}

static void handshake_worker(void *arg)
{
    (void)arg;
    while (1) {
        handshake_attempt_t *attempt = NULL;
        if (xQueueReceive(s_jobs, &attempt, portMAX_DELAY) != pdTRUE) continue;
        const int64_t started = now_ms();
        handshake_http_context_t context = { .attempt = attempt };
        device_handshake_http_t http = {
            .context = &context, .begin = http_begin, .read = http_read,
            .complete = http_complete, .close = http_close,
            .cancelled = http_cancelled, .now_ms = http_now_ms,
        };
        char mac[18] = {0};
        wifi_service_get_mac_str(mac, sizeof(mac), ESP_MAC_WIFI_STA);
        wifi_service_set_setup_active(WIFI_SETUP_HTTP, true);
        device_handshake_exchange(&http, &attempt->config, esp_app_get_description()->version,
                                   mac, &attempt->result);
        wifi_service_set_setup_active(WIFI_SETUP_HTTP, false);
        ESP_LOGI(TAG, "HTTP handshake timing (ms): queue=%lld init=%lld open=%lld write=%lld headers=%lld read=%lld total=%lld status=%d",
                 (long long)(started - attempt->acquired_ms), (long long)context.init_ms,
                 (long long)context.open_ms, (long long)context.write_ms, (long long)context.headers_ms,
                 (long long)attempt->result.read_ms, (long long)(now_ms() - attempt->acquired_ms),
                 attempt->result.status);
        device_handshake_wipe(&attempt->config, sizeof(attempt->config));
        /* 只有一个 outstanding 请求，队列长度 1；结果发布时 HTTP 已彻底释放。
         * 所有权从 worker 转给 main_task，生产者之后不得访问 attempt。 */
        xQueueSend(s_results, &attempt, portMAX_DELAY);
    }
}

static bool ensure_worker(void)
{
    if (s_jobs) return true;
    s_jobs = xQueueCreate(1, sizeof(handshake_attempt_t *));
    s_results = xQueueCreate(1, sizeof(handshake_attempt_t *));
    if (s_jobs && s_results &&
        xTaskCreatePinnedToCore(handshake_worker, "device_http", 8192, NULL, 2, NULL, 0) == pdPASS)
        return true;
    if (s_jobs) vQueueDelete(s_jobs);
    if (s_results) vQueueDelete(s_results);
    s_jobs = s_results = NULL;
    return false;
}

esp_err_t device_handshake_service_refresh(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    ++s_mqtt_epoch;
    s_disabled = false; /* 读取失败必须阻止语音，不能回退到旧 key。 */
    device_handshake_refresh(&s_schedule);
    atomic_store(&s_cancel_generation, s_schedule.generation);
    clear_snapshot();
    if (!s_config) s_config = device_handshake_alloc(sizeof(*s_config));
    if (!s_config) { s_schedule.blocked = true; return ESP_ERR_NO_MEM; }
    device_handshake_wipe(s_config, sizeof(*s_config));
    esp_err_t err = load_config(s_config);
    s_disabled = err == ESP_OK && !s_config->enabled;
    if (err != ESP_OK) s_schedule.blocked = true;
    if (err != ESP_OK || !s_config->enabled) {
        device_handshake_wipe(s_config, sizeof(*s_config));
        free(s_config);
        s_config = NULL;
    }
    return err;
}

esp_err_t device_handshake_service_init(void)
{
    if (s_initialized) return ESP_OK;
    esp_err_t err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        on_network_event, NULL, &s_wifi_handler);
    if (err != ESP_OK) return err;
    err = esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                              on_network_event, NULL, &s_ip_handler);
    if (err != ESP_OK) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_handler);
        return err;
    }
    /* IDF HTTP_CLIENT 的 DEBUG 会输出请求头；避免 PSK 随调试日志泄露。 */
    esp_log_level_set("HTTP_CLIENT", ESP_LOG_INFO);
    s_initialized = true;
    err = device_handshake_service_refresh();
    if (err == ESP_OK && s_config) {
        handshake_attempt_t *cached = device_handshake_alloc(sizeof(*cached));
        if (cached && device_handshake_cache_load(s_config, &cached->result.response,
                                                  &cached->cached_server_ms)) {
            cached->acquired_ms = now_ms();
            s_snapshot = cached;
            ESP_LOGI(TAG, "Boot handshake cache loaded; checking token lifetime on Wi-Fi");
        } else {
            release_attempt(cached);
        }
    }
    return err;
}

const device_handshake_response_t *device_handshake_service_get_configuration(void)
{
    if (!s_snapshot || !s_schedule.online || s_schedule.blocked ||
        s_seen_epoch != atomic_load(&s_network_epoch)) return NULL;
    if (!s_schedule.ready && (now_ms() >= s_reuse_until_ms ||
        snapshot_remaining_ms() < (int64_t)DEVICE_HANDSHAKE_WS_REFRESH_MARGIN_SECONDS * 1000))
        return NULL;
    return &s_snapshot->result.response;
}

bool device_handshake_service_is_enabled(void) { return !s_disabled; }
bool device_handshake_service_busy(void) { return s_pending; }
bool device_handshake_service_is_blocked(void) { return !s_initialized || s_schedule.blocked; }
unsigned device_handshake_service_websocket_epoch(void) { return s_schedule.generation; }
unsigned device_handshake_service_mqtt_epoch(void) { return s_mqtt_epoch; }

bool device_handshake_service_renew_credentials(void)
{
    if (!s_initialized || s_disabled || s_schedule.blocked || !s_snapshot) return false;
    /* A cached token can be rejected while its refresh is queued or in flight.
     * Keep that job's generation and normal HTTP backoff. */
    if (!device_handshake_renew(&s_schedule) && !s_schedule.failures)
        s_schedule.retry_at_ms = 0;
    atomic_store(&s_cancel_generation, s_schedule.generation);
    device_handshake_cache_erase();
    clear_snapshot();
    return true;
}

device_ws_readiness_t device_handshake_service_prepare_websocket(
    const device_handshake_response_t **configuration)
{
    if (!configuration) return DEVICE_WS_BLOCKED;
    *configuration = NULL;
    if (!s_initialized) return DEVICE_WS_BLOCKED;
    if (s_disabled) return DEVICE_WS_DISABLED;
    if (!s_config || s_schedule.blocked) return DEVICE_WS_BLOCKED;
    const device_handshake_response_t *ready = device_handshake_service_get_configuration();
    s_ws_waiting_for_credentials = ready == NULL;
    if (!ready) {
        /* A cached token may cross the expiry margin during the standby head
         * start. A needed exchange skips that delay, preserving failure backoff. */
        if (!s_schedule.failures) s_schedule.retry_at_ms = 0;
        return DEVICE_WS_WAITING;
    }
    int64_t remaining = snapshot_remaining_ms();
    if (remaining < (int64_t)DEVICE_HANDSHAKE_WS_REFRESH_MARGIN_SECONDS * 1000) {
        ESP_LOGI(TAG, "WebSocket token near expiry (%lld s); renewing before connect",
                 (long long)(remaining / 1000));
        device_handshake_service_renew_credentials();
        return DEVICE_WS_WAITING;
    }
    *configuration = ready;
    return DEVICE_WS_READY;
}

void device_handshake_service_poll(bool network_available, bool voice_priority)
{
    if (!s_initialized) return;
    /* prepare_websocket is polled while a connection needs credentials. Consume
     * its request once so a previous wake or network generation cannot keep
     * starting background work during an established call. */
    bool ws_waiting = s_ws_waiting_for_credentials;
    s_ws_waiting_for_credentials = false;
    unsigned epoch = atomic_load(&s_network_epoch);
    if (epoch != s_seen_epoch || network_available != s_schedule.online) {
        ws_waiting = false;
        ++s_mqtt_epoch;
        s_seen_epoch = epoch;
        device_handshake_network_changed(&s_schedule, network_available);
        atomic_store(&s_cancel_generation, s_schedule.generation);
        const int64_t now = now_ms();
        s_reuse_until_ms = network_available ? now + HANDSHAKE_CACHE_REUSE_MS : 0;
        if (network_available && device_handshake_service_get_configuration()) {
            /* Give the standby socket a head start. Never postpone a failed HTTP
             * retry again just because a new IP notification arrives. */
            if (!s_schedule.failures && !s_pending && s_schedule.retry_at_ms == 0)
                s_schedule.retry_at_ms = now + HANDSHAKE_BACKGROUND_DELAY_MS;
            ESP_LOGI(TAG, "Wi-Fi ready; cached credentials available for preconnect");
        }
    }
    handshake_attempt_t *finished = NULL;
    if (s_results && xQueueReceive(s_results, &finished, 0) == pdTRUE) {
        s_pending = false;
        device_handshake_result_t *result = &finished->result;
        /* 新发 token 已不足安全窗口时停止并明确报错，避免 HTTP 200 续期风暴。 */
        if (result->success && device_handshake_token_remaining_ms(
                result->response.websocket.expires_in, finished->acquired_ms, now_ms()) <
                (int64_t)DEVICE_HANDSHAKE_WS_REFRESH_MARGIN_SECONDS * 1000) {
            result->success = false;
            result->reason = "websocket_token_lifetime_too_short";
        }
        bool current = finished->network_epoch == atomic_load(&s_network_epoch) &&
                       device_handshake_complete(&s_schedule, finished->generation,
                                                  result->status, result->success, now_ms());
        if (!current) ESP_LOGI(TAG, "Discarded obsolete HTTP result");
        else if (result->success) {
            /* A token refresh on the same gateway keeps its authenticated standby
             * transport. A changed endpoint requires a new transport. */
            if (s_snapshot && strcmp(s_snapshot->result.response.websocket.url,
                                     result->response.websocket.url) != 0) {
                ++s_schedule.generation;
                atomic_store(&s_cancel_generation, s_schedule.generation);
            }
            clear_snapshot();
            s_snapshot = finished;
            struct timeval tv = {
                .tv_sec = result->response.server_time_ms / 1000,
                .tv_usec = (result->response.server_time_ms % 1000) * 1000,
            };
            if (settimeofday(&tv, NULL) != 0) ESP_LOGW(TAG, "Server time could not be applied");
            device_handshake_cache_save(s_config, &result->response);
            ESP_LOGI(TAG, "HTTP handshake ready; MQTT/WS configuration validated; WS token TTL=%lu s",
                     (unsigned long)result->response.websocket.expires_in);
#if DEVICE_HANDSHAKE_LOG_WS_TOKEN
            /* 用户显式要求的串口联调诊断；仅私有配置开启，量产必须关闭。
             * 每次成功握手打印一次，绝不输出 PSK、MQTT password 或完整响应。 */
            ESP_LOGW(TAG, "DEV ONLY websocket.token=%s", result->response.websocket.token);
#endif
        } else if (s_schedule.blocked) {
            clear_snapshot();
            device_handshake_cache_erase();
            ESP_LOGE(TAG, "HTTP %d: %s; stopped until explicit configuration refresh/reboot",
                     result->status, result->reason);
        } else {
            ESP_LOGW(TAG, "HTTP %d: %s; retry in %u ms", result->status, result->reason,
                     device_handshake_retry_delay_ms(s_schedule.failures));
        }
        if (finished != s_snapshot) release_attempt(finished);
    }
    if (!s_config || !s_config->enabled || s_pending || !device_handshake_due(&s_schedule, now_ms()))
        return;
    /* An authenticated call can outlast the boot cache window. Only a caller
     * preparing a new connection needs that cached snapshot replaced now.
     * Missing/explicitly invalidated credentials always need an exchange. */
    if (voice_priority && s_snapshot &&
        (!ws_waiting || device_handshake_service_get_configuration())) return;
    handshake_attempt_t *attempt = ensure_worker() ? device_handshake_alloc(sizeof(*attempt)) : NULL;
    if (!attempt) {
        device_handshake_complete(&s_schedule, s_schedule.generation, 0, false, now_ms());
        ESP_LOGW(TAG, "HTTP worker allocation failed; retry in %u ms",
                 device_handshake_retry_delay_ms(s_schedule.failures));
        return;
    }
    attempt->config = *s_config;
    attempt->generation = s_schedule.generation;
    attempt->acquired_ms = now_ms();
    attempt->network_epoch = s_seen_epoch;
    if (xQueueSend(s_jobs, &attempt, 0) != pdTRUE) {
        release_attempt(attempt);
        device_handshake_complete(&s_schedule, s_schedule.generation, 0, false, now_ms());
        return;
    }
    s_pending = true;
    ESP_LOGI(TAG, "HTTP handshake started");
}
