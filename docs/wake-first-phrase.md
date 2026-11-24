# 唤醒首句补传与实时上传

目标是让用户连说「你好小智，今天天气怎么样」时能直接提问。WakeNet 命中帧的末尾是本地保留语音的起点。用户可以在听到设备回答之前说完整个问题。

## 待机预连接

开启 `main/device_preconnect_config.h` 中的 `DEVICE_WS_PRECONNECT_ENABLED` 后，设备联网并取得 HTTP 握手凭据，在待机时建立 WSS。网关需要支持以下两步。

| 时机 | 设备发出 | 网关返回 | 设备动作 |
| --- | --- | --- | --- |
| 待机 | 带 `preconnect:true` 的 `auth` | `__preconnect_ready__` 和正整数 `expires_in`（秒） | 保持待机，维持连接，无麦克风上行 |
| 用户或提醒唤醒 | 同一条 WSS 发送普通 `auth`，提醒附 `task_ids` | `__proxy_connected__`，确认 Opus、16000 Hz、单声道 | 启动全双工上传，播放平台声音 |

两步鉴权都使用 HTTP 握手签发的 `websocket.token`、本机 `device_id` 和 Opus 格式。网关应当只在第二步创建通话。旧网关若忽略 `preconnect:true`，可能在后台探测时就创建一次通话；部署这版前应确认网关已上线两阶段鉴权，或将开关设为 `0` 后重新编译。

## 语音唤醒时已有预连接

正式鉴权期间，AFE 按采样顺序保留唤醒命中后的 PCM。收到正式 ACK 就启动上传，先送出实际积累的声音，再接上持续采集的语音。即使用户还在说「今天的天气」，也无需等待句尾或凑满 512 ms。若鉴权超过 512 ms 的实时窗口，设备继续保护句首；网络越慢，需要补传的声音可能越长。按键、触屏及 MQTT 唤醒从正式鉴权后开启麦克风上行。在线提醒继续由平台在已有会话内直接推送。

## 需要重新建连时的首句保护

预连接缺失、过期或被拒时，有唤醒请求才会新建正式通话。使用两阶段协议的网关在冷连接上也收到包含设备号和 Opus 格式的普通鉴权。显式关闭预连接或已识别旧网关时沿用原鉴权格式。

1. WakeNet 命中帧及其之前的音频不上传，其后的 PCM 写入原有大 FIFO（约 10.24 秒容量）。
2. 切到 LITE AFE 后，以有效人声启动首句检测；AFE 连续报告静音 160ms 后冻结首句边界。AFE 自身的静音判定时间仍然保留，所以 **160ms 不是从用户停嘴算起的总时间**。该边界是声学停顿，不是语义识别。
3. 边界之后的 PCM 写入独立的 512ms 滚动 FIFO。大 FIFO 中尚未补传的首句不被覆盖。
4. 上传任务按序处理完首句后读取实时 FIFO；旧实时帧可以淘汰，已进入 Opus 编码器或 TX 队列的采样不清空、不重排。无需等平台 ACK，也无需等首句的本地网络发送完成。
5. 如果首句尚未结束，但发送已经追到实时附近，继续沿用原有提前恢复实时的路径。

普通对话仍连续上传，打断继续由平台决定。两种路径复用已有的 AFE、编码任务和 PSRAM FIFO。

## 配置与日志

参数位于 `main/wake_word.h`：

- `WAKE_WORD_FIRST_PHRASE_HANDOFF_ENABLED=1`：启用本次首句到实时的切换。设为 `0` 并重新编译可恢复此前整段预录音追赶方式。
- `WAKE_WORD_CONNECT_PRE_ROLL_ENABLED=0`：关闭全部唤醒建连预录音，恢复鉴权后才向上传 FIFO 存入音频的行为；AFE 仍持续运行。
- `WAKE_WORD_FIRST_PHRASE_SILENCE_MS=160`：AFE 已报告静音后的额外确认时间。

预连接已就绪后，语音唤醒时可从以下串口日志读取实际耗时。`Wake to voice auth` 统计 WakeNet 唤醒事件到正式 ACK 的单调时间差；`buffered_ms` 表示启动上行时已录下多少语音。

```text
Standby WebSocket ready: lease_s=...
Activating standby WebSocket; waiting for __proxy_connected__
Wake to voice auth: ... ms (WakeNet event to formal ACK)
Hot pre-roll upload ready: boundary_sample=... buffered_ms=... dropped_new=...
```

冷连接也可观察首句切换。

```text
First phrase boundary captured; later audio uses live FIFO
First phrase queued; switched to live audio (skipped_ms=... live_ms=...)
```

`queued` 表示已交给本地编码上传流程，不能据此认定平台已经收到或识别。`skipped_ms` 是为恢复实时而跳过的旧实时音频。

## 验证范围

`scripts/check_audio.py` 覆盖首句/实时 FIFO 顺序、模式切换残余 VAD、短停顿、纯静音、溢出、采样坐标回绕、关闭开关，以及 512-sample AFE 帧与 320-sample Opus 帧在切换处的全部对齐方式。

烧录后等待串口显示 `Standby WebSocket ready`，然后连说「你好小智，今天天气怎么样」。核对正式 ACK 与启动上传的时间、平台收到的首句文字，以及网关只在本次唤醒时创建一条通话。可用触屏挂断，等设备重新预连接，再测一次。另在预连接未就绪时唤醒，验证重新建连也能收到完整问题。文档的 200ms 目标衡量唤醒至正式 ACK；用户听到首声还包括上行、模型响应与下行播放时间。
