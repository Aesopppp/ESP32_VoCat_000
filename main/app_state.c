/**
 * @file app_state.c
 * @brief 应用状态机实现
 */

#include "app_state.h"
#include <string.h>
#include "esp_log.h"
#include "audio_in.h"
#include "wake_word.h"
#include "event_bus.h"
#include "ws_service.h"

static const char *TAG = "app_state";

/* 状态仅用于应用和界面；音频收发与静默判定由主任务统一协调。 */
static app_state_t s_state = APP_STATE_INIT;
static SemaphoreHandle_t s_state_lock;

esp_err_t app_state_init(event_bus_t bus)
{
    (void)bus;
    if (!s_state_lock) s_state_lock = xSemaphoreCreateMutex();
    if (!s_state_lock) return ESP_ERR_NO_MEM;
    s_state = APP_STATE_INIT;
    return ESP_OK;
}

app_state_t app_state_get(void)
{
    app_state_t s;
    if (s_state_lock && xSemaphoreTake(s_state_lock, portMAX_DELAY) == pdTRUE) {
        s = s_state;
        xSemaphoreGive(s_state_lock);
    } else {
        s = APP_STATE_INIT;
    }
    return s;
}

app_state_t app_state_set(app_state_t new_state)
{
    app_state_t old_state = APP_STATE_INIT;
    if (s_state_lock && xSemaphoreTake(s_state_lock, portMAX_DELAY) == pdTRUE) {
        old_state = s_state;
        if (old_state != new_state) {
            ESP_LOGI(TAG, "state: %s -> %s",
                     app_state_name(old_state),
                     app_state_name(new_state));
            s_state = new_state;
        }
        xSemaphoreGive(s_state_lock);
    }
    return old_state;
}

const char *app_state_name(app_state_t s)
{
    switch (s) {
        case APP_STATE_INIT:            return "INIT";
        case APP_STATE_WIFI_CONNECTING: return "WIFI_CONNECTING";
        case APP_STATE_WIFI_CONNECTED:  return "WIFI_CONNECTED";
        case APP_STATE_WS_CONNECTING:   return "WS_CONNECTING";
        case APP_STATE_WS_CONNECTED:    return "WS_CONNECTED";
        case APP_STATE_WS_CONNECTING_WAKE: return "WS_CONNECTING_WAKE";
        case APP_STATE_WAIT_WAKE:       return "WAIT_WAKE";
        case APP_STATE_LISTENING:       return "LISTENING";
        case APP_STATE_PLAYING:         return "PLAYING";
        case APP_STATE_ERROR:           return "ERROR";
        case APP_STATE_SLEEP:           return "SLEEP";
        case APP_STATE_PROVISIONING:    return "PROVISIONING";
        default:                        return "UNKNOWN";
    }
}

esp_err_t app_state_start_listening(void)
{
    if (ws_service_get_state() != WS_STATE_CONNECTED) {
        ESP_LOGW(TAG, "Cannot listen: WebSocket authentication is not ready");
        return ESP_ERR_INVALID_STATE;
    }
    app_state_set(APP_STATE_LISTENING);
    /* 播放/平台打断期间保持同一上传任务；首次认证后才需要创建。
     * 幂等 start 不清空半包 PCM、不重置编码器，避免截掉句首。 */
    audio_in_handle_t ai = audio_in_get_handle();
    esp_err_t err = audio_in_start_from_ringbuf(ai, wake_word_get_ringbuf());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio_in_start_from_ringbuf failed: %s", esp_err_to_name(err));
        app_state_set(APP_STATE_ERROR);
        return err;
    }

    ESP_LOGI(TAG, "Full-duplex microphone upload started");
    return ESP_OK;
}

esp_err_t app_state_back_to_wait_wake(void)
{
    app_state_t prev = app_state_get();

    /* 停止 audio_in 收音 */
    audio_in_handle_t ai = audio_in_get_handle();
    esp_err_t err = audio_in_stop(ai);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio_in_stop failed: %s", esp_err_to_name(err));
        return err; /* 上传任务仍持有当前采样位置，不能先切模式清空它的 FIFO。 */
    }
    /* 在允许下一次唤醒之前清理。发布 WAIT_WAKE 后再清会误删新唤醒的首句。 */
    wake_word_clear_audio_buffer();
    /* 停上传后再切 AFE，保持缓存清空与采样计数复位的生命周期一致。 */
    if (prev != APP_STATE_WAIT_WAKE) {
        if (wake_word_set_mode(WAKE_WORD_MODE_WAKE) == ESP_OK) {
            ESP_LOGI(TAG, "AFE mode -> WAKE (WakeNet enabled for wake word detection)");
        }
    }

    wake_word_reset();
    app_state_set(APP_STATE_WAIT_WAKE);
    ESP_LOGI(TAG, "Back to WAIT_WAKE, waiting for wake word...");
    return ESP_OK;
}
