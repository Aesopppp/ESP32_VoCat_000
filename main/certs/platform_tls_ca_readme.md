# 平台 HTTPS/WSS 信任根

`platform_tls_ca.pem` 用于平台 443 入口（`https://119.29.215.114` / `wss://119.29.215.114/v1/realtime`）。文件仅含标准自签 **ISRG Root X2**，没有私钥。

SHA-256：`69729b8e15a86efc177a57afb7171dfc64add28c2fca8cf1507e34453ccb1470`。

上游另附的 X1 交叉签名 X2 与自签版使用同一公钥；本工程只保留自签根，避免重复的信任路径。当前服务器证书链使用该根的无凭据 TLS 握手已在电脑上验证通过，主机名/IP 校验开启；仍需 ESP32 实机验证。

## 行为与性能边界

HTTP 和 WebSocket 客户端通过 `cert_pem` 使用此 PEM，禁用这两个客户端的 `crt_bundle_attach`。MQTT 保持独立的 `device_mqtt_ca.pem`。

使用较小的信任集合不会跳过叶证书、中间证书、有效期和主机名验证。当前链为叶证书 → YE1 → Root YE → X2，仍须验证三个 ECDSA P-384 签名。ESP-IDF 的全量 bundle 使用预生成数据和二分查找，并非每次完整解析约 140 张证书；不能据此保证只验签一次或把握手固定降到 0.3 秒。实际延迟应以同设备、同网络下的 HTTPS/WSS 日志比较。

此信任根会用于配置的所有平台 HTTPS/WSS 地址。平台改用其他根 CA 时，需要更新 PEM 并重新编译，或回退系统 bundle；失败时不会跳过证书校验。

## 更换与回退

1. 从可信渠道获得平台所用根 CA，核对指纹与 CA 约束，替换本 PEM。
2. 执行 `idf.py reconfigure` 和 `idf.py build`。
3. 先用一台设备验证 HTTPS 和 WSS，再部署其他设备。

构建前移走 `platform_tls_ca.pem`，重新 `reconfigure` 和 `build` 后回退到全量 CA bundle。该回退发生在编译期，不会在运行中因证书验证失败自动降低校验要求。

## token 缓存现状

本次上游 D3 在网络变化时保留 RAM 快照，但同时清除 `ready`，恢复网络后仍会安排 HTTP 握手；并未实现“有效 token 直接复用、免去一次 HTTPS”。旧快照不对客户端开放；新成功结果替换它，永久失败、显式刷新或续期时销毁。真正复用还需补齐有效期、鉴权拒绝、配置变更及旧请求隔离策略。
