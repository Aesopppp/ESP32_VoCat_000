# 连接时延打点日志（LAT）

固件在唤醒→可说话的关键路径上打了单调时间戳，统一 TAG `LAT`，
格式固定 `LAT: <event> t=<毫秒>`（`esp_timer_get_time()`，上电起单调）。
**纯日志**：不改变状态、不增加任务、不发网络报文。

## 抓日志

串口 115200，过滤：

```
grep " LAT: " device.log
```

## 事件顺序与各段含义

一次"唤醒→说话"的完整序列（有 HTTP 握手时）：

| 事件 | 含义 |
|---|---|
| `wake-requested` | 用户说"你好小智"/按键，开始建会话 |
| `ws-connect-scheduled` | 决定要建 WS 连接（此路无已连接会话） |
| `http-handshake-start` | HTTP worker 真正开始发 HTTPS 请求（拿 token） |
| `http-handshake-finished` | HTTPS 请求完成（响应读完） |
| `http-handshake-result-dequeued` | 主任务拿到结果（若与上一行差很多→主任务被抢占） |
| `credentials-ready` | 凭据校验通过，主任务准备调 WS connect |
| `ws-service-connect-enter` | 进入 `ws_service_connect()` |
| `ws-before-tcp-connect` | esp-websocket 已起任务、准备 TCP connect |
| `ws-tls-and-handshake-done` | TCP+TLS+HTTP 101 全完成（WS 事件 CONNECTED） |
| `ws-proxy-connected` | 网关回 `__proxy_connected__` |
| `ev-ws-auth-ok` | app_main 收到 EV_WS_AUTH_OK |
| `audio-upload-ready` | 麦克风/FIFO 就绪，用户可以说话 |

## 如何算各段耗时

把相邻两行的 `t=` 相减即可。重点看：

1. `http-handshake-finished` → `credentials-ready`：
   之前抓包看到 **4.5s 设备空等**就在这里。如果这一行很大，说明设备
   收到 HTTP 响应后没有立即去连 WS（任务调度/其他阻塞）。
2. `http-handshake-start` → `http-handshake-finished`：HTTPS 握手+请求耗时。
   D1（自定义 CA）后应从 ~6.4s 降到 <1s。
3. `ws-before-tcp-connect` → `ws-tls-and-handshake-done`：WS 的 TLS 握手耗时。
   D1 后应从 ~2.7s 降到 <0.5s。
4. `ws-tls-and-handshake-done` → `ws-proxy-connected`：auth 往返。
5. `wake-requested` → `audio-upload-ready`：用户感知的总时延。

## 与网关日志对齐

网关的 `开始建立会话` 时刻（UTC）对应设备 `ws-proxy-connected` 之前一点
（网关 auth 通过 → 回 ACK → `ws-proxy-connected`）。用一通电话的近似对齐点
把设备的 `t=` 映射成 UTC，就能与网关 session 日志逐段比对。
