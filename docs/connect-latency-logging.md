# 唤醒时延统一打点与导出

## 设备日志

工单 01 在设备固件中输出独立的 `LAT` 行，格式固定为：

```text
YYYY-MM-DDTHH:MM:SS.mmmZ LAT session=<wake-session-id> event=<event> monotonic_ms=<ms> [room=<gw-room>] [detail=<text>]
```

事件含义：

| 事件 | 含义 |
|---|---|
| `wake-requested` | 唤醒检测任务确认“你好小智”，主任务开始建连 |
| `ws-before-tcp-connect` | WebSocket transport 即将开始 TCP/TLS |
| `ws-transport-connected` | TLS 与 WS 升级完成，准备发鉴权 |
| `ws-tls-and-handshake-done` | 主任务收到 WS connected，鉴权开始 |
| `proxy-connected` | 网关确认代理已连接 |
| `audio-upload-ready` | 设备可上行音频 |
| `pre-roll-ready` | 鉴权封存预录音，detail 带 buffered_ms/dropped_ms |
| `first-live-uplink` | 第一包实时音频成功发送 |
| `agent-first-audio` | 设备收到智能体首段音频/响应事件 |
| `ws-dns-warmed` | 待机阶段完成 WebSocket 域名预解析（`detail=standby`，不属于唤醒会话） |

`session` 由启动随机种子和唤醒时刻单调毫秒组成，一次唤醒内稳定；
网关房间号目前没有通过 ACOS 协议下发，设备侧不能凭空关联。
排障时可在拿到平台话单后，用 `python scripts/latency_assign_room.py <device.log> <gw-room>` 给同一次唤醒补写 `room=` 字段，再运行汇总。

## 汇总

```bash
python scripts/latency_summary.py <device.log> --json
python scripts/latency_summary.py <device.log>
```

输出固定包含三段耗时：

- `wake_to_ws_transport_connected`
- `auth_to_first_live_uplink`
- `speech_stopped_to_agent_first_audio`

ISO 时间来自设备 UTC 时钟；若设备时钟偏差大，可先按网关日志中同一话单的绝对时间校准。
`monotonic_ms` 是 ESP32 单调时钟，优先用于分段计算，避免设备校时跳变污染耗时。


