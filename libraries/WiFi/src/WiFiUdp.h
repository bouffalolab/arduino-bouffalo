#pragma once

#include "Arduino.h"
#include "api/Udp.h"

#define WIFI_UDP_BUFFER_SIZE 1460 /* Separate RX and TX buffers, following ESP32 Arduino WiFiUDP. */

/*
 * WiFiS3-compatible UDP. Allocate buffers in begin() or the first beginPacket(). Send at most
 * WIFI_UDP_BUFFER_SIZE bytes per datagram; discard excess received bytes. begin/beginPacket fail
 * without a usable network. Without begin(), use a stack-assigned local port, matching WiFiNINA.
 * beginMulticast binds a nonzero port and joins an IPv4 group; closing in stop() or a later
 * begin() leaves groups. The noncopyable object owns its socket/buffers and closes on destruction.
 * Use each instance from one task.
 */
class WiFiUDP : public arduino::UDP {
public:
    /**
     * @brief Construct an inactive UDP socket and empty packet buffers.
     */
    WiFiUDP() = default;
    /**
     * @brief Close the UDP socket and release packet state.
     */
    ~WiFiUDP();
    /**
     * @brief Disallow copying this uniquely owned socket resource.
     */
    WiFiUDP(const WiFiUDP &) = delete;
    /**
     * @brief Disallow copying this uniquely owned socket resource.
     */
    WiFiUDP &operator=(const WiFiUDP &) = delete;

    /**
     * @brief Bind a local port, closing an existing socket first. Return 1 on success.
     */
    uint8_t begin(uint16_t port) override;
    /**
     * @brief Bind a local address and port, following the WiFiS3 extension.
     */
    uint8_t begin(IPAddress ip, uint16_t port);
    /**
     * @brief Bind a nonzero UDP port and join the specified IPv4 multicast group.
     *
     * @param ip IPv4 multicast group; the implicit all-hosts group 224.0.0.1 is rejected.
     * @param port Nonzero local UDP port.
     * @return 1 on success; 0 for invalid input, unavailable networking or resource/join
     *   failure.
     */
    uint8_t beginMulticast(IPAddress ip, uint16_t port) override;
    /**
     * @brief Select this socket's joined multicast group as the pending destination.
     */
    int beginMulticastPacket();
    /**
     * @brief Close the UDP socket and release both packet buffers and destination state.
     */
    void stop() override;

    /**
     * @brief Reuse the most recent send destination or source of a successfully parsed packet;
     * fail if no destination exists.
     */
    int beginPacket();
    /**
     * @brief Select a datagram destination, reusing the most recent one when no address is
     * supplied.
     */
    int beginPacket(IPAddress ip, uint16_t port) override;
    /**
     * @brief Select a datagram destination, reusing the most recent one when no address is
     * supplied.
     */
    int beginPacket(const char *host, uint16_t port) override;
    /**
     * @brief Send the buffered datagram and report whether transmission was accepted.
     */
    int endPacket() override;
    /**
     * @brief Return zero if no datagram is being built or the buffer is full.
     */
    size_t write(uint8_t b) override;
    /**
     * @brief Append bytes to the bounded pending datagram and return the accepted count.
     */
    size_t write(const uint8_t *buffer, size_t size) override;
    using Print::write;

    /**
     * @brief Nonblocking: discard unread bytes from the previous datagram. Return the next
     * datagram length, or zero if none is available.
     */
    int parsePacket() override;
    /**
     * @brief Return the currently readable byte count without consuming input.
     */
    int available() override;
    /**
     * @brief Consume unread bytes from the current datagram without waiting.
     */
    int read() override;
    /**
     * @brief Consume unread bytes from the current datagram without waiting.
     */
    int read(unsigned char *buffer, size_t len) override;
    /**
     * @brief Consume unread bytes from the current datagram without waiting.
     */
    int read(char *buffer, size_t len) override;
    /**
     * @brief Return the next input byte without consuming it, or -1 when none is available.
     */
    int peek() override;
    /**
     * @brief Discard unread bytes from the current datagram.
     */
    void flush() override;
    /**
     * @brief Source of the current datagram; zero before parsePacket().
     */
    IPAddress remoteIP() override;
    /**
     * @brief Return the connected peer or current datagram source port.
     */
    uint16_t remotePort() override;

private:
    /**
     * @brief Open an absolute path with a supported mode, returning an invalid File on
     * failure.
     */
    bool open(uint32_t addr, uint16_t port);

    int sock_ = -1;
    uint8_t *rx_ = nullptr; /* Two WIFI_UDP_BUFFER_SIZE regions; tx_ points to the second half. */
    uint8_t *tx_ = nullptr;
    size_t rx_len_ = 0;
    size_t rx_pos_ = 0;
    uint32_t rx_addr_ = 0;
    uint16_t rx_port_ = 0;
    bool tx_open_ = false;
    size_t tx_len_ = 0;
    uint32_t tx_addr_ = 0;
    uint16_t tx_port_ = 0;
    uint32_t multicast_addr_ = 0;
    uint16_t multicast_port_ = 0;
};
