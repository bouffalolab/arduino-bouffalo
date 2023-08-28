#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "Arduino.h"

#include "HardwareTimer.h"
#include "pins_arduino.h"


//bf_timer_t bf_timer[3];


bf_timer_t * HardwareTimer::bfTimerBegin(uint8_t timerId, uint32_t compVal)
{
    if(timerId >= TIMER_COMP_NONE){
        printf("timer ID out of range\r\n");
    }

    switch(timerId){
        case 0 :  bf_timer[timerId].pdev = bflb_device_get_by_name("timer0");
            break;
        case 1 :  bf_timer[timerId].pdev = bflb_device_get_by_name("timer1");
            break;
        case 2 :  bf_timer[timerId].pdev = bflb_device_get_by_name("timer2");
            break;
        default :
            break;
    }

    bf_timer[timerId].pcfg.counter_mode = TIMER_COUNTER_MODE_PROLOAD;
    bf_timer[timerId].pcfg.clock_source = TIMER_CLKSRC_NO;
    bf_timer[timerId].pcfg.clock_div = 9;
    bf_timer[timerId].pcfg.trigger_comp_id = TIMER_COMP_ID_0;
    bf_timer[timerId].pcfg.comp0_val = compVal;
    bf_timer[timerId].pcfg.comp1_val = 0x00FFFFFF;
    bf_timer[timerId].pcfg.comp2_val = 0xFFFFFFFF;
    bf_timer[timerId].pcfg.preload_val = 0;

    bflb_timer_deinit(bf_timer[timerId].pdev);
    bflb_timer_init(bf_timer[timerId].pdev, (const struct bflb_timer_config_s *)&(bf_timer[timerId].pcfg));
    bflb_timer_start(bf_timer[timerId].pdev);

    return &(bf_timer[timerId]);
}

void HardwareTimer::bfTimerEnd(bf_timer_t *timer)
{
    bflb_timer_stop(timer->pdev);
}

void HardwareTimer::bfTimerSetConfig(bf_timer_t *timer, bf_timer_cfg_t *cfg)
{
    memcpy((uint8_t *)&(timer->pcfg), (uint8_t *)(cfg), sizeof(bf_timer_cfg_t));
}

bf_timer_cfg_t * HardwareTimer::bfTimerGetConfig(bf_timer_t *timer)
{
    return &(timer->pcfg);
}

void HardwareTimer::bfTimerSetCountType(bf_timer_t *timer, uint8_t countType)
{
    timer->pcfg.counter_mode = countType;
}

void HardwareTimer::bfTimerSetClock(bf_timer_t *timer, uint8_t src, uint16_t div)
{
    timer->pcfg.clock_source = src;
    timer->pcfg.clock_div = div;
}

void HardwareTimer::bfTimerSetCompVal(bf_timer_t *timer, uint32_t compVal0, uint32_t compVal1, uint32_t compVal2)
{
    timer->pcfg.comp0_val = compVal0;
    timer->pcfg.comp1_val = compVal1;
    timer->pcfg.comp2_val = compVal2;
}

void HardwareTimer::bfTimerSetPreloadVal(bf_timer_t *timer, uint32_t preloadVal)
{
    timer->pcfg.preload_val = preloadVal;
}

void HardwareTimer::bfTimerStart(bf_timer_t *timer)
{
    bflb_timer_stop(timer->pdev);
    bflb_timer_deinit(timer->pdev);
    bflb_timer_init(timer->pdev, (const struct bflb_timer_config_s *)&(timer->pcfg));
    bflb_timer_start(timer->pdev);
}

void HardwareTimer::bfTimerStop(bf_timer_t *timer)
{
    bflb_timer_stop(timer->pdev);
}

void HardwareTimer::bfTimerIntAttch(void)
{
    ;
}

