# BL616CL UNO R4 Bridge TODO

## 新板重调试（2026-08-29 起）

- [x] 控制台重映射（UART0→UART1 GPIO8/9）全部作废：SDK 副本
      board.c / macsw_bare / btble_cli 已 git 回退；bluetooth 子模块
      5 个探针文件（btblecontroller_port_uart.c、btble_dma_uart.c、
      llm_adv.c、hci_tl.c、hci_driver.c）已回退；bridge 侧
      compat_console_restore / usb_log 改回 UART0 GPIO34/35（4324a54），
      patches/bl616cldk-console-uart1-gpio8-9.patch 删除；
      bridge README 同步（32bf38b）。此前基于旧板（UNO R4 载板，
      FT232 接 GPIO8/9）的调试结论与实验状态作废，milestone 标签
      milestone-ble-adv-on-air 前提已失效（待用户决定删除）
- [ ] 新板接线确认（串口设备名、控制台引脚、RST/BOOT 控制线）
- [ ] 新板上重新调试 HCI：SDK 默认配置（控制台 UART0 GPIO34/35，
      HCI UART1 GPIO27-30 DMA 2M）跑 btble_cli / btblecontroller_test 基线

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
- [x] 修复 CDC OUT 背靠背传输丢字节：OUT 端点在传输回调里无条件重新武装，
      环形缓冲（4096 B）未及时排空时 `onOutData()` 静默丢弃后续传输
      （`AT+SETCAROOT` 装载 5178 B 证书时 `readBytes` 永远等不到缺失字节、
      AT 任务卡死）。现改为环形缓冲能容纳整段 staging 传输时才重新武装，
      否则保持端点 NAK（USB 背压），消费者从 `read()` 排空后再武装；
      已实机验证 5178 B CA 证书装载 + TLS 握手 + HTTPS GET
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
- [x] 修复 compat 层 `freeaddrinfo` 空桩导致的 `EAI_MEMORY`：lwip/netdb.h 把
      POSIX 名宏映射为 `lwip_freeaddrinfo`，空桩实际顶替了真实实现，
      `lwip_getaddrinfo()` 从唯一的 MEMP_NETDB 池取元素后永不归还，
      第二次解析起全部失败（表象为 EAI_MEMORY）；已删除桩函数
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
- [x] CI_throughput 复测完成（IP 字节序修复后）：连接 zrrong + DHCP
      （192.168.133.40/24）、DNS 解析 example.com、ping 网关
      （192.168.133.2，4 ms）、ping 对端 Mac（.49）、AT TCP/UDP echo
      全部通过；此前 gateway/DNS 探针不通确为 IP 字节序 bug 所致
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
- [x] 用 Bouffalo `wifi_mgmr` API 实现 STA：扫描/连接/断开/状态、
      IP/网关/掩码/DNS/MAC 查询（WiFi 类均已实机验证）
- [ ] softAP：`WiFi.softAP()` 仍为桩，AP 事件（LISTENING/STACONNECTED 等）
      待 softAP 落地后补齐
- [x] 将 `ping.cpp` 从 ESP ping 桩切换到 lwIP ICMP（raw socket 自实现，
      实机 ping 192.168.133.49 4/4 成功；补 DEFAULT_RAW_RECVMBOX_SIZE=8）
- [x] 修复 `WiFi.SSID()/BSSID()/RSSI()` 无参重载：此前默认参数解析成扫描列表
      第 0 项，`AT+GETSSID?` 误报 TP-LINK_3D67；现在返回当前 STA 连接信息，
      实机验证返回 zrrong / 64:64:4A:82:73:74 / 实时 RSSI
- [x] 修复数字 IP 字符串解析：lwIP 的 `lwip_getaddrinfo()` 只有带
      `AI_NUMERICHOST` 才解析点分 IP，否则一律走 DNS；新增
      `lwip_resolve_host()`（先数字、后 DNS）并用于 WiFiClient/WiFiUDP/ping
- [x] 修复 `WiFiClient::available()`：该 lwIP 配置（LWIP_SO_RCVBUF=0、
      LWIP_FIONREAD_LINUXMODE=0）下 FIONREAD 被编译掉，ioctl 恒返回 0，
      TCP 回读拿不到数据；改用 `recv(MSG_PEEK|MSG_DONTWAIT)` 探测
- [x] 修复 `WiFiUDP::parsePacket()` 阻塞：无数据报时改为 `MSG_DONTWAIT`，
      按 ESP32 语义返回 0，避免 bridge AT 任务永久卡死
- [x] bridge AT 命令经 USB CDC 冒烟：AT/GMR/WIFISCAN/BEGINSTA/GETSTATUS/
      IPSTA/GETSSID/GETBSSID/GETRSSI/MACSTA 全通，连接 zrrong 并 DHCP 成功
      （AT 与 USB CDC 共用 USBSerial 时由 `AT_ON_USBCDC` 关闭 loop() 透传抢流）
- [x] `AT+PING` 实机验证：到 PC（192.168.133.49）往返成功并返回整数 RTT
      （此前 `%f` 在 CONFIG_LIBC_FLOAT=0 下打印异常）；无 IP 时已加保护
- [x] 澄清此前“lwIP 堆耗尽/网关不通/DNS 失败”的误判：三者同根——
      `freeaddrinfo` 桩泄漏唯一的 MEMP_NETDB 池元素，后续所有 getaddrinfo
      返回 EAI_MEMORY（解析层即失败，根本未发出数据包）；AP 无隔离、PC 在
      5GHz/设备在 2.4GHz 桥接互通。修复后连续 ping PC/网关、DNS 全部正常
- [x] 回退 MEM_SIZE 8KB→60KB 改动（根因不在堆大小）；8KB 堆下完整
      AT 冒烟（ping×3/DNS/TCP echo/UDP echo）通过，并发负载下的堆余量
      留待多连接压力测试再确认
- [x] 定位"无 IP 时 raw socket ping 破坏 wl80211 TX/lwIP 堆"的根因：
      不是驱动/堆破坏，而是 `tcpip_init()` 只在 `ensure_wifi_started()`
      （WiFi 首次操作）里惰性调用；WiFi 未启动时 `tcpip_mbox` 从未创建，
      无 IP ping 的 `lwip_socket()` 首次触碰 tcpip 消息路径，
      `tcpip_send_msg_wait_sem` 命中 `LWIP_ASSERT("Invalid mbox")` → ebreak
      崩溃（实机复现 mcause=3、mepc 落在 tcpip.c 该断言）。
      修复：esp32_wifi.cpp 静态构造器提前 `tcpip_init`（调度器启动前），
      `ensure_wifi_started()` 不再重复初始化；实机验证无 IP ping 优雅返回
      -2、不崩溃，随后连接+ping 网关/DNS/TCP/UDP 全通（12/12）
- [x] bridge AT TCP/UDP 数据通路实机验证：CLIENTCONNECT→CLIENTSEND→
      CLIENTRECEIVE 回读 PC TCP echo；UDPBEGIN→BEGINPACKETIP→WRITE→
      ENDPACKET→PARSE→READ 回读 PC UDP echo；CLIENTCLOSE/UDPSTOP 正常
- [x] bridge AT TLS 命令冒烟：`AT+SSLBEGINCLIENT`→`AT+SETCAROOT`（装载
      example.com 证书链 5178 B，顺带暴露并修复 CDC OUT 丢字节）→
      `AT+SSLCLIENTCONNECTNAME=2000,example.com,443` 握手成功→
      `AT+SSLCLIENTSEND`+`AT+SSLCLIENTRECEIVE` 回读 HTTPS 200；
      `+SSLERR=<sock>` 保留为 TLS 错误诊断命令

## 第四阶段：存储与 OTA

- [x] 在 BL616CL 上实现 `SPIFFS`/`FS` 兼容层：SDK LittleFS vendored 进
      esp32-compat，`SPIFFS.begin` 挂载 `media` 分区（挂载点 `/spiffs`），
      `File` 支持读写/seek/目录遍历；newlib `fopen("/spiffs/...")` 经裁剪
      syscalls 分发到同一实例；StorageTest 全 PASS + 复位持久化验证
- [x] 实现 `Preferences` NVM 后端：对接 EasyFlash（底层 LittleFS on PSM），
      put/get 全类型 + getType/remove/clear/freeEntries，StorageTest round-trip
- [ ] 平台镜像布局与分区表槽位冲突：`bflb_fw_post_proc` 产出的完整镜像把 app
      代码放在 flash 0xE000 起的区域，与 boot2 分区表槽位（0xE000/0xF000）
      重叠；用 `write_flash_files 0xE000 partition.bin` 写入时按 4KB 扇区
      擦除，抹掉 0xE000-0xFFFF 的 app 代码（bridge 固件表现为启动时非法指令
      崩溃，mepc=0x8000eede 处读到 0xFFFFFFFF）。StorageTest（小镜像）不受
      影响；bridge 需要存储/OTA 功能前须在平台侧为分区表预留 0xE000-0xF200
      或调整槽位地址，并让上传流程自动烧写分区表
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

## 第六阶段：BLE HCI 透传（RA4M1 AT → BL616CL 蓝牙广播）

架构：RA4M1 上跑 ArduinoBLE host，通过 bridge 的 AT+HCI_* 命令把 HCI 透传给
BL616CL；BL616CL 只跑 BLE controller（external-host / HCI 透传模式），广播
参数全部由 host 侧决定。对应 bridge 侧 `cmds_ble_bridge.h` 的
AT+HCIBEGIN/HCIWRITE/HCIREAD/HCIAVAILABLE/HCIWAIT 命令。

- [x] esp32-compat 实现 HCIVirtualTransport（AT 侧 HCI 字节流 ↔ 环形缓冲）
      + btblecontroller port 覆写（`btble_uart_read/write/flow_on/flow_off`
      等，接 `hci_transport` 队列；lib 经 `rwip_eif_api uart_api` 函数指针表
      调用这些端口函数）
- [x] BLE 启动路径：AT+HCIBEGIN → `HCIVirtualTransport::begin()` →
      `ble_controller_start()` → `ensure_rfparam()`（与 WiFi 共享一次性
      rfparam_init 保护）+ `btble_controller_init(configMAX_PRIORITIES-1)`
- [x] runtime bundle 的 BLE controller flavor 从 m2s1 切到 **uarthci**
      （controller-only 外部 host 库，m2s1 是片上 host 组合 flavor、
      PLF_UART=0 不带 eif 表）：重建 libbtblecontroller_bl616cl_uarthci.a，
      platform.txt 改为 -lbtblecontroller_bl616cl_uarthci。
      ⚠️ bundle 生成器不产 WiFi/blestack 库也不处理 autoconf.h 手工段，
      重新生成后必须：合并 fresh WiFi 库、恢复 libblestack/libpka 等、
      补回 autoconf.h 的 CONFIG_POSIX undef 块与 MACSW_SELECT_INCLUDE
      （缺 CONFIG_POSIX undef 会触发 libstdc++ gthreads 头错误）
- [x] 链接验证：`uart_api` 表（uarthci lib arch_main.o）与我们的
      `btble_uart_*`（ble_hci_port.cpp.o）正确解析
- [x] 烧写流程打通：进 ISP = DTR 拉高 BOOT + RTS 脉冲复位（picocom
      `--lower-dtr --lower-rts` 或 python 脉冲）；烧写 = BLFlashCommand
      `--interface=uart --chipname=bl616cl --port=<ROM CDC 设备>`
      —— ROM 把 ISP 桥接到 USB CDC（Generics 0xFFFF:0xFFFF），CDC 才是
      烧录口；FT232 UART 上的 ROM ISP 不响应（FTDI DTR# 低有效与工具
      CH343 极性相反，工具的复位时序会把 BOOT 拉低）
- [x] AT+HCI 通路硬件验证：AT+HCIBEGIN/WRITE/READ/AVAILABLE 全通
      （二分定位 bisect 1/2），USB CDC 上 AT 服务器工作正常
- [x] **btble_controller_init 崩溃根因 = 链接时未预留 BT 交换内存**：
      SDK 例子链接带 `-Wl,--defsym,__LD_CONFIG_EM_SIZE=32`（EM 32KB），
      我们的 ELF 里 `__EM_SIZE=0`，ROM-code controller 无交换内存即崩。
      修复：platform.txt `compiler.c.elf.extra_flags` 加同样的 defsym；
      AT+HCIBEGIN 不再崩溃。已排除：rfparam、AT 通路、堆（heap_3→malloc
      →TLSF，空闲 131KB）、低 128K SRAM 布局（两构建一致）
- [x] HCI host→controller 通路打通：H4TL 的 RX 走 djob 延迟处理，该链在
      无 UART ISR 的传输上不工作（RW 任务有消息、队列清空、prevent_sleep=0
      但 djob 从不执行）。改由 HCIVirtualTransport 自己解析 HCI 组帧，
      直接调 `hci_tl_cmd_received(HCI_TL_H4=0, opcode, len, payload)` /
      `hci_tl_acl_tx_data_*`，不再喂 H4TL 的 armed read
- [x] HCI 首条事件回传验证：AT+HCIWRITE(HCI_Reset) → Command Complete
      （04 0F 04 ...）经 h4tl_write→eif write→AT+HCIREAD 回读成功
- [x] 第二条命令起事件不回传的根因：`hci_tl_cmd_received()` 读的是
      `hci_env.p_cmd_desc`（由 `hci_tl_cmd_get_max_param_size(opcode)` 的
      副作用设置），直接调用时描述符为 NULL、命令被当"不支持"丢弃。
      修复：解析器在每条命令前调用 `hci_tl_cmd_get_max_param_size()`。
      另外在 write 回调与 wake 路径同步调 `btble_ke_event_schedule()`
      排空 djob（tx_done 链）
- [x] **LE 广播命令序列实机全通**（状态均 0x00 成功）：
      HCI_Reset(0x0C03) → Set_Event_Mask(0x2001) →
      Set_Adv_Params(0x2006, 100ms/ADV_IND/37+38+39) →
      Set_Adv_Data(0x2008, flags + name "BL616CL") →
      Set_Advertise_Enable(0x200A, 1) 全部返回 Command Complete
      且重复 enable 返回 0x0C（Command Disallowed = 广播已在运行，
      佐证广播状态活跃）；广播开启后 20s+ 板子稳定无崩溃
- [x] Mac 蓝牙授权完成；Swift CoreBluetooth 扫描器可用
      （blueutil 2.13 无 LE 扫描，--inquiry 是经典查询），
      能扫到房间内大量 BLE 设备（iPad/PC/显示器等）
- [x] **UART0 静默悬案破解**：实验台 FT232 接的是 UART1 GPIO8/GPIO9
      （不是 UART0 GPIO34/35）。此前用未打重映射补丁的本地 SDK 构建的
      固件控制台在 UART0 → 自然静默。已把控制台重映射补丁应用到本地
      SDK（board.c + macsw_bare/main.c），重建 adv 例子后 FT232 控制台
      完全恢复（boot banner + 周期日志全可见）
- [x] SDK adv 例子（m0b1 beacon-only）运行验证：3 个广播周期全部
      err=0、adv_start OK、无内存泄漏；RF 参数走默认（pwr_ble=13，
      eFuse 槽全空：no pwr_offset/no capcode）
- [x] WiFi 全链路复测通过（同一块板、同一 RF 前端）：扫描 16 AP、
      连接 bts + DHCP（192.168.184.138）、ping 网关 17ms、ping 公网
      9ms、ping 域名 207ms —— WiFi 收发完全正常
- [x] **BLE 空口问题根因定位（2026-08-29 判别实验，推翻"RF 已工作"）**：
      0) 撤回前一日结论：空口看到的 "testblezrr" 是用户的另一块实验板发出
         的（用户澄清）；本板自己的广播名字从未在空口出现。milestone 标签
         milestone-ble-adv-on-air 描述不再成立（见 repo tag 说明）
      1) 判别实验 A：btble_cli（m2s1 完整栈）在本板用唯一名 ZZTESTSDK
         广播 → 空口可见 17 次/25s（RSSI -44dBm）→ RF 硬件/前端正常
      2) 判别实验 B：官方 SDK 参照 btblecontroller_test（uarthci 纯
         controller + 真 UART H4 host，HCI UART 补丁到 GPIO8/9 @2M）：
         全部 HCI 命令 CMD_COMPLETE status=0（含 ADV_ENABLE），
         HCIIN/unpack/LLM 分发全正常 —— **但空口 0 次**。排除：
         虚拟传输（真 UART 同样结果）、rfparam（TLV 缺失会直接
         "PHY RF init failed" 拒绝启动，已用 btble_cli 镜像 0x1400 处的
         2048B TLV 拼入修复）、BTDM 复位（清不掉）
      3) 真根因（证据链）：空口的 ZZTESTSDK 广告与我们 HCI 的
         enable/disable 严格同步、RSSI -44、把本板按在复位里就消失 →
         **空口广告是本板 BT core 的陈旧自主状态**（上一次 m2s1 会话的
         残留），uarthci 的 LLM 无法重新编程它：探针证实 LLM 收到正确
         adv data 并 em_wr 进 EM（new_buf=0x1054, len=16 全对），
         EM CS 区也被写入调度结构 —— 但 BT core 持续发送旧内容。
         uarthci 在 BL616CL 上是上游未验证路径：README 支持列表无
         BL616CL；rwip_config 的 HCI_UPDATE_UART_CONF 只开 BL616；
         btblecontroller_software_btdm_reset 也只编译 BL616/BL618DG
      4) 附带根因：SDK 例子的 rfparam TLV 位于镜像内 0x1400（XIP
         0x80000400，boot2 设 XIP 偏移 0x1000），TLV 魔数
         BLRFPARA+O6DkXb1k；btblecontroller_test 构建只生成 HEAD1、
         HEAD2 为空 → rfparam_init 失败。`--firmware=<bin>` 是整包从
         0x0 烧写（flash 内容 == bin 偏移已验证）。对照：bridge 镜像
         0x1400 有完整 TLV（platform.txt 的 bflb_fw_post_proc 注入）
         → rfparam 通过 → WiFi 可用；Blink 镜像同样只有 HEAD1。
         rfparam 失败 ⟺ WiFi 不可用（板上 eFuse rcal 正常：xtal 40MHz、
         icx 34、iptat 11），与本板 RF 无关，也非 BLE 不上天的原因
      5) 方向：a) 问 Bouffalo（uarthci+BL616CL 的 LLM→BT core 编程
         路径）；b) 换 m2s1 flavor + 我们的虚拟传输直灌 controller
         （不启动片上 host，需验证 m2s1 的 HCI 入口）；c) 完整栈放
         BL616CL + AT 高层命令（RA4M1 协议需改）
- [x] 实验台经验（已入 README/TODO）：FT232 开端口会拉低 RST 复位芯片
      （CDC 掉线根因）；SDK 例子烧写必须镜像+分区表一起烧（0xE000 最后写，
      否则 easyflash 失败栈溢出）；btble_cli 的 shell 也要 UART1 重映射；
      bridge 的 USB 初始化会破坏 printf 的 console 绑定（调试打印须用
      bflb_uart_putchar 直写）；HCI Set_Adv_Data 的 payload 必须带
      adv_data_len 前导字节且补齐到 CMD 表要求的 32 字节（"B31B"）
- [x] 台架 BOOT 悬空失效模式（2026-09-17 破案）：FT232 关端口后 DTR
      释放把 BOOT 拉高，任何意外复位都让芯片静默进 ROM ISP（无 UART、
      无 USB，形似挂死）。规程：调试期间保持 FT232 打开且 DTR=1；根治
      需给 BOOT 加下拉。当天两次"无人值守挂死"均此模式
- [x] BLE 硬件复核（2026-09-18）：btble_cli（m2s1）烧写+shell 驱动
      广播（ZZHWTEST918，ble_start_adv 0 0 走默认间隔）→ 空口 22 次/15s、
      RSSI -38~-63dBm。RF 硬件再次确认正常。注意 CLI 差异：
      ble_set_device_name 只带名字不带长度；ble_start_adv 带间隔参数会
      除以 1.6，换算后 <0x20 报 err -22（EINVAL），用两参数形式最稳
- [x] 应用侧初始化补全 + 判别实验（2026-09-18，ble_hci_port.cpp）：
      0) AT+HCISTATE? 未启动时崩溃根因 = hci_transport_state() 里
         eTaskGetState(NULL)/uxQueueMessagesWaiting(NULL) 触发 FreeRTOS
         断言。已加 NULL 守卫（未启动报 eInvalid/0）
      1) AT+HCIWRITE 在 HCIBEGIN 之前会把控制器打进 ke_mem 断言
         （ke_mem.c:319）。HCIVirtualTransport 加 s_controller_started
         门禁：未启动 write() 返回 0，AT 快速失败（0.2s ERROR）
      2) EM 判别实验：EM_SEL 探针（GLB_SRAM_CFG3 bits3:0）实测复位
         默认 = 3（WRAM128K/EM32K），与 bridge 链接的 EM 32KB 一致 →
         **EM 硬件窗口假设被证伪**（m2s1 不配置也能上天是因为默认档
         恰好够 16KB；bl_sys_em_config() 只是同步链接值到硬件）
      3) 补 rf_init(40MHz)（uarthci 对 CL 是空分支，符号在
         libbl616cl_phyrf.a）+ bl_sys_em_config()（no-op，保留）+
         BTDM 域复位（等效被编译掉的 software_btdm_reset）：全部无
         崩溃且 HCI 通路正常，但空口仍 0 包
      4) 补 PDS 域复位（等效 software_pds_reset）：第一条 HCI 命令
         当场打崩芯片（USB 掉线）→ 已移除。PDS 域与 BLE 核状态强关联
      5) 结论：应用侧补丁到极限，问题锁定在 uarthci 库的 flavor 配置。
         编译定义对比：BL616CL 的 uarthci 构建吃的是 BT+BLE 组合档
         （BREDR/BIS/CIS/EM32、无 CONFIG_BLE_PDS），而非 BLE-only 档
         （BLE_PDS+ROM code，ble_common_readonly.cmake:594）
- [ ] 下一步实验序列：a) 重编 uarthci lib 加 -DCONFIG_BLE_PDS（单变量，
      lib 与 bridge 链接的 EM 大小必须同步改）；b) 必要时放开
      arch_main.c:1416 的 pds_reset 门控（defined(BL616CL)）；
      c) m2s1 + 虚拟传输直灌（需验证 m2s1 的 HCI eif 入口）；
      d) 问 Bouffalo CL 的 BLE controller ROM 是否存在
- [x] 实验序列执行结果（2026-09-18 下午，按序）：
      a) **PDS lib 重编（1a）**：btblecontroller_test 构建目录单变量加
         -DCONFIG_BLE_PDS（需把 Xuantie bin 加入 PATH 否则 ar 找不到；
         新 lib md5 fcc5dfa3）。结果：稳定、HCI 全通、空口仍 0 包 →
         无效但无害
      b) **pds_reset 放开 CL 门控（1b）**：同时补 port.c:215 和
         arch_main.c:1416 两处门控（只改一处会缺符号）。结果：AT 任务
         在 HCIBEGIN 期间卡死（init 走完、alive 还在打、AT 不应答）
         → 与外部 PDS 复位崩溃同类，**PDS 复位路径在 CL 上被否决**
         （大概率 CL 的 BLE controller ROM 无 PDS-aware 恢复路径）。
         两处补丁均已回退（arch_main 留了 REJECTED 注释）
      c) **m2s1 直灌（2/2b）**：nm 确认 m2s1 库导出 rwip_eif_get +
         hci_tl_cmd_received/get_max_param_size/acl_tx_*（与 uarthci
         同机制），platform.txt 换 -lbtblecontroller_bl616cl_m2s1 后
         链接成功（816KB）。结果：HCIBEGIN 正常，但**第一条 HCIWRITE
         返回堆损坏图案（\xa5\xa5 填充）且 AT 任务死亡**；加
         hci_driver_init()（libblestack.a，host 侧描述符初始化）后
         **完全相同**。结论：m2s1 的 hci_tl 是内嵌 host 槽位模式，
         直接注入需要重接 HCI slot（等于给 CL 重做 external-host 库）
      d) **基线已恢复**：platform.txt 回 uarthci、boards.txt 去掉
         -DUSE_M2S1_CONTROLLER、ble_hci_port.cpp 的补全按 flavor 条件
         化（uarthci=rf_init+BTDM reset+m2s1=hci_driver_init 门控保留），
         实测 AT/HCIBEGIN/HCI Reset CC/HCISTATE 全部正常
      e) m2s1 相关产物留在库里备用：libbtblecontroller_bl616cl_m2s1.a
         （btble_cli 构建产物）、uarthci 原始 lib 备份
         .bak-0917、PDS 构建方法论（改 flags.make + Xuantie PATH）
- [x] **深度取证与大实验矩阵（2026-09-18 晚，全源码级）**：
      ⚠️ 重要背景：用户澄清这是 Bouffalo 内部研发 SDK（全源码），
      所有"厂商问题"结论作废，一切都可直接查/改/重编。
      0) **插桩基础设施（保留在库源码里，-DBT_BRINGUP_TRACE 开关）**：
         rwip_driver.c/sch_arb.c/sch_prog.c/lld_adv.c/rwble.c 共 15 个
         trace 探针（AARM/ADIS/T1ISR/ESTR/AES/AEP/SPP/APD/AINS/FIFOISR/
         TXISR/RXISR/ENDISR/BLEISR/SWISR + actfifostat 直读 AFS=xxxx），
         btble_trace 由 bridge 直写 UART0；btble_cli 加 ble2dump/blewide/
         emdump 命令。重编方法：btblecontroller_test/build_btblecontroller
         目录 make（PATH 加 Xuantie bin），cp 到 tools/sdk/bl616cl/lib/
      1) **最关键实测结论：MAC 侧从未发射**——广播序列全通后：
         FIFOISR×79 + ENDISR×79（MAC 在跑事件循环、ET 索引 11..14 前进），
         但 **TXISR/RXISR/BLEISR 全部为 0**；actfifostat 原始值
         =0x0X008002（只有 ENDACT，无 TXINT/RXINT）。反向扫描实验
         （LE_Set_Scan_Parameters+Enable 走 HCI）10 秒**零条 adv report**
         （房间里 400+ 设备在广播）→ **TX/RX 双哑 = RF/PHY 域不通**，
         不是调度层、不是 HCI 层
      2) 工作态（btble_cli/m2s1）寄存器全量快照 vs 静默态（bridge/
         uarthci）逐字节 diff（112 寄存器）：仅 12 处差异，全部是
         "运行态 vs 未运行"，含 IP-ic1 的 bit5(TGT1) 差、ACTSCHCNTL=0
         差；BLE 核 cntl/ver/conf 完全一致（00500707/0b001300/6d02d090）
      3) 已排除的假设（每个都做了实机单变量实验）：
         - WLAN coex（唯一 m2s1 没有 uarthci 有的功能性 flag）：改
           rwip.c 两处 rwip_wlcoex_set(1)→(0)，COEX0/1 读数 00000000，
           空口仍 0 → 排除
         - EM 硬件窗口：EM_SEL 探针 before=3 after=3（默认已 32K 档，
           与链接一致）→ EM_SEL 假设证伪
         - EM=16 单变量（对齐 m2s1）：HCI 直接不通（更糟）→ 回滚 32
         - BTDM 域复位（我们补的）：移除后无变化 → 排除
         - PDS 域复位（1b 已记录）：炸 → 排除
         - RF 侧 rf_init(40MHz)（uarthci 对 CL 是空分支，已补并保留）：
           执行正常但 TX 仍 0
         - ROM 数据指针（btble_data_set 0x20000B9C）：编译门控一致，
           当前构建走 flash 版全局变量，机制惰性 → 排除
         - BTBLE_RF_SWITH_TO_COMBOPATH：uarthci=0、m2s1 无此代码 → 排除
      4) 两边库同源确认：m2s1 和 uarthci 都从 btblecontroller 树编译
         （build.make 证据），唯一差异 = flags.make 的编译定义集合
         （m2s1 独有 CONFIG_BLE_HOST/BLE_PDS/EM16；
          uarthci 独有 ISO 全家/BLE_TEST_MODE_SUPPORT/LE_PWR_CTRL/EM32）
      5) 下一步实验候选（按性价比）：
         a) uarthci lib 去掉 uarthci 独有 extras（-DCONFIG_LE_PWR_CTRL、
            -DCONFIG_BIS/-DCONFIG_CIS/-DCONFIG_EXT_ADV/ISO_*）重编——
            LE_PWR_CTRL 直接碰 RF 功控路径，最可疑
         b) dump RF/PHY 寄存器（drivers/soc/bl616/phyrf 的寄存器图）在
            两种状态下的差异，定位 PHY TX 未使能的寄存器位
         c) m2s1 库 + 单改 hci_tl 槽位（重接 HCI slot 到虚拟传输）
         d) 用 m2s1 的 btble_controller_init 完整替换 uarthci 的
            （两者同源同签名，仅 init 序列差异已知）
- [ ] ⚠️ 当前部署状态（2026-09-18 晚）：tools/sdk 里的 uarthci lib
      是"带 trace + coex(0) + EM32 回滚"的实验版；platform.txt 的
      extra_flags 全部回滚（EM=32）；ble_hci_port.cpp 保留 EM 探针 +
      rf_init + 宽域 dump + btble_trace；patch 后 bridge 固件 HCI 全通、
      广播仍不上天（未修复）。若需干净基线：去掉 BT_BRINGUP_TRACE
      define 重编 lib 即可（探针变 no-op）
- [ ] 收敛后的根因判断：应用侧与编译配置侧能补的都补了（EM/RF/BTDM/
      PDS/host-driver），BLE MAC 仍不调度 → 剩余差异在 CL 的 BLE
      controller ROM 内容或 uarthci 库与 CL ROM 的配对上，属厂商问题。
      最优路径：向 Bouffalo 提供"uarthci+CL 全 CC 成功但空口 0 包 +
      m2s1 同板上天"的判别数据要答案；或自建 CL 专用 external-host
      controller 库（工作量=重做 Bouffalo 的 flavor 生成）
- [ ] SDK 探针清理：btble_dma_uart.c/btblecontroller_port_uart.c/
      llm_adv.c/btblecontroller_test main.c 的调试探针与引脚补丁是本地
      实验状态（用户维护的 SDK 副本），后续需还原或移入 patch 存档
- [ ] 实验台 UART0 控制台静默：连纯 SDK btblecontroller_test 例子
      （BFLB_LOG=y）board_init 横幅都不输出；23:22 时 macsw shell 响应
      还正常，此后 FT232 数据线可能被改动/断开（RST/BOOT 控制线正常）。
      需要确认 FT232 是否仍接在 BL616CL GPIO34/35
- [ ] 崩溃诊断工具已就位：`cores/bl616cl/crash_debug.cpp`（覆写
      exception_entry，mcause/mepc/mtval 直推 CDC IN EP 0x83）+
      AT+GETCRASH 命令。经验：SRAM noinit 区跨复位被清（计数器实验），
      不能跨 boot 记录；exception_entry 覆写必须放 cores（放库会被 GC）
- [ ] 后续：确认 FT232 接线 → 重跑 SDK btblecontroller_test 差分实验
      （BFLB_LOG=y 版本已构建）→ 定位崩溃 → HCI host 驱动 LE 广播
      （Set_Adv_Data/Params/Advertise_Enable）→ 手机扫描可见

- [x] **⚠️ 重大发现：插桩破坏 BLE 发射（2026-09-18 深夜，可复现）**：
      纯净源码重建的 m2s1 btble_cli **重新上天**（ZZHWTEST918，15s 内
      13 次命中、RSSI -42~-51dBm）；而加了 BT_BRINGUP_TRACE 探针的
      同一份 m2s1 **不上天**（0 命中）。根因：探针在 ISR 里做 UART 输出
      （2Mbaud 下每条约 40us），远超半 slot(312us) 的时序裕量，直接
      破坏 MAC 事件编程/触发的实时性。
      **教训与纠正**：
      1) 此前基于插桩的核心观测——"FIFOISR/ENDISR 循环但 TXISR/RXISR/
         BLEISR 永远为 0"——**不可信**，那是插桩自身的时序破坏，
         not uarthci 独有现象。该结论作废。
      2) 所有 BT_BRINGUP_TRACE 探针仅可用于非实时路径（init 阶段），
         ISR/sch_prog/lld 路径上的探针必须移除或改成"只写 SRAM 计数器"
         （几纳秒，不碰 UART），后续诊断一律用 SRAM 计数器法+事后读取
      3) 插桩源码已 git stash（stash@{0}: bringup-instrumentation-
         20260918），需要时谨慎恢复
      4) 纯净性恢复：uarthci 库回滚到 .bak-0917（md5 3bc9f344）；
         m2s1 纯净重建；bridge 侧 BZ-arm/wide-dump 等实验代码已清理，
         固件尺寸回到 934,962B 基线
- [x] 基线复现（2026-09-18 深夜，纯净环境最终验证）：
      - 纯净 m2s1 btble_cli：**上天**（13 次命中，复现上午结果）
      - 纯净 uarthci bridge：HCI 五条命令全 CC 成功、空口 0 命中
        （540 设备扫描）——"uarthci 不上天"是真实、独立于插桩的现象
- [x] **RF 寄存器三方对照法（2026-09-18）**：同固件两次运行间就有
      38 个 RF 寄存器因校准噪声不同 → 排除 30 处假差异；真差异仅 7 处：
      TBB(0x20001058 bit8)、RBB2(0x20001070)、5 处校准区
      (0x1324/0x132c/0x133c/0x1368/0x13d4)。TBB=TX基带、RBB2=RX基带，
      方向正确但单独调整无效（BZ-arm 实验已试）
- [ ] **下一步（插桩不可用后的正确方法）**：
      1) SRAM 计数器探针：ISR 里只 `counter[tag]++`（SRAM noinit 区），
         空闲任务周期性打印——不破坏时序又能观测中断分布
      2) 用 SRAM 计数器对比纯净 uarthci 与纯净 m2s1 的中断分布
         （T1ISR/ESTR/TX/RX/FIFO/END 各计数多少）
      3) 重点验证：uarthci 的 sch_prog_tx_isr 是否真被调用、ET 状态机
         是否推进到 TX 触发（对比 m2s1 同点位计数）
      4) 若中断分布相同：对比两个库反汇编（同 object 文件 diff），
         定位指令级差异——既然同源同编译器，差异必来自宏定义分支
- [ ] 单变量排除实验累计（均无效，2026-09-18）：PDS lib 重编、
      pds_reset 门控、ISO 组(6 个 define)、LE_PWR_CTRL、BZ-arm
      (wl_cfg mode=2 + wl_init)、WLAN_COEX 置 0、EM_SEL、EM=16、
      BTDM 复位、rf_init 补全——全部排除后 uarthci 仍静默
- [x] **本轮排查最终收敛（2026-09-18 深夜）——完整报告见
      `docs/BLE-UARTHCI-BRINGUP-REPORT.md`**：
      1) **问题定性**：不是"Arduino vs btble_cli"差异，而是
         **uarthci flavor 在 BL616CL 上的固有限制**。决定性判据：
         官方 SDK 自带 uarthci 例程 btblecontroller_test 烧同一块板
         同样"HCI 全通、空口 0 包"→ Arduino 环境（USB/DAP/任务拓扑）
         全部排除。旁证：README 的 uarthci 支持列表无 BL616CL；
         software_btdm_reset / HCI_UPDATE_UART_CONF 仅为 BL616/618DG 编译
      2) **数据面已证正常**：修正测试脚本 adv_data_len 编码 bug 后，
         bridge 的 EM 里正确出现完整载荷（02 01 06 08 07 "BL616CL"
         在 EM+0x1054），与能上天的 cli 载荷偏移完全一致
      3) **调度软件链正常**：SRAM 计数器（不破坏时序）显示两侧中断
         分布同构（T1ISR/ESTR/SPP/FIFOISR/ENDISR 计数一致；
         TXISR/RXISR/BLEISR 两侧均 0，是常态）
      4) **EM 尺寸非根因**：反向实验——能上天的 cli 切 EM_SEL=3(32K)
         后仍发射；uarthci 切 EM=16 则初始化即崩（资源布局放不进）
      5) **RF 仅剩模拟校准级差异**：7 处（TBB/RBB2 + 5 处校准区），
         单独调整无效
      6) ⚠️ **方法论教训**：ISR 里写 UART 的 trace 探针会破坏 BLE 时序
         （40µs vs 312µs 半 slot），导致同一份 m2s1 从"上天"变"静默"。
         此前基于该插桩的观测全部不可信。正确方法 = SRAM 计数器。
         插桩已 stash（stash@{0}: bringup-instrumentation-20260918）
      7) **下一步（推荐）**：本报告即复现单，交 controller 团队；
         诉求 = BL616CL 是否支持 uarthci / 为何数字域全通而 MAC 不发射

## 第七阶段：core 无线初始化 + BLE 完整栈（2026-09-18 晚）

- [x] **core init() 收口无线初始化**（用户指正：不应为空）：
      新增 `cores/bl616cl/bl_wireless.cpp` 的 `bl_wireless_init()`，
      由 `wiring.cpp` 的 `init()` 在调度器启动前调用（等价 SDK 例程在
      main() 里调 rfparam_init 的位置）：
      1) `rfparam_init(0,NULL,0)` —— RF 校准参数加载（WiFi/BLE 共享，
         全平台一次）；2) `bl_sys_em_config()` —— 按链接的
         __LD_CONFIG_EM_SEL 编程 GLB EM/WRAM 划分（BT controller 仅在
         beacon-only 构建里自己做，uarthci/m2s1 组合 flavor 需要平台补）。
      探针：启动横幅后打印 `[rfparam] ret= wl= cfg_status= mode= em_sel=`。
      注：phyrf 的 `rf_init()` 有意不调——SDK 所有 BL616 系例程都不调，
      WiFi 走 wl_init/phy_init、BLE 走 btble_rf_init 自管理。
- [x] esp32-compat 的 `ensure_rfparam()` 改为 core 的薄包装（幂等），
      `ble_hci_port.cpp` 里重复的 EM_SEL 写入/rf_init 补齐删除（下沉到 core）
- [x] **修复 lwIP 随机数钩子缺失**：bundle 的 lwipopts 把 LWIP_RAND()
      映射到 bl_rand()，mbedtls 以 --whole-archive 链接会拉入 igmp.c
      引用它 → 纯 sketch（Blink 等）链接失败。core 侧提供 weak `bl_rand`
      （esp32-compat 的强符号在 Bridge 构建里照常覆盖）。
      修复后 Blink 226,546B / Bridge 937,010B 均编译通过。
- [x] **修复 WiFi 事件被静默丢弃（既有 bug，非本次改动引入）**：
      A/B 对照（core 初始化启用 vs 禁用）扫描都是 ~5.5s 超时返回 -1。
      根因：bundle 的 `sdk/wifi6/wifi_mgmr_ext.h` 仍是 fhost 档的
      `EV_WIFI 0x0002`，而实际链接的 wl80211 库用
      `EV_WIFI = (uintptr_t)wifi_mgmr_init`（wl80211/include/wifi_mgmr.h）
      注册/投递事件 → esp32-compat 注册的 filter 永不触发，
      g_mgmr_started 永远为 false。修复：esp32_wifi.cpp 里
      `#undef EV_WIFI` 后按库的值重定义。修复后 AT+WIFISCAN 3.5s 返回
      11+ AP（CMW-AP-1/vela/tplink334…，RSSI -21~-40）。
- [x] **BLE 完整栈在 Arduino 工程内上天（首次）**——按用户提示的
      `examples/wifi/sta/smartconfig_ble`（BL616CL WiFi+BLE 共存官方
      demo，用 m2s1 而非 uarthci）复刻：新增
      `examples/BleAdvTest`，顺序 = bflb_mtd_init/easyflash_init →
      btble_controller_init → hci_driver_init → bt_enable(cb) →
      bt_set_name/bt_le_adv_start。构建用 btble_cli 产出的
      libbtblecontroller_bl616cl_m2s1.a + libblestack.a + libblemesh.a
      （blemesh 提供 adv/loopback/friend buf pool 符号），
      `__LD_CONFIG_EM_SIZE=16`，454,706B。
      实机：bt ready（BD_ADDR B4:E8:42:3C:A7:DD）、adv_start ret=0；
      CoreBluetooth 扫描 45s 命中 12 次 `ZZARDUINO918`
      （RSSI -40~-65，Connectable=1，厂商数据 "BL616"）；
      429 台设备共存环境。**结论：BLE 在 Arduino 工程里可用，
      前提是走 m2s1 完整栈，而非 uarthci external-host。**
      遗留：EasyFlash 因分区表无 PSM 分区报错（不影响广播），
      连接/配对路径未测。
- [x] **里程碑记录**：已提交并打标签 `milestone-ble-adv-m2s1-fullstack`
      （取代 8/29 失效的 milestone-ble-adv-on-air）；复现构建命令见
      `examples/BleAdvTest/BleAdvTest.ino` 文件头注释；
      调查报告附录更新于 `docs/BLE-UARTHCI-BRINGUP-REPORT.md`。
- [x] 新增无 pyserial 依赖的 AT 测试脚本（本机 python3 无 pyserial，
      全部用 stdlib termios；位于工作区根目录）：`at_test_termios.py`
      （AT+HCI 冒烟）、`at_cmd_termios.py`（单条 AT 命令+超时）、
      `console_while_at.py`（发 AT 期间同步抓 UART0 控制台）、
      `reset_board.py`（RTS 脉冲复位、DTR 保持 BOOT 低）
- [ ] 桥接固件 BLE 架构选型（待定）：
      a) 维持 uarthci + RA4M1 host（现状，空口不发射，等 controller 团队）；
      b) 改 m2s1 完整栈 + BL616CL 自广播（已验证可行），RA4M1 侧
         协议需从 HCI 透传改为高层 AT 命令；
      c) 双模并存需评估 RF/EM 资源。

## 第八阶段：BLE 团队 EM 假说复核 + 配置对齐实验（2026-09-21）

- [x] **BLE 团队的"EM 被二次覆盖"假说被证伪**（对方分析我们的
      ELF/map：bl_sys_em_config → GLB_Set_EM_Sel(2)，认为 2 走
      default → 16KB）：
      1) 枚举定义 `GLB_WRAM128KB_EM32KB = (2)`（bl616cl_glb.h:566），
         GLB_Set_EM_Sel 按**枚举** switch：case 2 → 写字段 0x03（32KB）；
         对方把 case 标签误读成了寄存器值
      2) 反汇编确认：bl_sys_em_config 常量折叠出 em_size=0x8000，
         else 分支传 a0=2（=枚举 32KB），调用 ROM GLB_Set_EM_Sel
      3) 运行时探针（BridgeCoreInit 启动日志）在 bl_sys_em_config
         之后实测 em_sel=3 —— 与链接值一致，无覆盖
- [x] **配置对齐实验矩阵**（目标：复刻 BLE 同事"btblecontroller_test
      1.6.200 + 真 UART H4 能上天"的实验，全部在我们板上）：
      | btble lib | phyrf | 传输 | 结果 |
      |---|---|---|---|
      | 1.6.198 | 旧(8/5) | 真UART(8/29) | 静默 |
      | 1.6.198 | 旧 | 虚拟(bridge) | 静默 |
      | 1.6.200 | 旧 | 虚拟(bridge) | 静默 |
      | 1.6.200 | 旧 | 真UART | **静默** |
      | 1.6.200 | **新(9/3)** | 真UART | **静默** |
      | m2s1 | 旧 | 片上host | **上天**（BleAdvTest） |
      | 1.6.200(同事板) | 新(推测) | 真UART | **上天**（同事实测） |
      每轮命令面全通（6 条 CC，re-enable=0x0C），空口 0 命中
      （425/447 台设备可见，扫描器正常）。
      真UART 实验 = stock btblecontroller_test（bin 自带 rfparam TLV，
      0x1400 BLRFPARA，8/29 的 TLV 缺失问题新版已修）+ HCI UART 重映射
      UART0 GPIO34/35 关流控（bench FT232 当 H4 主机，
      h4_host_termios.py 驱动 H4 序列）。
- [x] **结论收敛**：软件侧（controller 库版本、phyrf 版本、flavor、
      传输层、命令序列）已与同事的可用配置完全对齐，我们这块板依然
      静默；同板 m2s1 广播强信号、WiFi 双向正常 → RF 硬件 TX 通路
      正常。剩余变量只在**板级/芯片个体**：
      a) eFuse RF 参数（本板：slot 全空、no capcode、TLV 默认 128；
         若同事的板有 capcode/功率偏移且 uarthci 路径对其处理不同，
         可解释"同软件不同板一通一不通"）
      b) 芯片个体/Revision（chip id 本板 = dca73c42e8b4，ISP 日志
         flashcube_20260921.log 可查）
      → 需同事配合：提供他们板的 rfparam 启动日志（对比 eFuse
         slot/capcode 行）+ chip id；或在他们板上烧我们的静默 bin
         （md5 ae445cb7，UART0-HCI 版）交叉验证
- [x] **工作区状态变更（实验后保留）**：
      - bluetooth 仓库：detached 于 43096480（=1.6.200）；原 HEAD
        c02b93c6；SRAM 计数器探针已 stash（stash@{0}
        sram-counter-probes-20260918，原 bringup-instrumentation
        顺位 stash@{1}）；port_uart_conf.c 补丁为 UART0/34-35/无流控
        （原文件备份 /tmp/port_uart_conf.c.orig）
      - phyrf 仓库：detached 于 origin/master 3722703（新版 lib
        md5 f5fda106）；原 825435a
      - Arduino bundle：libbtblecontroller_bl616cl_uarthci.a 已换
        1.6.200（md5 e99d17b0；1.6.198 版备份
        /tmp/libuarthci_1.6.198_backup.a）；ble_hci_port.cpp 加
        btble_bringup_ctr[16] 零值 stub（正式库无探针，AT+BLECTR
        返回全零）—— 两处均未提交
      - 板子当前运行：btblecontroller_test（1.6.200+新phyrf+UART0-HCI
        补丁，bin md5 ae445cb7）
      - 新工具（工作区根）：h4_host_termios.py（FT232 H4 主机）、
        ble_adv_termios.py（CDC 驱动完整广播序列）

## 第九阶段：官方代码可用性的复核（2026-09-21 上午，进行中）

- [x] **用户澄清（推翻第八阶段"板级差异"结论）**：同一块板，官方
      SDK 代码（btblecontroller_test + 串口工具发 HCI）能正常发空口包
      → 板子没问题，差异在软件
- [x] 全树对齐到同事可用窗口：主仓库 dfd20103（8/21，bluetooth 固定
      1.6.200 的窗口），子模块按该窗口 gitlink 固定（bluetooth 保持
      43096480=1.6.200），phyrf 新旧两版都测过
- [x] **关键自查：我今天所有"官方例程"测试都带了 UART0-HCI 补丁**
      （台架 FT232 只接 GPIO34/35，官方 HCI 在 UART1 27-30）——
      这个补丁在不同树上表现不同（旧树 HCI 通、对齐树 HCI 死），
      是污染变量。官方可用验证用的是默认 UART1 引脚
- [x] 已构建并烧录**完全无补丁**的官方固件（bin md5 37d2c10e，
      shell 开、HCI=UART1 27/28@2M 流控），等待串口工具在 GPIO27/28
      上复测
- [ ] 待用户提供：a) 串口工具的接线方式（哪个 UART/引脚、波特率、
      流控），以便本机复刻；b) 那份已知可用的 bin（在同事机器上），
      烧到本板做 A/B + map diff，直接定位软件差异

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
