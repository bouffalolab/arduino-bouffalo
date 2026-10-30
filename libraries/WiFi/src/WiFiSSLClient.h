#pragma once

#include "WiFiClient.h"

/*
 * WiFiS3-compatible TLS 1.2 client using the package Mozilla root bundle by default. Verify
 * certificates and hostnames; setCACert limits trust to the supplied PEM. Certificate validity
 * dates are not checked under the current compatibility policy. setCertificate/setPrivateKey
 * support software client-identity slots. Each session consumes one socket and about 38 KiB of
 * heap (43-52 KiB at handshake peak); connect fails below TLS_OCRAM_MIN. setConnectionTimeout
 * covers TCP and handshake (default 10 s). IPAddress connections verify against the dotted address
 * string, matching WiFiS3. Copies/destructors follow WiFiClient; explicitly call stop().
 */
class WiFiSSLClient : public WiFiClient {
public:
    /**
     * @brief Open TCP then authenticate a bounded TLS handshake using the selected trust and
     * optional client credentials.
     */
    int connect(IPAddress ip, uint16_t port) override;
    /**
     * @brief Open TCP then authenticate a bounded TLS handshake using the selected trust and
     * optional client credentials.
     */
    int connect(const char *host, uint16_t port) override;
    /**
     * @brief NUL-terminated PEM roots, possibly multiple certificates, read during connect();
     * nullptr restores the package root bundle.
     *
     * @param root_ca Caller-owned NUL-terminated PEM roots retained for future connects; nullptr
     *   selects bundled roots.
     */
    void setCACert(const char *root_ca);
    /**
     * @brief Select the caller-owned client certificate used by future TLS handshakes.
     *
     * @param certificate Caller-owned NUL-terminated PEM client certificate, retained for future
     *   connects.
     */
    void setCertificate(const char *certificate);
    /**
     * @brief Select the caller-owned client private key used by future TLS handshakes.
     *
     * @param private_key Caller-owned NUL-terminated PEM private key, retained for future connects.
     */
    void setPrivateKey(const char *private_key);
    /**
     * @brief Encrypt and send bytes with a bounded wait, returning the accepted plaintext
     * count.
     */
    size_t write(const uint8_t *buf, size_t size) override;
    /**
     * @brief Return the currently readable byte count without consuming input.
     */
    int available() override;
    /**
     * @brief Consume available input bytes; return the byte/count or the interface-specific
     * empty/error value.
     */
    int read(uint8_t *buf, size_t size) override;
    /**
     * @brief Return the next input byte without consuming it, or -1 when none is available.
     */
    int peek() override;
    /**
     * @brief Attempt bounded TLS close_notify, release session state and close TCP.
     */
    void stop() override;
    /**
     * @brief Report whether the client is connected or retains unread received data.
     */
    uint8_t connected() override;
    using WiFiClient::read;
    using WiFiClient::write;

private:
    const char *root_ca_ = nullptr;
    const char *certificate_ = nullptr;
    const char *private_key_ = nullptr;
};
