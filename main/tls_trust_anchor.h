#ifndef TLS_TRUST_ANCHOR_H
#define TLS_TRUST_ANCHOR_H

#include <stddef.h>

/* 平台 HTTPS/WSS 入口（119.29.215.114:443）使用的信任根。
 *
 * 返回以 NUL 结尾的 PEM 字符串；编译进固件的静态缓冲区，调用方不得释放。
 * 使用标准自签 ISRG Root X2；仍需验证叶证书和中间证书至该根的签名链，
 * 不跳过有效期或主机名校验。实际握手耗时取决于链、网络和设备负载。
 *
 * 如果平台换 CA，必须同步更新 main/certs/platform_tls_ca.pem 并重新编译，
 * 否则所有 wss / HTTPS 握手都会失败（不会静默跳过校验）。
 */
const char *tls_trust_anchor_ca_pem(void);

/* PEM 字节数（不含结尾 NUL 之外的额外数据）；诊断用。 */
size_t tls_trust_anchor_ca_pem_len(void);

#endif /* TLS_TRUST_ANCHOR_H */
