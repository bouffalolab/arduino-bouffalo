# BL616CL UNO R4 Bridge TODO

## 代码核查状态（2026-10-04）

下面的状态以当前 `arduino-bouffalo` 源码、bridge 命令处理器和已有实机
冒烟记录为准。`[x]` 表示该功能在代码中有真实后端且已有验证；只提供头文件
或返回固定失败值的兼容接口不计为已支持。

| bridge 依赖 | 状态 | 结论 |
| --- | --- | --- |
| USB CDC ACM + CMSIS-DAP HID | 已支持 | CherryUSB 后端已实现；DK 上枚举、CDC 收发和 DAP 命令往返已验证。SWD 时序、最终载板接线仍待验证。 |
| WiFi STA / 扫描 / DHCP / 静态 IPv4 / DNS / ping | 已支持 | `esp32_wifi.cpp` 使用 wl80211/`wifi_mgmr`，对应 AT 冒烟已通过；SoftAP/APSTA、IPv6、自动连接和持久化设置仍未实现。 |
| TCP client/server、UDP | 已支持 | `WiFiClient`、`WiFiServer`、`WiFiUDP` 使用 lwIP socket，AT echo 已通过。 |
| TLS client | 已支持 | `WiFiClientSecure` 使用 mbedTLS v3；CA 装载、TLS 1.2 和 HTTPS GET 已通过。 |
| SPIFFS/FS、Preferences | 代码已支持，镜像布局阻塞 bridge | LittleFS/EasyFlash 后端和 `StorageTest` 已通过；bridge 完整镜像仍会与 0xE000/0xF000 分区表槽位重叠，修复布局和上传流程前不能宣称部署可用。 |
| BLE HCI 透传 | 已支持（范围有限） | AT HCI 队列、H4 解析和 controller 初始化已实现；2026-10-03 用公共 SDK v2.3.36 自带的 uarthci 1.6.210 归档实机验证 Reset/LE 广播序列并在空口发现 bridge 广播（详见 `docs/BLE-UARTHCI.md`）。其他 HCI/ACL/连接命令和长期稳定性仍未测。 |
| SoftAP / APSTA | 不支持 | `WiFi.softAP*()` 和 AP 事件仍是固定失败/空返回。 |
| RA4M1 OTA / BOSSA | 不支持 | `Update.h`、`Arduino_ESP32_OTA.h`、`BossaArduino.h` 是失败桩；`BossaUnoR4WiFi::program()` 无法完成 SAM-BA 刷写。 |

桥接工程当前可以完整编译链接，但 AT 命令表中调用未实现 API 的命令仍会失败；
因此“能编译”与“完整 UNO R4 bridge 可用”必须分开记录。

## 当前进度

- [x] 第一阶段：ESP32 Arduino API 兼容骨架
- [x] 第二阶段：BL616CL CherryUSB CDC ACM + HID 后端
- [x] 第二阶段硬件验证（USB 部分）
- [x] 第三阶段：WiFi STA / TCP / UDP / TLS（SoftAP 仍未实现）
- [x] Runtime 由 SDK 子模块现编；legacy bundle 与 vendor 归档已全部移除
      （2026-10-03）
- [x] BLE HCI 透传：公共 SDK v2.3.36 自带 uarthci 1.6.210，实机验证通过
      （2026-10-03，详见 `docs/BLE-UARTHCI.md`）
- [ ] 第四阶段：OTA 与 bridge 镜像布局（SPIFFS/Preferences 后端已完成）
- [ ] 第五阶段：完整 UNO R4 板级适配

## 第二阶段收尾

- [x] 核查 arduino-bouffalo PR #10（2026-09-29，head `e6c3179`）：
      该 PR 引入独立 bridge runtime profile、临时载板 variant 和 UART/runtime
      改动，未包含 SWD 时序实现或校准结果；PR 自身仍将下面的时序校准项
      标为未完成。已获取到本地 `upstream/pr-10`，尚未合入当前分支；合并
      预检查发现 `platform.txt` 和 runtime bundle 生成脚本有内容冲突。
      当时 bridge `dap_config.h` 仍使用 ESP32 引脚 SWCLK=7 / SWDIO=8
      与延时常数 7700 / fast clock 2400000 Hz，尚未按载板引脚适配
      （2026-10-04 落实为 SWCLK=GPIO8 / SWDIO=GPIO9）。
- [x] 单独移植 PR #10 的 `HardwareSerial` 中断接收、4096 字节环形缓冲、
      接收/溢出计数、`clearRx()`、原位切换波特率和底层句柄接口；保留当前
      variant 引脚映射。Xuantie 交叉编译、Serial 示例和完整 bridge
      编译链接通过；当前 Linux 工具链缺少 `lto-wrapper`，链接时须临时
      加 `-fno-use-linker-plugin`（保留原 `__LD_CONFIG_EM_SIZE=32`）。
- [x] 2026-09-30 BL616CL 上板回归：FT232 `BG02CSA6` 接 UART0/ISP
      （GPIO34/35）、`BG03YFET` 接 HCI 物理 UART1（GPIO27 TX / GPIO28 RX），
      两端均以 2 Mbaud 收到 READY 和回显；重复 end/begin 后回显仍正常；
      UART1 原位 `updateBaudRate(2000000)` 后回显正常，512 字节连续回显通过。
      暂停前台读取后发送 6000 字节，接收计数 6526、溢出 1905、回显
      4095 字节，符合环形缓冲可用容量；`clearRx()` 后统计归零。
      测试固件已移除并刷回 bridge，USB 重新枚举为 2341:1002，CDC
      `AT` 返回 `OK`。此次使用临时 `HardwareSerial(1, 28, 27)`，不代表
      默认 `Serial1` 的 GPIO24/25 或 BLE 控制器共用 UART1 已验证。
- [ ] 测试非 2 Mbaud 的原位波特率切换；首次 115200 测试的主机脚本
      存在帧读取过量问题，结果不能用于判断固件是否支持切换。
- [x] 确认调试日志串口与 Bouffalo SDK 默认板级配置一致：
      UART0 = GPIO34 TX / GPIO35 RX @ 2 Mbit/s，UART1 = GPIO24 TX / GPIO25 RX
      （carrier variant 已把 UART1 改到 GPIO6/7 接 RA4M1 AT 口，见第五阶段）
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

### 2026-10-02 子模块现编（替代 checked-in bundle）

- [x] `tools/sdk/bouffalo_sdk` 作为 git 子模块固定到 `v2.3.35`
      (`63784aa0d14a4d67b081f91a4fa7ed18331c7470`，2026-10-03 更新到
      `v2.3.36`，见下节)；runtime 头文件/库/boot2/DTS 在 Arduino 编译期由
      `platform.txt` 的 prebuild hook
      （`tools/runtime_bundle/build_sdk_runtime.py`）从子模块现编，产物缓存在
      `~/.cache/arduino-bouffalo/sdk-runtime/<key>` 并 symlink 进
      `{build.path}/sdk_runtime/{mcu}`；不再读取 checked-in bundle
      （legacy `tools/sdk/bl616cl/` 已于 2026-10-03 删除）。
- [x] 8 个 SDK 补丁构建期 apply、结束 revert（`patches/patches.json`）；
      `wl80211-connect-ssid-filter.patch` 在上游 v2.3.35 改为预编译库后标记
      obsolete（该库已含修复）。
- [x] 移除 `-lblestack`：uarthci 预设强制 `CONFIG_BLE_HOST_DISABLE=y`，bridge
      经 AT 虚拟 HCI 传输直接驱动控制器，`USE_M2S1_CONTROLLER` 未启用，
      链接器验证无任何 blestack 符号引用。
- [x] 构建卫生（2026-10-03）：patch 加 `--no-backup-if-mismatch`（消除
      `.orig` 残留），SDK Python 子进程加 `PYTHONDONTWRITEBYTECODE=1`
      （SDK 仓库 track 的 `__pycache__/*.pyc` 不再被改写），回退后检查
      patch 触及文件是否仍脏并告警。验证：从纯净子模块（`git status`
      为空）执行 `arduino-cli compile`，8 个 patch apply→build→revert 后
      子模块仍零残留；新缓存键 `4f08c39952e0c51e`，warm 编译 12.9 s。
- [x] 删除 legacy `tools/sdk/bl616cl/`（2026-10-03，755 个文件 / 67 MB）：
      过时的头文件与静态库全部移除，`tools/sdk/` 下只剩 `bouffalo_sdk`
      子模块；仓库内不再有任何已编译产物（`tools/vendor/` 亦已删除）

- [ ] 在 GNU Make 4+ 环境重新运行 `generate_runtime_bundle.py`，验证 CherryUSB 配置可复现
- [ ] 补充 macOS 下直接 CMake 构建说明或增加 CMake 回退路径
- [x] 记录 SDK 本地补丁：`tools/runtime_bundle/patches/`（bflb_usb_v2 EP0 控制传输修复）
- [x] 修复 compat 层 `freeaddrinfo` 空桩导致的 `EAI_MEMORY`：lwip/netdb.h 把
      POSIX 名宏映射为 `lwip_freeaddrinfo`，空桩实际顶替了真实实现，
      `lwip_getaddrinfo()` 从唯一的 MEMP_NETDB 池取元素后永不归还，
      第二次解析起全部失败（表象为 EAI_MEMORY）；已删除桩函数
### 2026-10-03 SDK v2.3.36：uarthci 由公共 SDK 提供，vendor 清理

- [x] 子模块更新到 `v2.3.36`
      (`8a3df34cb73f459ac71deb1bdf0b33fa016ab6fb`)：公共 SDK 首次发布
      BL616CL `uarthci` 控制器归档
      （`components/wireless/bluetooth/btblecontroller/lib/`，1.6.210，
      sha256 `621ba45b…`），不再依赖 BLE 团队的私有构建。
- [x] 修复 runtime staging：发布版 SDK 的预编译控制器归档此前只从
      `build_btblecontroller/`（源码构建目录）取，导致 Arduino 链接
      `-lbtblecontroller_bl616cl_uarthci` 失败；现在按 `defconfig` 的
      `CONFIG_BTBLECONTROLLER_LIB` 从 SDK `btblecontroller/lib/` 拷贝，
      未发布的 flavor 可用 `BOUFFALO_BLE_CONTROLLER_LIB=<archive>` 兜底。
- [x] 删除 `tools/vendor/`（1.6.208，sha256 `d2b948e1…`）及代码/文档引用；
      runtime manifest 的 `vendor_libs` 字段改名 `extra_libs`（正常为空），
      链接归档与 SDK 树逐字节一致（sha256 `621ba45b…`）。
- [x] 实机复验（2026-10-03，提交 `07af605`）：清缓存重编
      （最终 runtime 键 `41521dfcceb35f2d`，956688 B / 45%）；启动日志 `component_version_sdk: 1.1.1 8a3df34c+`、
      `lib_version_btblecontroller_1.6.210`；AT+HCI Reset / AdvParams /
      AdvData(36B) / AdvEnable 四条 Command Complete `status=0x00`；
      空口扫描到 `B4:E8:42:3C:A7:DD BL616CL-HCI`。

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
- [x] 用 lwIP ICMP（raw socket）实现 ESP-IDF 语义的 `esp_ping_*`，
      实机 ping 192.168.133.49 4/4 成功（补 DEFAULT_RAW_RECVMBOX_SIZE=8）；
      2026-10-04 起实现移入平台 `libraries/esp32-compat/src/ping/esp_ping.cpp`
      （后台任务 + 回调），bridge 的 `ping.cpp` 恢复上游调用（见第五阶段）
- [x] 修复 `WiFi.SSID()/BSSID()/RSSI()` 无参重载：此前默认参数解析成扫描列表
      第 0 项，`AT+GETSSID?` 误报 TP-LINK_3D67；现在返回当前 STA 连接信息，
      实机验证返回 zrrong / 64:64:4A:82:73:74 / 实时 RSSI
- [x] 修复数字 IP 字符串解析：新增平台 helper `lwip_resolve_host()`
      （先 `AI_NUMERICHOST` 解析、失败再走 DNS）并用于 WiFiClient/WiFiUDP。
      事后按源码核查：lwIP 的 `dns_gethostbyname_addrtype()` 本身带
      `ipaddr_aton()` 快速路径（dns.c），因此上游 `ping.cpp` 的普通
      `getaddrinfo()` 数字/域名都能解析，bridge 侧不再需要该改写
- [x] 修复 `WiFiClient::available()`：该 lwIP 配置（LWIP_SO_RCVBUF=0、
      LWIP_FIONREAD_LINUXMODE=0）下 FIONREAD 被编译掉，ioctl 恒返回 0，
      TCP 回读拿不到数据；改用 `recv(MSG_PEEK|MSG_DONTWAIT)` 探测
- [x] 修复 `WiFiUDP::parsePacket()` 阻塞：无数据报时改为 `MSG_DONTWAIT`，
      按 ESP32 语义返回 0，避免 bridge AT 任务永久卡死
- [x] bridge AT 命令经 USB CDC 冒烟：AT/GMR/WIFISCAN/BEGINSTA/GETSTATUS/
      IPSTA/GETSSID/GETBSSID/GETRSSI/MACSTA 全通，连接 zrrong 并 DHCP 成功
      （历史：当时 AT 与 USB CDC 共用 USBSerial，用 DK 专用宏关闭 loop() 透传；
      该宏已随 bridge 最小化收敛移除）
- [x] `AT+PING` 实机验证：到 PC（192.168.133.49）往返成功并返回整数 RTT
      （`%f` 在 CONFIG_LIBC_FLOAT=0 下不可用，bridge 输出改为 `%d`）；
      未关联时表现为普通超时（返回 2）
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
      `ensure_wifi_started()` 不再重复初始化；实机验证无 IP ping 不再崩溃，
      随后连接+ping 网关/DNS/TCP/UDP 全通（12/12）。
      （早期加的“无 IP 返回 -4”守卫已随 bridge 收敛移除；tcpip 提前初始化
      后未关联时走正常超时路径）
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
- [x] AT 传输默认切到 RA4M1 物理 UART（2026-10-04）：`SERIAL_AT = Serial1`
      （UART1 GPIO6 TX / GPIO7 RX @115200，RA4M1 侧 SCI1 P501/P502），
      USB CDC 改为 `SERIAL_USER` 与 UART0（GPIO34/35，接 RA4M1 日志/
      烧录口）双向透传（编译验证通过，RA4M1 实机链路待载板接线后验证）
- [x] carrier 关键信号映射落实（2026-10-04）：BL616CL RESET=GPIO3、
      MD=GPIO10、SWCLK=GPIO8、SWDIO=GPIO9；bridge 侧改为 `#ifndef`
      默认值（保留上游 9/4/8/7），实际值由 `boards.txt` 的
      `CONFIG_BRIDGE_GPIO_*` build flags 注入；UART0=GPIO34/35、
      UART1=GPIO6/7
- [x] bridge 仓库收敛为最小补丁（2026-10-04）：`esp_ping_*` 由平台
      `libraries/esp32-compat/src/ping/esp_ping.cpp` 实现（lwIP raw ICMP、
      后台 FreeRTOS 任务、ESP-IDF 回调语义），bridge 的 `ping.cpp`/`ping.h`
      恢复上游；删除 BLE 取证诊断命令、`+SSLERR`、DK 专用 `AT_ON_USBCDC`、
      `tools/at_smoke/` 与 README 的 BL616CL 章节；`%.0f` 改 `%d`
      （`CONFIG_LIBC_FLOAT=0`）；UART `begin()` 在 BL616CL 下走 variant
      默认引脚。bridge 净 diff 收敛为 6 文件 +75/−3，编译通过
      （953 616 B / 66 332 B），详见 `docs/UNO-R4-BRIDGE-BL616CL.md`
- [x] DAP SWDIO 决定保持上游“每次传输切换方向”实现（2026-10-04）：
      BL616CL 无 open-drain，常开双向（`GPIO_MODE_INPUT_OUTPUT`）在目标
      驱动 SWDIO 的读阶段会推挽对驱；提速前需先做波形与电流验证
- [ ] 验证 RA4M1 与 BL616CL 的串口透传（UART1 AT @115200 + UART0 透传）
- [ ] 验证通过 USB CMSIS-DAP 对 RA4M1 进行编程
- [x] 验证 BLE HCI 透传（2026-10-03 用公共 SDK v2.3.36 uarthci 完成 Reset/LE 广播序列和空口验证；ACL/连接及长期稳定性仍待测）
- [ ] 执行端到端 bridge 回归和更新包打包

## BLE HCI 透传：架构与已解决的坑（历史归档）

> 当前状态、复现步骤与历史结论见 `docs/BLE-UARTHCI.md`。
> 2026-09-18 至 09-21 的排查实验（BLE 团队 EM 假说、btble_cli 剥离阶梯、
> UART 插桩观测、私有大树对照等）的结论已被 2026-10-03 公共 SDK v2.3.36
> uarthci 实测取代，相关日志不再保留在本文件，需要时见 git 历史。

架构：RA4M1 跑 BLE host，经 bridge 的
`AT+HCIBEGIN/HCIWRITE/HCIREAD/HCIAVAILABLE/HCIWAIT` 把 HCI 透传给 BL616CL
controller（uarthci flavor），广播参数全部由 host 决定。

仍然有效的实现要点：

- [x] `HCIVirtualTransport` 在 Arduino 侧组 H4 帧，直接调用
      `hci_tl_cmd_received(HCI_TL_H4=1, opcode, len, payload)` /
      `hci_tl_acl_tx_data_*`；不要喂 SDK 的 H4TL armed-read 路径
      （无 UART ISR 的虚拟传输上 djob 永不执行）
- [x] 每条命令前调用 `hci_tl_cmd_get_max_param_size()`：它通过副作用设置
      `hci_env.p_cmd_desc`，漏掉时第二条命令起被当“不支持”丢弃
- [x] 链接必须带 `-Wl,--defsym,__LD_CONFIG_EM_SIZE=32`（BT 交换内存），
      否则 `btble_controller_init()` 崩溃
- [x] BLE/WiFi 共享一次 `rfparam_init()`；`bl_sys_em_config()` 在 core 的
      `bl_wireless_init()` 里、调度器启动前执行
- [x] 烧录/ISP：进 ISP = DTR 拉高 BOOT + RTS 脉冲复位；烧录走
      BLFlashCommand + ROM 桥出的 USB CDC（FT232 UART 上的 ROM ISP 不响应）
- [x] ⚠️ 方法学：ISR 里写 UART 的 trace 探针会破坏 BLE 时序（2 Mbaud
      每条约 40µs vs 312µs 半 slot），纯净固件也会被打成空口静默；
      实时路径观测只用 SRAM 计数器 + 空闲任务打印
- [x] 最终验证（2026-10-03）：公共 SDK v2.3.36 自带
      `libbtblecontroller_bl616cl_uarthci.a`（1.6.210）直接链接，
      AT+HCI 四条命令 `status=0x00`，空口扫描到 BL616CL-HCI

遗留：其他 HCI/ACL/连接命令与长期稳定性未测。

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
