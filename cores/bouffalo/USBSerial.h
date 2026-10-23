#pragma once

#include "api/HardwareSerial.h"

/*
 * USB CDC 串口。按 Arduino 官方惯例，电脑能看到的口是 Serial，硬件 UART 是 Serial1、Serial2。
 * Core 在 setup() 之前启动 USB 设备；begin()/end() 不改变 USB 状态，波特率由主机决定。
 * 主机未打开端口（DTR 无效）时写入被丢弃；主机打开但不读取时，写入最多等待 100 ms。
 * 主机以 1200 波特率打开再关闭端口时，设备复位进入下载模式（Arduino 的 1200 bps touch 上传），可用 setTouchReset(false) 关闭。
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

    /* 1200 bps touch 是否让 BL616CL 复位进下载模式，默认是；应用自己处理 1200 波特率时关闭（如 bridge） */
    void setTouchReset(bool enable);
    /*
     * 主机每次设置线路参数（SET_LINE_CODING）或控制线（SET_CONTROL_LINE_STATE）之后调用一次 callback；NULL 取消。
     * 回调在 Core 的线路任务（优先级高于 loop）中运行，可以 delay()，用 baud()、dtr()、rts() 读当前值；
     * 回调运行期间到达的请求在它返回后逐个补调，读到的是最新值。注册之前的请求不补调。
     * 首次注册时创建线路任务，失败返回 false。
     */
    bool onLineChange(void (*callback)(void));
};

#if defined(USB_VID)
extern USBSerial Serial;
#endif
