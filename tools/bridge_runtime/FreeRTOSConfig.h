/*
 * FreeRTOS configuration shared by the generated SDK archive and Arduino Core.
 * Keep this file ABI-identical in both locations.
 */
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include <stdint.h>
#include <stdio.h>

#define configSUPPORT_STATIC_ALLOCATION         1
#define configSUPPORT_DYNAMIC_ALLOCATION        1
#define configUSE_PREEMPTION                    1
#define configUSE_IDLE_HOOK                     0
#define configUSE_TICK_HOOK                     0
#define configCPU_CLOCK_HZ                      ((uint32_t)(1U * 1000U * 1000U))
#define configTICK_RATE_HZ                      ((TickType_t)1000U)
#define configMAX_PRIORITIES                    7
#define configMINIMAL_STACK_SIZE                ((unsigned short)128U)
#define configTOTAL_HEAP_SIZE                   ((size_t)24U * 1024U)
#define configMAX_TASK_NAME_LEN                 16
#define configUSE_TRACE_FACILITY                1
#define configUSE_STATS_FORMATTING_FUNCTIONS    1
#define configUSE_16_BIT_TICKS                  0
#define configIDLE_SHOULD_YIELD                 0
#define configUSE_MUTEXES                       1
#define configQUEUE_REGISTRY_SIZE               8
#define configCHECK_FOR_STACK_OVERFLOW          2
#define configUSE_RECURSIVE_MUTEXES             1
#define configUSE_MALLOC_FAILED_HOOK            1
#define configUSE_APPLICATION_TASK_TAG          1
#define configUSE_COUNTING_SEMAPHORES           1
#define configGENERATE_RUN_TIME_STATS           0
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 1
#define configUSE_TICKLESS_IDLE                 0
#define configUSE_POSIX_ERRNO                   1

#define configUSE_CO_ROUTINES                   0
#define configMAX_CO_ROUTINE_PRIORITIES         2

#define configUSE_TIMERS                        1
#define configTIMER_TASK_PRIORITY               (configMAX_PRIORITIES - 1)
#define configTIMER_QUEUE_LENGTH                4
#define configTIMER_TASK_STACK_DEPTH            configMINIMAL_STACK_SIZE

#define INCLUDE_vTaskPrioritySet                1
#define INCLUDE_uxTaskPriorityGet               1
#define INCLUDE_vTaskDelete                     1
#define INCLUDE_vTaskCleanUpResources           1
#define INCLUDE_vTaskSuspend                    1
#define INCLUDE_vTaskDelayUntil                 1
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_eTaskGetState                   1
#define INCLUDE_xTimerPendFunctionCall          1
#define INCLUDE_xTaskAbortDelay                 1
#define INCLUDE_xTaskGetHandle                  1
#define INCLUDE_xSemaphoreGetMutexHolder        1

void vAssertCalled(void);
void vApplicationMallocFailedHook(void);

#define configASSERT(expression)                                        \
    do {                                                                \
        if (!(expression)) {                                            \
            printf("FreeRTOS assert: %s:%d: %s\r\n",                 \
                   __FILE__, __LINE__, #expression);                    \
            vAssertCalled();                                            \
        }                                                               \
    } while (0)

#endif
