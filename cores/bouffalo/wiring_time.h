#pragma once

/* 记录 millis()/micros() 的起点；在 init() 中调用一次。 */
void wiring_time_init(void);
