#pragma once

/* 创建 C++ 局部静态变量初始化用的锁；在全局构造之前、调度器启动之前调用一次。 */
void cxx_runtime_init(void);
