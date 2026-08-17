#include "lwip_compat_sockets.h"

#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>

#include "WiFiClientSecure.h"

#include <string.h>

WiFiClientSecure::WiFiClientSecure()
    : ssl_(nullptr), ssl_config_(nullptr), ca_cert_(nullptr),
      client_cert_(nullptr), pk_key_(nullptr), ctr_drbg_(nullptr),
      entropy_(nullptr), ca_pem_(nullptr), ca_bundle_(nullptr),
      cert_pem_(nullptr), key_pem_(nullptr), insecure_(false),
      handshake_done_(false), tls_setup_(false), last_error_(0), peek_buf_(0),
      peek_len_(0)
{
    ssl_ = new mbedtls_ssl_context;
    ssl_config_ = new mbedtls_ssl_config;
    ca_cert_ = new mbedtls_x509_crt;
    client_cert_ = new mbedtls_x509_crt;
    pk_key_ = new mbedtls_pk_context;
    ctr_drbg_ = new mbedtls_ctr_drbg_context;
    entropy_ = new mbedtls_entropy_context;

    mbedtls_ssl_init(static_cast<mbedtls_ssl_context *>(ssl_));
    mbedtls_ssl_config_init(static_cast<mbedtls_ssl_config *>(ssl_config_));
    mbedtls_x509_crt_init(static_cast<mbedtls_x509_crt *>(ca_cert_));
    mbedtls_x509_crt_init(static_cast<mbedtls_x509_crt *>(client_cert_));
    mbedtls_pk_init(static_cast<mbedtls_pk_context *>(pk_key_));
    mbedtls_ctr_drbg_init(static_cast<mbedtls_ctr_drbg_context *>(ctr_drbg_));
    mbedtls_entropy_init(static_cast<mbedtls_entropy_context *>(entropy_));
}

WiFiClientSecure::~WiFiClientSecure()
{
    stop();
    if (tls_setup_) {
        mbedtls_ssl_free(static_cast<mbedtls_ssl_context *>(ssl_));
    }
    mbedtls_ssl_config_free(static_cast<mbedtls_ssl_config *>(ssl_config_));
    mbedtls_x509_crt_free(static_cast<mbedtls_x509_crt *>(ca_cert_));
    mbedtls_x509_crt_free(static_cast<mbedtls_x509_crt *>(client_cert_));
    mbedtls_pk_free(static_cast<mbedtls_pk_context *>(pk_key_));
    mbedtls_ctr_drbg_free(static_cast<mbedtls_ctr_drbg_context *>(ctr_drbg_));
    mbedtls_entropy_free(static_cast<mbedtls_entropy_context *>(entropy_));
    delete static_cast<mbedtls_ssl_context *>(ssl_);
    delete static_cast<mbedtls_ssl_config *>(ssl_config_);
    delete static_cast<mbedtls_x509_crt *>(ca_cert_);
    delete static_cast<mbedtls_x509_crt *>(client_cert_);
    delete static_cast<mbedtls_pk_context *>(pk_key_);
    delete static_cast<mbedtls_ctr_drbg_context *>(ctr_drbg_);
    delete static_cast<mbedtls_entropy_context *>(entropy_);
}

void WiFiClientSecure::setCACert(const char *cert)
{
    ca_pem_ = cert;
    ca_bundle_ = nullptr;
}

void WiFiClientSecure::setCACertBundle(const uint8_t *bundle)
{
    ca_bundle_ = bundle;
    ca_pem_ = nullptr;
}

void WiFiClientSecure::setCertificate(const char *cert)
{
    cert_pem_ = cert;
}

void WiFiClientSecure::setPrivateKey(const char *key)
{
    key_pem_ = key;
}

void WiFiClientSecure::setInsecure()
{
    insecure_ = true;
}

int WiFiClientSecure::tls_send(void *ctx, const unsigned char *buf, size_t len)
{
    WiFiClientSecure *self = static_cast<WiFiClientSecure *>(ctx);
    int ret = static_cast<int>(lwip_send(self->sockfd_, buf, len, 0));
    if (ret < 0) {
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    }
    return ret;
}

int WiFiClientSecure::tls_recv(void *ctx, unsigned char *buf, size_t len)
{
    WiFiClientSecure *self = static_cast<WiFiClientSecure *>(ctx);
    int ret = static_cast<int>(lwip_recv(self->sockfd_, buf, len, 0));
    if (ret < 0) {
        return MBEDTLS_ERR_SSL_WANT_READ;
    }
    if (ret == 0) {
        return MBEDTLS_ERR_SSL_CONN_EOF;
    }
    return ret;
}

int WiFiClientSecure::do_tls_handshake(const char *hostname)
{
    mbedtls_ssl_context *ssl = static_cast<mbedtls_ssl_context *>(ssl_);
    mbedtls_ssl_config *conf = static_cast<mbedtls_ssl_config *>(ssl_config_);
    mbedtls_x509_crt *ca = static_cast<mbedtls_x509_crt *>(ca_cert_);
    mbedtls_x509_crt *crt = static_cast<mbedtls_x509_crt *>(client_cert_);
    mbedtls_pk_context *key = static_cast<mbedtls_pk_context *>(pk_key_);
    mbedtls_ctr_drbg_context *drbg = static_cast<mbedtls_ctr_drbg_context *>(ctr_drbg_);
    mbedtls_entropy_context *entropy =
        static_cast<mbedtls_entropy_context *>(entropy_);

    int seed_ret = mbedtls_ctr_drbg_seed(
        drbg, mbedtls_entropy_func, entropy,
        reinterpret_cast<const unsigned char *>("wifi"), 4);
    if (seed_ret != 0) {
        last_error_ = seed_ret;
        return 0;
    }

    int cfg_ret = mbedtls_ssl_config_defaults(
        conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
        MBEDTLS_SSL_PRESET_DEFAULT);
    if (cfg_ret != 0) {
        last_error_ = cfg_ret;
        return 0;
    }
    mbedtls_ssl_conf_rng(conf, mbedtls_ctr_drbg_random, drbg);
    /* The hardware ECC accelerator only implements secp256r1. */
    static const mbedtls_ecp_group_id curves[] = {
        MBEDTLS_ECP_DP_SECP256R1, MBEDTLS_ECP_DP_NONE
    };
    mbedtls_ssl_conf_curves(conf, curves);
    if (insecure_) {
        mbedtls_ssl_conf_authmode(conf, MBEDTLS_SSL_VERIFY_NONE);
    } else {
        mbedtls_ssl_conf_authmode(conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    }

    const unsigned char *ca_data = ca_pem_ != nullptr
                                       ? reinterpret_cast<const unsigned char *>(ca_pem_)
                                       : ca_bundle_;
    size_t ca_len = ca_data != nullptr ? strlen(reinterpret_cast<const char *>(ca_data)) + 1
                                       : 0;
    if (ca_data != nullptr && ca_len > 1) {
        int ret = mbedtls_x509_crt_parse(ca, ca_data, ca_len);
        if (ret < 0) {
            last_error_ = ret;
            return 0;
        }
        mbedtls_ssl_conf_ca_chain(conf, ca, nullptr);
    } else if (!insecure_) {
        /* verification requested but no CA was configured */
        last_error_ = MBEDTLS_ERR_X509_BAD_INPUT_DATA;
        return 0;
    }

    if (cert_pem_ != nullptr && key_pem_ != nullptr) {
        int ret = mbedtls_x509_crt_parse(
            crt, reinterpret_cast<const unsigned char *>(cert_pem_),
            strlen(cert_pem_) + 1);
        if (ret == 0) {
            ret = mbedtls_pk_parse_key(
                key, reinterpret_cast<const unsigned char *>(key_pem_),
                strlen(key_pem_) + 1, nullptr, 0, mbedtls_ctr_drbg_random, drbg);
        }
        if (ret != 0) {
            last_error_ = ret;
            return 0;
        }
        mbedtls_ssl_conf_own_cert(conf, crt, key);
    }

    int setup_ret = mbedtls_ssl_setup(ssl, conf);
    if (setup_ret != 0) {
        last_error_ = setup_ret;
        return 0;
    }
    tls_setup_ = true;
    if (hostname != nullptr && hostname[0] != '\0') {
        int ret = mbedtls_ssl_set_hostname(ssl, hostname);
        if (ret != 0) {
            last_error_ = ret;
            return 0;
        }
    }
    mbedtls_ssl_set_bio(ssl, this, tls_send, tls_recv, nullptr);

    int ret;
    do {
        ret = mbedtls_ssl_handshake(ssl);
    } while (ret == MBEDTLS_ERR_SSL_WANT_READ ||
             ret == MBEDTLS_ERR_SSL_WANT_WRITE);
    if (ret != 0) {
        last_error_ = ret;
        return 0;
    }
    if (!insecure_) {
        uint32_t flags = mbedtls_ssl_get_verify_result(ssl);
        if (flags != 0) {
            last_error_ = static_cast<int>(flags);
            return 0;
        }
    }
    handshake_done_ = true;
    return 1;
}

int WiFiClientSecure::connect(IPAddress ip, uint16_t port)
{
    return connect(ip, port, WIFI_CLIENT_DEF_CONN_TIMEOUT_MS);
}

int WiFiClientSecure::connect(const char *host, uint16_t port)
{
    return connect(host, port, WIFI_CLIENT_DEF_CONN_TIMEOUT_MS);
}

int WiFiClientSecure::connect(IPAddress ip, uint16_t port, int32_t timeout)
{
    stop();
    if (!WiFiClient::connect(ip, port, timeout)) {
        return 0;
    }
    return do_tls_handshake(nullptr);
}

int WiFiClientSecure::connect(const char *host, uint16_t port, int32_t timeout)
{
    stop();
    if (!WiFiClient::connect(host, port, timeout)) {
        return 0;
    }
    return do_tls_handshake(host);
}

int WiFiClientSecure::available()
{
    mbedtls_ssl_context *ssl = static_cast<mbedtls_ssl_context *>(ssl_);
    if (!handshake_done_ || sockfd_ < 0) {
        return 0;
    }
    fd_set read_set;
    struct timeval tv = {0, 0};
    FD_ZERO(&read_set);
    FD_SET(sockfd_, &read_set);
    size_t pending = mbedtls_ssl_get_bytes_avail(ssl);
    if (lwip_select(sockfd_ + 1, &read_set, nullptr, nullptr, &tv) > 0) {
        return 1;
    }
    return static_cast<int>(pending);
}

int WiFiClientSecure::read()
{
    uint8_t value = 0;
    int ret = read(&value, 1);
    return ret == 1 ? value : -1;
}

int WiFiClientSecure::read(uint8_t *buffer, size_t size)
{
    mbedtls_ssl_context *ssl = static_cast<mbedtls_ssl_context *>(ssl_);
    if (buffer == nullptr || size == 0 || !handshake_done_) {
        return -1;
    }
    if (peek_len_ > 0 && size > 0) {
        buffer[0] = peek_buf_;
        peek_len_ = 0;
        if (size == 1) {
            return 1;
        }
        int ret = mbedtls_ssl_read(ssl, buffer + 1, size - 1);
        if (ret <= 0) {
            return 1;
        }
        return ret + 1;
    }
    int ret;
    do {
        ret = mbedtls_ssl_read(ssl, buffer, size);
    } while (ret == MBEDTLS_ERR_SSL_WANT_READ ||
             ret == MBEDTLS_ERR_SSL_WANT_WRITE);
    if (ret <= 0) {
        last_error_ = ret;
        if (ret == MBEDTLS_ERR_SSL_CONN_EOF ||
            ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            stop();
        }
        return -1;
    }
    return ret;
}

int WiFiClientSecure::peek()
{
    if (peek_len_ > 0) {
        return peek_buf_;
    }
    mbedtls_ssl_context *ssl = static_cast<mbedtls_ssl_context *>(ssl_);
    if (!handshake_done_) {
        return -1;
    }
    int ret;
    do {
        ret = mbedtls_ssl_read(ssl, &peek_buf_, 1);
    } while (ret == MBEDTLS_ERR_SSL_WANT_READ ||
             ret == MBEDTLS_ERR_SSL_WANT_WRITE);
    if (ret == 1) {
        peek_len_ = 1;
        return peek_buf_;
    }
    return -1;
}

size_t WiFiClientSecure::write(uint8_t value)
{
    return write(&value, 1);
}

size_t WiFiClientSecure::write(const uint8_t *buffer, size_t size)
{
    mbedtls_ssl_context *ssl = static_cast<mbedtls_ssl_context *>(ssl_);
    if (buffer == nullptr || size == 0 || !handshake_done_) {
        return 0;
    }
    size_t sent = 0;
    while (sent < size) {
        int ret = mbedtls_ssl_write(ssl, buffer + sent, size - sent);
        if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
            ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            continue;
        }
        if (ret <= 0) {
            last_error_ = ret;
            break;
        }
        sent += static_cast<size_t>(ret);
    }
    return sent;
}

void WiFiClientSecure::flush()
{
}

void WiFiClientSecure::stop()
{
    mbedtls_ssl_context *ssl = static_cast<mbedtls_ssl_context *>(ssl_);
    if (tls_setup_) {
        if (handshake_done_) {
            mbedtls_ssl_close_notify(ssl);
        }
        mbedtls_ssl_session_reset(ssl);
        tls_setup_ = false;
    }
    handshake_done_ = false;
    peek_len_ = 0;
    last_error_ = 0;
    WiFiClient::stop();
}

uint8_t WiFiClientSecure::connected()
{
    if (!handshake_done_) {
        return 0;
    }
    return WiFiClient::connected();
}

int WiFiClientSecure::lastError(char *buf, size_t size)
{
    if (buf == nullptr || size == 0) {
        return 0;
    }
    /* MBEDTLS_ERROR_C is disabled in the SDK build, so format the code. */
    snprintf(buf, size, "tls error -0x%04x", static_cast<unsigned int>(-last_error_));
    return static_cast<int>(last_error_);
}
