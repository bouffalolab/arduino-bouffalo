# arduino-bl

通用 Bouffalo Arduino 平台仓库，从 BL616CL 起步，后续逐步支持多芯片。

当前阶段：最小平台骨架。Core 与第一块板卡可以用 arduino-cli 编译、生成镜像，经板上 USB 口一键上传；`Serial` 是 USB 串口，`Serial1`、`Serial2` 是硬件 UART。

普通用户的目标体验是通过 Boards Manager / Arduino CLI 安装后直接编译上传，无需完整 Bouffalo SDK。底层依赖由独立构建器发布。

当前平台需要 `bl616cl-libs` 0.6.0 和 `bl-flash-tools` 0.2.0（新增 littlefs 引擎与统一分区工具，保留 HID/BLE 能力）；平台与依赖包应一起更新。

本地共享入口：[文档](../docs/README.md)、[开发规范](../AGENTS.md)、[已确认验收](../docs/project/charter.md)。这些文件目前位于非 Git 的父工作区，独立分发文档的方式待确定。

## 目录

| 路径 | 内容 |
| --- | --- |
| `platform.txt` | 编译、链接、镜像后处理、size、上传配方；参数和库全部来自板卡所选的预编译底层包 |
| `boards.txt` | 板卡：`unor4wifi_bl616cl`（UNO R4 WiFi，协处理器换成 BL616CL） |
| `cores/bouffalo/` | Core：入口、C++ 运行时、GPIO、时间、USB 串口与 UART、软件进入下载模式 |
| `cores/bouffalo/usb_hid_port.h` | 可选 64 字节 raw HID；应用在全局构造时注册报告描述符与 Feature 回调，USB 启动前固定接口组合 |
| `cores/bouffalo/api/` | ArduinoCore-API 原样拷贝，来源见下 |
| `libraries/WiFi/` | 平台 WiFi 库（接口同官方 WiFiS3），经 `library.properties` 的 `ldflags` 链入底层包的 Wi-Fi 组 |
| `libraries/LittleFS/` | 平台程序 Flash 文件系统；内部回调直连原厂 Flash API，bridge/AT 无关，显式格式化 |
| `libraries/BLEController/` | external-host H4 Controller接口与USB原始HCI示例，无BL端BLE Host；经`ldflags`选择BLE链接组 |
| `variants/unor4wifi_bl616cl/` | 板卡引脚、引脚表（引脚号到 GPIO，禁用系统引脚）与 `Serial1`、`Serial2` 实例 |
| `tests/` | 测试 sketch 与编译检查脚本；不进入平台发布归档（`.gitattributes`） |

编译和上传依赖三个工具：底层包 `bl616cl-libs`、烧录工具包 `bl-flash-tools`（都由 `arduino-bl-lib-builder` 生成）和 Xuantie GCC V2.6.1。平台通过 `runtime.tools.bl616cl-libs.path`、`runtime.tools.bl-flash-tools.path` 和 `runtime.tools.xuantie-gcc-riscv.path` 引用它们；不经包索引安装时，本地测试用 `--build-property`（编译）和 `--upload-property`（上传）指定。

## 镜像与上传

编译后 `bflb_fw_post_proc` 原地改写 `<sketch>.ino.bin` 的启动头，并在编译目录生成 `partition.bin`。上传用 `BLFlashCommand` 把底层包中的 boot2、分区表、固件分别写到 0x0、0xE000、0x10000（地址与容量由构建目录的 `layout.json`/`layout.toml` 固定），写后校验 SHA-256，再用 warm reset 启动新固件。UNO R4 WiFi 上的 BL616CL 经板上 USB 口上传，选 sketch 运行时的 USB 串口（`349b:0011`）：

```sh
arduino-cli upload -b bouffalolab:bl:unor4wifi_bl616cl -p <串口> --input-dir <编译目录> <sketch>
```

上传按 Arduino 的 1200 bps touch 进行：arduino-cli 以 1200 波特率打开再关闭这个串口，Core 收到后设置“复位后进入下载模式”的请求并上电复位，ROM 在 USB 上提供下载串口（`ffff:ffff`），arduino-cli 等它出现后烧录。新固件启动时 Core 清除这个请求，之后的软件复位正常启动固件。sketch 调用 `Serial.setTouchReset(false)` 后不再响应 touch（应用自己处理 1200 波特率时用，如 bridge），这时由 sketch 调用 `resetToBootloader()` 或按下文手动进入下载模式后上传。

板上固件不含本平台 Core、已卡死或没有枚举 USB 时，先手动进入下载模式（复位时 bootstrap 引脚 GPIO36 为高电平），再对 ROM 的下载串口执行同样的命令；arduino-cli 等不到新端口，约 10 s 后改用这个串口。

限制：

- 工具包目前仅提供 Linux x86_64 版本；上传在构建目录的 `flash-runner/` 中运行 FlashCommand 副本，工具安装目录可只读。
- 镜像不含 SDK 原生流程追加的 DWARF CFI 表，崩溃时 `backtrace` 输出 0 帧。
- dts 使用 SDK 开发板的工厂参数（晶振、RF），替换模组的参数尚未确认。

## 第三方代码

`cores/bouffalo/api/` 是 [ArduinoCore-API](https://github.com/arduino/ArduinoCore-API) 1.5.2 的 `api/` 目录，未作修改：

- commit：`cd91833d90b4fe50e428021ba5051e2b7ceafc84`
- 归档：`https://github.com/arduino/ArduinoCore-API/archive/refs/tags/1.5.2.tar.gz`
- 归档 SHA-256：`2c76d4dcea29d1d093d821804dcc1ccd83d172ef0d359d7c651cdbba6e6bfa57`
- 许可：LGPL-2.1，全文见 `cores/bouffalo/LICENSE.ArduinoCore-API`

`cores/bouffalo/dtostrf.c` 包含该目录中的 `deprecated-avr-comp/avr/dtostrf.c.impl`。

## 本地编译检查

可选 raw HID 与 CDC 共用一次 USB 初始化；未注册时仅枚举 CDC，注册后增加接口 2 及中断 IN/OUT 端点。应用在任务中读写 HID，Feature 回调在 USB 中断中运行、不得阻塞。报告含义和目标芯片控制由应用实现，Core 不包含 CMSIS-DAP 或 RA4M1 逻辑。

Linux 下 raw HID 需要设备权限；平台附有 `tools/99-arduino-bl.rules`（USB 与 hidraw，仅 VID:PID `349b:0011`）。安装到 `/etc/udev/rules.d/` 并重载规则、重新连接设备后生效；规则使用 `plugdev` 组与 `uaccess`。本轮实测采用仅对当前设备节点生效的临时 ACL，没有改系统永久规则。

```sh
tests/compile_check.sh <bl616cl-libs 包目录> <Xuantie GCC 目录> <bl-flash-tools 目录> [要隐藏的路径...]
```

平台配置另用 Arduino 官方 [arduino-lint](https://arduino.github.io/arduino-lint/) 检查，严格模式应无错误：

```sh
arduino-lint --project-type platform --compliance strict .
```

脚本把平台复制到临时 sketchbook，在 bubblewrap 中把给出的路径（SDK 检出、SDK 构建目录等）替换为空目录后，编译 `tests/sketches/` 下的每个 sketch，再用 `tests/check_map.py` 检查 map：未链入 Wi-Fi 链接组，`_malloc_r` 来自 SDK libc 移植层，`main` 来自 Core；用 `tests/check_image.py` 检查后处理产物：启动头已写入，`partition.bin` 存在。需要 `arduino-cli`、`bwrap` 和 `python3`；arduino-cli 每次会在临时目录重新下载自带工具（ctags、discovery 等），需要联网。

## 功能状态

只有编译链接证据、没有实机运行证据的功能标为 experimental。未另注版本的表中证据针对板卡 `unor4wifi_bl616cl`（UNO R4 WiFi，BL616CL 模组 1V0）与 `bl616cl-libs` 0.4.0（0.4.0 上复验了编译检查与安装链，TLS 的实机证据来自 0.4.0，其余实机证据来自 0.3.2 至 0.3.5），主机为 Linux x86_64；Windows、macOS 未测。

| 功能 | 状态 | 构建证据 | 实机证据或说明 |
| --- | --- | --- | --- |
| `BLEController` external-host原始H4命令/事件/ACL、关闭再初始化（BL端无Host） | experimental | 0.5.0：13个普通sketch构建/map/镜像辅助检查不含Controller，另编译ControllerUSB；均不计实机验收 | Linux x86_64、1V0 + AX200：HCI查询、真实广告/central扫描、peripheral连接与双向ACL，1/20/21/100/244字节通知及Wi-Fi TCP/USB CDC并发、关闭重启通过；官方ArduinoBLE示例尚无实机结果。见BLEController README和ARDUINO-43任务。 |
| 可选 raw HID（`usb_hid_port.h`），与 CDC 组成三接口 USB 设备 | experimental | 0.4.1：13 个 sketch 编译/map/镜像检查通过；完整 bridge 编译通过 | Linux x86_64、1V0、bridge：64 字节 HID 连续收发、Feature、真实 RA4M1 SWD 读写/编程/调试；与 CDC 并发回显 50 B–64 KB。未注册 HID 的 sketch 仍仅有 CDC；上层 DAP 证据在 bridge 任务。 |
| 编译链接（`unor4wifi_bl616cl`，`bl616cl-libs` 0.4.0） | experimental | Blink、CxxStd、DebugLog、SerialEcho、Serial1Echo、UsbLineHook、WiFiScan、WiFiServerCheck、WiFiSSLCheck、WiFiStaticIP、WiFiStation、WMath 通过 `compile_check.sh` | 首块板上 Blink、UsbAlive、SerialEcho、Serial1Echo 烧录后运行 |
| `setup`/`loop` 任务（FreeRTOS，栈 8 KB，优先级 1）、`serialEvent`、`serialEvent1`、`serialEvent2` | experimental | 同上 | UsbAlive 的 `setup`/`loop` 持续运行；`serialEvent*` 未测 |
| C++ 全局构造、`new`/`delete`、局部静态变量初始化保护 | experimental | 同上 | UsbAlive：全局构造执行 1 次；其余未测 |
| `pinMode`（`INPUT`、`OUTPUT`、`INPUT_PULLUP`、`INPUT_PULLDOWN`）、`digitalWrite`、`digitalRead`；引脚号即 GPIO 号，GPIO4/5（32 kHz 晶振）、32/33（USB）、36（bootstrap）不可用，调用被忽略 | experimental | 同上 | UsbAlive：`OUTPUT` 下 GPIO34 写 0/1 读回 0/1；RX LED（DL2）每秒亮灭一次（目测）；Blink：经 J6.3 观测 GPIO34 每 1.00 s 拉低一次，连续 10 次；对 USB 引脚 32/33 调用 `pinMode`、`digitalWrite` 后 USB 照常输出；输入上下拉未测 |
| `F_CPU`（320 MHz） | experimental | 同上 | 实测 CPU 时钟读回 320000000 Hz |
| `millis`、`micros`（从程序开始计时）、`delay`、`delayMicroseconds`、`yield` | experimental | 同上 | UsbAlive：`delay(1000)` 实测 1001 ms，`millis` 连续；经 USB 上传（warm reset）后 SerialEcho 启动约 1.8 s 时 `millis` 为 1837；其余未测 |
| `Serial`（USB CDC，`349b:0011`，序列号为芯片 ID；收发缓冲各 1 KB；主机未打开端口时写入被丢弃；`baud()`、`dtr()`、`rts()` 等读主机设置的线路状态） | experimental | 同上 | SerialEcho：描述符字符串正确；启动行、每秒 uptime 正常，50 字节与 64 KB 连续数据回显一致；以 115200 关闭再打开不复位；主机发设备不支持的控制请求（IN、OUT 各类，各 50 次）被拒后 USB 继续正常响应；Linux 运行时挂起后恢复 30 轮（含挂起时还有待发数据）收发正常；压力测试：复位后枚举 50 次、端口开关 800 次全部正常；双向回显 15 分钟、约 2.3 GiB 逐字节一致（约 2.6 MiB/s），其间夹带约 3.5 万次改波特率、翻转 RTS、读描述符与状态；连续输出时主机停读 30 秒或关闭端口后恢复读取，数据格式与序号正确；`baud()`、`dtr()`、`rts()` 见下一行；`flush`、`end` 未单独测 |
| `Serial.setTouchReset(bool)`（默认开）、`Serial.onLineChange(回调)`、`resetToBootloader()`（平台自有接口，非官方 API）：主机每个 SET_LINE_CODING、SET_CONTROL_LINE_STATE 请求之后在 Core 的线路任务（栈 4 KB，优先级 2）中调用一次回调，回调可以 `delay()`，回调运行期间到达的请求在它返回后逐个补调、读到最新值，注册前的请求不补调；`resetToBootloader()` 软件复位进 ROM 下载模式 | experimental | UsbLineHook 通过 `compile_check.sh` | UsbLineHook：打开端口、改波特率、单独翻转 RTS 或 DTR 各得 1 次回调且读数与主机设置一致，连续 5 次改波特率得 5 次回调、最后一次读数正确，回调内 `delay(20)` 实测 20–21 ms；关闭 touch 后 1200 bps touch 不复位（设备未重新枚举）；`resetToBootloader()` 与重新打开 touch 后的 1200 bps touch 都进入 ROM 下载口；Linux 枚举时发的 SET_LINE_CODING(9600) 也会触发一次回调 |
| `Serial1`（uart0，GPIO34 发、GPIO35 收，J6.3/J6.5）、`Serial2`（uart1，GPIO6 发、GPIO7 收）；接收缓冲 256 字节；校验只支持无、奇、偶 | experimental | 同上 | Serial1Echo：J6.3/J6.5 接 USB 串口，115200 8N1 下启动行正常，50 字节与 4 KB 连续数据回显一致。`Serial2`：经 U4 接 RA4M1 SCI1（RA4M1 回显），115200 下收到 RA4M1 每秒心跳，50 B、2 KB、8 KB 回显一致；1 号 Uart 改接 GPIO11/GPIO8 对 USB 串口双向 8 KB 一致。`begin()` 先软件复位 UART 控制器：烧录工具最后的 warm reset 不复位外设，ROM 下载模式期间 RA4M1 在发数据时 uart0 接收器会停在忙状态（`UART_STATUS` 的 `URX_BUS_BUSY`），之后收不到数据；修复前 9 次 warm reset 启动有 2 次收不到，修复后 17 次（其中 7 次启动时接收器已忙）都收得到，冷启动 16 次正常。其他波特率、校验、`flush`、`end` 未测 |
| `String`、`Print`、`Stream` 等 ArduinoCore-API 通用类 | experimental | 同上 | 无 |
| `random`、`randomSeed`（C 库 `rand`/`srand`，语义同官方 Core） | experimental | WMath 通过 `compile_check.sh` | WMath：同一种子序列相同，`random(10)`、`random(-5, 5)` 各 1000 次都在范围内，上限 0、下限不小于上限的返回值正确 |
| 调试日志（板卡菜单 Debug log，默认关）：打开时 `printf` 与 SDK 日志从 uart2 输出（J6 IO41 发、IO42 收，1V0 模组为 GPIO11、GPIO8，2000000 波特率），这两个引脚归日志使用 | experimental | DebugLog 两种菜单值都编译通过 | DebugLog：打开时 uart2 收到连续的 `printf` 行，USB 串口与 uart0 上没有日志；关闭时 uart2 没有输出；WiFiStation 打开日志时 uart2 收到 SDK 的 rfparam、wl80211、wifi_mgmr 日志。1V1 模组的引脚未适配 |
| C++ 标准库 `<string>`、`<memory>`、`<sstream>`、`<vector>`、`<map>`、`<functional>`、`<algorithm>`、`<ctime>`、`<chrono>` 等（`bl616cl-libs` 0.3.5 起） | experimental | CxxStd 通过 `compile_check.sh` | CxxStd：`std::string`、`std::make_shared`、`std::map` 加 `std::function`、`std::ostringstream` 拼出的行与预期一致，`time()` 每秒递增 |
| C++ 线程设施 `<mutex>`、`<thread>`、`<condition_variable>`、`<future>` | unimplemented | 能编译 | 按 libstdc++ 源码与反汇编（未上板）：工具链的 libstdc++ 判定线程未启用，`std::mutex` 不加锁，`std::thread`、`std::call_once` 运行时失败；跨任务同步用 FreeRTOS 接口 |
| `OUTPUT_OPENDRAIN` | unimplemented | — | `pinMode` 不改动引脚 |
| `analogRead`、`analogWrite`、`analogReference`、`attachInterrupt`、`detachInterrupt`、`tone`、`noTone`、`pulseIn`、`pulseInLong`、`shiftIn`、`shiftOut`、`interrupts`、`noInterrupts` | unimplemented | — | 使用时编译或链接失败 |
| C 标准输入输出（`printf`、`getchar`、`scanf` 等） | unimplemented | — | 调试日志关闭时输出被丢弃（打开时见上一行）；读取返回 EOF |
| `WiFi` 库（接口同官方 WiFiS3）：STA（WPA2/WPA3 个人与开放网络，PMF 可选）、DHCP；AP（`beginAP`：WPA2 或开放网络，可设信道，带 DHCP）；`begin`、`disconnect`、`status`（`WL_*`）、`reasonCode`（wl80211 状态码）、`localIP`、`subnetMask`、`gatewayIP`、`dnsIP`、`RSSI`、`SSID`、`BSSID`、`macAddress`、`hostByName`、`ping`；`WiFiClient`（TCP 客户端，每个连接 512 字节接收缓冲，`available()` 最多报 512，连接超时默认 10 s；STA 未连接且 AP 未运行时 `connect` 直接失败；对象复制后共享同一连接，用完调用 `stop()`）；只在一个任务中调用 | experimental | WiFiStation 通过 `compile_check.sh` | WiFiStation（AP 为 WPA3-SAE、WPA2 各一次）：4.5–5.0 s 连上并取得地址，同一网络再次 `begin` 立即返回；ping 网关 20/20；DNS 解析外网域名；TCP 连网关 80 端口收发 HTTP；错误密码返回 `WL_CONNECT_FAILED`（状态码 2，WPA3 2.1 s、WPA2 4.8 s），不存在的 SSID 1.5 s 返回 `WL_NO_SSID_AVAIL`（12）；断开、重连正常；连接进行中调用 `disconnect()` 后保持断开、之后能再连（本次连接在断开等待的 3 s 内完成，连接晚于 3 s 完成的分支未实测）。与局域网电脑的 TCP 收发 1 B–4 KB 两端一致（经 bridge 的 AT 通道）。开放网络、长时间连接、AP 断电后的状态、多连接并发未测；Wi-Fi 断开后已有连接的 `connected()` 要等 TCP 重传超时才变 0 |
| `WiFi.scanNetworks()` 与 `SSID(i)`、`RSSI(i)`、`encryptionType(i)`、`BSSID(i, bssid)`、`channel(i)`：主动扫描全部信道并等待完成，未调用 `begin` 时也可用；按 RSSI 从强到弱保留前 10 个（同 WiFiS3、WiFiNINA），隐藏网络也占位、SSID 为空串；加密方式取 WiFiS3 的 `wl_enc_type`，WPA/WPA2 混合为 `ENC_TYPE_WPA2`、WPA2/WPA3 混合为 `ENC_TYPE_WPA3`；失败返回 -1 | experimental | WiFiScan 通过 `compile_check.sh` | WiFiScan：未连接时连续 3 次各扫到 10 个网络、1.5–1.7 s，字段有效、RSSI 有序，每次都有测试 AP（WPA2）；经 bridge 由 WiFiS3 在未连接与已连接时扫描（见 bridge README），已连接时列出当前 AP，扫描后连接保持、ping 网关成功。底层不区分 WEP 与开放网络（WEP 报告为开放，代码阅读所得，未实测）；扫描与 TCP 收发并发未测 |
| `WiFiUDP`（接口同 WiFiS3）：`begin(port)`、`begin(ip, port)`、`beginPacket`（地址或域名；未 `begin` 时用自动分配的本地端口）、`write`、`endPacket`、`parsePacket`（不等待）、`available`、`read`、`peek`、`flush`、`remoteIP`、`remotePort`、`stop`；收发缓冲各 1460 字节，一个数据报最多发 1460 字节，收到更长的截断；广播可发；STA 未连接且 AP 未运行时 `begin`、`beginPacket` 直接失败；对象不能复制。lwIP 共 8 个 socket，`WiFiClient`、`WiFiUDP`、`ping` 共用 | experimental | WiFi 库源码随 WiFiScan、WiFiStation 通过 `compile_check.sh` | 经 bridge 由 RA4M1 的 WiFiS3 调用（见 bridge README）：与局域网电脑往返 1–1460 字节二进制数据报 3 轮 18 个一致（41–336 ms），回应发到来源端口；未修改的 WiFiUdpSendReceiveString、WiFiUdpNtpClient 例程正常（NTP 时间与电脑相差 1.4–1.6 s，含例程等待 1 s）。广播、超过 1460 字节的接收、socket 用尽时的行为未实测 |
| `WiFiServer`（接口同 WiFiS3）：`begin()`、`begin(port)`、`available()`（接受新连接并保留最多 4 个，返回其中一个有数据可读的连接，同一连接可以多次返回；对端关闭且数据读完后服务端关闭它）、`accept()`（每个新连接只返回一次，之后由调用者 `stop()`）、`write`（发给 `available()` 保留的连接，返回各连接写出字节数中最少的）、`end()`、`operator bool`；STA 未连接且 AP 未运行时 `begin` 失败；对象不能复制。监听与每个连接各占一个 socket，与 `WiFiClient`、`WiFiUDP`、`ping` 共用 lwIP 的 8 个 | experimental | WiFiServerCheck 通过 `compile_check.sh` | WiFiServerCheck（电脑作客户端）：`available()` 对同一连接逐行应答；`write` 发给两个连接各一次，调用者 `stop()` 一个连接后不再重复发送；对端关闭的连接被回收，之后 6 个连接依次得到应答；`accept()` 的连接收发后被关闭。经 bridge 由 WiFiS3 调用（见 bridge README）。socket 用尽、长时间连接、大数据量收发未实测 |
| `WiFiSSLClient`（接口同 WiFiS3）：TLS 1.2 客户端（底层包的 mbedTLS 3.6，曲线 P-256、P-384、P-521、X25519），`connect`（地址或域名；SNI 与证书名称校验；超时含 TCP 与握手，默认 10 s，`setConnectionTimeout` 可改）、`setCACert`（PEM，之后只信任它，`nullptr` 恢复）、`write`、`available`、`read`、`peek`、`connected`、`stop`（发 close_notify）、`remoteIP`、`remotePort`；默认用包内根证书束（SDK 的 `cacrt_all.pem`，147 张，Mozilla 2024-03-11）。不检查证书有效期（同上游 ESP32 固件）；证书束按链顶证书的签发者找根；找不到时，链顶与束内根的名称和公钥都相同则信任（COMODO RSA CA 的 AddTrust 交叉签发版本即如此，本机 OpenSSL 同样接受）；没有 `setEccSlot`。每个连接常驻约 38 KB 堆，握手时约 43–52 KB，先占 OCRAM；OCRAM 空闲不足 56 KB 时 `connect` 失败（FreeRTOS 只从 OCRAM 分配，耗尽会使固件停止）。TCP socket 与 `WiFiClient` 等共用 lwIP 的 8 个 | experimental | WiFiSSLCheck 通过 `compile_check.sh` | WiFiSSLCheck（`bl616cl-libs` 0.4.0）：证书束下 www.baidu.com（RSA）、valid-isrgrootx2.letsencrypt.org（P-384 根）、ecc384.badssl.com 成功，wrong.host、self-signed、untrusted-root.badssl.com 失败；expired.badssl.com 成功（链顶为束内根）；链顶与束内根同名、公钥不同的测试链失败；`setCACert` 测试 CA 后与电脑 TLS 服务往返 HTTP 100062 字节、字节和一致，回显一致，名称不符失败，过期证书成功；同时开 8 个连接时 3 个成功、其余失败，之后照常工作，结束时空闲堆与开始相差不到 1 KB。经 bridge 由 WiFiS3 调用（见 bridge README）。修复后复跑 run-tls-5：43 项全部通过（平台与 bridge 两轮）。本测试网络连不上 www.google.com（DNS 解析到不通的地址）；长时间连接未实测 |
| 静态地址（接口同 WiFiS3）：`config(local_ip[, dns_server[, gateway[, subnet]]])`，省略的网关为 x.y.z.1、掩码 255.255.255.0，只给地址时 DNS 也用该网关；之后每次连接都不启动 DHCP，已连接时立即生效；`local_ip` 为 0.0.0.0 时恢复 DHCP（同上游 ESP32 固件）。`setDNS(dns1[, dns2])`：静态地址时更新 DNS，用 DHCP 且已连接时以当前地址转为静态地址（同 WiFiS3 经上游固件的结果），用 DHCP 且未连接时不起作用 | experimental | WiFiStaticIP 通过 `compile_check.sh` | WiFiStaticIP：连接前设 .200 后连上，地址、掩码、网关、DNS 与配置相同，ping 网关、DNS 解析、与电脑 TCP 回显成功，电脑 ping 得到应答；断开重连（地址不变）仍为 .200；`setDNS` 后用新 DNS 解析成功；已连接时改为 .201 立即生效；已连接时 `config(0.0.0.0)` 后 2 s 内由 DHCP 取得地址；DHCP 下 `setDNS` 转为静态地址，之后重连地址不变。经 bridge 由 WiFiS3 调用（见 bridge README）。地址冲突检测（ARP 探测）没有做 |
| `WiFi` AP：`beginAP`（WPA2 或开放网络，可设信道）、`status`（`WL_AP_LISTENING` / `WL_AP_CONNECTED` / `WL_AP_FAILED`）、AP 地址与 SSID 查询、`end` 停止 AP；同一时间只运行 STA 或 AP；AP 地址可用 `config` 设置，默认 `192.168.4.1/24`，DHCP 地址池为 10 个地址 | experimental | WiFiAPCheck 通过 `compile_check.sh` | 实板 run-ap-2：电脑取得 `192.168.4.2`，AP 状态 8，ping 应答；WiFiServer TCP 与 WiFiUDP 均完成电脑—板子回显；AP 运行时 `begin` 切换 STA 成功。AP 下 `WiFiClient::connect`、`ping(IPAddress)`、多设备连接未验证 |
| WiFi 的 UDP 组播（底层包未开 IGMP，`beginMulticast` 返回 0）、`setHostname` | unimplemented | — | `setHostname` 尚无接口 |
| Wire、SPI 等其他平台库 | unimplemented | — | — |
| 镜像后处理 | experimental | Blink、SerialEcho 通过 `check_image.py`；启动头与 SDK 原生镜像只在长度和 CRC 上不同，`partition.bin`、boot2 逐字节相同 | 烧录后固件正常启动 |
| 上传（USB，1200 bps touch 自动进入下载模式） | experimental | 用假烧录程序核对过 `arduino-cli upload` 展开的命令行 | 首块板：从手动进入的下载模式上传 1 次、经运行中固件的 USB 串口上传 4 次，均写后校验通过并自动运行新固件；之后软件上电复位回到固件而不是下载模式；早先手动下载模式烧录后读回与 boot2、分区表、固件逐字节一致 |
| 包索引安装 | experimental | builder 的 `tools/install_check.sh`：本地包索引安装到空目录、移动数据目录、只读安装、修改 Core 后都能编译，工具包与 manifest 一致 | 公网索引未做；0.2.0 起通过构建目录运行副本，安装目录可只读 |

### Program Flash filesystem

平台库 `LittleFS` 与 bridge 独立；具体 API、错误与布局见
[libraries/LittleFS/README.md](libraries/LittleFS/README.md)。依赖固定包 0.6.0，
存储416 KiB，固件上限1.5 MiB；挂载失败不自动格式化。当前为 experimental：
BL616CL 1V0 实机已验证显式格式化、跨块读写、满盘恢复、复位与写循环中断恢复；
全片读回验证格式化范围及普通 bridge 上传的 FS 字节保持。真实掉电验收仍待现场配合。
软件构建、map 和镜像检查只作辅助，不计实机通过。
