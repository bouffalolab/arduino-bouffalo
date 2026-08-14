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
      （实测：macOS 下烧录/复位后 `/dev/cu.usbmodem01` 常不重新枚举，
      需再按一次 RTS 复位才恢复；`usb:event_configured` 已打印但系统无节点）

## Runtime Bundle 维护

- [ ] 在 GNU Make 4+ 环境重新运行 `generate_runtime_bundle.py`，验证 CherryUSB 配置可复现
- [ ] 补充 macOS 下直接 CMake 构建说明或增加 CMake 回退路径
- [x] 记录 SDK 本地补丁：`tools/runtime_bundle/patches/`（bflb_usb_v2 EP0 控制传输修复）
- [x] 用修复后的 `liblhal.a` 重建并提交 BL616CL 运行时库
- [x] 重新生成并提交 `tools/sdk/bl616cl/manifest.json`（记录 wl80211/macsw/lwip
      与各 submodule commit，含新增 wl80211/supplicant 库）
- [x] 修复 wl80211 连接不按 SSID 选择 AP 的问题
      （`wl80211-connect-ssid-filter.patch`：join 扫描按请求 SSID 过滤候选、
      接受隐藏 SSID AP 的定向探测响应；重建并提交 `libwl80211_bl616cl.a`）
- [x] 修复 lwIP 堆过小导致的 `EAI_MEMORY`（`lwip-mem-size-60k.patch`：
      MEM_SIZE 8KB→60KB，重建 `liblwip.a`；probe `main.c` 补 `bl_rand`）
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
- [x] 实机验证 WPA2-PSK 连接与 DHCP：连接 zrrong AP 成功，DHCP 拿到
      192.168.133.40/24，`CODE_WIFI_ON_GOT_IP` 正常；修复 wl80211
      `ip_got_cb` 在 tcpip 线程二次加 lwIP core 锁导致的死锁
      （`netifapi_netif_set_default` → `netif_set_default`）
- [x] 实机验证数据通路：DNS 解析 example.com 成功，TCP connect 80 端口成功
- [x] 实现 `WiFiClient`、`WiFiServer`、`WiFiUDP`（lwIP socket 后端）
- [x] 实机端到端验证（zrrong，PC 192.168.133.49 作为对端）：
      WiFiClient 连 PC TCP echo 成功收发、WiFiUDP 往返 echo 成功、
      WiFiServer 接受 PC 连接并回读数据成功；remoteIP/localIP 字节序正确
- [x] 修复 IP 字节序 bug：IPAddress 的 uint32_t 与 sin_addr.s_addr 同为
      网络字节序，connect/beginPacket 不应 lwip_htonl、remoteIP/localIP
      不应 lwip_ntohl；这是此前所有 TCP/UDP“连不上/回环失败”的根因
      （此前误判为 lwIP loopback port 问题，实际 raw 正确字节序回环是通的）
- [x] 兼容性修复：lwIP 与 newlib errno 值域不一致、INADDR_NONE 宏冲突、
      非阻塞 connect 提前可写导致误判失败、SO_RCVTIMEO 需传 struct timeval
- [ ] CI_throughput 待复测：连接+DHCP 正常（192.168.28.226），但此前网关
      TCP 53/DNS 探针同样受到 IP 字节序 bug 影响，结论需修正后重跑；
      若仍不通则说明该网隔离、需同网段对端
- [x] 实现 `WiFiClientSecure`（mbedTLS v3 后端）：
      实机验证 www.bing.com:443 的 TLS 1.2 握手与 HTTPS GET 收发（390 字节响应）
      - 解决 compat 层 v2 桩冲突：v2 桩改 weak + libmbedtls whole-archive，
        SSE.cpp 移植到 v3（pk_sign 新签名、mbedtls_sha256、pk_parse_key）
      - 修复 config-tls-generic.h 缺 MBEDTLS_ECP_HAVE_* 映射导致 X.509 OID
        表不含命名曲线的问题（重建 libmbedtls.a）
- [x] 修复 ECDSA 证书链解析：证书链混用 P-256/P-384，补齐
      CONFIG_MBEDTLS_ECP_DP_SECP384R1_ENABLED；实机验证 example.com:443
      TLS 握手 + HTTPS GET 收发（869 字节响应）
- [x] 映射 WiFi 事件到 bridge 的 `CAtHandler::onWiFiEvent`：
      STA ready/scan/connected/disconnected/got-ip 经 compat 层转成
      ARDUINO_EVENT_*，实机验证 `AT+GETSTATUS?` 在连接后返回 3
      （WIFI_ST_CONNECTED）；AP 相关事件待 softAP 落地后补齐
- [ ] 用 Bouffalo `wifi_mgmr` API 实现 STA、AP、扫描、IP/DNS/MAC 查询
- [x] 将 `ping.cpp` 从 ESP ping 桩切换到 lwIP ICMP（raw socket 自实现，
      实机 ping 192.168.133.49 4/4 成功；补 DEFAULT_RAW_RECVMBOX_SIZE=8）
- [x] 修复 `WiFi.SSID()/BSSID()/RSSI()` 无参重载：此前默认参数解析成扫描列表
      第 0 项，`AT+GETSSID?` 误报 TP-LINK_3D67；现在返回当前 STA 连接信息，
      实机验证返回 zrrong / 64:64:4A:82:73:74 / 实时 RSSI
- [x] 修复数字 IP 字符串解析：lwIP 的 `lwip_getaddrinfo()` 只有带
      `AI_NUMERICHOST` 才解析点分 IP，否则一律走 DNS；新增
      `lwip_resolve_host()`（先数字、后 DNS）并用于 WiFiClient/WiFiUDP/ping
- [x] bridge AT 命令经 USB CDC 冒烟：AT/GMR/WIFISCAN/BEGINSTA/GETSTATUS/
      IPSTA/GETSSID/GETBSSID/GETRSSI/MACSTA 全通，连接 zrrong 并 DHCP 成功
      （AT 与 USB CDC 共用 USBSerial 时由 `AT_ON_USBCDC` 关闭 loop() 透传抢流）
- [x] `AT+PING` 实机验证：到 PC（192.168.133.49）往返成功并返回整数 RTT
      （此前 `%f` 在 CONFIG_LIBC_FLOAT=0 下打印异常）；无 IP 时已加保护
- [ ] 定位 lwIP 堆在忙信道下持续耗尽的问题：多次网络操作后
      `lwip_getaddrinfo()` 稳定返回 EAI_MEMORY（60KB 堆仍复现），怀疑
      wl80211 RX 路径 pbuf 未及时归还（环境有 45+ AP 的大量广播流量）
- [ ] 定位设备→网关/DNS 不通：设备 ping PC 通、ping 网关 192.168.133.2 与
      DNS 查询失败（DHCP 能拿到地址），需确认 AP 客户端隔离还是 ARP 出站问题
- [ ] 定位无 IP 时 raw socket ping 破坏 wl80211 TX/lwIP 堆的根因（当前在
      AT 层加了 localIP 保护，底层根因未修）
- [ ] 用 bridge AT 命令补 TCP/UDP/TLS 冒烟：AT 侧 BEGINCLIENT/CLIENTCONNECT
      路径已修，数据通路待 lwIP 堆/网关问题解决后复测

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
- [ ] 主要功能完成后，为 esp32-compat 适配层的主要 API（WiFi 扫描/连接/状态、
      WiFiClient/WiFiUDP/WiFiServer、USB CDC/HID、CMSIS-DAP、IP/DNS 等）编写
      单元测试，并编译一个专用固件集中运行全部 API 单元测试（正常路径、
      边界与错误路径、资源回收）
- [ ] 整理上板烧录与调试步骤
