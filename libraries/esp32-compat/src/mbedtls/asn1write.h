#ifndef BL616CL_ESP32_COMPAT_MBEDTLS_ASN1WRITE_H_
#define BL616CL_ESP32_COMPAT_MBEDTLS_ASN1WRITE_H_

#include <stddef.h>
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

int mbedtls_asn1_write_len(unsigned char **p, unsigned char *start, size_t len);
int mbedtls_asn1_write_tag(unsigned char **p, unsigned char *start, unsigned char tag);
int mbedtls_asn1_write_mpi(unsigned char **p, unsigned char *start, const mbedtls_mpi *X);

#ifdef __cplusplus
}
#endif

#endif
