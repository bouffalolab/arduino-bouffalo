# Arduino BL WiFi 库

本平台库在 BL616CL 上提供兼容 Arduino WiFiS3/WiFiNINA 风格的接口，状态为
**experimental**。包含 `WiFi.h` 可使用 STA/AP、扫描、地址配置、DNS 和 ping；
网络收发使用 `WiFiClient`、`WiFiServer`、`WiFiUDP` 和 `WiFiSSLClient`。
当前本地候选组合为平台 0.1.0、`bl616cl-libs` 0.9.0 和 `bl-flash-tools` 0.2.0，
主机为 Linux x86_64，板卡为 BL616CL UNO R4 WiFi 1V0。
其他主机系统和 1V1 模组尚未验收。

## 模块归属与使用

C++ API 负责 Arduino 兼容语义。私有 `wifi_port`、`net_port` 和 `tls_port`
接口适配固定 SDK 的 wl80211、lwIP 和 mbedTLS。通用网络能力由本库提供；
AT 命令和 RA4M1 策略由独立的 bridge 应用负责。
每条连接的调用应在应用任务上下文中串行执行。
客户端对象的副本共享 socket，析构时不关闭连接，使用结束后须显式调用 `stop()`。
服务端保留 `available()` 返回的客户端；`accept()` 将一条新连接交给调用者管理。

已验收的 STA/AP 并行场景要求两者使用同一信道。
支持 DHCP 主机名、SNTP 时间戳和 STA 链路本地 IPv6 地址；
不承诺 IPv6 路由或跨信道 STA/AP。UDP 数据报上限为 1460 字节。
组播接口明确拒绝 `224.0.0.1`，该地址是 SDK 隐式加入的所有主机组。
示例见 `examples/WiFiMulticast` 和 `examples/WiFiTime`。

TLS 使用内置或显式指定的 PEM 根证书校验证书链信任和对端主机名。
现行策略不检查证书有效期，即使已完成 SNTP 同步也不检查。
客户端身份使用软件 PEM 密钥，不提供安全元件能力。
每条 TLS 会话占用较多堆内存，连接成功不代表保证固定数量的并发会话。
sketch 提供的凭据指针在替换前及连接期间必须保持有效。
返回值和超时约定见公共头文件。

## 构建集成

`library.properties` 通过 `precompiled=true` 和 `ldflags`，
选择固定依赖工具包中的响应文件。
Arduino CLI 1.5.1 只有在 `src/bl616cl/` 非空时才追加这些链接参数，
因此该目录中的占位文件需要保留。
实际归档、头文件、ABI 参数和启动资源属于 `bl616cl-libs`，不存放在本库目录中。
目录非空这一条件是依赖 CLI 实现的兼容选择，
与规范中的 `src/{build.mcu}/lib*.a` 库内二进制布局不同。
升级 Arduino CLI 时必须重新检查链接命令和 map。普通 sketch 不构建 SDK。

## 验收证据与许可

Wi-Fi、网络、时间和组播的分项实机结果记录在 ARDUINO-45、ARDUINO-49 至
ARDUINO-51；统一本地候选回归记录在 ARDUINO-53。
构建、宿主 socket 和 fake 检查仅作为辅助证据，本 README 不新增实机验收结论。
本库沿用仓库的 LGPL-2.1 许可，全文见 `LICENSE`。
SDK 库和厂家工具继续采用各自记录的许可。
