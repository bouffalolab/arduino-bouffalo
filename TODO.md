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
- [x] 验证 CDC 收发：主机写入 17 字节经 CDC OUT→固件→CDC IN 原样回读
      （临时回显代码已还原）；与 UART1 的对端透传待板级连接验证
- [x] 验证 CMSIS-DAP HID 命令：DAP_Info（capabilities/字符串）、
      DAP_HostStatus、DAP_SWJ_CLOCK 均经 HID 往返返回正确内容
- [ ] 用逻辑分析仪校准 DAP SWDIO/SWCLK 时序和延时常数
- [ ] 完善 CDC DTR/RTS 状态机与串口流控
- [ ] 为 HID `SendReport()` 增加待发送队列，避免 IN 端点繁忙时丢包
- [ ] 确认 USB 复位/挂起/重连后的重初始化行为

## Runtime Bundle 维护

- [ ] 在 GNU Make 4+ 环境重新运行 `generate_runtime_bundle.py`，验证 CherryUSB 配置可复现
- [ ] 补充 macOS 下直接 CMake 构建说明或增加 CMake 回退路径
- [x] 记录 SDK 本地补丁：`tools/runtime_bundle/patches/`（bflb_usb_v2 EP0 控制传输修复）
- [x] 用修复后的 `liblhal.a` 重建并提交 BL616CL 运行时库
- [x] 重新生成并提交 `tools/sdk/bl616cl/manifest.json`（记录 wl80211/macsw/lwip
      与各 submodule commit，含新增 wl80211/supplicant 库）
- [ ] 确认 `libcherryusb.a`、`liblhal.a`、`autoconf.h` 与 SDK commit 对应关系

## 第三阶段：WiFi6 / TCP / TLS

- [x] 启用 `CONFIG_WIFI6`、lwIP、mbedTLS 并重建 runtime bundle
      （macsw/fhost/wpa/lwip/mbedtls 库已入库，linker 脚本与头文件已同步）
- [x] 实现 `WiFi` 类骨架：初始化、扫描、连接、状态、IP/MAC、事件回调
      （基于 `wifi_mgmr` + lwIP）
- [x] 迁移到 wl80211 方案（fhost 全栈 RAM 占用超预算，wl80211 仅省去
      wpa_supplicant 的 Android 完整栈，保留 macsw 固件 RAM；wifi6 库与
      `libapp.a` 需同步重建，否则 board_init 不挂 WIFI IRQ，STA VIF 卡死）
- [x] 实机验证扫描：BL616CL DK 上 `WiFi.scanNetworks()` 返回 16 个 AP，
      `CODE_WIFI_ON_SCAN_DONE` 正常触发，wpa_attach/连接路径解阻塞
- [ ] 实机验证 WPA2-PSK 连接与 DHCP（通过 bridge AT 命令）
- [ ] 实现 `WiFiClient`、`WiFiServer`、`WiFiUDP`（lwIP socket 后端）
- [ ] 实现 `WiFiClientSecure`（mbedTLS v3 后端，并解决 compat 层 v2 桩冲突）
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
- [x] 记录固件尺寸和 RAM 预算（wl80211 版本：flash 545,854 B / 26%，
      globals 44,784 B / 13%；对比 fhost 版本 flash 902,366 B，
      globals 48,904 B）
- [ ] 整理上板烧录与调试步骤
