# FS 兼容层（LittleFS）与 Preferences（EasyFlash）设计

日期：2026-08-17
状态：已批准（用户确认设计无问题）
范围：第四阶段第 1、2 项——SPIFFS/FS 兼容层、Preferences NVM 后端

## 1. 目标

在 BL616CL Arduino 平台（`arduino-bouffalo`）的 esp32-compat 库中实现：

1. `SPIFFS`/`FS`/`File` 兼容层，底层对接 Bouffalo SDK 的 LittleFS；
2. `Preferences` NVM 后端，对接 SDK 的 EasyFlash 接口（底层同为 LittleFS）。

额外要求：桥接固件（uno-r4-wifi-usb-bridge）的 OTA 下载与 BOSSA 刷写都通过
`fopen("/spiffs/...")` 的 newlib FILE 接口访问存储，因此本设计同时打通
newlib 系统调用到 LittleFS 的分发路径。

## 2. 总体方案

把 Bouffalo SDK 中现成的存储组件以源码形式 vendored 进 esp32-compat 库，
随 Arduino 构建一起编译。平台保持自包含，不重新生成 runtime bundle，不改动
任何预编译 SDK 库。

### 2.1 Vendored 组件清单

来源均为 `bouffalo_sdk_full/bouffalo_sdk/components/`，拷贝进
`arduino-bouffalo/libraries/esp32-compat/src/` 下的子目录：

| 组件 | SDK 源路径 | 编译后提供 |
|---|---|---|
| LittleFS 核心 | `fs/littlefs/littlefs/lfs.c`、`lfs_util.c` + 头文件 | lfs_* API |
| XIP flash 移植 | `fs/littlefs/port/lfs_xip_flash.c` + `lfs_port.h` | `lfs_xip_init`，bflb_flash 读写擦 |
| EasyFlash | `fs/littlefs/easyflash_port/lfs_easyflash.c` + `easyflash.h` | `ef_set/get_env_blob` 等 |
| POSIX/newlib 层 | `libc/newlib/port_file_littlefs.c` + `port_file_littlefs.h` + `port_file_fd.h` | `lfs_port_full_init`、`_open_file_lfs_r` 等 |
| newlib syscalls | `libc/newlib/syscalls.c` | `_open_r/_read_r/...` 按挂载点分发 |
| MTD | `utils/bflb_mtd/bflb_mtd.c` + `bflb_boot2.c` + 头文件 | `bflb_mtd_open(name)` 查分区 |

保留各文件的许可证与版权头；在 vendored 目录加 README 说明来源与同步策略。

### 2.2 需要的宏

- `LFS_THREADSAFE`：LittleFS 巨锁（FreeRTOS 递归互斥）。
- `CONFIG_FREERTOS`：平台已定义，供各移植层启用 FreeRTOS 锁。
- `CONFIG_LFS_USE_MTD`：port_file_littlefs 默认值 1，走 MTD 查分区。

## 3. API 映射

### 3.1 SPIFFSClass / FS / File

| Arduino API | 实现 |
|---|---|
| `SPIFFS.begin(formatOnFail)` | 尝试挂载；挂载失败且 `formatOnFail==true` 时先 `lfs_format` 再重挂，否则返回 false（ESP32 语义，不用 SDK 的 auto-format 捷径） |
| `SPIFFS.end()` | 卸载 LittleFS（保留 EasyFlash 实例不动） |
| `SPIFFS.format()` | `lfs_format` 后重新挂载 |
| `SPIFFS.exists(path)` | `lfs_stat("/spiffs" + path)` |
| `SPIFFS.remove(path)` | `lfs_remove` |
| `SPIFFS.open(path, mode)` | 打开 `lfs_file_t` 并包装为 `File`；`"r"/"w"/"a"` 映射 `LFS_O_*` |
| `File` | `lfs_file_t` 包装：read/write/seek/position/size/name/close/available/peek/flush；`name()` 返回 open 时传入的路径 |
| 目录 | `File::isDirectory()`、`File::openNextFile()`、`rewindDirectory()`：`lfs_dir_*` 基础遍历 |

路径规则：`SPIFFS` API 的路径直接拼到挂载点 `/spiffs` 后面；`fopen` 的
`/spiffs/...` 路径由 newlib syscalls 层解析。

### 3.2 newlib FILE（桥接固件 OTA/BOSSA 依赖）

- `syscalls.c` 的 `_open_r/_read_r/_write_r/_lseek_r/_close_r/_stat_r/...`
  按路径匹配已注册的 LittleFS 挂载点，命中则转发到 `_*_file_lfs_r`；
  未命中保持原有 tty/控制台行为（printf/串口不受影响）。
- 桥接固件 `OTA.cpp`（`fopen("/spiffs/...", "wb")` + `fwrite`）与
  `BossaUnoR4WiFi::program`（`fopen` + `flash()`）由此路径直接可用。

### 3.3 Preferences → EasyFlash

| Arduino API | 实现 |
|---|---|
| `begin(name)` | `easyflash_init()`（幂等）；记录命名空间 |
| `putX(key, value)` | 编码 `<type><len><data>` 后 `ef_set_env_blob("ns/key", ...)` |
| `getX(key, default)` | `ef_get_env_blob` 解码；无 key/类型不符返回 default |
| `getType(key)` | 读 blob 首字节 |
| `remove(key)` | `ef_del_env` |
| `clear()` | `ef_foreach_env` 删除本命名空间全部 key |
| `freeEntries()` | `ef_foreach_env` 统计本命名空间 key 数 |

键名：`namespace/key` 拼接（EasyFlash key 最大 64 字节，其 `kv_key2path`
已转义 `/` 与 `}`）。`end()` 不刷盘——EasyFlash 每次写即时持久化。

## 4. 分区与挂载点

| 用途 | 分区 | 地址 | 大小 |
|---|---|---|---|
| FS 数据区（LittleFS） | `media` | 0x378000 | 0x71000（452 KB） |
| NVM（EasyFlash） | `PSM` | 0x3E9000 | 0x8000（32 KB） |

- 两个 LittleFS 实例分别挂在不同分区，互不干扰。
- `media`/`PSM` 均已在现有分区表（`tools/partitions/partition_cfg_4M.toml`）
  中，无需改表。
- 挂载点固定为 `/spiffs`，与桥接固件现有路径前缀一致。

## 5. 线程安全

- LittleFS：`LFS_THREADSAFE` + `lfs_xip_init` 创建的 FreeRTOS 递归互斥巨锁。
- EasyFlash：`lfs_easyflash.c` 自带 env_giant_lock。
- 调用方：AT 任务、loop 任务均可并发访问；Flash 写操作在锁内串行化。

## 6. 构建集成

- 全部源码置于 esp32-compat 库 `src/` 下，Arduino builder 自动编译 .c/.cpp。
- `platform.txt` 的 `compiler.sdk.includes` 已包含 SDK 头文件路径
  （`sdk/lhal`、`sdk/flash`、`sdk/utils/...`），vendored 源码可直接 include。
- 链接保持现状（LFS/EF 无新增外部库依赖；`bflb_flash` 在 liblhal）。

## 7. 验证计划

新增 `examples/StorageTest` 测试固件，输出到 UART0 控制台：

1. **FS**：format → 写/读/追加 → seek/position → rename → remove → exists →
   目录遍历。
2. **fopen**：`fopen("/spiffs/x")` 写读、`fwrite`/`fread`、关闭后重开。
3. **Preferences**：全部类型 put/get round-trip、getType、remove、clear、
   freeEntries、字符串/blob 边界。
4. **持久化**：写入标记 → 重启 → 读回（两次烧录/复位验证）。
5. **回归**：bridge 固件编译通过；AT 冒烟（网络 + TLS）不回归。

## 8. 已知风险与对策

1. **newlib syscalls 覆盖**：现有固件 `_open_r` 为默认桩；引入 SDK
   `syscalls.c` 后需确保 printf/控制台不受影响（funopen 已绕过 `_open`）。
   对策：第一步先编译冒烟，确认控制台输出正常后再继续。
2. **bflb_boot2 分区表读取**：MTD 依赖从 flash 读取真实分区表；板上
   media/PSM 已存在，预计直接可用。对策：StorageTest 首步打印分区信息验证。
3. **EasyFlash 与 PSM 冲突**：当前固件未使用 SDK PSM 组件；若后续引入，
   需评估共存。记录在案即可。

## 9. 不在范围内

- `Update`/`Arduino_ESP32_OTA` 后端（第 3 项）、BOSSA 实机刷写（第 4 项）、
  证书分区读取（第 5 项）——依赖本设计落地的存储层，后续另行实施。
- 目录 API 的完整 ESP32 语义（如 `openNextFile` 递归）只做基础版。
