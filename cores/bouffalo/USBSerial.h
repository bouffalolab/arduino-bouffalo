#pragma once

#include "api/HardwareSerial.h"

/*
 * USB CDC 串口。按 Arduino 官方惯例，电脑能看到的口是 Serial，硬件 UART 是 Serial1、Serial2。
 * Core 在 setup() 之前启动 USB 设备；begin()/end() 不改变 USB 状态，波特率由主机决定。
 * 主机未打开端口（DTR 无效）时写入被丢弃；主机打开但不读取时，写入最多等待 100 ms。
 * 主机以 1200 波特率打开再关闭端口时，设备复位进入下载模式（Arduino 的 1200 bps touch 上传）。
 */
class USBSerial : public arduino::HardwareSerial {
public:
    void begin(unsigned long baudrate) override;
    void begin(unsigned long baudrate, uint16_t config) override;
    void end() override;
    int available(void) override;
    int peek(void) override;
    int read(void) override;
    void flush(void) override;
    int availableForWrite(void) override;
    size_t write(uint8_t c) override;
    size_t write(const uint8_t *buffer, size_t size) override;
    using Print::write;
    /* 主机已打开端口（DTR 有效） */
    operator bool() override;

    /* 主机设置的线路参数与控制线，与官方 SAMD 核心的 USB 串口同名 */
    uint32_t baud(void);
    uint8_t stopbits(void);
    uint8_t paritytype(void);
    uint8_t numbits(void);
    bool dtr(void);
    bool rts(void);
};

#if defined(USB_VID)
extern USBSerial Serial;
#endif
