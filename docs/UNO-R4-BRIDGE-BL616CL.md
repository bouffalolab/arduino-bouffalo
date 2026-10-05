# UNO R4 WiFi BL616CL 载板：bridge 适配

**状态（2026-10-05）：编译通过（953 616 B flash / 66 332 B RAM），RA4M1
实机链路待载板接线验证。**

bridge 固件仓库是 `uno-r4-wifi-usb-bridge`（上游
`arduino/uno-r4-wifi-usb-bridge`）。BL616CL 的专属实现（runtime、ESP32 API
兼容层、variant、板级引脚）全部放在本平台仓库，bridge 仓库只保留最小移植
补丁，便于持续跟随上游合并。

## bridge 侧最小补丁

| 文件 | 改动 | 原因 |
|---|---|---|
| `UNOR4USBBridge.ino` | `ARDUINO_ARCH_BL616CL` 下 UART0/UART1 用不带引脚参数的 `begin()`；`USB.begin()` 后重开 UART0 | 载板 UART 引脚由 variant 决定；USB 初始化会重新绑定 UART0 控制台 |
| `at_handler.h`、`dap_config.h` | GPIO 映射直接改为载板值（BOOT=10 / RST=3 / SWDIO=9 / SWCLK=8） | 引脚映射属于 app 行为，保留在 bridge 内；平台不注入任何 `-D` 覆盖 |
| `cmds_esp_generic.h` | 平均 RTT 输出 `%.0f` → `%d`（int 截断） | BL616CL libc 以 `CONFIG_LIBC_FLOAT=0` 构建，无 `%f` |
| — | `ping.cpp` / `ping.h` 保持上游不动 | 平台在 `esp32-compat` 里实现了真正的 `esp_ping_*` |

bring-up 期间的诊断命令（`+GETCRASH`/`+GETHEAP`/`+HCISTATE`/`+BLECTR`/
`+WCHECK`/`+EMFULL`/`+RAWDUMP`、`+SSLERR`）、DK 专用 `AT_ON_USBCDC` 通道、
`tools/at_smoke/` 与 bridge README 的 BL616CL 章节均已从 bridge 主线移除；
需要 DK 无 UART 对端调试时请使用单独的 bring-up 分支。

## 平台侧支撑

- `libraries/esp32-compat/src/ping/esp_ping.cpp`：lwIP raw ICMP 上的
  ESP-IDF 语义实现（后台 FreeRTOS 任务、`on_ping_success/timeout/end`
  回调、会话结束自释放）。
- `variants/bl616cldk/pins_arduino.h`：UART0 = GPIO34 TX / GPIO35 RX，
  UART1（`Serial1`）= GPIO6 TX / GPIO7 RX。
- `HCIVirtualTransport::write()` 在 controller 启动前直接返回 0，bridge
  侧不需要额外的前置检查。

## 引脚表（RA4M1 ↔ BL616CL）

| 信号 | BL616CL | 备注 |
|---|---|---|
| RA4M1 RESET | GPIO3 | bridge 通过 `CONFIG_BRIDGE_GPIO_RST` 驱动 |
| RA4M1 AT UART RXD | GPIO6（BL616CL UART1 TX） | AT 命令通道，115200 |
| RA4M1 AT UART TXD | GPIO7（BL616CL UART1 RX） | 同上 |
| RA4M1 SWCLK | GPIO8 | CMSIS-DAP，经电平转换 U4 |
| RA4M1 SWDIO | GPIO9 | 同上 |
| RA4M1 MD | GPIO10 | bridge 的 `CONFIG_BRIDGE_GPIO_BOOT`（下载模式） |
| RA4M1 日志/烧录 UART（SCI9） | UART0：GPIO34 TX / GPIO35 RX | 与 USB CDC 双向透传，115200 |
| RA4M1 AT UART（SCI1） | UART1：GPIO6 TX / GPIO7 RX | AT 服务器，115200 |

`loop()` 里 `SERIAL_USER`（USB CDC）与 `SERIAL_USER_INTERNAL`（UART0）
双向透传；AT 服务器跑在 `SERIAL_AT = Serial1` 上。UART0 同时是 SDK 控制台
（2 Mbaud），因此 UART0 上的 SDK 日志会出现在 RA4M1 的日志口一侧。

## 台架与烧录

- FT232R（UART0）既是烧录口也是控制台：2 Mbaud，`/dev/cu.usbserial-BG02CSA6`
  （macOS 台架）/ `/dev/ttyUSB*`（Linux），flash 脚本支持 `FTDI_PORT` 覆盖。
  调制解调器控制线接 strapping：RTS#→RST（低有效），DTR#→BOOT（低有效）。
- 进入 ROM ISP：BOOT 拉高 + 复位脉冲；烧写：
  `BLFlashCommand-<os> --interface=uart --chipname=bl616cl --port=<FT232> --baudrate=2000000 --firmware=<build>.bin`。
  工作区根目录的 `flash_and_log.py` / `uart0_logger.py` 封装了全流程。
- 板载 USB 枚举为 "UNO WiFi R4 CMSIS-DAP"（VID 0x2341 / PID 0x1002，
  CDC-ACM + CMSIS-DAP HID 复合设备）。

## 设计决定

- **引脚映射保留在 bridge/app 内**：RA4M1 的 RESET/MD/SWCLK/SWDIO 属于载板
  布线，bridge 直接定义；平台只提供芯片级支持（UART 默认引脚等），不用
  build flag 干预 app 行为。
- **DAP SWDIO 保持上游的“每次传输切换方向”实现**，不使用常开双向
  （`GPIO_MODE_INPUT_OUTPUT`）优化：BL616CL 没有 open-drain，双向常开在
  目标驱动 SWDIO 的读阶段会形成推挽对驱。若后续要提速，先做波形与电流验证。
- 无 IP 时不再单独返回 `-4`：平台已把 `tcpip_init()` 提前到 C++ 静态初始化，
  未关联时 ping 表现为普通超时（返回 2），与 ESP-IDF 行为一致。
