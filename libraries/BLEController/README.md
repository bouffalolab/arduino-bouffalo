# BLEController

BL616CL external-host Controller 的原始H4字节接口，状态为 **experimental**。需要与平台一起使用 `bl616cl-libs` 0.5.0；BL端没有ArduinoBLE、NimBLE或另一套BLE Host。UNO R4 WiFi上RA4M1 ArduinoBLE的HCI over AT接入仍归应用，不由此库实现。

`BLEController.begin()` 分配传输资源并启动Controller；成功后外部Host仍必须发送HCI Reset与初始化命令并检查事件。`write(data, size)` 返回实际接受字节数，短写时Host保留剩余数据再发；`available()`、`read()`及`read(data, size)`提供Controller发出的H4事件/ACL。`end()`在Host已停止收发、断开连接后关闭Controller并释放传输资源，可再次`begin()`。所有公共调用由同一应用任务串行执行；这是单Host接口。命令流不能混入日志。

H4解析保留上游UART HCI引擎，通过配套头文件的UART完成回调接入FreeRTOS字节流，不占用物理UART或引脚。传输任务优先级30、Controller任务29（当前32级配置），保证上游ISR完成回调先注册完deferred job，再调度Controller；初版反向优先级下Reset无应答已在实机复现。Wi-Fi与BLE共用一次射频初始化。两个方向各有1024字节缓冲，Host须遵守Controller报告的HCI包长与流控；本轮LE报告ACL长度251、4个发送buffer。

原始USB示例见 `examples/ControllerUSB`。源码来源、配套头库和配置记录在底层包manifest：上游 `libbtblecontroller_bl616cl_uarthci.a`，固定SDK提交 `1cd1fd514df7ed0dfc1221be871f5aae03f31bbc`，原归档SHA-256 `307b72a88fd2a1a9875b283de959972a53851d0085079fa2c5d69c60e0128bab`。库字符串1.6.212；上游`btble_controller_get_lib_ver()`实际返回文本`NULL`，因此版本/能力通过真实HCI查询验收，不包装该接口。SDK根目录Apache-2.0文本随包保存，产品完整再分发审核仍归ARDUINO-54。

实机证据入口：[ARDUINO-43任务](../../../.tasks/2026-10-26-arduino-43-ble-controller/STATUS.md)。软件构建/链接检查另列，不代替实机验收。当前不声明完整ArduinoBLE官方示例、配对/加密、ISO、两条并发连接或长期稳定性已通过。
