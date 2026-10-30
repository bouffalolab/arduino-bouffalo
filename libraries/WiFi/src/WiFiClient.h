#pragma once

#include "Arduino.h"
#include "api/Client.h"

/*
 * TCP client compatible with WiFiS3/WiFiNINA. Objects hold a socket number; copies share the
 * connection and destructors do not close it. Call stop() when finished, matching WiFiNINA. Use
 * each connection from one task.
 */
class WiFiClient : public arduino::Client {
public:
    /**
     * @brief Construct a client handle; accepted socket handles share explicit stop ownership.
     */
    WiFiClient();

    /**
     * @brief Establish a bounded connection to the supplied address or hostname; report
     * failure when no network is usable.
     */
    int connect(IPAddress ip, uint16_t port) override;
    /**
     * @brief Establish a bounded connection to the supplied address or hostname; report
     * failure when no network is usable.
     */
    int connect(const char *host, uint16_t port) override;
    /**
     * @brief Write the supplied byte or buffer and return the accepted byte count.
     */
    size_t write(uint8_t c) override;
    /**
     * @brief Write the supplied byte or buffer and return the accepted byte count.
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
    int read() override;
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
     * @brief Provide the Arduino client compatibility hook; unbuffered socket output has
     * nothing to drain.
     */
    void flush() override;
    /**
     * @brief Close the shared socket and invalidate this handle; copied handles must no longer
     * use it.
     */
    void stop() override;
    /**
     * @brief Report a live socket or received data still buffered after peer closure.
     */
    uint8_t connected() override;
    /**
     * @brief Report whether this object currently refers to a usable resource.
     */
    operator bool() override;
    using Print::write;

    /**
     * @brief Return the connected TCP peer IPv4 address, or 0.0.0.0 when unavailable.
     */
    IPAddress remoteIP();
    /**
     * @brief Return the connected TCP peer port, or zero when unavailable.
     */
    uint16_t remotePort();
    /**
     * @brief Return the socket local IPv4 address, or 0.0.0.0 when unavailable.
     */
    IPAddress localIP();
    /**
     * @brief Return the socket's bound local port.
     */
    uint16_t localPort();
    /**
     * @brief Connection timeout in milliseconds; zero selects the WiFiS3 default of 10 s.
     *
     * @param timeout Connection deadline in milliseconds; nonpositive values select the 10 s
     *   default.
     */
    void setConnectionTimeout(int timeout);

protected:
    int sock_ = -1; /* WiFiSSLClient also identifies TLS sessions by socket number. */
    int connect_timeout_ = 0;

private:
    friend class WiFiServer;
    /**
     * @brief Construct a client handle; accepted socket handles share explicit stop ownership.
     */
    explicit WiFiClient(int sock); /* WiFiServer transfers accepted connections through this constructor. */
};
