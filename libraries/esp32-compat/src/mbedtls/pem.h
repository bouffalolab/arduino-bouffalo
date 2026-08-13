#ifndef BL616CL_ESP32_COMPAT_MBEDTLS_PEM_H_
#define BL616CL_ESP32_COMPAT_MBEDTLS_PEM_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int mbedtls_pem_write_buffer(const char *header, const char *footer,
                             const unsigned char *der_data, size_t der_len,
                             unsigned char *buf, size_t buf_len, size_t *olen);

#ifdef __cplusplus
}
#endif

#endif
