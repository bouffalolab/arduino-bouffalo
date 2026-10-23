arduino-cli 只在 precompiled=true 且 src/<build.mcu>/ 非空时加入 library.properties 的 ldflags（Wi-Fi 链接组），
本文件让该目录非空；库的源码仍照常编译。
