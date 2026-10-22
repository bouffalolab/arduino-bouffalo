/*
 * C++ 运行时支持。底层包不提供 operator new/delete 与 __cxa_*，由 Core 提供。
 * 内存来自 malloc，即 SDK libc 移植层接到的 TLSF 堆。编译使用 -fno-exceptions，
 * 编译器不检查普通 new 的返回值，所以普通 new 分配失败时 abort()；nothrow 版本返回 nullptr。
 * SDK 的 malloc(0) 返回 NULL，0 字节按 1 字节分配。
 */
#include <cxxabi.h>
#include <new>
#include <stdlib.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include "cxx_runtime.h"

extern "C" {
void *__dso_handle = &__dso_handle;

void __cxa_pure_virtual(void)
{
    abort();
}

void __cxa_deleted_virtual(void)
{
    abort();
}
}

static void *alloc_or_abort(size_t size)
{
    void *ptr = malloc(size ? size : 1);
    if (ptr == nullptr) {
        abort();
    }
    return ptr;
}

void *operator new(size_t size)
{
    return alloc_or_abort(size);
}

void *operator new[](size_t size)
{
    return alloc_or_abort(size);
}

void *operator new(size_t size, const std::nothrow_t &) noexcept
{
    return malloc(size ? size : 1);
}

void *operator new[](size_t size, const std::nothrow_t &) noexcept
{
    return malloc(size ? size : 1);
}

void operator delete(void *ptr) noexcept
{
    free(ptr);
}

void operator delete[](void *ptr) noexcept
{
    free(ptr);
}

void operator delete(void *ptr, size_t) noexcept
{
    free(ptr);
}

void operator delete[](void *ptr, size_t) noexcept
{
    free(ptr);
}

/*
 * 局部静态变量的线程安全初始化。调度器启动前只有一个执行流，不加锁；
 * 之后用递归互斥量，允许构造函数里再初始化别的局部静态变量。
 * guard 的第一个字节非零表示已初始化（通用 C++ ABI）。
 */
static SemaphoreHandle_t guard_mutex;

void cxx_runtime_init(void)
{
    guard_mutex = xSemaphoreCreateRecursiveMutex();
    if (guard_mutex == NULL) {
        abort();
    }
}

static bool scheduler_running(void)
{
    return xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED;
}

extern "C" int __cxa_guard_acquire(__cxxabiv1::__guard *guard)
{
    if (*reinterpret_cast<volatile char *>(guard) != 0) {
        return 0;
    }
    if (scheduler_running()) {
        xSemaphoreTakeRecursive(guard_mutex, portMAX_DELAY);
    }
    if (*reinterpret_cast<volatile char *>(guard) != 0) {
        if (scheduler_running()) {
            xSemaphoreGiveRecursive(guard_mutex);
        }
        return 0;
    }
    return 1;
}

extern "C" void __cxa_guard_release(__cxxabiv1::__guard *guard)
{
    *reinterpret_cast<volatile char *>(guard) = 1;
    if (scheduler_running()) {
        xSemaphoreGiveRecursive(guard_mutex);
    }
}

extern "C" void __cxa_guard_abort(__cxxabiv1::__guard *guard)
{
    (void)guard;
    if (scheduler_running()) {
        xSemaphoreGiveRecursive(guard_mutex);
    }
}
