#ifndef BL616CL_ESP32_COMPAT_MBEDTLS_SHA256_H_
#define BL616CL_ESP32_COMPAT_MBEDTLS_SHA256_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int mbedtls_sha256_ret(const unsigned char *input, size_t ilen,
                       unsigned char output[32], int is224);

#ifdef __cplusplus
}
#endif

#endif
