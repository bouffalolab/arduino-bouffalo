#ifndef BL616CL_ESP32_COMPAT_MBEDTLS_CTR_DRBG_H_
#define BL616CL_ESP32_COMPAT_MBEDTLS_CTR_DRBG_H_

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

void mbedtls_ctr_drbg_init(mbedtls_ctr_drbg_context *ctx);
void mbedtls_ctr_drbg_free(mbedtls_ctr_drbg_context *ctx);
int mbedtls_ctr_drbg_seed(mbedtls_ctr_drbg_context *ctx,
                          int (*f_entropy)(void *, unsigned char *, size_t),
                          void *p_entropy, const unsigned char *custom, size_t len);
int mbedtls_ctr_drbg_random(void *p_rng, unsigned char *output, size_t output_len);

#ifdef __cplusplus
}
#endif

#endif
