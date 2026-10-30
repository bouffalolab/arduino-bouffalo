#pragma once

#include "Arduino.h"
#include "api/Server.h"

#include "WiFiClient.h"

#define WIFI_SERVER_MAX_CLIENTS 4 /* Maximum connections retained by available(). */

/*
 * WiFiS3-compatible TCP server. available() accepts and retains up to WIFI_SERVER_MAX_CLIENTS
 * connections, returning one with readable data or an invalid WiFiClient. It may return the same
 * connection repeatedly. Close a peer-closed connection once drained; relinquish one explicitly
 * stopped by the caller. accept() transfers each new connection once to the caller and does not
 * retain it. write() broadcasts to retained connections. begin fails without a network and
 * operator bool then returns false. The eight lwIP sockets are shared by the listener and clients.
 * Noncopyable; use each instance from one task.
 */
class WiFiServer : public arduino::Server {
public:
    /**
     * @brief Construct an inactive TCP listener for the requested port.
     */
    WiFiServer(uint16_t port = 80);
    /**
     * @brief Close the listener and its retained connections.
     */
    ~WiFiServer();
    /**
     * @brief Disallow copying this uniquely owned socket resource.
     */
    WiFiServer(const WiFiServer &) = delete;
    /**
     * @brief Disallow copying this uniquely owned socket resource.
     */
    WiFiServer &operator=(const WiFiServer &) = delete;

    /**
     * @brief Start the TCP listener on the selected port only when a network interface is
     * usable.
     */
    void begin() override;
    /**
     * @brief Switch the listening port, calling end() first if already listening.
     */
    void begin(uint16_t port);
    /**
     * @brief Return a retained client with readable data, or an invalid client when none is
     * ready.
     */
    WiFiClient available();
    /**
     * @brief Transfer one new TCP client to the caller, which must stop it when finished.
     *
     * @return A new client, or an invalid client if none is pending. The caller must call
     *   stop().
     */
    WiFiClient accept();
    /**
     * @brief Return the smallest byte count written to any retained client, or zero with no
     * clients.
     */
    size_t write(uint8_t b) override;
    /**
     * @brief Broadcast to retained clients and return the smallest accepted count, or zero
     * when none are connected.
     */
    size_t write(const uint8_t *buf, size_t size) override;
    using Print::write;
    /**
     * @brief Stop listening and close connections retained by available().
     */
    void end();
    /**
     * @brief Report whether this object currently refers to a usable resource.
     */
    explicit operator bool();

private:
    /**
     * @brief Reclaim completed retained clients and accept new connections into free server
     * slots.
     */
    void refresh();

    uint16_t port_;
    int sock_ = -1;
    int conns_[WIFI_SERVER_MAX_CLIENTS];
    uint32_t closes_[WIFI_SERVER_MAX_CLIENTS]; /* net_port_close_count captured when this connection was accepted. */
};
