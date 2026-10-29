#pragma once

#include "WiFiClient.h"

/*
 * TLS 客户端，接口同 Arduino 官方 WiFi 库（WiFiS3）的 WiFiSSLClient。TLS 1.2；默认用底层包内的根证书束（Mozilla）
 * 校验服务器证书与主机名，setCACert 之后只信任给定的证书；不检查证书有效期（同上游 ESP32 固件，见 ticket 0029）。
 * setCertificate/setPrivateKey 支持软件客户端证书槽。每个连接占一个 socket，另需约 38 KB 堆（握手时约 43–52 KB），OCRAM 空闲不足时
 * connect 失败（见 tls_port.c 的 TLS_OCRAM_MIN）。connect 的超时（setConnectionTimeout，
 * 默认 10 s）包含 TCP 连接与握手；connect(IPAddress) 同 WiFiS3，按点分地址字符串校验证书。
 * 复制、析构语义同 WiFiClient：用完调用 stop()。
 */
class WiFiSSLClient : public WiFiClient {
public:
    int connect(IPAddress ip, uint16_t port) override;
    int connect(const char *host, uint16_t port) override;
    /* PEM 格式的根证书（可含多个），以 '\0' 结尾，connect 时读取；nullptr 恢复用根证书束 */
    void setCACert(const char *root_ca);
    void setCertificate(const char *certificate);
    void setPrivateKey(const char *private_key);
    size_t write(const uint8_t *buf, size_t size) override;
    int available() override;
    int read(uint8_t *buf, size_t size) override;
    int peek() override;
    void stop() override;
    uint8_t connected() override;
    using WiFiClient::read;
    using WiFiClient::write;

private:
    const char *root_ca_ = nullptr;
    const char *certificate_ = nullptr;
    const char *private_key_ = nullptr;
};
