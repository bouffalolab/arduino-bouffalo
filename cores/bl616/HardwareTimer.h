#ifndef HardwareTimer_h
#define HardwareTimer_h

#include <inttypes.h>

// #include "bouffalo_sdk.h"
#include "bflb_timer.h"

typedef struct bflb_device_s bf_timer_dev_t;
typedef struct bflb_timer_config_s bf_timer_cfg_t;
typedef struct bf_timer_type {
    bf_timer_dev_t *pdev;
    bf_timer_cfg_t pcfg;
} bf_timer_t;


class HardwareTimer
{
    private:
        bf_timer_t bf_timer[3];
    public:

        bf_timer_t *bfTimerBegin(uint8_t timerId, uint32_t compVal);
        void bfTimerEnd(bf_timer_t *timer);

        void bfTimerSetConfig(bf_timer_t *timer, bf_timer_cfg_t *cfg);
        bf_timer_cfg_t *bfTimerGetConfig(bf_timer_t *timer);

        void bfTimerSetCountType(bf_timer_t *timer, uint8_t countType);
        void bfTimerSetClock(bf_timer_t *timer, uint8_t src, uint16_t div);
        void bfTimerSetCompVal(bf_timer_t *timer, uint32_t compVal0, uint32_t compVal1, uint32_t compVal2);
        void bfTimerSetPreloadVal(bf_timer_t *timer, uint32_t preloadVal);
        void bfTimerStart(bf_timer_t *timer);
        void bfTimerStop(bf_timer_t *timer);

        void bfTimerIntAttch(void);

};

#endif
