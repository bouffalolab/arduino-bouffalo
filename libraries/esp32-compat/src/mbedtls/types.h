#ifndef BL616CL_ESP32_COMPAT_MBEDTLS_TYPES_H_
#define BL616CL_ESP32_COMPAT_MBEDTLS_TYPES_H_

#include <stddef.h>
#include <stdint.h>

#define MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED (-0x006E)
#define MBEDTLS_ERR_ASN1_LENGTH_MISMATCH (-0x0064)
#define MBEDTLS_ERR_ECP_BUFFER_TOO_SMALL (-0x4F80)

#define MBEDTLS_ASN1_INTEGER 0x02
#define MBEDTLS_ASN1_SEQUENCE 0x30
#define MBEDTLS_ASN1_CONSTRUCTED 0x20
#define MBEDTLS_ASN1_CHK_ADD(len, op) do { (len) += (op); } while (0)

#define MBEDTLS_ECDSA_MAX_LEN 72
#define MBEDTLS_PK_SIGNATURE_MAX_SIZE 512

#define MBEDTLS_MD_SHA256 4
#define MBEDTLS_PK_ECKEY 2
#define MBEDTLS_ECP_DP_SECP256R1 23

typedef int mbedtls_pk_type_t;
typedef int mbedtls_ecp_group_id;

typedef struct mbedtls_mpi {
    int s;
    size_t n;
    uint32_t *p;
} mbedtls_mpi;

typedef struct mbedtls_ecp_point {
    mbedtls_mpi X;
    mbedtls_mpi Y;
    mbedtls_mpi Z;
} mbedtls_ecp_point;

typedef struct mbedtls_ecp_group {
    mbedtls_ecp_group_id id;
} mbedtls_ecp_group;

typedef struct mbedtls_ecp_keypair {
    mbedtls_ecp_group grp;
    mbedtls_mpi d;
    mbedtls_ecp_point Q;
} mbedtls_ecp_keypair;

typedef struct mbedtls_pk_context {
    void *pk_ctx;
    const void *pk_info;
} mbedtls_pk_context;

typedef struct mbedtls_ctr_drbg_context {
    int dummy;
} mbedtls_ctr_drbg_context;

typedef struct mbedtls_entropy_context {
    int dummy;
} mbedtls_entropy_context;

#ifdef __cplusplus
extern "C" {
#endif

int mbedtls_entropy_func(void *data, unsigned char *output, size_t len);

#ifdef __cplusplus
}
#endif

#endif
