#ifndef BL616CL_ESP32_COMPAT_WIFICLIENTSECURE_H_
#define BL616CL_ESP32_COMPAT_WIFICLIENTSECURE_H_

#include "WiFiClient.h"

class WiFiClientSecure : public WiFiClient {
public:
    WiFiClientSecure();
    virtual ~WiFiClientSecure();

    void setCACert(const char *cert);
    void setCACertBundle(const uint8_t *bundle);
    void setCertificate(const char *cert);
    void setPrivateKey(const char *key);
    void setInsecure();

    virtual int connect(IPAddress ip, uint16_t port) override;
    virtual int connect(const char *host, uint16_t port) override;
    virtual int connect(IPAddress ip, uint16_t port, int32_t timeout) override;
    virtual int connect(const char *host, uint16_t port, int32_t timeout) override;

    virtual int available() override;
    virtual int read() override;
    virtual int read(uint8_t *buffer, size_t size) override;
    virtual int peek() override;
    virtual size_t write(uint8_t value) override;
    virtual size_t write(const uint8_t *buffer, size_t size) override;
    virtual void flush() override;
    virtual void stop() override;
    virtual uint8_t connected() override;

    int lastError(char *buf, size_t size);

private:
    void *ssl_;
    void *ssl_config_;
    void *ca_cert_;
    void *client_cert_;
    void *pk_key_;
    void *ctr_drbg_;
    void *entropy_;
    const char *ca_pem_;
    const uint8_t *ca_bundle_;
    const char *cert_pem_;
    const char *key_pem_;
    bool insecure_;
    bool handshake_done_;
    bool tls_setup_;
    int last_error_;
    uint8_t peek_buf_;
    int peek_len_;

    int do_tls_handshake(const char *hostname);
    static int tls_send(void *ctx, const unsigned char *buf, size_t len);
    static int tls_recv(void *ctx, unsigned char *buf, size_t len);
};

#endif
