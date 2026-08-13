#ifndef BL616CL_ESP32_COMPAT_MBEDTLS_ASN1_H_
#define BL616CL_ESP32_COMPAT_MBEDTLS_ASN1_H_

#include <stddef.h>
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

int mbedtls_asn1_get_tag(unsigned char **p, const unsigned char *end,
                         size_t *len, int tag);

#ifdef __cplusplus
}
#endif

#endif
