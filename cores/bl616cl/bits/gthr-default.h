/*
 * The Xuantie bare-metal libstdc++ package defaults to its POSIX gthread
 * adapter. Its pthread ABI is not compatible with Bouffalo SDK's FreeRTOS
 * POSIX layer, so Arduino applications use the single-threaded libstdc++
 * adapter. Bridge concurrency is implemented with the SDK's FreeRTOS APIs.
 */
#ifndef ARDUINO_BL616CL_GTHR_DEFAULT_H
#define ARDUINO_BL616CL_GTHR_DEFAULT_H

#include <bits/gthr-single.h>

#endif
