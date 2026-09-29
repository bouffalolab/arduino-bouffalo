# BL616CL UART HCI 广播实测里程碑（2026-09-24）

## 结论与范围

同一块 BL616CL 板上，使用 `/home/virusv/bouffalolab/bouffalo_sdk_full/bouffalo_sdk`
的 `examples/btble/btblecontroller_test` 固件，通过 UART1 H4 HCI 配置 BLE
广播成功。本机 Intel `hci0` 在空口扫描中发现名称 `BL616CL-HCI`、地址
`B4:E8:42:3C:A7:DD`，RSSI 约 `-40 dBm`。这同时验证了 HCI 命令通路和
射频发射；仅有 Command Complete `status=0x00` 不足以证明空口广播。

这是独立 SDK 例程的实机基线；后续已使用重建后的 Arduino bridge 固件完成
同一块板的 AT+HCI 空口验证，详见下节。
此前 [排查报告](BLE-UARTHCI-BRINGUP-REPORT.md) 和
[协助请求](BLE-UARTHCI-SUPPORT-REQUEST.md) 中“官方 uarthci 例程同板空口
0 包”的描述是 9 月 18 日在另一 SDK 工作树上的历史观测，不再适用于本次
指定 SDK 及其已烧录镜像；造成差异的具体源码或构建因素尚未定位。

## 软件 HCI 与 Arduino bridge 验证

随后在同一 SDK 示例中启用 `CONFIG_BTBLE_SOFT_HCI=y`，通过 UART0 Shell
直接解析连续十六进制 H4 Command，不初始化物理 UART1。四条命令均返回
`status=0x00`，扫描再次发现 `B4:E8:42:3C:A7:DD / BL616CL-HCI`。

Arduino bridge runtime bundle 已使用当前 SDK 和 `/home/virusv/linux64/bin`
GCC 10.2.0 重建并覆盖，解决了原 bundle GCC 10.4 LTO 与本机工具链不兼容的
问题。bridge 编译成功并烧录到 `/dev/ttyUSB0`；其 AT 通道是板载 USB CDC
`/dev/ttyACM0`，不是烧录 UART。实测结果：

```text
AT+HCIBEGIN       -> +HCIBEGIN: OK
AT+HCISTATE?      -> +HCISTATE: 1,0,0,1,0,0,1,1,1,0,2,0
AT+HCIWRITE       -> OK
AT+HCIAVAILABLE?  -> +HCIAVAILABLE: 7
AT+HCIREAD        -> 04 0E 04 05 <opcode LE> 00
```

Reset、LE Advertising Parameters、Advertising Data、Advertising Enable 四条
命令均通过 `AT+HCIWRITE` 成功，空口扫描发现 `BL616CL-HCI`。bridge 中
`HCIVirtualTransport.cpp` 的 `HCI_TL_H4` 已从错误的 `0` 修为 SDK 枚举值 `1`。
因此 Arduino `AT+HCI` 路径已完成实机验证，原问题是 transport type、旧
runtime bundle/toolchain 及 API/header 版本不匹配的组合，而不是 BLE RF 或
HCI 命令本身不可用。

## 固件与连接

| 项目 | 本次实测值 |
|---|---|
| SDK | `/home/virusv/bouffalolab/bouffalo_sdk_full/bouffalo_sdk`，顶层 HEAD `2e9e55ec2`；工作树有改动，不能仅凭此 commit 重建相同镜像 |
| 示例 | `examples/btble/btblecontroller_test`，`CHIP=bl616cl BOARD=bl616cldk` |
| 已烧录镜像 | `build/build_out/btblecontroller_test_bl616cl.bin`，SHA256 `e720d5dfc0d3f6768ec9a1cb0ef4589f26ca8735a18b697b8e4f38b39925db65` |
| 烧录口 | `/dev/ttyUSB0`，FT232 序列号 `BG02CSA6`，UART0/ISP |
| HCI 口 | `/dev/ttyUSB1`，FT232 序列号 `BG03YFET`，UART1 H4，2,000,000 baud，8N1 |
| UART1 引脚 | BL616CL GPIO27 TX、GPIO28 RX、GPIO29 CTS、GPIO30 RTS |
| Host 串口设置 | `rtscts=False`，`dsrdtr=False` |

固件 `main.c` 的 `btble_uart_config_t` 设置 UART1、DMA、2 Mbps 和
`flow_ctrl_enable=1`。本次虽然按板上接线使用 CTS/RTS，引脚在 FT232
主机侧未形成可用的硬件流控：启用主机 `rtscts` 会阻塞发送，关闭后 HCI
收发正常。尚未测定具体是哪根控制线或哪一侧配置导致该现象；不要据此
修改固件的流控设置并声称已经验证。

设备名可能随插拔顺序变化；复测时先用 `/dev/serial/by-id/` 核对序列号。
`ttyUSB0` 只用于 ISP，HCI 命令发给 `ttyUSB1`。

## HCI 验证记录

按 SDK [HCI_TEST.md](../../../bouffalolab/bouffalo_sdk_full/bouffalo_sdk/examples/btble/btblecontroller_test/HCI_TEST.md)
第 5 节逐条发送，每条等待完整 Command Complete 后再发下一条：

| 命令 | H4 发送字节（十六进制） | 实际应答 |
|---|---|---|
| HCI Reset | `01 03 0C 00` | `04 0E 04 05 03 0C 00` |
| LE Set Advertising Parameters | `01 06 20 0F A0 00 A0 00 03 00 00 00 00 00 00 00 00 07 00` | `04 0E 04 05 06 20 00` |
| LE Set Advertising Data | `01 08 20 20 10 02 01 06 0C 09 42 4C 36 31 36 43 4C 2D 48 43 49 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00` | `04 0E 04 05 08 20 00` |
| LE Set Advertising Enable | `01 0A 20 01 01` | `04 0E 04 05 0A 20 00` |

四个应答的最后一字节均为 `00`。广播类型 `03` 为
`ADV_NONCONN_IND`，间隔 `A0 00` 为 100 ms，信道掩码 `07` 为
37/38/39。Advertising Data 的 HCI 参数长 `0x20`，其中首字节
`0x10` 是有效数据长度，余下补齐到固定 31 字节数据字段；发送短版
参数会返回 `0x12`（Invalid HCI Command Parameters）。

本机 `hci0` 通过 `btmgmt --index 0 find -l` 扫描约 10 秒，输出包含：

```text
hci0 dev_found: B4:E8:42:3C:A7:DD type LE Public rssi -40 flags 0x0004
AD flags 0x06
name BL616CL-HCI
```

扫描结束后本机 `hci0` 已恢复 `DOWN`；板端广播保持开启。

## 复测步骤

在指定 SDK 根目录构建并通过 ISP 口烧录（烧录需要板子进入 BootROM
模式，结束后复位到应用）：

```bash
cd /home/virusv/bouffalolab/bouffalo_sdk_full/bouffalo_sdk/examples/btble/btblecontroller_test
make CHIP=bl616cl BOARD=bl616cldk
make flash CHIP=bl616cl COMX=/dev/ttyUSB0
sha256sum build/build_out/btblecontroller_test_bl616cl.bin
```

Host 端用 `pyserial` 打开 `/dev/ttyUSB1`，设置 2 Mbps、8N1、
`rtscts=False`，按上表发送四条 H4 命令并检查每条应答末尾为 `00`。
最后用本机另一个蓝牙适配器做空口确认：

```bash
sudo hciconfig hci0 up
sudo btmgmt --index 0 find -l
# 看到 BL616CL-HCI 后用 Ctrl-C 停止扫描
sudo hciconfig hci0 down
```

本机 `btmgmt` 在非交互终端下不显示发现事件，需在终端运行，或用
`script -qefc 'sudo btmgmt --index 0 find -l' /dev/null` 提供伪终端。

## 后续工作

Arduino bridge 的 `AT+HCI` 空口广播已验证。后续可继续对照独立 SDK
固件，定位旧 SDK 工作树静默的具体差异，并单独验证其他 HCI 命令、
连接流程及长期稳定性；本里程碑不涵盖这些功能。
