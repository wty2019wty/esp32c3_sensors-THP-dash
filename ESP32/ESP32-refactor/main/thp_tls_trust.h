/*
 * TLS 信任锚与信任模式
 *
 * 重要：ESP-IDF esp_http_client 中 crt_bundle_attach **优先于** cert_pem，
 * 两者同时设置时 cert_pem 会被忽略。因此信任模式必须二选一，不能叠写。
 *
 *   THP_TLS_TRUST_BUNDLE — 系统证书捆绑包（默认，覆盖公有 CA）
 *   THP_TLS_TRUST_PINNED — 仅用下方内嵌根证书 PEM
 *   THP_TLS_TRUST_NONE   — 不校验（仅调试）
 *
 * 何时改 PINNED：线上证书链经 GTS/ECDSA 时，若 esp-x509-crt-bundle 报
 *   "No matching trusted root certificate found"（espressif/esp-idf#18674），
 *   可改为 PINNED，信任下方内嵌根。证书换发/换 CA 时需同步更新本文件。
 *
 * PINNED 模式可在下方继续追加**真实**根证书 PEM（例如 ISRG Root X1），
 * 勿手写或臆造证书内容。
 */
#pragma once

#define THP_TLS_TRUST_BUNDLE 0
#define THP_TLS_TRUST_PINNED 1
#define THP_TLS_TRUST_NONE   2

#ifndef THP_TLS_TRUST
#define THP_TLS_TRUST THP_TLS_TRUST_BUNDLE
#endif

/* GTS Root R4（ESP-IDF cacrt_all.pem，有效期 2016-2036） */
static const char THP_TLS_ROOT_PEM[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIICCTCCAY6gAwIBAgINAgPlwGjvYxqccpBQUjAKBggqhkjOPQQDAzBHMQswCQYDVQQGEwJVUzEi\n"
    "MCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjQw\n"
    "HhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAwMDAwWjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZ\n"
    "R29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjQwdjAQBgcqhkjO\n"
    "PQIBBgUrgQQAIgNiAATzdHOnaItgrkO4NcWBMHtLSZ37wWHO5t5GvWvVYRg1rkDdc/eJkTBa6zzu\n"
    "hXyiQHY7qca4R9gq55KRanPpsXI5nymfopjTX15YhmUPoYRlBtHci8nHc8iMai/lxKvRHYqjQjBA\n"
    "MA4GA1UdDwEB/wQEAwIBhjAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQWBBSATNbrdP9JNqPV2Py1\n"
    "PsVq8JQdjDAKBggqhkjOPQQDAwNpADBmAjEA6ED/g94D9J+uHXqnLrmvT/aDHQ4thQEd0dlq7A/C\n"
    "r8deVl5c1RxYIigL9zC2L7F8AjEA8GE8p/SgguMh1YQdc4acLa/KNJvxn7kjNuK8YAOdgLOaVsjh\n"
    "4rsUecrNIdSUtUlD\n"
    "-----END CERTIFICATE-----\n";
