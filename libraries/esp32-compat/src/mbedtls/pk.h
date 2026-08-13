#ifndef BL616CL_ESP32_COMPAT_MBEDTLS_PK_H_
#define BL616CL_ESP32_COMPAT_MBEDTLS_PK_H_

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

void mbedtls_pk_init(mbedtls_pk_context *ctx);
void mbedtls_pk_free(mbedtls_pk_context *ctx);
const void *mbedtls_pk_info_from_type(mbedtls_pk_type_t type);
int mbedtls_pk_setup(mbedtls_pk_context *ctx, const void *info);
mbedtls_pk_type_t mbedtls_pk_get_type(const mbedtls_pk_context *ctx);
#define mbedtls_pk_ec(key) ((mbedtls_ecp_keypair *)((key).pk_ctx))
int mbedtls_pk_parse_key(mbedtls_pk_context *ctx, const unsigned char *key,
                         size_t keylen, const unsigned char *pwd, size_t pwdlen);
int mbedtls_pk_parse_public_key(mbedtls_pk_context *ctx,
                                const unsigned char *key, size_t keylen);
int mbedtls_pk_write_key_der(mbedtls_pk_context *ctx, unsigned char *buf, size_t size);
int mbedtls_pk_write_pubkey_der(mbedtls_pk_context *ctx, unsigned char *buf, size_t size);
int mbedtls_pk_sign(mbedtls_pk_context *ctx, int md_alg,
                    const unsigned char *hash, size_t hash_len,
                    unsigned char *sig, size_t *sig_len,
                    int (*f_rng)(void *, unsigned char *, size_t), void *p_rng);
int mbedtls_pk_verify(mbedtls_pk_context *ctx, int md_alg,
                      const unsigned char *hash, size_t hash_len,
                      const unsigned char *sig, size_t sig_len);

int mbedtls_ecp_gen_key(mbedtls_ecp_group_id grp_id, mbedtls_ecp_keypair *key,
                        int (*f_rng)(void *, unsigned char *, size_t), void *p_rng);
int mbedtls_mpi_write_binary(const mbedtls_mpi *X, unsigned char *buf, size_t buflen);
int mbedtls_ecp_group_load(mbedtls_ecp_group *grp, mbedtls_ecp_group_id id);
int mbedtls_ecp_point_read_binary(const mbedtls_ecp_group *grp,
                                  mbedtls_ecp_point *P,
                                  const unsigned char *buf, size_t ilen);
void mbedtls_mpi_init(mbedtls_mpi *X);
int mbedtls_mpi_read_binary(mbedtls_mpi *X, const unsigned char *buf, size_t buflen);

#ifdef __cplusplus
}
#endif

#endif
