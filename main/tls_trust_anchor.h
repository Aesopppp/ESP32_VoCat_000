#ifndef TLS_TRUST_ANCHOR_H
#define TLS_TRUST_ANCHOR_H

#include <stddef.h>

/* 平台 HTTPS/WSS 入口（119.29.215.114:443）使用的信任根。
 *
 * 返回以 NUL 结尾的 PEM 字符串；编译进固件的静态缓冲区，调用方不得释放。
 * 当前服务端证书是 Let's Encrypt IP shortlived profile（YE1 链，4 级，
 * 含 3 个 ECDSA P-384 签名）。esp_crt_bundle_attach 要解析 ~140 张 Mozilla CA，
 * 且在 240MHz 上做 3 次 P-384 验签 ≈ 2.9s/次握手；换成本文件里的 ISRG Root X2
 * 后只需 1 次验签（RSA 或 P-384，取决于服务端给出的交叉签名形态）。
 *
 * 如果平台换 CA，必须同步更新 main/certs/platform_tls_ca.pem 并重新编译，
 * 否则所有 wss / HTTPS 握手都会失败（不会静默跳过校验）。
 */
const char *tls_trust_anchor_ca_pem(void);

/* PEM 字节数（不含结尾 NUL 之外的额外数据）；诊断用。 */
size_t tls_trust_anchor_ca_pem_len(void);

#endif /* TLS_TRUST_ANCHOR_H */
