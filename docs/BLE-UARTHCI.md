# BL616CL BLE UART HCI（AT+HCI）状态

**状态（2026-10-03）：已使用公共 SDK 跑通并实机验证。**
`bouffalo_sdk` v2.3.36 首次公开发布 BL616CL `uarthci` 控制器归档
（`components/wireless/bluetooth/btblecontroller/lib/`，1.6.210，
sha256 `621ba45b…`）。Arduino bridge 直接链接该归档，
`tools/vendor/` 和 legacy `tools/sdk/bl616cl/` bundle 均已删除。

## 架构

RA4M1 侧跑 BLE host（ArduinoBLE 或任意 HCI host），通过 bridge 的 AT 命令
（`AT+HCIBEGIN` / `AT+HCIWRITE` / `AT+HCIREAD` / `AT+HCIAVAILABLE` /
`AT+HCIWAIT`）把 HCI 字节流透传给 BL616CL；BL616CL 只跑 controller
（uarthci external-host flavor），广播参数全部由 host 决定。

bridge 侧实现：`libraries/esp32-compat/src/ble/ble_hci_port.cpp`、
`libraries/esp32-compat/src/utility/HCIVirtualTransport.cpp`；
`platform.txt` 链接 `-lbtblecontroller_bl616cl_uarthci`。

## 实机验证记录（2026-10-03）

- 固件由子模块 v2.3.36（`8a3df34c`）现编，启动日志：
  `component_version_sdk: 1.1.1 8a3df34c+`、
  `lib_version_btblecontroller_1.6.210`。
- HCI 序列（H4 发送 → Command Complete 应答），四条 `status=0x00`：

| 命令 | H4 发送 | 应答 |
|---|---|---|
| HCI Reset | `01 03 0C 00` | `04 0E 04 05 03 0C 00` |
| LE Set Advertising Parameters | `01 06 20 0F A0 00 A0 00 03 00 00 00 00 00 00 00 00 07 00` | `04 0E 04 05 06 20 00` |
| LE Set Advertising Data | `01 08 20 20` + 32 字节参数（H4 帧共 36 B） | `04 0E 04 05 08 20 00` |
| LE Set Advertising Enable | `01 0A 20 01 01` | `04 0E 04 05 0A 20 00` |

- 空口扫描（`bluetoothctl` MGMT 路径）可见
  `B4:E8:42:3C:A7:DD BL616CL-HCI`。
- ⚠️ Advertising Data 是定长命令：参数长度字段 `0x20`（32 字节），
  H4 帧必须完整 36 字节；少发字节的短帧会被 controller 静默吞掉。

## 复现

编译/烧录与普通 sketch 相同（runtime 会从 SDK 子模块现编）：

    arduino-cli compile --config-file arduino-cli.yaml \
      --fqbn bouffalo:bl616cl:unor4_bl616cl <sketch>

串口约定（本测试台）：

| 用途 | 设备 | 说明 |
|---|---|---|
| 控制台 / ISP 烧录（DK）；carrier 下为 RA4M1 日志/烧录口 | FT232 `BG02CSA6`（Linux `/dev/ttyUSB1`） | UART0 GPIO34/35 @ 2 Mbaud |
| HCI 物理口（历史实验用） | FT232 `BG03YFET`（Linux `/dev/ttyUSB0`） | UART1 GPIO27 TX / 28 RX @ 2 Mbaud |
| AT 通道 | UART1 `Serial1` GPIO6 TX / 7 RX @ 115200 | 接 RA4M1 SCI1 AT 口（P501/P502） |
| 主机调试 / 用户串口 | 板载 USB CDC `/dev/ttyACM0`（2341:1002） | carrier 构建下与 UART0 双向透传，不再承载 AT |

设备名随插拔顺序变化，复测前先用 `/dev/serial/by-id/` 核对序列号。

## 历史与已作废的结论

2026-09-18 曾在另一棵私有 SDK 工作树上观测到“官方 uarthci 例程 HCI
全通、空口 0 包”，并据此推断 uarthci flavor 在 BL616CL 上不可用；
该推断对当前公共 SDK（v2.3.36 / 1.6.210）不成立。相关的排查报告、
协助请求与 09-24 里程碑文档已删除，需要回溯时见 git 历史
（删除提交之前的 `docs/BLE-UARTHCI-*.md`）。

Arduino AT+HCI 路径早期失败的实际原因（均已修复）：

1. `HCIVirtualTransport` 把 HCI_TL transport type 写成 `0`，
   应为 `HCI_TL_H4 = 1`；
2. 旧 runtime bundle / 工具链与 SDK 头文件版本错配；
3. 测试脚本 Advertising Data 帧长错误（声明 `0x20` 却未发满 32 字节），
   controller 静默吞帧。

方法论教训（仍然有效）：**不要在 ISR 里做 UART trace 探针**——
2 Mbaud 下每条约 40 µs，远超半 slot（312 µs）的裕量，会把本来能上天的
固件打成空口静默；实时路径观测请用 SRAM 计数器 + 空闲任务打印。

## 未覆盖

其他 HCI/ACL 命令、连接/配对路径与长期稳定性仍未测试。
