/*
 * random、randomSeed：api/Common.h 只有声明。语义同官方 Core（SAMD、renesas 的 WMath.cpp）：
 * 用 C 库 rand/srand，种子为 0 时不改种子，上限为 0 或下限不小于上限时直接返回。
 */
#include <stdlib.h>

#include "Arduino.h"

void randomSeed(unsigned long seed)
{
    if (seed != 0) {
        srand(seed);
    }
}

long random(long howbig)
{
    if (howbig == 0) {
        return 0;
    }
    return rand() % howbig;
}

long random(long howsmall, long howbig)
{
    if (howsmall >= howbig) {
        return howsmall;
    }
    return random(howbig - howsmall) + howsmall;
}
