# RA4M1 AT 往返诊断

目标为专用 BL616CL 配对板上的 RA4M1 Sketch；不修改 BL616CL 固件。
固定 115200、每次响应最多 10 秒，每组 20 次。电脑发单次 `S` 启动，测量中
只在 RA4 记录计数，全部结束后输出；排除主机 USB/IP 往返对本地计时的影响。

正常模式比较直接 Serial2 和安装版 WiFiS3 Modem 的 AT、FWVERSION、GETSTATUS。
直接模式记录 write 返回、首字节可读、完整终止符三处相对时间；这些是软件
观测点，首字节可读不等于线上首位电平或 BL 内部 dispatch 时间。

`--scan-window` 使用本仓库 ArduinoBLE，依次扫描关/开/再关，各测 AT 及
未分配 slot 2000 的 SSLAVAILABLE、SSLPEEK。后两者应严格返回 ERROR；故意
选无 TLS 连接的拒绝路径来隔离无线数据等待，只用于确认 window 控制开销，
不能代替有效 TLS/吞吐/共存验收，不缩短现有扫描保护延迟。

```sh
python3 tools/run_ra4_at_roundtrip_timing.py --build-dir build/at-timing-new-run
python3 tools/run_ra4_at_roundtrip_timing.py --scan-window --build-dir build/at-scan-new-run
```

每次选择独立 build-dir 保留失败源、ELF、capture；runner 最终恢复标准 WiFiS3
扫描 Sketch。无上游 AP 或 STA 凭据依赖。主机校验要求样本完整、唯一、成功，
且原始时间满足先后关系，不以缺样本平均值或失败重试替代完整通过。
