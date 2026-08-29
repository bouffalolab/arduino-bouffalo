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
- [x] **BLE 空口问题根因定位（软件侧追踪完成）**：
      1) 对照 btble_cli（m2s1，可出波）抓取了片上 host 的内部 HCI 序列
         （hci_driver 加打印）：开广播的命令序列与我们完全一致，
         无任何特殊命令 → 命令序列不是原因
      2) 我们的 uarthci 路径其实**能出波**：AT 使能/停止广播在空口上
         可验证（-38~-49dBm）——此前"不上天"的假象是两件事叠加：
         a) H4TL 被绕过导致 prevent_sleep 的 RW_TL_1_RX_ONGOING 卡位，
            LL 不调度 RF 事件（已修复：解析后 clear 0x400）
         b) 广播内容一直显示 "testblezrr"（btble_cli 时代的残留），
            掩盖了"我们的广播在发射"的事实
      3) 外部 HCI 的 Set_Adv_Data 内容不生效：HCIIN 探针证实命令描述符、
         ll_dest=BLE_MNG、TASK_LLM 分发、unpack 全部正常，但空口内容
         仍是残留的 testblezrr → **EM（交换内存，wifi RAM 区）跨热复位
         残留**，LL 广播活动/数据缓冲带着旧状态；外部 HCI 的 adv data
         更新路径（llm_adv.c 的 hci_le_set_adv_data_cmd_handler 已确认
         存在且走 em_wr）与残留活动之间存在错位
      4) 下一步修复方向：(a) BLE init 时清空 EM 区域（需确认 EM 基址：
         __EM_SIZE=32K 位于 wifi RAM 区顶部）；(b) 或向 Bouffalo 确认
         外部 host 模式下 adv 数据缓冲/活动的正确更新路径
- [x] 实验台经验（已入 README/TODO）：FT232 开端口会拉低 RST 复位芯片
      （CDC 掉线根因）；SDK 例子烧写必须镜像+分区表一起烧（0xE000 最后写，
      否则 easyflash 失败栈溢出）；btble_cli 的 shell 也要 UART1 重映射；
      bridge 的 USB 初始化会破坏 printf 的 console 绑定（调试打印须用
      bflb_uart_putchar 直写）
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
