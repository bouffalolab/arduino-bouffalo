#pragma once
#ifndef BL616CL_TCP_RECEIVE_WINDOW_BYTES
#define BL616CL_TCP_RECEIVE_WINDOW_BYTES 0
#endif
#if BL616CL_TCP_RECEIVE_WINDOW_BYTES
static_assert(BL616CL_TCP_RECEIVE_WINDOW_BYTES >= 1460 &&
              BL616CL_TCP_RECEIVE_WINDOW_BYTES <= 35040,
              "TCP receive diagnostic cap must fit the bridge profile");
#endif
