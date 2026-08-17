# Vendored Bouffalo SDK storage components

本目录从 Bouffalo SDK **2.3.30-local-wl80211**（`bouffalo_sdk_full/bouffalo_sdk`）vendored
对应存储组件源码，随 esp32-compat 库一起编译，使平台保持自包含。

## 文件与上游路径

所有路径均相对于 `bouffalo_sdk/components/`：

| 本目录文件 | 上游路径 |
|---|---|
| `lfs.c` / `lfs_util.c` / `lfs.h` / `lfs_util.h` | `fs/littlefs/littlefs/` |
| `lfs_port.h` / `lfs_xip_flash.c` | `fs/littlefs/port/` |
| `easyflash.h` / `lfs_easyflash.c` | `fs/littlefs/easyflash_port/` |
| `port_file_fd.h` / `port_file_littlefs.h` / `port_file_littlefs.c` | `libc/newlib/` |
| `bflb_mtd.h` / `bflb_boot2.h` / `bflb_mtd.c` / `bflb_boot2.c` | `utils/bflb_mtd/`（头文件在 `include/` 子目录） |
| `partition.h` / `partition.c` | `utils/partition/`（`bflb_boot2` 的依赖；计划 cp 清单遗漏，构建冒烟补入） |

各文件保留上游许可证与版权头（LittleFS 为 BSD-3-Clause，其余见各自文件头）。

## 同步策略

- 优先使用 SDK 原文件，不做功能改动；升级 SDK 时直接覆盖对应文件。
- 允许的本地改动仅限：
  1. `lfs_port.h` 顶部强制 `LFS_THREADSAFE=1`（启用 FreeRTOS 递归互斥巨锁）；
  2. 必要的构建适配（如 `#ifndef` 保护、避免与现有符号冲突），且不改变 SDK 逻辑。
     - `lfs_xip_flash.c`：在 `#include "lfs.h"` 前同样设置 `LFS_THREADSAFE`（该文件
       先包含 `lfs.h` 再包含 `lfs_port.h`，否则 `struct lfs_config` 缺少 lock/unlock 字段）。
- 任何额外改动需在提交说明中注明原因。
