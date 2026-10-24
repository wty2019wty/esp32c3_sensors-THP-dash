/*
 * TLS 信任锚与信任模式
 *
 * 重要：ESP-IDF esp_http_client 中 crt_bundle_attach **优先于** cert_pem，
 * 两者同时设置时 cert_pem 会被忽略。因此信任模式必须二选一，不能叠写。
 *
 *   THP_TLS_TRUST_BUNDLE — 系统证书捆绑包（默认，覆盖公有 CA）
 *   THP_TLS_TRUST_PINNED — 仅用内嵌根证书 PEM（见 thp_tls_trust.c，可多段拼接）
 *   THP_TLS_TRUST_NONE   — 不校验（仅调试）
 *

 * 何时用 PINNED：bundle 报 "No matching trusted root certificate found"
 *   （espressif/esp-idf#18674），或握手 mbedtls -0x7200 / CONN_EOF 时。
 * 证书换发：根未变可不动；换根 CA 时把新根 PEM 追加到 thp_tls_trust.c 即可。
 *
 * 勿手写或臆造证书内容，只放真实根证书 PEM。
 */
#pragma once

#define THP_TLS_TRUST_BUNDLE 0
#define THP_TLS_TRUST_PINNED 1
#define THP_TLS_TRUST_NONE   2

/*
 * 信任模式由 thp_config.h 单独定义（唯一真相源），此处不再提供兜底默认值：
 * 写漏时直接编译失败，避免不同 include 顺序静默得到不同的 TLS 模式。
 */
#ifndef THP_TLS_TRUST
#error "未定义 THP_TLS_TRUST：请在 thp_config.h 中设为 0(BUNDLE) / 1(PINNED) / 2(NONE)"
#endif

/*
 * 多根信任锚（可追加更多真实根 PEM）：
 * 1) DigiCert Global Root G2 — 当前 TrustAsia 链的信任锚（2013-08-01 ~ 2038-01-15）
 * 2) GTS Root R4             — Google Trust Services（2016-06-22 ~ 2036-06-22）
 *
 * 实体定义在 thp_tls_trust.c，仅 PINNED 模式参与编译（BUNDLE/NONE 下不占 flash）。
 */
extern const char THP_TLS_ROOT_PEM[];
