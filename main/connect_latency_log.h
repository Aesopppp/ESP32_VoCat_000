#ifndef CONNECT_LATENCY_LOG_H
#define CONNECT_LATENCY_LOG_H

/* 唤醒→可说话全链路的单调时间戳打点。
 *
 * 所有打点用同一个 TAG "LAT" 和 esp_timer_get_time()（单调毫秒），
 * 设备组抓串口日志后 `grep " LAT: "` 即可按 t= 排序算各段耗时，
 * 与网关 session 日志（UTC）对齐时只需一对齐点。
 *
 * 纯日志：不改变任何状态、不增加任务、不发送网络报文。
 */
#include "esp_log.h"
#include "esp_timer.h"

#define LAT_TAG "LAT"
#define LAT_LOG(event) \
    ESP_LOGI(LAT_TAG, "%s t=%lld", (event), (long long)(esp_timer_get_time() / 1000))

#endif /* CONNECT_LATENCY_LOG_H */
