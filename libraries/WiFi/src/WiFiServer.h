#pragma once

#include "Arduino.h"
#include "api/Server.h"

#include "WiFiClient.h"

#define WIFI_SERVER_MAX_CLIENTS 4 /* available() 保留的连接数 */

/*
 * TCP 服务端，接口同 Arduino 官方 WiFi 库（WiFiS3）的 WiFiServer。
 * available() 接受新连接并保留在服务端（最多 WIFI_SERVER_MAX_CLIENTS 个），返回其中一个有数据可读的连接，没有时返回无效的
 * WiFiClient；同一连接可以多次返回。对端关闭且数据读完后服务端关闭它；调用者 stop() 后服务端不再管它。
 * accept() 只把每个新连接返回一次，之后由调用者管理（用完 stop()），不进入 available() 的连接。
 * write() 发给 available() 保留的所有连接。未连上网络时 begin() 失败，operator bool 为 false。
 * lwIP 共 8 个 socket，监听与每个连接各占一个。对象不能复制；同一对象只在一个任务中使用。
 */
class WiFiServer : public arduino::Server {
public:
    WiFiServer(uint16_t port = 80);
    ~WiFiServer();
    WiFiServer(const WiFiServer &) = delete;
    WiFiServer &operator=(const WiFiServer &) = delete;

    void begin() override;
    /* 改用 port 监听；已在监听时先 end() */
    void begin(uint16_t port);
    WiFiClient available();
    WiFiClient accept();
    /* 返回各连接都写出的字节数（取最少的）；没有连接时返回 0 */
    size_t write(uint8_t b) override;
    size_t write(const uint8_t *buf, size_t size) override;
    using Print::write;
    /* 停止监听，并关闭 available() 保留的连接 */
    void end();
    explicit operator bool();

private:
    void refresh();

    uint16_t port_;
    int sock_ = -1;
    int conns_[WIFI_SERVER_MAX_CLIENTS];
    uint32_t closes_[WIFI_SERVER_MAX_CLIENTS]; /* 接入时的 net_port_close_count */
};
