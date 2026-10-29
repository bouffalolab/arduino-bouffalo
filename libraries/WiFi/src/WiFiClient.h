#pragma once

#include "Arduino.h"
#include "api/Client.h"

/*
 * TCP 客户端，接口同 Arduino 官方 WiFi 库（WiFiS3、WiFiNINA）的 WiFiClient。
 * 对象只持有 socket 号：复制后指向同一连接，析构不关闭连接，用完调用 stop()（与 WiFiNINA 相同）。
 * 同一连接只在一个任务中使用。
 */
class WiFiClient : public arduino::Client {
public:
    WiFiClient();

    int connect(IPAddress ip, uint16_t port) override;
    int connect(const char *host, uint16_t port) override;
    size_t write(uint8_t c) override;
    size_t write(const uint8_t *buf, size_t size) override;
    int available() override;
    int read() override;
    int read(uint8_t *buf, size_t size) override;
    int peek() override;
    void flush() override;
    void stop() override;
    uint8_t connected() override;
    operator bool() override;
    using Print::write;

    IPAddress remoteIP();
    uint16_t remotePort();
    IPAddress localIP();
    uint16_t localPort();
    /* 连接超时（毫秒），0 为默认 10 s；同 WiFiS3 */
    void setConnectionTimeout(int timeout);

protected:
    int sock_ = -1; /* WiFiSSLClient 的 TLS 连接也以 socket 号标识 */
    int connect_timeout_ = 0;

private:
    friend class WiFiServer;
    explicit WiFiClient(int sock); /* WiFiServer 交出已接入的连接 */
};
