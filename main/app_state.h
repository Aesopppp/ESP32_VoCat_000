/**
 * @file app_state.h
 * @brief 应用状态机 — 驱动 main_task 状态转移与 LED 指示
 */
#ifndef APP_STATE_H
#define APP_STATE_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "event_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_STATE_INIT        = 0,
    APP_STATE_WIFI_CONNECTING,
    APP_STATE_WIFI_CONNECTED,
    APP_STATE_WS_CONNECTING,    /* legacy: pre-wake auto-connect (unused after wake-deferred flow) */
    APP_STATE_WS_CONNECTED,
    APP_STATE_WS_CONNECTING_WAKE, /* wake word detected, WS handshaking */
    APP_STATE_WAIT_WAKE,       /* Wi-Fi up, ready for wake word (WS lazily opened) */
    APP_STATE_LISTENING,
    APP_STATE_PLAYING,
    APP_STATE_ERROR,
    APP_STATE_SLEEP,
    APP_STATE_PROVISIONING,
} app_state_t;

const char *app_state_name(app_state_t state);
app_state_t app_state_get(void);
app_state_t app_state_set(app_state_t state);
esp_err_t app_state_init(event_bus_t bus);
esp_err_t app_state_start_listening(void);
esp_err_t app_state_back_to_wait_wake(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_STATE_H */
