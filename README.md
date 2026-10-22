# arduino-bl

通用 Bouffalo Arduino 平台仓库，从 BL616CL 起步，后续逐步支持多芯片。

当前阶段：最小平台骨架。Core 与第一块板卡可以用 arduino-cli 编译链接；镜像后处理、上传和包索引安装尚未实现，也没有实机运行证据。

普通用户的目标体验是通过 Boards Manager / Arduino CLI 安装后直接编译上传，无需完整 Bouffalo SDK。底层依赖由独立构建器发布。

本地共享入口：[文档](../docs/README.md)、[开发规范](../AGENTS.md)、[已确认验收](../docs/project/charter.md)。这些文件目前位于非 Git 的父工作区，独立分发文档的方式待确定。

## 目录

| 路径 | 内容 |
| --- | --- |
| `platform.txt` | 编译、链接、size 配方；参数和库全部来自板卡所选的预编译底层包 |
| `boards.txt` | 板卡：`unor4wifi_bl616cl`（UNO R4 WiFi，协处理器换成 BL616CL） |
| `cores/bouffalo/` | Core：入口、C++ 运行时、GPIO、时间、Serial |
| `cores/bouffalo/api/` | ArduinoCore-API 原样拷贝，来源见下 |
| `variants/unor4wifi_bl616cl/` | 板卡引脚与 Serial 实例 |
| `tests/` | 测试 sketch 与编译检查脚本 |

编译依赖两个工具：底层包 `bl616cl-libs`（由 `arduino-bl-lib-builder` 生成）和 Xuantie GCC V2.6.1。平台通过 `runtime.tools.bl616cl-libs.path` 和 `runtime.tools.xuantie-gcc-riscv.path` 引用它们；包索引接入前，本地测试用 `--build-property` 指定。

## 第三方代码

`cores/bouffalo/api/` 是 [ArduinoCore-API](https://github.com/arduino/ArduinoCore-API) 1.5.2 的 `api/` 目录，未作修改：

- commit：`cd91833d90b4fe50e428021ba5051e2b7ceafc84`
- 归档：`https://github.com/arduino/ArduinoCore-API/archive/refs/tags/1.5.2.tar.gz`
- 归档 SHA-256：`2c76d4dcea29d1d093d821804dcc1ccd83d172ef0d359d7c651cdbba6e6bfa57`
- 许可：LGPL-2.1，全文见 `cores/bouffalo/LICENSE.ArduinoCore-API`

`cores/bouffalo/dtostrf.c` 包含该目录中的 `deprecated-avr-comp/avr/dtostrf.c.impl`。

## 本地编译检查

```sh
tests/compile_check.sh <bl616cl-libs 包目录> <Xuantie GCC 目录> [要隐藏的路径...]
```

脚本把平台复制到临时 sketchbook，在 bubblewrap 中把给出的路径（SDK 检出、SDK 构建目录等）替换为空目录后，编译 `tests/sketches/` 下的每个 sketch，再用 `tests/check_map.py` 检查 map：未链入 Wi-Fi 链接组，`_malloc_r` 来自 SDK libc 移植层，`main` 来自 Core。需要 `arduino-cli`、`bwrap` 和 `python3`；arduino-cli 每次会在临时目录重新下载自带工具（ctags、discovery 等），需要联网。

## 功能状态

只有编译链接证据、没有实机运行证据的功能标为 experimental。

| 功能 | 状态 | 构建证据 | 实机证据或说明 |
| --- | --- | --- | --- |
| 编译链接（`unor4wifi_bl616cl`，`bl616cl-libs` 0.3.0） | experimental | Blink、SerialEcho 通过 `compile_check.sh` | 无 |
| `setup`/`loop` 任务（FreeRTOS，栈 8 KB，优先级 1）、`serialEvent` | experimental | 同上 | 无 |
| C++ 全局构造、`new`/`delete`、局部静态变量初始化保护 | experimental | 同上 | 无 |
| `pinMode`（`INPUT`、`OUTPUT`、`INPUT_PULLUP`、`INPUT_PULLDOWN`）、`digitalWrite`、`digitalRead` | experimental | 同上 | 无 |
| `millis`、`micros`、`delay`、`delayMicroseconds`、`yield` | experimental | 同上 | 无 |
| `Serial`（uart0，GPIO34 发、GPIO35 收，接收缓冲 256 字节；校验只支持无、奇、偶） | experimental | 同上 | 无；引脚落点待确认 |
| `String`、`Print`、`Stream` 等 ArduinoCore-API 通用类 | experimental | 同上 | 无 |
| `OUTPUT_OPENDRAIN` | unimplemented | — | `pinMode` 不改动引脚 |
| `analogRead`、`analogWrite`、`analogReference`、`attachInterrupt`、`detachInterrupt`、`tone`、`noTone`、`pulseIn`、`pulseInLong`、`shiftIn`、`shiftOut`、`interrupts`、`noInterrupts` | unimplemented | — | 使用时编译或链接失败 |
| C 标准输入输出（`printf`、`getchar`、`scanf` 等） | unimplemented | — | 输出被丢弃；读取返回 EOF |
| Wire、SPI、WiFi 等平台库 | unimplemented | — | — |
| 镜像后处理、上传、包索引安装 | unimplemented | — | — |
