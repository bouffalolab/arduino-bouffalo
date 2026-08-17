# 非 arduino-bouffalo 仓库改动记录

本文记录本工作区中 `arduino-bouffalo` 之外仓库的本地改动，作为上游 diff 的
审计与复现依据。包含：

1. `bouffalo_sdk_full`（Bouffalo SDK 检出，非 git 仓库）——本地补丁与运行时库重建；
2. `uno-r4-wifi-usb-bridge`（Arduino UNO R4 WiFi USB Bridge 固件）——本地提交。

## 1. bouffalo_sdk_full（SDK）

### 1.1 概览

- SDK 检出目录：`bouffalo_sdk_full/bouffalo_sdk`，不是 git 仓库；
- 所有本地改动以补丁形式保留在 `arduino-bouffalo/tools/runtime_bundle/patches/`，
  重新生成 runtime bundle 时应用；
- 运行时库产物在 `arduino-bouffalo/tools/sdk/bl616cl/`，`manifest.json` 记录
  `source_commits`（各组件/子模块 commit）与全部文件的 SHA-256；
- `sdk_version`：`2.3.30-local-wl80211`（本地定制版本号，标识从 fhost 切换到
  wl80211 主机栈）。

关键 source_commits：

| 组件 | commit |
|---|---|
| drivers/lhal | `b39aa29f` |
| components/wireless/wl80211 | `70f2c250e` |
| components/wireless/macsw | `78718af29` |
| components/wireless/wifi6 | `910812dbe` |
| components/net/lwip/lwip | `ceade879a` |
| components/crypto/mbedtls/mbedtls | `3bc632160` |
| components/usb/cherryusb | `2652a5b5b` |

### 1.2 本地补丁清单

#### 1.2.1 bflb_usb_v2-ep0-control-transfer-fixes.patch

- 目标：`drivers/lhal` 的 `src/bflb_usb_v2.c`（上游 `b39aa29`）。
- 问题（BL616CL 上 UNO R4 USB bridge 调试发现）：
  1. EP0 `usbd_ep_set_stall` 未写 `USB_CX_DONE`，stall 后控制 FIFO 卡死，
     下一个 SETUP 被读成重复首字；
  2. EP0 零长度 `usbd_ep_start_read` 是空操作，状态阶段由控制器在数据包
     结束后自行完成；
  3. VDMA 完成回调在 EP0 IN 以短包结束时写 `USB_CX_DONE`，让控制器在
     正确时机 ACK 后续 OUT 状态阶段（macOS 枚举依赖此行为）。
- 验证：Linux/macOS 均枚举成功（VID 0x2341 / PID 0x1002，CDC ACM +
  CMSIS-DAP 复合设备）。

#### 1.2.2 wifi6-lwipopts-runtime-config-fixes.patch

- 目标：`components/wireless/wifi6` 的 `wifi6_lwip_adapter/include/lwipopts.h`
  与 `wifi6_lwip_adapter/tx_buffer_copy.c`（上游 `910812db`）。
- 问题：macsw 的 TX/RX 缓冲数量在 macsw master 改为运行时配置，而
  `lwipopts.h` 仍引用已删除的编译期宏，`CONFIG_WIFI6`/`CONFIG_FHOST` 下
  无法构建 `liblwip.a`；`tx_buffer_copy.c` 的 macsw 头文件包含顺序也有误。
- 修复：lwIP 队列尺寸回退到默认编译期值，修正包含顺序。

#### 1.2.3 lwip-wl80211-include-and-pbuf-fixes.patch

- 目标：`components/net/lwip/lwip` 的 `CMakeLists.txt` 与
  `lwip-port/config/lwipopts.h`。
- 问题：wl80211 的静态断言引用通用的 `lwip-port/config/lwipopts.h`，但 lwIP
  库没有把它作为公共 include 路径；且 `PBUF_LINK_ENCAPSULATION_HLEN` 原值 48
  装不下 wl80211 TX 描述符 + macsw 帧头，`wl80211_lwip_tx()` 的编译期
  `CTASSERT` 失败。
- 修复：暴露 include 路径；`PBUF_LINK_ENCAPSULATION_HLEN` 48 → 388。

#### 1.2.4 lwip-default-raw-recvmbox-size.patch

- 目标：`components/net/lwip/lwip` 的 `lwip-port/config/lwipopts.h`。
- 问题：端口定义了 UDP/TCP/accept 的 recvmbox 大小，但
  `DEFAULT_RAW_RECVMBOX_SIZE` 保持 lwIP 默认 0；创建
  `SOCK_RAW`/`IP_PROTO_ICMP` socket 时 `xQueueCreate(0, ...)` 触发 FreeRTOS
  `pxNewQueue` 断言。
- 修复：设为 8（bridge 的 `AT+PING` 依赖 raw ICMP socket）。

#### 1.2.5 wl80211-ip-got-cb-core-lock-deadlock-fix.patch

- 目标：`components/wireless/wl80211` 的 `lwip.c`（上游 `3c19c8d1`）。
- 问题：`ip_got_cb`（STA netif 状态回调）在 tcpip 线程上执行且 lwIP core
  互斥锁已被持有，却调用 `netifapi_netif_set_default()`——在
  `LWIP_TCPIP_CORE_LOCKING=1` 下会再次获取非递归 core 锁 → 死锁。
- 表象：DHCP ACK 已处理、IP 已分配，但 `CODE_WIFI_ON_GOT_IP` 永不发出；
  15 秒 mgmr DHCP 看门狗随后断开 WiFi，`netifapi_dhcp_stop` 无法完成。
- 修复：改用 core-locked 的 `netif_set_default()` 直接调用。
- 验证：实机连接 zrrong 后 DHCP 拿到 192.168.133.40/24，GOT_IP 正常触发。

#### 1.2.6 wl80211-connect-ssid-filter.patch

- 目标：`components/wireless/wl80211` 的 `src/macsw/connect.c`。
- 问题：`scan_done_cb()` 把 join 扫描到的所有 AP 都收进来，只按 RSSI 选最强，
  从不比较 SSID 是否等于请求值 → `WiFi.begin("zrrong", ...)` 在别的 AP 信号
  更好时关联到无关 AP（实测 `bl_test_816`）。
- 修复：
  - 候选按请求 SSID 过滤后再选最强 RSSI，并打印选中的 SSID；
  - `scan_ind_cb()` 在缓存记录之外也接受新的定向探测响应，使隐藏 SSID 的 AP
    （只回应定向探测）可被发现；
  - 无匹配时返回 `WLAN_FW_SCAN_NO_BSSID_AND_CHANNEL` 失败，不再静默连错。
- 验证：实机扫描 16 个 AP、WPA2-PSK 连接 zrrong 成功。

#### 1.2.7 mbedtls-config-tls-ecp-have-curves.patch

- 目标：`components/crypto/mbedtls` 的 `config-tls-generic.h`。
- 问题：`config-tls-generic.h` 启用了 `MBEDTLS_ECP_DP_*_ENABLED` 但没有派生
  `MBEDTLS_ECP_HAVE_*` 名（默认 `mbedtls_config.h` 由
  `config_adjust_legacy_crypto.h` 完成），导致 X.509 OID 表不含命名曲线，
  解析 ECDSA 证书公钥失败（`MBEDTLS_ERR_PK_UNKNOWN_NAMED_CURVE`）。
- 修复：补映射；另在构建中加入 `CONFIG_MBEDTLS_ECP_DP_SECP384R1_ENABLED`
  （公网站点常混用 P-256/P-384 证书链）。
- 验证：www.bing.com:443、example.com:443 TLS 1.2 握手与 HTTPS GET 成功。

### 1.3 运行时库选择与重建注意事项

- BL616CL 使用 wl80211 主机栈（`CONFIG_WL80211=y`、
  `CONFIG_BL_WPA_SUPPLICANT=y`，无 `CONFIG_FHOST`）；fhost 全栈 RAM 超预算，
  wl80211 + macsw 固件 + lwIP 在预算内。
- 重新生成 runtime bundle 时，`defconfig`、`autoconf.h`、
  `libwl80211_<chip>.a`（`src/` 构建）与 `libwl80211_plat.a`（顶层文件）
  必须同步。
- `libapp.a` 必须与 `CONFIG_WIFI6` 的构建配套：`board.c` 只在
  `CONFIG_WIFI6` 下挂 WiFi MAC IRQ，旧的 `libapp.a` 会导致首次 STA VIF add
  卡在 `MM_GOING_TO_IDLE`。

## 2. uno-r4-wifi-usb-bridge（固件仓库）

### 2.1 概览

- 上游：`arduino/uno-r4-wifi-usb-bridge`（origin/main）。
- 本地分支：`main`，当前领先 origin/main 7 个提交（截至 2026-08-17）。
- 用途：将原 ESP32-S3 bridge 固件移植到 BL616CL DK 验证 + 维护 AT 冒烟工具。

### 2.2 本地提交

| commit | 内容 |
|---|---|
| `2349b7b` | 忽略本地构建产物、规范化 EOF 换行 |
| `f7155eb` | SSE 移植到 mbedTLS v3 API |
| `31167cd` | ping 从 ESP ping 桩切换到 lwIP raw ICMP socket |
| `164db98` | AT 服务器改走 USB CDC（BL616CL DK） |
| `7f2188d` | AT+PING：数字 IP 解析、整数 RTT、无 IP 保护 |
| `6d30a25` | 新增 AT 冒烟测试工具；保留 +SSLERR 诊断命令；修正 ping 保护注释 |
| `88598de` | 移除误提交的 __pycache__，忽略 Python 字节码 |

#### 2.2.1 2349b7b —— 构建产物与格式

- `.gitignore` 增加本地构建输出；`server.cpp` 等文件 EOF 换行规范化。

#### 2.2.2 f7155eb —— SSE 移植到 mbedTLS v3

- `SSE.cpp` 适配 mbedTLS v3：`pk_sign` 新签名、`mbedtls_sha256`、
  `pk_parse_key`。
- 背景：compat 层 mbedTLS v2 桩改为 weak + `libmbedtls.a` whole-archive，
  v3 实现优先生效。

#### 2.2.3 31167cd —— lwIP raw ICMP ping

- `ping.cpp`：用 `lwip_socket(AF_INET, SOCK_RAW, IP_PROTO_ICMP)` 自实现
  ICMP echo（id/seq/校验和/RTT 统计），替换 ESP ping 桩。
- 依赖 `lwip-default-raw-recvmbox-size.patch`（见 1.2.4）。
- 验证：实机 ping 192.168.133.49 4/4 成功。

#### 2.2.4 164db98 —— AT 走 USB CDC

- `AT_ON_USBCDC` 宏：BL616CL DK 无 UART 对端，AT 服务器挂到 USBSerial，
  同时关闭 loop() 的 CDC 透传避免抢流；UNO R4 载体板上可取消宏回到 Serial1。
- `CAtHandler` 传输类型从 `HardwareSerial*` 改为 `Stream*`，适配 CDC 的
  bulk read。
- 验证：AT/GMR/WIFISCAN/BEGINSTA/GETSTATUS/IPSTA/GETSSID/GETBSSID/
  GETRSSI/MACSTA 全通（/dev/cu.usbmodem01）。

#### 2.2.5 7f2188d —— AT+PING 收尾

- `lwip_getaddrinfo` 先带 `AI_NUMERICHOST` 解析点分 IP，失败再走 DNS；
- `%f` 在 `CONFIG_LIBC_FLOAT=0` 下不可用，RTT 改为整数；
- 无 IP 时返回 `-4`（当时注释为“raw ping 破坏 wl80211 TX/lwIP 堆”，
  后在 arduino-bouffalo 侧定位为惰性 `tcpip_init` 导致的 “Invalid mbox”
  崩溃，见 arduino-bouffalo 提交 `c67a8e0`；`6d30a25` 已修正注释）。

#### 2.2.6 6d30a25 —— 测试工具与诊断

- 新增 `tools/at_smoke/`：pyserial AT 客户端 + 网络回归（12 项）+ TLS 冒烟
  （9 项）+ 对端 echo server；在 BL616CL bridge 上全部通过。
- `cmds_wifi_SSL.h`：`+SSLERR=<sock>` 保留为 TLS 错误诊断命令（原 TEMP DEBUG）。
- `cmds_esp_generic.h`：保留无 IP ping 保护，注释改为真实原因（tcpip 惰性
  初始化已修复，保护仅用于返回显式 -4）。

#### 2.2.7 88598de —— 字节码清理

- 移除误提交的 `tools/at_smoke/__pycache__`，新增 `.gitignore`
  （`__pycache__/`、`*.pyc`）。

## 3. 维护约定

- SDK 侧任何新改动：先写补丁到 `patches/`，重建受影响的运行时库，更新
  `manifest.json` 的 source_commits 与文件哈希，再在本文档记录问题与验证。
- 固件侧任何新改动：保持提交粒度（一个修复一个提交），并同步更新本文档与
  `arduino-bouffalo/TODO.md`。
