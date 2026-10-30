# Preferences

本库是基于 `LittleFS` 的标准 Arduino 平台库，源码位于 `libraries/Preferences/src`。
包含 `<Preferences.h>` 后，Arduino 会发现并编译本库及其声明的 `LittleFS` 依赖。
它不依赖 bridge、RA4M1、AT、ESP32 NVS、SDK 构建 hook 或额外的链接归档。
需要配套的 LittleFS 平台实现，以及固定的 `bl616cl-libs 0.6.0` /
`bl-flash-tools 0.2.0` 包。

公共 API 改编自 Espressif Arduino-ESP32 2.0.9 的 `Preferences.h`
（`https://github.com/espressif/arduino-esp32/tree/2.0.9/libraries/Preferences`），
采用 Apache-2.0 许可，全文见 LICENSE；持久化实现独立编写。
支持的类型化调用包括有符号／无符号 8/16/32/64 位值、long 别名、bool、float、double、
String/C 字符串和字节数组，以及 begin/end、clear/remove、isKey/getType/freeEntries。
在已验收的 BL616CL 目标上，float/double 按本机 IEEE 二进制字节存储；
整数使用显式小端编码。

## 使用与错误处理

`begin(namespace, readOnly=false, partition=nullptr)` 挂载时不格式化。
默认、空字符串和 `"lfs"` 分区标签都指向同一个平台分区，其他标签会失败。
以只读方式打开不存在的命名空间会失败。
新建 LittleFS 时需显式调用其自身 API；任何 Preferences 操作都不会格式化文件系统。
带返回值检查的用法见 `examples/Counter`。
所有 Preferences 和 LittleFS 方法须在任务上下文中串行调用；
库内不加锁，不支持在 ISR 中调用。

命名空间名称和键名长度为 1–15 字节。每个命名空间最多 64 个键，单值最多 1023 字节。
类型化读取要求存储类型匹配，缺失或类型不符时返回调用者提供的默认值。
`lastError()` 使用 littlefs 负数错误码区分 NOENT、INVAL、BADF、NOSPC、IO、CORRUPT 和 NOMEM。
实例未打开／已关闭，或在只读模式下写入时，以 BADF 失败。
应检查错误，不能将所有默认返回值都视为读取成功。
对已打开的实例再次调用 begin 会失败。

put 方法返回成功提交的数据字节数。字符串不含 NUL 终止符；
空字符串返回零，且 **lastError()==0**，该键仍作为 PT_STR 存在。
零字节 blob 会被拒绝。读取 C 字符串时，返回的字节数**包含** NUL；
输出缓冲不足时失败，不复制数据。读取字节数组时返回 blob 长度；
缓冲指针为空或最大长度为零时，仅查询长度。
put 可以改变值的类型。`clear()` 仅影响当前打开的命名空间，`end()` 不写存储。
这是兼容的 API 子集，不是 ESP32 NVS 实现。

`freeEntries()` 返回 **64 减去当前命名空间的键数**，
不是 NVS 全局空闲条目数，也不是可用 Flash 字节数。
物理空间可能先耗尽，原子替换还需要额外临时空间。
存储与普通文件共享，没有预留配额，也不提供安全擦除。

## 存储格式与更新策略

Preferences 使用 `/.preferences/<hex namespace bytes>.kv`，
其中命名空间字节按十六进制编码；同目录的 `.tmp` 是尚未提交的替换文件。
平台 LittleFS 仍提供管理用的原始文件 API。
bridge 文件 AT 拒绝访问该根目录及其子路径，包括 `/./` 和重复斜杠构成的别名，
以防通过该前端误改存储。

版本 1 以 `BLP1` 开头，随后为小端 u16 版本号 1、小端 u16 记录数。
每条记录包含带 NUL 的 key[16]、u8 类型（PT_I8..PT_BLOB = 0..9）、
值为零的 u8 保留字段、小端 u16 长度，以及指定长度的数据。字符串不含 NUL。
未知版本或格式损坏会失败，不复位数据，也不自动迁移。
没有额外的值 CRC，不能保证检测所有介质位损坏。

更新时使用 256 字节拷贝缓冲，将现有记录流式写入 `.tmp`，
成功关闭后再通过原子 rename 替换 `.kv`，不会把整个命名空间复制到栈或堆。
写入失败或中断时保留上一次已提交的快照；遗留 `.tmp` 会被忽略，
并在本库下一次更新时覆盖。多个实例串行使用时，每次重新读取当前快照。
String getter 仅为所需的值分配内存，最多 1024 字节。

采用相同布局的普通 Arduino 上传会保留文件系统。
布局变更和未来存储格式升级需要显式备份／导出及迁移，没有隐式转换。
显式 LittleFS.format 会清除全部命名空间和文件，Preferences.clear 只清除一个命名空间。

## 验收范围

在 BL616CL 1V0 上标为 experimental。以下场景已通过实机验证：
直接平台 API、原版 RA4M1 Modem 二进制往返、真实满容量失败与恢复、写入中复位、
用户确认的完整断电、断电前后全片 Flash 读回不变、标准上传保留文件系统，
以及真实挂载失败时不修改文件系统。
验收证据保存在 ARDUINO-48 task，实机结果与 POSIX/fake 协议检查分开记录。
仅构建成功不代表功能验收通过。其他芯片／板卡和并发访问尚未验证。
