/*
 * random/randomSeed are only declared by api/Common.h. Match the official SAMD/Renesas cores using
 * rand/srand: zero seed leaves the seed unchanged; zero upper bound or an empty interval returns
 * immediately.
 */
#include <stdlib.h>

#include "Arduino.h"

/**
 * @brief Seed the C random generator unless seed is zero, matching Arduino behavior.
 */
void randomSeed(unsigned long seed)
{
    if (seed != 0) {
        srand(seed);
    }
}

/**
 * @brief Return a pseudorandom value in the requested half-open interval, preserving Arduino
 * empty-range behavior.
 */
long random(long howbig)
{
    if (howbig == 0) {
        return 0;
    }
    return rand() % howbig;
}

/**
 * @brief Return a pseudorandom value in the requested half-open interval, preserving Arduino
 * empty-range behavior.
 */
long random(long howsmall, long howbig)
{
    if (howsmall >= howbig) {
        return howsmall;
    }
    return random(howbig - howsmall) + howsmall;
}
