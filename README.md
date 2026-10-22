# arduino-bl

通用 Bouffalo Arduino 平台仓库，从 BL616CL 起步，后续逐步支持多芯片。

当前阶段：仓库初始化与资料基线；尚未实现 Core、平台包或上传流程。

普通用户的目标体验是通过 Boards Manager / Arduino CLI 安装后直接编译上传，无需完整 Bouffalo SDK。底层依赖由独立构建器发布。

本地共享入口：[文档](../docs/README.md)、[开发规范](../AGENTS.md)、[已确认验收](../docs/project/charter.md)。这些文件目前位于非 Git 的父工作区，独立分发文档的方式待确定。
