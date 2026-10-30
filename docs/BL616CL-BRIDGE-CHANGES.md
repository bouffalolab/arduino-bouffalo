# BL616CL UNO R4 bridge 变更说明

本文说明相对原主干新增和调整的功能、代码归属及兼容边界，供源码审阅和集成使用。
构建与使用方法见 [BL616CL bridge 使用说明](BL616CL-BRIDGE.md)。

- 比较基线：`ea3c0eb941600d5416bd67a1c4371ef3de16784e`。
- 功能集截至：`85735a4a0ec7d8fe194ebfaf13f37223fcea57ba`，共 23 个新增提交。
- 文档日期：2026-10-30。本说明不代表新的正式固件发布版本。

## 1. 新增功能

| 范围 | 新增内容 | 主要代码位置 |
| --- | --- | --- |
| 独立桥接应用 | 新增 BL616CL 原生桥接应用，提供 RA4M1 AT 服务、原始二进制负载 framing、任务与缓冲区管理 | `UNOR4BL616CLBridge/` |
| USB 与 RA4 串口 | CDC 用户串口透传、波特率与 line-coding 控制、RA4 下载模式及 BOSSA/SAM-BA 上传交接 | 应用中的 `usb_*`、`cdc_*`、`ra4_bootstrap_*`、`ra4_samba_*` |
| Wi-Fi 与网络 | STA、SoftAP、AP+STA、扫描、DNS、主机名、重连、IPv6 link-local、TCP client/server、UDP、TLS 和资源回收 | 应用中的 `wifi_*`、`tcp_*`、`udp_*`、`tls_*`、`bouffalo_*_backend.*` |
| BLE | raw HCI 传输、controller 适配、Wi-Fi/BLE/USB 共存策略，以及 RA4 ArduinoBLE host 修复 | 应用中的 `ble_*`、`bouffalo_hci_*`，`libraries/ArduinoBLE/`、`patches/` |
| 存储与密码学 | LittleFS 文件服务、Preferences、SoftSE、P-256 与持久化密钥槽 | 应用中的 `storage_service.*`、`softse_service.*` |
| RA4 OTA | HTTP/HTTPS 下载、包解码与校验、独立缓存、内部 SAM-BA 编程、失败及中断恢复 | 应用中的 `ra4_ota_*`、`bouffalo_ra4_ota_download_backend.cpp`、`ra4_samba_*` |
| CMSIS-DAP/SWD | 64 字节 HID、DP/AP 与内存访问、halt/reset/run、RA4 Flash 编程支持 | 应用中的 `freedap.*`、`dap_config_bl616cl.h`、`usb_cmsis_dap*` |
| BL 自身 FOTA | 向非活动固件槽写入 BFNP 应用，校验 SHA256 后切换启动槽，支持取消和不完整传输恢复 | `UNOR4BL616CLBridge/self_fota_service.*` |

网络服务同时增加接收窗口、发送预分配、堆归属和资源上限管理，以约束并发连接
与 Wi-Fi/BLE/USB 共存时的内存占用。应用保留运行状态诊断接口，便于维护和故障定位。

## 2. 平台与构建方式

新增板型 `bouffalo:bl616cl:unor4_bl616cl_provisional`，对应
`variants/unor4_bl616cl_provisional/`。该板型使用专用引脚、bridge profile、
分区和构建属性；Ai-M64L 板型及其 SDK 路径继续保留。

桥接应用采用独立的 fhost/m2s1 SDK 配置，与平台原有 wl80211/uarthci 配置分开：

- SDK 源码入口：`third_party/bouffalo_sdk`，固定提交
  `240ff49ba0189ebeba92e6d5c42533f63d90a161`。
- 源码及嵌套依赖锁：`third_party/bouffalo_sdk.lock.json`。
- 配置、原生补丁和源码/供应商边界声明：`tools/bridge_runtime/`。
- 初始化与来源检查：`tools/prepare_bouffalo_sdk_source.py`。
- 运行库生成：`tools/build_bridge_runtime.py`。
- 完整镜像构建：`compile-bl616cl-bridge.sh`。

构建在独立 sketchbook 中解析板型属性，并从锁定源码生成运行库。缓存键覆盖源码锁、
配置、补丁、构建器和工具链版本；源码提交或工作区状态不符时拒绝继续构建。
初始化工具支持空子模块目录和精确提交获取，不覆盖已有错误或未提交的 SDK 工作区。
镜像生成后检查分区容量及 Flash/RAM 预算。

RF/PHY/controller 供应商预编译组件与 ROM ABI 仍是明确保留的依赖边界，不能视为
全部由开放源码重建。SDK 仓库当前需要授权访问；个人 SSH 用户在本机配置，不写入源码锁。

## 3. 对原有核心的调整

| 位置 | 调整与原因 |
| --- | --- |
| `boards.txt` | 增加桥接专用板型和镜像配方属性，隔离不同 SDK/profile 的构建输入 |
| `cores/bl616cl/bl_wireless.cpp`、`wiring.cpp` | 在桥接配置下由应用负责无线初始化，避免核心与应用重复初始化 |
| `cores/bl616cl/WString.h` | 在桥接配置下关闭 `std::string` 转换和相关头文件，隔离桥接 SDK 与 C++ 标准库的 ABI 差异；其他板型保留原接口 |
| `cores/bl616cl/crash_debug.cpp`、`cxx_fault.cpp` | 为桥接配置选择相应的 C++ 故障处理方式 |
| `cores/bl616cl/main.cpp` | 增加由宏控制的 C++ 构造期堆选择，并在任务启动前恢复既定分配顺序 |

`HardwareSerial.*`、`platform.txt` 保留基线实现。桥接特有行为主要通过独立应用、
板型属性与条件编译实现，避免替换原有板型的无线栈或串口 API。

## 4. 兼容性与更新边界

- RA4M1 侧沿用 WiFiS3 AT 命令、响应与原始负载 framing；BLE 通过 raw HCI 对接
  ArduinoBLE。USB 采用 `2341:1002`、CDC 与 64 字节 CMSIS-DAP HID 的接口约定。
- 新增 `partition_cfg_4M_bridge_v1.toml`。RA4 OTA 缓存与用户文件、Preferences/密钥、
  BL 固件槽分开；FW B 与 `mfg` 共享同一物理区域，不能重复计算可用空间。
- RA4 OTA 解码镜像上限为 240KiB。只有完整校验的缓存可进入编程流程，复位会撤销
  缓存编程授权；RA4 单应用区不提供 A/B 原子回滚。
- BL 自身 FOTA 与 RA4 OTA 是不同功能。BL 更新不写 RA4 应用、boot2 或用户数据区，
  也不承担分区布局迁移。
- 自身 FOTA 的 SHA256 用于完整性校验，不等同于签名认证；当前不提供应用健康检查
  后的自动回滚。
- 板型仍为 provisional，适用范围是对应配对载板；不能据此推断最终 PCB、其他载板
  或所有主机平台均已完成产品验收。新增功能需继续满足 Flash/RAM 预算门。

## 5. 测试与维护工具

新增主机协议/状态机测试、RA4 示例与硬件回归入口，覆盖以下范围：

- WiFiS3 命令覆盖、AT framing、USB 串口与上传交接。
- TCP/UDP/TLS 资源、Wi-Fi/BLE/USB 共存、可控 AP 失联及重连策略。
- 连续 32 包 16KiB UDP 双向数据校验。
- BLE 长 GATT 读写、Notify、Indicate 与长通知。
- 文件、Preferences、SoftSE 与复位持久化。
- RA4 HTTP/HTTPS OTA 下载、实际编程、坏包/容量边界及中断恢复。
- pyOCD/OpenOCD 的 DP/AP/内存访问、halt/reset/run 与 RA4 Flash 编程读回。
- BL 自身 FOTA 往返切换、非法输入、超时与传输中复位恢复。

功能集已在对应配对载板完成上述回归；主机测试入口为
`test-bl616cl-at-transport.sh` 和 `test-arduinoble-host.sh`。
编排入口位于 `tools/hardware_regression/`，具体功能 runner 位于 `tools/`。

硬件测试按实时 USB 身份选择目标，ESP 对照设备需要显式配置序列号。Windows Flash
工具路径和外部测试夹具源码位置由本地配置提供。更新/恢复工具增加完整 Flash
备份及读回校验、写入等待和受控读取流量，失败时保留可恢复状态。

## 6. 交付内容

新增 `tools/bl616cl_release.py` 与 `tools/update_bl616cl_release.py`，提供包生成、
镜像哈希与分区校验、目标绑定和更新支持。发行包采用文件白名单，包含固件、更新工具、
使用说明、源码/供应商边界清单及许可证；排除 ELF、map、预处理源码、原始日志、
本地运行库清单和设备 Flash 备份。即使调试文件被加入哈希清单，校验器也会拒绝它。

新增的依赖许可证与声明位于 `licenses/bl616cl/`。内部迁移审计和设备实测原始记录
不属于客户交付内容，构建产物与本地证据保存在忽略目录中。

## 7. 提交分组索引

以下索引覆盖上述 23 个功能与工具提交；同组提交共同完成一项能力，不是固件发布顺序。

| 分组 | 提交 |
| --- | --- |
| 板型、源码运行库、C++ 适配与构建预算 | `c3f6391`、`19a75a5`、`8491964`、`86deaec` |
| AT、USB 与 RA4 串口桥接应用 | `545092c` |
| WiFiS3 网络及资源管理 | `cdeea7d` |
| BLE raw HCI 与 ArduinoBLE host | `d70ed78` |
| 存储、SoftSE、RA4 OTA、SWD 与自身 FOTA | `c16e38c` |
| 主机及硬件回归、持久化、隔离网络和 STA OTA | `b0ecef6`、`8ec576e`、`3851564`、`7670f96`、`27b9414`、`79b97c3` |
| 更新、备份与恢复工具 | `850ffb6`、`6c1499a`、`c8f4890`、`0a08664`、`fc49c02`、`c73a576` |
| 本地硬件与 SDK 认证配置 | `663a2cf` |
| 许可证、对外说明与发行包过滤 | `d6ee59b`、`85735a4` |
