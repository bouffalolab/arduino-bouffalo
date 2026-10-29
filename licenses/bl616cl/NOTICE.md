# BL616CL 组件来源与许可记录

本目录保存固定SDK版本及桥接实现中的原始许可证、版权头和组件清单。
`components.json`记录来源路径、源文件与notice的SHA256；SDK组件版本来自
`third_party/bouffalo_sdk.lock.json`。保留原始文件头，不用SDK总许可证覆盖组件声明。
`runtime-build-audit/`另保存包内入口重建时744个CMake编译单元的SHA、SPDX标记与
原始文件头。该清单覆盖运行库编译，不等同于最终固件链接可达性；没有SPDX标记
不能直接推定没有许可证，也不能补造授权。

| 组件 | 当前证据 |
| --- | --- |
| Bouffalo SDK、CherryUSB、Zephyr派生BLE host | Apache-2.0及各源文件版权声明 |
| FreeRTOS | MIT，保留Amazon声明 |
| lwIP、LittleFS、WPA、TLSF、TinyCrypt | 各自BSD声明，保存对应原文 |
| Mbed TLS v3与framework | 保存原始双许可证文本；本项目采用Apache-2.0选项 |
| FreeDAP | BSD-3-Clause，保留Alex Taradov声明 |
| SDK NuttX派生libc | Apache-2.0；源文件引用的ASF NOTICE尚需匹配原版补齐 |

`components.json`中的source分类只表示可从固定源码重建，不意味着文件许可已逐项
全部审定。BLE controller包含RivieraWaves版权头但未给出独立授权的文件；PHY/RF、
PKA、boot2、供应商工具和编译器也各有单独边界，不能仅凭SDK根Apache文本认定
全部可再分发。工具链GCC运行时例外、libstdc++及newlib/libgloss需匹配原工具链
发行版的许可材料。

根项目与平台没有统一LICENSE文件。此处不为原作者新增授权，也不重写已有许可。
当前本地源码安装包用于已授权的项目开发；对外发布前仍须完成components.json所列
未决项。包中不附供应商SDK、工具链、RF/PKA库或boot2二进制；构建时从明确的
外部输入取得这些依赖。无远端发布或自动上传步骤。
