#ifndef BL616CL_ESP32_COMPAT_MBEDTLS_ENTROPY_H_
#define BL616CL_ESP32_COMPAT_MBEDTLS_ENTROPY_H_

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

void mbedtls_entropy_init(mbedtls_entropy_context *ctx);
void mbedtls_entropy_free(mbedtls_entropy_context *ctx);

#ifdef __cplusplus
}
#endif

#endif
