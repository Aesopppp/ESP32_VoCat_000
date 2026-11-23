# 平台 HTTPS/WSS 信任根

`platform_tls_ca.pem` 是平台 443 入口（`https://119.29.215.114` /
`wss://119.29.215.114/v1/realtime`）使用的信任根，**只有公开根证书，无私钥**。

当前内容包含两个 ISRG Root X2：

1. 自签版（浏览器/操作系统信任库里的版本，ECDSA P-384 签名）
2. 交叉签名版（由 ISRG Root X1 用 RSA 签名，服务端实际下发的链顶版本）

两者都放进去是为了 mbedTLS 不论服务端下发哪一种链形态都能匹配到信任锚点。

## 为什么不用 `esp_crt_bundle_attach`

平台的 Let's Encrypt IP 短周期证书是 4 级链，含 3 个 ECDSA P-384 签名。
ESP32-S3 在 240MHz 上用 mbedTLS 软件验一次 P-384 ≈ **0.91s**，每次 TLS
握手光验链就要 ~2.9s；`esp_crt_bundle_attach` 还要先解析 ~140 张 Mozilla CA。
设备一次唤醒要建**两次** TLS（HTTPS 握手拿 token + WS 连接），
实测这 tandem 就要 **6.4s + 2.7s**（`gw-4423739` 抓包）。

换成这里的两张 X2 根后，验证只用到链顶的 1 次签名（RSA 或 P-384），
且不用扫描整包 CA。

## 更换流程

平台如果换 CA（不是 Let's Encrypt，或改成别的根），必须：

1. 从平台服务端取新链的**根证书**（PEM），替换本文件；
2. `idf.py reconfigure && idf.py build`，烧录设备；
3. 先用一台设备验证握手成功、再灰度，不能只靠抓包判断。

⚠️ 根证书不匹配会让 HTTPS 握手和 WS 连接**直接失败**（不会静默降级），
这是有意为之——绝不为省时间关掉证书校验。

如果临时需要回退到原来的全量 CA bundle，删掉 `platform_tls_ca.pem`
后重新 `idf.py reconfigure && idf.py build` 即可（会打印 WARNING）。
