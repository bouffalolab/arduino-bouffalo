/*
 * Core-owned C++ runtime. The dependency package does not supply operator new/delete or __cxa_*
 * functions. Allocation uses malloc through the SDK TLSF heap. With -fno-exceptions, ordinary new
 * must abort on allocation failure because callers do not check its return; nothrow returns
 * nullptr. SDK malloc(0) returns NULL, so allocate one byte for a zero-size request.
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

/**
 * @brief Abort on an invalid call to a pure virtual function.
 */
void __cxa_pure_virtual(void)
{
    abort();
}

/**
 * @brief Abort on an invalid call to a deleted virtual function.
 */
void __cxa_deleted_virtual(void)
{
    abort();
}
}

/**
 * @brief Allocate at least one byte from the SDK heap and abort if allocation fails.
 */
static void *alloc_or_abort(size_t size)
{
    void *ptr = malloc(size ? size : 1);
    if (ptr == nullptr) {
        abort();
    }
    return ptr;
}

/**
 * @brief Allocate an object from the SDK heap; the throwing form aborts on exhaustion.
 */
void *operator new(size_t size)
{
    return alloc_or_abort(size);
}

/**
 * @brief Allocate array storage from the SDK heap; the throwing form aborts on exhaustion.
 */
void *operator new[](size_t size)
{
    return alloc_or_abort(size);
}

/**
 * @brief Allocate object storage from the SDK heap, returning nullptr on exhaustion.
 */
void *operator new(size_t size, const std::nothrow_t &) noexcept
{
    return malloc(size ? size : 1);
}

/**
 * @brief Allocate array storage from the SDK heap, returning nullptr on exhaustion.
 */
void *operator new[](size_t size, const std::nothrow_t &) noexcept
{
    return malloc(size ? size : 1);
}

/**
 * @brief Release object storage to the SDK heap.
 */
void operator delete(void *ptr) noexcept
{
    free(ptr);
}

/**
 * @brief Release array storage to the SDK heap.
 */
void operator delete[](void *ptr) noexcept
{
    free(ptr);
}

/**
 * @brief Release object storage to the SDK heap.
 */
void operator delete(void *ptr, size_t) noexcept
{
    free(ptr);
}

/**
 * @brief Release array storage to the SDK heap.
 */
void operator delete[](void *ptr, size_t) noexcept
{
    free(ptr);
}

/*
 * Thread-safe local-static initialization. Before scheduler startup there is one execution flow
 * and no lock is required; afterward use a recursive mutex to permit nested local-static
 * construction. The first guard byte is nonzero after initialization, following the generic C++
 * ABI.
 */
static SemaphoreHandle_t guard_mutex;

/**
 * @brief Create the recursive local-static initialization guard before constructors run.
 */
void cxx_runtime_init(void)
{
    guard_mutex = xSemaphoreCreateRecursiveMutex();
    if (guard_mutex == NULL) {
        abort();
    }
}

/**
 * @brief Report whether FreeRTOS has left its not-started state.
 */
static bool scheduler_running(void)
{
    return xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED;
}

/**
 * @brief Acquire the local-static guard and report whether initialization is still required.
 */
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

/**
 * @brief Publish successful local-static initialization and release its recursive guard.
 */
extern "C" void __cxa_guard_release(__cxxabiv1::__guard *guard)
{
    *reinterpret_cast<volatile char *>(guard) = 1;
    if (scheduler_running()) {
        xSemaphoreGiveRecursive(guard_mutex);
    }
}

/**
 * @brief Release a failed local-static initialization guard without marking it initialized.
 */
extern "C" void __cxa_guard_abort(__cxxabiv1::__guard *guard)
{
    (void)guard;
    if (scheduler_running()) {
        xSemaphoreGiveRecursive(guard_mutex);
    }
}
