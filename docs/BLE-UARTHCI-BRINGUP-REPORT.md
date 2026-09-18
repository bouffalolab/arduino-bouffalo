# BL616CL + uarthci flavor 广播静默问题排查报告

**日期**：2026-09-18
**状态**：未解决，收敛到 flavor 固有限制，待 controller 团队修复
**影响**：UNO R4 WiFi bridge 固件（Arduino 侧）无法通过 uarthci 驱动 BLE 广播

---

## 1. 问题陈述

两块固件编译自**同一棵源码树**（`components/wireless/bluetooth/btblecontroller/`），
在同一块 BL616CL 硬件上运行，表现截然不同：

| 固件 | flavor | HCI 命令 | EM 数据 | 空口广播 |
|---|---|---|---|---|
| btble_cli | m2s1（片上 host+controller） | 正常 | 正确 | **✅ 上天**（RSSI -42~-51dBm，15s 内 13~22 次命中） |
| bridge（Arduino） | uarthci（纯 controller，HCI 走外部） | 全通（CC status=0） | 正确（载荷落在正确偏移） | **❌ 静默**（540 设备扫描 0 命中） |

**关键对照**：官方 SDK 自带的 uarthci 例程 `btblecontroller_test` 烧到同一块板，
**同样是 HCI 全通、空口 0 包**。→ 问题不在 Arduino 环境（USB/DAP/任务拓扑均排除），
在 **uarthci flavor 本身**。

**旁证**：SDK README 的 uarthci 支持列表**不含 BL616CL**；
`btblecontroller_software_btdm_reset`、`HCI_UPDATE_UART_CONF` 等代码仅为 BL616/BL618DG 编译。
→ **BL616CL + uarthci 是一条从未被上游验证过的路径**。

---

## 2. 已确认正常的层面（排除项）

### 2.1 HCI 命令面 ✅
五条广播序列命令全部返回 Command Complete，status=0x00：
```
HCI_Reset(0x0C03) → Set_Event_Mask(0x2001) → Set_Adv_Params(0x2006)
  → Set_Adv_Data(0x2008) → Set_Advertise_Enable(0x200A)
```
重复 enable 返回 0x0C（Command Disallowed），佐证 LLM 认为广播已在运行。

### 2.2 EM 数据面 ✅（本日新证实）
修正测试脚本的 `adv_data_len` 编码 bug 后，bridge 的 EM 窗口里**正确出现完整广播载荷**：
- `02 01 06 08 07 "BL616CL"` 落在 **EM+0x1054**
- 与能上天的 cli 的载荷偏移**完全一致**（cli 的 "ZZHWTEST918" 也在 0x1059）

→ LLM 写 EM 的数据通路正常。

### 2.3 调度器软件链 ✅
SRAM 计数器探针（不破坏时序）显示两侧中断分布**完全同构**：

| 计数器 | m2s1（上天）| uarthci（静默）| 含义 |
|---|---|---|---|
| rwip_isr | +60 | +92 | IP 中断服务 |
| T1ISR | +29 | +45 | 仲裁定时器中断 |
| ESTR | +30 | +46 | 事件启动 |
| SPP | +30 | +46 | ET 编程 |
| FIFOISR | +30 | +46 | FIFO 中断 |
| ENDISR | +30 | +46 | 事件结束 |
| TXISR / RXISR / BLEISR | **0** | **0** | 收发中断（两侧都是 0，是常态） |

→ 软件调度链正常，问题在 MAC 取数/发射环节。

### 2.4 EM 尺寸/窗口 ❌ 非根因
- **反向实验**：能上天的 cli 切到 EM_SEL=3（32K）后**仍然发射**（22 次命中）→ EM_SEL/EM 尺寸不是生死线。
- uarthci 在 EM=16（与 m2s1 对齐）配置下**初始化即崩**——说明 uarthci 的资源布局放不进 16K EM，两 flavor 的 EM 需求本就不同。
- 但两者在各自配置下 EM 内容布局一致。

### 2.5 RF 寄存器：仅剩模拟校准级差异
三方对照法（同固件两次运行做噪声基线）排除 30 处 run-to-run 校准噪声后，
真实差异仅 7 处，集中在：
- `TBB`（TX 基带，0x20001058 bit8）
- `RBB2`（RX 基带，0x20001070）
- 5 处校准区（0x1324/0x132c/0x133c/0x1368/0x13d4）

单独调整无效（BZ-arm 实验已试）。

---

## 3. 关键技术教训

### ⚠️ 插桩会破坏 BLE 时序（重要）
**可复现的对照实验**：
- 纯净源码重建的 m2s1 → **上天**（13 次命中）
- 加 `BT_BRINGUP_TRACE` 探针（ISR 里直接写 UART）的同一份 m2s1 → **不上天**（0 命中）

**根因**：UART 输出在 2Mbaud 下每条约 40µs，远超 BLE 半 slot（312µs）的时序裕量，
直接破坏 MAC 事件编程/触发的实时性。

**纠正**：此前基于插桩得到的"FIFOISR/ENDISR 循环但 TXISR 永远为 0"这一核心观测
**不可信**——那是插桩自伤。

**正确方法**：SRAM 计数器探针（`counter[tag]++`，纳秒级，不碰 UART），
空闲任务周期打印。本次已建成 13 个计数器探针。

---

## 4. 已完成的单变量排除实验（全部无效）

| 实验 | 做法 | 结果 |
|---|---|---|
| PDS lib 重编 | 加 `-DCONFIG_BLE_PDS` | 稳定但无效 |
| pds_reset 门控 | 放开 `arch_main.c` 门控 | AT 任务卡死 → 排除 |
| ISO 组 | 去 6 个 define（BIS/CIS/ISO_CON…） | 稳定但无效 |
| LE_PWR_CTRL | 单一变量去除 | 稳定但无效 |
| BZ-arm | `wl_cfg.mode=BZ` + `wl_init()` | 执行成功（ret=0）但无效 |
| WLAN_COEX | `rwip_wlcoex_set(1→0)` | COEX 寄存器归零但无效 |
| EM_SEL | 1↔3 双向切换 | 无效（cli 切 3 仍上天） |
| EM=16 | lib+链接+硬件同步 | HCI 崩溃 → 排除 |
| BTDM 复位 | 移除 | 无效 |
| `rf_init` 补全 | 补 40MHz 调用 | 执行正常但无效 |

---

## 5. 当前工作区状态

### 5.1 SDK 源码树（`bouffalo_sdk_full/`）
**未提交的修改**（git stash 之外）：
```
components/wireless/bluetooth/btblecontroller/
  ├── CMakeLists.txt                    # 全局启用 BT_BRINGUP_COUNTERS（实验代码）
  ├── ip/ll/ble/src/co/rwble.c          # SRAM 计数器探针
  ├── ip/ll/ble/src/lld/lld_adv.c       # SRAM 计数器探针
  ├── ip/ll/sch/src/sch_arb.c           # SRAM 计数器探针
  ├── ip/ll/sch/src/sch_prog.c          # SRAM 计数器探针
  └── modules/rwip/src/rwip_driver.c    # SRAM 计数器探针
examples/btble/btble_cli/main.c         # 加诊断命令：ble2dump/blewide/emdump/blectr/wcheck/emset
```
**已 stash 的插桩改动**：`stash@{0}: bringup-instrumentation-20260918`
（UART trace 探针，会破坏时序，谨慎恢复）

### 5.2 Bridge 侧（`uno-r4-wifi-usb-bridge/` + `arduino-bouffalo/`）
```
libraries/esp32-compat/src/ble/ble_hci_port.cpp   # 诊断函数 + 显式 EM_SEL 写入
libraries/esp32-compat/src/ble/ble_hci_port.h     # 声明
libraries/esp32-compat/src/esp32_compat.cpp       # rfparam 探针
UNOR4USBBridge/cmds_ble_bridge.h                  # AT+BLECTR/EMFULL/WCHECK/RAWDUMP
UNOR4USBBridge/commands.h                         # 命令名定义
platform.txt                                      # __LD_CONFIG_EM_SIZE=32
tools/sdk/bl616cl/lib/libbtblecontroller_bl616cl_uarthci.a  # EM=32 重编版（覆盖原始 .bak-0917）
```
**原始库备份**：`libbtblecontroller_bl616cl_uarthci.a.bak-0917`（md5 `3bc9f344`，纯净版）

### 5.3 硬件
- 板子当前跑 bridge 固件（带全部诊断命令）
- FT232 必须全程保持打开 + DTR=1（否则 BOOT 悬高，意外复位会进 ROM ISP 静默）

### 5.4 可用的测试工具
| 工具 | 路径 | 用途 |
|---|---|---|
| 空口扫描器 | `/tmp/ble_scan`（源码 `ble_scan.swift`）| CoreBluetooth 扫描，15s 窗口 |
| 烧录+抓日志 | `flash_and_log.py` | ISP 进模式→烧录→广播→抓 UART0 |
| 广播测试 | `ble_advertise_test.py` | 驱动 AT+HCI 广播序列 |

---

## 6. 建议的后续路径

1. **交给 controller 团队**（推荐）：本报告即为复现单。
   核心诉求：BL616CL 的 uarthci flavor 是否支持？若支持，
   为何 HCI 全通、EM 写入正确、调度器运行，但 MAC 不发射？
   最直接的方法：把 BL616CL 加入 uarthci 支持列表，
   对照 m2s1 排查 MAC 发射路径的编译宏分支差异。

2. **自建 external-host 库**：以 m2s1 为基础，重接 HCI slot
   到虚拟传输（工作量大，等于重做 flavor 生成）。
   已知障碍：m2s1 的 hci_tl 是内嵌 host 槽位模式，
   直接注入会返回堆损坏图案（`\xa5\xa5` 填充）并杀死 AT 任务。

3. **临时方案**：若短期需要 BLE 功能，可考虑在 bridge 上
   叠加 btble_cli 式完整栈（放弃 external-host 模式），
   但这会改变 RA4M1 侧的 AT 协议假设。

---

## 7. 复现步骤（最短路径）

```bash
# 1. 烧录官方 uarthci 例程
cd bouffalo_sdk_full/bouffalo_sdk/examples/btble/btblecontroller_test
BL_SDK_BASE=$PWD/../../.. gmake CHIP=bl616cl BOARD=bl616cldk
# 烧写 build/build_out/btblecontroller_test_bl616cl.bin（需配合分区表）

# 2. 通过 AT+HCI_WRITE 下发广播序列（或直接跑 btblecontroller_test 的自带流程）

# 3. 用 CoreBluetooth 扫描 15s，观察 0 命中
/tmp/ble_scan

# 对照：烧 btble_cli（同目录 ../btble_cli），同样操作，可见 13~22 次命中
```

---

## 附：2026-09-18 晚 后续进展（里程碑）

- **结论更新**：BLE 广播在 Arduino 工程内已可用，但可行路径是 **m2s1
  完整栈**（片内 host+controller），不是 uarthci external-host。官方
  WiFi+BLE 共存样例 `examples/wifi/sta/smartconfig_ble`（BL616CL 分支即
  用 m2s1）给出了参照序列。
- 验证：新增 `examples/BleAdvTest`（`btble_controller_init` →
  `hci_driver_init` → `bt_enable(cb)` → `bt_le_adv_start`；库取 btble_cli
  产出的 m2s1 + blestack + blemesh，链接 `__LD_CONFIG_EM_SIZE=16`）→
  空口 45s 命中 12 次 `ZZARDUINO918`，RSSI -40~-65 dBm，Connectable=1。
- 平台侧配套：无线初始化（`rfparam_init` + `bl_sys_em_config`）已收口到
  core 的 `init()`（`cores/bl616cl/bl_wireless.cpp`，调度器启动前运行）；
  `ble_hci_port.cpp` 里重复的 EM_SEL/rf_init 补齐代码移除。
- 另修复 WiFi 事件 filter 类型错配（bundle 头 `EV_WIFI=0x0002` vs 实际
  链接的 wl80211 库 `(uintptr_t)wifi_mgmr_init`）——此前 WiFi 事件被静默
  丢弃，`AT+WIFISCAN` 恒返回 -1；修复后正常返回 AP 列表。
- uarthci flavor 自身"HCI 全通、空口 0 包"的问题仍未解决，本报告正文
  即复现单，继续作为与 controller 团队的对接依据。

---

*报告生成于 2026-09-18，基于全源码级排查（16 轮单变量实验 + 寄存器级 A/B 对照）*
