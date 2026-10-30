Arduino CLI 1.5.1 仅在 src/<build.mcu>/ 非空时，才会为 precompiled=true 的库应用 ldflags。
本占位文件用于启用固定依赖包的 Wi-Fi 链接组；本库源码仍照常编译。
目录非空这一条件属于 CLI 的实现行为，不是规范中库内预编译二进制的目录布局要求。
