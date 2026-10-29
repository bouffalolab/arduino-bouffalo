#pragma once

#include "Arduino.h"
#include "api/Udp.h"

#define WIFI_UDP_BUFFER_SIZE 1460 /* 收、发各一块，同 ESP32 Arduino 的 WiFiUDP */

/*
 * UDP，接口同 Arduino 官方 WiFi 库（WiFiS3）的 WiFiUDP。收发缓冲在 begin 或第一次 beginPacket 时分配：
 * 一个数据报最多发 WIFI_UDP_BUFFER_SIZE 字节，收到更长的数据报时超出部分丢弃。
 * 未连上网络时 begin、beginPacket 失败；未调用 begin 时 beginPacket 用协议栈分配的本地端口（同 WiFiNINA）。
 * beginMulticast 绑定非零端口并加入 IPv4 组；stop（包括重新 begin）关闭 socket 时离组。
 * 对象持有 socket 与缓冲，不能复制；析构时关闭。同一对象只在一个任务中使用。
 */
class WiFiUDP : public arduino::UDP {
public:
    WiFiUDP() = default;
    ~WiFiUDP();
    WiFiUDP(const WiFiUDP &) = delete;
    WiFiUDP &operator=(const WiFiUDP &) = delete;

    /* 绑定本地端口；已打开时先关闭。成功返回 1 */
    uint8_t begin(uint16_t port) override;
    /* 绑定本地地址与端口（WiFiS3 的扩展） */
    uint8_t begin(IPAddress ip, uint16_t port);
    uint8_t beginMulticast(IPAddress ip, uint16_t port) override;
    int beginMulticastPacket();
    void stop() override;

    /* 复用最近发包目标或最近 parsePacket 成功的数据报来源；没有目标时失败 */
    int beginPacket();
    int beginPacket(IPAddress ip, uint16_t port) override;
    int beginPacket(const char *host, uint16_t port) override;
    int endPacket() override;
    /* 没有进行中的数据报或缓冲已满时返回 0 */
    size_t write(uint8_t b) override;
    size_t write(const uint8_t *buffer, size_t size) override;
    using Print::write;

    /* 不等待；丢弃上一个数据报未读的部分。返回新数据报的长度，没有时返回 0 */
    int parsePacket() override;
    int available() override;
    int read() override;
    int read(unsigned char *buffer, size_t len) override;
    int read(char *buffer, size_t len) override;
    int peek() override;
    /* 丢弃当前数据报未读的部分 */
    void flush() override;
    /* 当前数据报的来源；parsePacket 之前为 0 */
    IPAddress remoteIP() override;
    uint16_t remotePort() override;

private:
    bool open(uint32_t addr, uint16_t port);

    int sock_ = -1;
    uint8_t *rx_ = nullptr; /* 2 × WIFI_UDP_BUFFER_SIZE，后一半是 tx_ */
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
