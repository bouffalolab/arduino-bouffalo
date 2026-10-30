# LittleFS

本库使用板上的程序 Flash，为 BL616CL 提供平台存储。
需要固定的 `bl616cl-libs` 0.6.0 或更新版本（当前本地候选为 0.9.0），
以及 `bl-flash-tools 0.2.0` 包。库内不引用 bridge、AT 或 RA4M1。
内部 read/program/erase/sync 回调直接调用原厂 Flash API。
挂载时读取当前分区表，查找 `lfs`，并验证物理边界及与其他分区的隔离。
不需要 MTD 读写封装或独立的公共块设备库。
分区缺失或无效时明确失败，不猜测地址。

`begin()` 只挂载，不格式化。`format()` 显式初始化预留分区，已挂载时拒绝执行。
仍有文件打开时，`end()` 拒绝卸载。这些状态错误使用 `LFS_ERR_BADF`。
操作均有结果可查，`lastError()` 返回 littlefs 负数错误码。
`File::write()` 返回已写字节数，`read(buffer,n)` 返回字节数或 -1；
单字节 `read()` 在 EOF 或出错时返回 -1，可用 `lastError()` 区分。
`sync()` 和 `close()` 报告提交错误；`flush()` 沿用 Arduino 的 void API，
因此需检查 `lastError()`。

路径须以 `/` 开头，禁止通过父目录穿越。littlefs 的单个名称上限为 255 字节。
支持模式 `r`、`r+`、`w`、`w+`、`a`、`a+`；`w` 截断文件，`a` 追加。
`File` 实现 Arduino `Stream` 和 `Print`、size/position/seek，以及共享句柄的复制语义：
关闭一份副本会使所有副本关闭。文件系统对象的生命周期必须覆盖其打开的文件。
支持创建和删除目录，未实现目录遍历。
`usedBytes()` 返回已分配块的字节数，包含元数据，不是各文件逻辑长度之和。
littlefs 需要元数据和写时复制空间，可用容量小于 `totalBytes()`。

所有文件系统／文件操作须在同一任务中串行执行，不能在 ISR 中调用。
多任务访问时由调用者串行化。库不重定向 SDK 全局 libc/VFS，也没有后台文件系统任务。
成功的 `sync()` / `close()` 会提交数据，尚未提交的写入在中断时可能丢失。
将已提交的临时文件 rename 为目标文件，可为后续 Preferences 提供 littlefs 原子替换能力。
挂载失败不会触发格式化。格式化清除逻辑文件，不保证安全擦除所有曾使用的数据字节。

布局 v1 为 littlefs 预留 `[0x310000,0x378000)`，共 416 KiB，
包含 104 个 4 KiB 块，read/program/cache 粒度为 256 字节。
固件从 0x10000 开始，上限为 1.5 MiB。
FW1 位于 `[0x190000,0x310000)`，同为 1.5 MiB。
`mfg` 保留原命名，与 FW1 地址和大小相同：出厂前用于产测固件，出厂后作为 OTA B 分区。
builder 管理分区 TOML 和链接容量限制资源。
boot2、media、PSM、KEY、DATA 及 factory 地址保持不变。
普通 Arduino 上传配方只写 boot2、分区表和固件，按区段擦除，不包含文件系统镜像。
更新固件不能使用全片擦除。
旧平台／旧布局固件上传、OTA 布局、缩小存储空间和布局变更均属于迁移：
先备份全部 Flash 并导出文件，核对设备和边界，再明确批准并执行迁移。
不提供设备上的自动布局迁移或磁盘格式迁移。

固定 SDK 提供 littlefs 2.11 引擎，磁盘格式为 2.1，采用 BSD-3-Clause 许可，
许可文件随依赖包发布为 `LICENSE.littlefs`。
仅链接上游引擎，不使用 SDK 的自动格式化 port 和 KV/VFS 适配器。

## 默认与自定义分区

需要已验证的 Arduino CLI 1.5.1、advanced size 配方和 Linux x86_64 工具。
默认方案选择 `Tools > Partition Scheme > Default`。
自定义时选择 `Custom`，将 `partitions.toml` 放在 sketch 根目录；
可复制包内 `boot/partition_cfg_4M_lfs_v1.toml` 作为起点。
Flash 上的分区名为 `lfs`，因为厂家镜像工具只接受 1–7 个 ASCII 字符的分区名。
Arduino 库和 API 名仍为 `LittleFS`。

CLI/CI 可用 `--build-property build.bl_partition_file=/absolute/layout.toml`
显式选择分区文件，其优先级高于菜单；相对路径从 sketch 目录解析。
Custom 或显式文件缺失时失败，不回退到默认表。
当前板卡合同保留 boot2、分区表位置、media 和工厂／参数区，
两个大小相等的连续 FW 分区仍从 0x10000 开始，`mfg` 必须完全对应 FW1。
可以在这些边界内调整固件槽和文件系统大小，无需重建 SDK 或修改安装目录。

prebuild hook 在构建目录保存 `layout.toml` 和 `layout.json`。
链接容量限制、镜像后处理、advanced size 报告和上传均使用该快照。
上传前检查最终镜像和分区表的 SHA-256。
普通上传在构建目录运行可写的 FlashCommand 副本，安装的工具保持不变。

`arduino-cli compile --output-dir DIR` 会导出以镜像名为前缀的
`.bin.layout.json`、`.bin.layout.toml` 和 `.bin.partition.bin` 配套文件；
使用 `arduino-cli upload --input-dir DIR` 时，须将它们与镜像放在一起。
执行 Export compiled binary 还会在 sketch 目录生成 `bl-flash/` 交付包，
包含镜像、`partition.bin`、`layout.json` 和 `layout.toml`。
不要单独删除或替换某个配套文件。
独立上传不会重新读取当前 sketch 的分区文件，也不会替换为默认表。
修改布局不会自动迁移文件或启用 OTA。
