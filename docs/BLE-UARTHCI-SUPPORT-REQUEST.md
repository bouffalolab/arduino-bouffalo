# BL616CL uarthci（external-host）BLE 不发射 —— 背景说明与协助请求

**日期**：2026-09-18
**提交方**：UNO R4 WiFi / BL616CL 适配项目（bridge + Arduino core）
**对象**：BLE controller（btblecontroller）团队

---

## 1. 一句话问题

同一块 BL616CL 硬件、同一棵 `components/wireless/bluetooth/btblecontroller/`
源码树、同一工具链（Xuantie GCC 10.4.0），只换 flavor 编译宏集合：

| flavor | 形态 | 结果 |
|---|---|---|
| **m2s1** | 片上 host + controller | ✅ 广播正常上天（RSSI -42~-51 dBm，15s 内 13~22 次命中） |
| **uarthci** | 纯 controller，HCI 走外部主机 | ❌ HCI 全通、EM 数据正确、调度器正常，但 **MAC 从不发射，空口 0 包** |

**关键**：官方 SDK 自带的 uarthci 例程 `examples/btble/btblecontroller_test`
（纯 controller + 真 UART H4 host，非我们的虚拟传输）烧到同一块板，
**同样是 HCI 全通、空口 0 包**。因此问题不在应用侧（Arduino 环境、USB、
任务拓扑、虚拟传输均被此对照排除），在 **uarthci flavor 本身或其与
BL616CL 的配对上**。

## 2. 现象细节（uarthci 侧）

广播序列全部返回 Command Complete，status=0x00：

```
HCI_Reset(0x0C03) → Set_Event_Mask(0x2001) → Set_Adv_Params(0x2006)
  → Set_Adv_Data(0x2008) → Set_Advertise_Enable(0x200A, 1)
```

- 重复 enable 返回 0x0C（Command Disallowed）→ LLM 认为广播已在运行；
- EM 窗口里能读到完整广播载荷（`02 01 06 08 07 "BL616CL"` 落在
  **EM+0x1054**），与能上天的 m2s1 载荷偏移完全一致 → LLM 写 EM 正常；
- SRAM 计数器（不破坏时序的探针）显示两侧中断分布同构：
  T1ISR / ESTR / SPP / FIFOISR / ENDISR 计数一一对应（调度链在跑）。

**结论：数字域全通（HCI → LLM → EM → 调度器），断点在 MAC 取数/发射。**

## 3. 已排除项（全部实机单变量实验，非推断）

| 假设 | 实验 | 结果 |
|---|---|---|
| EM 尺寸 | m2s1 切 EM_SEL=32K 后仍发射；uarthci 切 EM=16 初始化即崩 | 非根因 |
| EM 硬件窗口 | GLB_SRAM_CFG3 探针 = 链接值一致 | 非根因 |
| RF 参数 / eFuse | rfparam ret=0；WiFi 全链路（扫描/连接/DHCP/ping）正常 | 正常 |
| RF 寄存器 | 三方对照法排除 30 处 run-to-run 校准噪声后，真实差异仅 7 处（TBB TX 基带 bit8、RBB2、5 处校准区），单独调整无效 | 疑似残留差异 |
| PDS 域 | 重编加 `-DCONFIG_BLE_PDS`：稳定但无效；放开 pds_reset 门控：AT 任务卡死 | 否决（CL 上 PDS 恢复路径疑似不存在） |
| BTDM 域复位 | 补上后无变化 | 排除 |
| ISO 组 define | 去 6 个 define 重编 | 无效 |
| LE_PWR_CTRL | 单变量去除 | 无效 |
| WLAN coex | 置 0（两 flavor 实际都带此宏） | 无效 |
| BZ-arm / rf_init 补全 | 均执行成功 | 无效 |

**射频硬件确认正常**：同一块板 m2s1 广播正常、WiFi 收发正常。

## 4. 两个 flavor 的精确编译差异（同源，仅 flags.make 不同）

**m2s1 独有**（功能性）：
```
-DCONFIG_BLE_HOST          # 片上 host 槽位
-DCONFIG_BLE_PDS
-DCONFIG_EM_SIZE=16
```
（另有 mesh/host 应用级宏：CONFIG_BT_MESH_*、CONFIG_BT_SETTINGS、
CONFIG_BLE_TP_SERVER、CONFIG_BT_TP_CLI、CONFIG_HW_SEC_ENG_DISABLE）

**uarthci 独有**：
```
-DCONFIG_BIS -DCONFIG_CIS -DCONFIG_EXT_ADV -DCONFIG_ISO_CON
-DCONFIG_ISOGEN -DCONFIG_ISOOHCI -DCONFIG_ISOPCM      # ISO 全家
-DCONFIG_BLE_TEST_MODE_SUPPORT
-DCONFIG_LE_PWR_CTRL
-DCONFIG_EM_SIZE=32
```

两侧都定义：`CONFIG_WLAN_COEX`、`CONFIG_BLE_EMB`、`CONFIG_EMB`。

链接侧：m2s1 例程 `__LD_CONFIG_EM_SIZE=16`，uarthci 例程/桥接固件 `=32`。

**旁证**：SDK README 的 uarthci 支持列表不含 BL616CL；
`btblecontroller_software_btdm_reset`、`HCI_UPDATE_UART_CONF` 等代码
仅为 BL616/BL618DG 编译 → **BL616CL + uarthci 疑似上游从未验证过的路径**。

## 5. ⚠️ 方法论提示（避免复现者误判）

**在 ISR 里写 UART 的 trace 探针会破坏 BLE 时序**：2 Mbaud 下每条约
40 µs，远超半 slot（312 µs）的裕量。可复现证据：纯净 m2s1 能上天，
加 UART trace 探针的同一份 m2s1 变成 0 命中。**任何基于此类插桩的
观测（例如"TXISR 永远为 0"）不可信**。正确方法 = SRAM 计数器
（`counter[tag]++`，纳秒级），空闲任务周期打印。

## 6. 想请团队回答的问题

1. **BL616CL 是否支持 uarthci flavor？** 若支持，为什么数字域全通
   （HCI/EM/调度器均正常）而 MAC 不发射？建议对照 m2s1 排查 MAC 发射
   路径的编译宏分支差异（同源同编译器，差异必来自宏定义分支）。
2. uarthci 库与 CL 的 BLE controller ROM 是否配对？CL 的 ROM 是否缺少
   PDS-aware 恢复路径（本板 PDS 复位实验均被否决）？
3. `btble_adv_api.h`（`CONFIG_BLE_BEACON_ONLY`，绕过 HCI 直接控制广播）
   在 BL616CL 上是否可用？能否作为 external-host 场景的替代？
4. 上游 `bouffalolab/arduino-bouffalo` PR #10（Wip migrate bl616cl
   bridge）在 BL616CL 上选的也是 **m2s1**（defconfig 注释
   "Keep the Wi-Fi/BLE coexistence controller selected by the BL616CL SDK
   demo"）——是否意味着 CL 上 external-host 路线不被推荐？

## 7. 最短复现步骤

```bash
# 1. 构建官方 uarthci 例程
cd <sdk>/examples/btble/btblecontroller_test
BL_SDK_BASE=$PWD/../../.. gmake CHIP=bl616cl BOARD=bl616cldk
# 烧写 build/build_out/btblecontroller_test_bl616cl.bin（需配合分区表）

# 2. 下发 HCI 广播序列（或跑例程自带流程）

# 3. 空口扫描 15s → 0 命中
# 对照：同目录 ../btble_cli（m2s1），同样操作 → 13~22 次命中、
#       RSSI -42~-51 dBm
```

## 8. 附：证据与产物清单（本仓库内）

| 内容 | 位置 |
|---|---|
| 完整排查报告（16 轮单变量实验 + 寄存器级对照） | `docs/BLE-UARTHCI-BRINGUP-REPORT.md` |
| uarthci 纯净基线库（md5 `3bc9f344`） | `tools/sdk/bl616cl/lib/libbtblecontroller_bl616cl_uarthci.a.bak-0917` |
| m2s1 库（btble_cli 构建产物） | `tools/sdk/bl616cl/lib/libbtblecontroller_bl616cl_m2s1.a` |
| 应用侧取证代码（BLE2 寄存器/EM/原始内存 dump） | `libraries/esp32-compat/src/ble/ble_hci_port.cpp` |
| 已验证可行的替代路径（m2s1 + blestack 完整栈在 Arduino 内广播成功，空口 12 次/45s 命中） | `examples/BleAdvTest/` |

SDK 版本（boot banner）：`component_version_sdk 1.1.1 674ea3f0+`，
`lib_version_btblecontroller_1.6.198`；
构建用源：`components/wireless/bluetooth/btblecontroller`（Bouffalo 内网
全源码树，非 binary 发布版）。

**附注**：本请求聚焦 uarthci external-host 路线；我们本地已有可用的
替代方案（m2s1 完整栈，见上表），若 uarthci 在 CL 上确认不支持，
请回复确认即可，我们将跟随上游 PR #10 的方向收敛架构。
