#ifndef UNOR4_BL616CL_BRIDGE_MBEDTLS_P256_CONFIG_H
#define UNOR4_BL616CL_BRIDGE_MBEDTLS_P256_CONFIG_H

/*
 * This bridge needs P-256 client authentication while retaining the
 * previously validated ECDHE-RSA and ECDHE-ECDSA TLS server paths. A 512-byte
 * MPI limit supports RSA keys through 4096 bits, but avoids the SDK default
 * 8192-bit allocation ceiling. The ECP settings reduce the temporary point
 * tables used while the client signs CertificateVerify.
 */
#ifdef MBEDTLS_MPI_WINDOW_SIZE
#undef MBEDTLS_MPI_WINDOW_SIZE
#endif
#define MBEDTLS_MPI_WINDOW_SIZE 1

#ifdef MBEDTLS_MPI_MAX_SIZE
#undef MBEDTLS_MPI_MAX_SIZE
#endif
#define MBEDTLS_MPI_MAX_SIZE 512

#ifdef MBEDTLS_ECP_WINDOW_SIZE
#undef MBEDTLS_ECP_WINDOW_SIZE
#endif
#define MBEDTLS_ECP_WINDOW_SIZE 2

#ifdef MBEDTLS_ECP_FIXED_POINT_OPTIM
#undef MBEDTLS_ECP_FIXED_POINT_OPTIM
#endif
#define MBEDTLS_ECP_FIXED_POINT_OPTIM 0

/* Keep 2 KiB incoming certificate records; reduce the separate outgoing
 * record allocation so a recycled eighth TLS slot needs a smaller free block.
 */
#ifdef MBEDTLS_SSL_OUT_CONTENT_LEN
#undef MBEDTLS_SSL_OUT_CONTENT_LEN
#endif
#define MBEDTLS_SSL_OUT_CONTENT_LEN 1024

#endif
