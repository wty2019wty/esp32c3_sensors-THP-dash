/*
 * TLS 信任锚（针对 Cloudflare + Google Trust Services）
 *
 * 现象：TCP 已通，但
 *   esp-x509-crt-bundle: No matching trusted root certificate found
 *   mbedtls_ssl_handshake returned -0x3000
 * 证书链：域名 → WE1 (GTS) → GTS Root R4
 * （线上还会带 GTS Root R4 的 GlobalSign 交叉签名）
 *
 * ESP-IDF 6.x 对 GTS/ECDSA 的捆绑包校验存在同类问题
 * （espressif/esp-idf#18674）。这里直接内嵌自签的 GTS Root R4
 * 作为 cert_pem 信任根，绕过捆绑包匹配失败。
 *
 * 来源：ESP-IDF cacrt_all.pem「GTS Root R4」（有效期 2016-2036）
 * 证书换发/换 CA 时需同步更新本文件。
 */
#pragma once

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
