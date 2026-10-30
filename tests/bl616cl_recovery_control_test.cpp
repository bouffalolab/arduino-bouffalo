#include <cassert>
#include <cstdint>

#include "bl616cl_recovery_control.h"

static uint32_t now_ms;
static int boot_config = -1;
static int boot_result;
static int resets;

unsigned long millis() { return now_ms; }
extern "C" int HBN_Set_User_Boot_Config(uint8_t value)
{
    boot_config = value;
    return boot_result;
}
extern "C" int bl_sys_reset_por() { ++resets; return 0; }

int main()
{
    assert(!bridge::bl616cl_recovery_window_elapsed(100U, 150U));
    assert(bridge::bl616cl_recovery_window_elapsed(150U, 150U));
    assert(bridge::bl616cl_recovery_window_elapsed(200U, 150U));
    assert(!bridge::bl616cl_recovery_window_elapsed(0xffffffe0U, 0xfffffff0U));
    assert(bridge::bl616cl_recovery_window_elapsed(0x00000010U, 0xfffffff0U));
    assert(!bridge::bl616cl_recovery_poll());
    now_ms = 100U;
    assert(bridge::bl616cl_reset_request());
    now_ms = 149U;
    assert(!bridge::bl616cl_recovery_poll());
    assert(resets == 0);
    now_ms = 150U;
    assert(bridge::bl616cl_recovery_poll());
    assert(boot_config == 2 && resets == 1);
    assert(!bridge::bl616cl_recovery_poll());
    now_ms = 0xfffffff0U;
    assert(bridge::bl616cl_recovery_request());
    now_ms = 33U;
    assert(!bridge::bl616cl_recovery_poll());
    now_ms = 34U;
    assert(bridge::bl616cl_recovery_poll());
    assert(boot_config == 1 && resets == 2);
    boot_result = -1;
    assert(bridge::bl616cl_reset_request());
    now_ms += 50U;
    assert(!bridge::bl616cl_recovery_poll());
    assert(resets == 2);
    return 0;
}
