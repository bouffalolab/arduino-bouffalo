#include <Arduino.h>

#include "WiFi.h"
#include "USB.h"
#include "USBHID.h"
#include "Arduino_DebugUtils.h"
#include "Update.h"
#include "utility/HCIVirtualTransport.h"
#include "esp_partition.h"
#include "mbedtls/pk.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/asn1.h"
#include "mbedtls/asn1write.h"
#include "mbedtls/sha256.h"
#include "mbedtls/pem.h"
#include "ping/ping_sock.h"

#include <string.h>

extern "C" {
#include "lwip/netdb.h"
#include "lwip/ip_addr.h"
#include "lwip/err.h"
#include "rfparam_adapter.h"
#include "bflb_gpio.h"
#include "bflb_uart.h"
}

Arduino_DebugUtils Debug;
UpdateClass Update;
HCIVirtualTransportClass HCIVirtualTransport;

/* Wireless platform init now lives in the Arduino core (cores/bl616cl/
 * bl_wireless.cpp, run from init() before the scheduler starts): RF
 * parameters, EM window and PHY RF bring-up.  Keep this as a lazy-path
 * wrapper for the WiFi and BLE callers; the core function is idempotent. */
extern "C" void bl_wireless_init(void);

extern "C" void ensure_rfparam(void)
{
    bl_wireless_init();
}

int WiFiGenericClass::hostByName(const char *hostname, IPAddress &address)
{
    if (hostname == nullptr) {
        return 0;
    }
    struct hostent *entry = lwip_gethostbyname(hostname);
    if (entry == nullptr || entry->h_addr_list == nullptr ||
        entry->h_addr_list[0] == nullptr) {
        return 0;
    }
    const ip_addr_t *addr = reinterpret_cast<const ip_addr_t *>(entry->h_addr_list[0]);
    address = IPAddress(ip4_addr_get_u32(ip_2_ip4(addr)));
    return 1;
}

extern "C" void usb_persist_restart(int mode)
{
    (void)mode;
}

extern "C" void bflb_uart_set_console(struct bflb_device_s *dev);

/* The USB device init chain clobbers the console UART binding.  Restore it
 * after USB.begin() so post-setup printf output remains visible on the
 * standard console (UART0, GPIO34/35 on bl616cldk). */
extern "C" void compat_console_restore(void)
{
    struct bflb_device_s *gpio = bflb_device_get_by_name("gpio");
    struct bflb_device_s *uart0 = bflb_device_get_by_name("uart0");
    struct bflb_uart_config_s cfg;
    if (gpio == NULL || uart0 == NULL) {
        return;
    }
    bflb_gpio_uart_init(gpio, GPIO_PIN_34, GPIO_UART_FUNC_UART0_TX);
    bflb_gpio_uart_init(gpio, GPIO_PIN_35, GPIO_UART_FUNC_UART0_RX);
    memset(&cfg, 0, sizeof(cfg));
    cfg.baudrate = 2000000;
    cfg.data_bits = UART_DATA_BITS_8;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.parity = UART_PARITY_NONE;
    cfg.flow_ctrl = 0;
    cfg.tx_fifo_threshold = 7;
    cfg.rx_fifo_threshold = 7;
    cfg.bit_order = UART_LSB_FIRST;
    bflb_uart_init(uart0, &cfg);
    bflb_uart_set_console(uart0);
}

/* Raw multi-byte write to the UART0 console for debug sinks that must not
 * touch the USB CDC (no CRLF mangling, no stdout involvement). */
extern "C" void compat_uart0_write(const char *s, size_t len)
{
    struct bflb_device_s *uart0 = bflb_device_get_by_name("uart0");
    if (uart0 == NULL || s == NULL) {
        return;
    }
    for (size_t i = 0; i < len; i++) {
        bflb_uart_putchar(uart0, s[i]);
    }
}

/* Probe: write straight to the UART0 peripheral, bypassing the stdout
 * redirect, to test the console path from the sketch. */
extern "C" void compat_uart0_raw_write(const char *s)
{
    struct bflb_device_s *uart0 = bflb_device_get_by_name("uart0");
    if (uart0 == NULL || s == NULL) {
        return;
    }
    while (*s) {
        bflb_uart_putchar(uart0, *s++);
    }
    bflb_uart_putchar(uart0, '\r');
    bflb_uart_putchar(uart0, '\n');
}

/* Do NOT define a freeaddrinfo() stub here.  lwip/netdb.h maps the POSIX
 * name to lwip_freeaddrinfo, so any such definition would interpose (and
 * silently replace) lwIP's real freeaddrinfo.  lwip_getaddrinfo() allocates
 * its result from the single-element MEMP_NETDB pool; a no-op stub leaks
 * that element and every later getaddrinfo() fails with EAI_MEMORY. */

extern "C" const esp_partition_t *esp_partition_find_first(uint32_t type,
                                                           uint32_t subtype,
                                                           const char *label)
{
    (void)type; (void)subtype; (void)label;
    return nullptr;
}

extern "C" int esp_partition_mmap(const esp_partition_t *partition, uint32_t offset,
                                  uint32_t size, uint32_t memory,
                                  const void **out_ptr,
                                  spi_flash_mmap_handle_t *out_handle)
{
    (void)partition; (void)offset; (void)size; (void)memory;
    (void)out_ptr; (void)out_handle;
    return -1;
}

extern "C" int esp_ping_new_session(const esp_ping_config_t *config,
                                    const esp_ping_callbacks_t *callbacks,
                                    esp_ping_handle_t *out_handle)
{
    (void)config; (void)callbacks; (void)out_handle;
    return ESP_FAIL;
}

extern "C" int esp_ping_start(esp_ping_handle_t handle)
{
    (void)handle;
    return ESP_FAIL;
}

extern "C" int esp_ping_stop(esp_ping_handle_t handle)
{
    (void)handle;
    return ESP_FAIL;
}

extern "C" void esp_ping_delete_session(esp_ping_handle_t handle)
{
    (void)handle;
}

extern "C" int esp_ping_get_profile(esp_ping_handle_t handle, int profile,
                                    void *data, uint32_t size)
{
    (void)handle; (void)profile; (void)data; (void)size;
    return ESP_FAIL;
}

/* mbedTLS v2-compat stubs for libraries that still call the old API.
 * Mark them weak so the real mbedTLS v3 implementations win whenever the
 * mbedTLS archive is pulled in (WiFiClientSecure links the real TLS stack).
 * Once the remaining ESP32 libraries are ported to v3 these go away. */
#define MBEDTLS_V2_STUB __attribute__((weak))

extern "C" MBEDTLS_V2_STUB void mbedtls_pk_init(mbedtls_pk_context *ctx)
{
    if (ctx != nullptr) {
        memset(ctx, 0, sizeof(*ctx));
    }
}

extern "C" MBEDTLS_V2_STUB void mbedtls_pk_free(mbedtls_pk_context *ctx)
{
    (void)ctx;
}

extern "C" MBEDTLS_V2_STUB const void *mbedtls_pk_info_from_type(mbedtls_pk_type_t type)
{
    (void)type;
    return nullptr;
}

extern "C" MBEDTLS_V2_STUB int mbedtls_pk_setup(mbedtls_pk_context *ctx, const void *info)
{
    (void)ctx; (void)info;
    return -1;
}

extern "C" MBEDTLS_V2_STUB mbedtls_pk_type_t mbedtls_pk_get_type(const mbedtls_pk_context *ctx)
{
    (void)ctx;
    return 0;
}

extern "C" MBEDTLS_V2_STUB int mbedtls_pk_parse_key(mbedtls_pk_context *ctx,
                                                   const unsigned char *key, size_t keylen,
                                                   const unsigned char *pwd, size_t pwdlen)
{
    (void)ctx; (void)key; (void)keylen; (void)pwd; (void)pwdlen;
    return -1;
}

extern "C" MBEDTLS_V2_STUB int mbedtls_pk_parse_public_key(mbedtls_pk_context *ctx,
                                                          const unsigned char *key, size_t keylen)
{
    (void)ctx; (void)key; (void)keylen;
    return -1;
}

extern "C" MBEDTLS_V2_STUB int mbedtls_pk_write_key_der(mbedtls_pk_context *ctx,
                                                       unsigned char *buf, size_t size)
{
    (void)ctx; (void)buf; (void)size;
    return -1;
}

extern "C" MBEDTLS_V2_STUB int mbedtls_pk_write_pubkey_der(mbedtls_pk_context *ctx,
                                                          unsigned char *buf, size_t size)
{
    (void)ctx; (void)buf; (void)size;
    return -1;
}

extern "C" MBEDTLS_V2_STUB int mbedtls_pk_sign(mbedtls_pk_context *ctx, int md_alg,
                                              const unsigned char *hash, size_t hash_len,
                                              unsigned char *sig, size_t *sig_len,
                                              int (*f_rng)(void *, unsigned char *, size_t), void *p_rng)
{
    (void)ctx; (void)md_alg; (void)hash; (void)hash_len;
    (void)sig; (void)sig_len; (void)f_rng; (void)p_rng;
    return -1;
}

extern "C" MBEDTLS_V2_STUB int mbedtls_pk_verify(mbedtls_pk_context *ctx, int md_alg,
                                                const unsigned char *hash, size_t hash_len,
                                                const unsigned char *sig, size_t sig_len)
{
    (void)ctx; (void)md_alg; (void)hash; (void)hash_len; (void)sig; (void)sig_len;
    return -1;
}

extern "C" MBEDTLS_V2_STUB int mbedtls_ecp_gen_key(mbedtls_ecp_group_id grp_id, mbedtls_ecp_keypair *key,
                                                  int (*f_rng)(void *, unsigned char *, size_t), void *p_rng)
{
    (void)grp_id; (void)key; (void)f_rng; (void)p_rng;
    return -1;
}

extern "C" MBEDTLS_V2_STUB int mbedtls_mpi_write_binary(const mbedtls_mpi *X, unsigned char *buf, size_t buflen)
{
    (void)X; (void)buf; (void)buflen;
    return -1;
}

extern "C" MBEDTLS_V2_STUB int mbedtls_ecp_group_load(mbedtls_ecp_group *grp, mbedtls_ecp_group_id id)
{
    (void)grp; (void)id;
    return -1;
}

extern "C" MBEDTLS_V2_STUB int mbedtls_ecp_point_read_binary(const mbedtls_ecp_group *grp,
                                                            mbedtls_ecp_point *P,
                                                            const unsigned char *buf, size_t ilen)
{
    (void)grp; (void)P; (void)buf; (void)ilen;
    return -1;
}

extern "C" MBEDTLS_V2_STUB void mbedtls_mpi_init(mbedtls_mpi *X)
{
    if (X != nullptr) {
        memset(X, 0, sizeof(*X));
    }
}

extern "C" MBEDTLS_V2_STUB int mbedtls_mpi_read_binary(mbedtls_mpi *X, const unsigned char *buf, size_t buflen)
{
    (void)X; (void)buf; (void)buflen;
    return -1;
}

extern "C" MBEDTLS_V2_STUB void mbedtls_ctr_drbg_init(mbedtls_ctr_drbg_context *ctx)
{
    if (ctx != nullptr) {
        memset(ctx, 0, sizeof(*ctx));
    }
}

extern "C" MBEDTLS_V2_STUB void mbedtls_ctr_drbg_free(mbedtls_ctr_drbg_context *ctx)
{
    (void)ctx;
}

extern "C" MBEDTLS_V2_STUB int mbedtls_ctr_drbg_seed(mbedtls_ctr_drbg_context *ctx,
                                                    int (*f_entropy)(void *, unsigned char *, size_t),
                                                    void *p_entropy, const unsigned char *custom, size_t len)
{
    (void)ctx; (void)f_entropy; (void)p_entropy; (void)custom; (void)len;
    return -1;
}

extern "C" MBEDTLS_V2_STUB int mbedtls_ctr_drbg_random(void *p_rng, unsigned char *output, size_t output_len)
{
    (void)p_rng; (void)output; (void)output_len;
    return -1;
}

extern "C" MBEDTLS_V2_STUB void mbedtls_entropy_init(mbedtls_entropy_context *ctx)
{
    if (ctx != nullptr) {
        memset(ctx, 0, sizeof(*ctx));
    }
}

extern "C" MBEDTLS_V2_STUB void mbedtls_entropy_free(mbedtls_entropy_context *ctx)
{
    (void)ctx;
}

extern "C" MBEDTLS_V2_STUB int mbedtls_entropy_func(void *data, unsigned char *output, size_t len)
{
    (void)data; (void)output; (void)len;
    return -1;
}

extern "C" MBEDTLS_V2_STUB int mbedtls_asn1_get_tag(unsigned char **p, const unsigned char *end,
                                                   size_t *len, int tag)
{
    (void)p; (void)end; (void)len; (void)tag;
    return -1;
}

extern "C" MBEDTLS_V2_STUB int mbedtls_asn1_write_len(unsigned char **p, unsigned char *start, size_t len)
{
    (void)p; (void)start; (void)len;
    return -1;
}

extern "C" MBEDTLS_V2_STUB int mbedtls_asn1_write_tag(unsigned char **p, unsigned char *start, unsigned char tag)
{
    (void)p; (void)start; (void)tag;
    return -1;
}

extern "C" MBEDTLS_V2_STUB int mbedtls_asn1_write_mpi(unsigned char **p, unsigned char *start, const mbedtls_mpi *X)
{
    (void)p; (void)start; (void)X;
    return -1;
}

extern "C" MBEDTLS_V2_STUB int mbedtls_sha256_ret(const unsigned char *input, size_t ilen,
                                                 unsigned char output[32], int is224)
{
    (void)input; (void)ilen; (void)output; (void)is224;
    return -1;
}

extern "C" MBEDTLS_V2_STUB int mbedtls_pem_write_buffer(const char *header, const char *footer,
                                                       const unsigned char *der_data, size_t der_len,
                                                       unsigned char *buf, size_t buf_len, size_t *olen)
{
    (void)header; (void)footer; (void)der_data; (void)der_len;
    (void)buf; (void)buf_len; (void)olen;
    return -1;
}
