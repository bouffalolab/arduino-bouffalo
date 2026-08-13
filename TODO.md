# BL616CL UNO R4 Bridge TODO

## 当前进度

- [x] 第一阶段：ESP32 Arduino API 兼容骨架
- [x] 第二阶段：BL616CL CherryUSB CDC ACM + HID 后端
- [x] 第二阶段硬件验证（USB 部分）
- [ ] 第三阶段：WiFi6 / TCP / TLS
- [ ] 第四阶段：存储与 OTA
- [ ] 第五阶段：完整 UNO R4 板级适配

## 第二阶段收尾

- [x] 确认调试日志串口与 Bouffalo SDK 默认板级配置一致：
      UART0 = GPIO34 TX / GPIO35 RX @ 2 Mbit/s，UART1 = GPIO24 TX / GPIO25 RX
- [x] 在 BL616CL DK 上验证 USB 枚举，确认 PID/VID 和字符串描述符
      （Linux 与 macOS 均枚举成功：VID 0x2341 / PID 0x1002，
      CDC ACM + CMSIS-DAP HID 复合设备，HS 480 Mbit/s）
- [ ] 验证 CDC 回环和主机侧串口收发（CDC 端口已出现
      `/dev/cu.usbmodem01`，数据通路待 UART1 对端配合验证）
- [ ] 验证 CMSIS-DAP HID 命令（HID 枚举已验证，DAP 命令未测）
- [ ] 用逻辑分析仪校准 DAP SWDIO/SWCLK 时序和延时常数
- [ ] 完善 CDC DTR/RTS 状态机与串口流控
- [ ] 为 HID `SendReport()` 增加待发送队列，避免 IN 端点繁忙时丢包
- [ ] 确认 USB 复位/挂起/重连后的重初始化行为

## Runtime Bundle 维护

- [ ] 在 GNU Make 4+ 环境重新运行 `generate_runtime_bundle.py`，验证 CherryUSB 配置可复现
- [ ] 补充 macOS 下直接 CMake 构建说明或增加 CMake 回退路径
- [x] 记录 SDK 本地补丁：`tools/runtime_bundle/patches/`（bflb_usb_v2 EP0 控制传输修复）
- [x] 用修复后的 `liblhal.a` 重建并提交 BL616CL 运行时库
- [ ] 重新生成并提交 `tools/sdk/bl616cl/manifest.json`（当前库为直接 CMake 重建，
      manifest 尚未同步刷新）
- [ ] 确认 `libcherryusb.a`、`liblhal.a`、`autoconf.h` 与 SDK commit 对应关系

## 第三阶段：WiFi6 / TCP / TLS

- [ ] 启用 `CONFIG_WIFI6`、lwIP、mbedTLS 并重新生成 runtime bundle
- [ ] 实现真实 `WiFi`、`WiFiClient`、`WiFiServer`、`WiFiUDP`、`WiFiClientSecure`
- [ ] 映射 WiFi 事件到 bridge 的 `CAtHandler::onWiFiEvent`
- [ ] 用 Bouffalo `wifi_mgmr` API 实现 STA、AP、扫描、IP/DNS/MAC 查询
- [ ] 将 `ping.cpp` 从 ESP ping 桩切换到 lwIP ICMP
- [ ] 用 bridge AT 命令做连接/扫描/TCP/UDP/TLS 冒烟测试

## 第四阶段：存储与 OTA

- [ ] 在 BL616CL 上实现 `SPIFFS`/`FS` 兼容层，底层选择 LittleFS 或 Flash 文件系统
- [ ] 实现 `Preferences` NVM 后端
- [ ] 实现 `Update` 和 `Arduino_ESP32_OTA` 后端
- [ ] 接入 BOSSA 刷写 RA4M1 的路径
- [ ] 验证证书分区读取、OTA 下载和重启

## 第五阶段：完整 UNO R4 板级适配

- [ ] 取得最终 UNO R4 载体板原理图和 GPIO 定义
- [ ] 更新 variant 的 USB、UART、BOOT/RESET、DAP 引脚映射
- [ ] 验证 RA4M1 与 BL616CL 的串口透传
- [ ] 验证通过 USB CMSIS-DAP 对 RA4M1 进行编程
- [ ] 验证 BLE HCI 透传
- [ ] 执行端到端 bridge 回归和更新包打包

## 测试与交付

- [ ] 固化 Blink/Serial/Bridge 三个编译回归命令
- [ ] 增加自动编译脚本或 CI 检查
- [ ] 记录固件尺寸和 RAM 预算
- [ ] 整理上板烧录与调试步骤
