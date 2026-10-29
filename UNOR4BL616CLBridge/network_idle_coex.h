#pragma once
#ifndef BL616CL_NETWORK_IDLE_COEX
#define BL616CL_NETWORK_IDLE_COEX 1
#endif
namespace bridge {
// AT task only, after BridgeRuntime::ready(). SoftAP receive RF scheduling.
void network_idle_coex_poll();
}
