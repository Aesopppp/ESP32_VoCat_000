#pragma once
/* 复制为 device_mqtt_local_config.h（Git 忽略）。
 * 待机任务触发全双工会话，auth 首帧携带 task_ids；在线平台直接推音频。
 * 设置为 0 可只保留 MQTT 状态上报/任务接收，不自动唤醒语音会话。 */
#define DEVICE_MQTT_FORWARD_TASK_TO_WS 1
/* CA 放 main/certs/device_mqtt_ca.pem，必须是平台重新签发的有效根证书。 */
