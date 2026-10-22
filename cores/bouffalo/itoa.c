/*
 * api/itoa.h 要求的转换函数中，newlib 只提供 itoa 与 utoa；这里补上 ltoa 与 ultoa。
 * 本平台是 ILP32，long 与 int 等宽，直接转给 newlib 的实现。
 */
#include <stdlib.h>

#include "api/itoa.h"

_Static_assert(sizeof(long) == sizeof(int), "ltoa/ultoa assume ILP32");

char *ltoa(long value, char *string, int radix)
{
    return itoa((int)value, string, radix);
}

char *ultoa(unsigned long value, char *string, int radix)
{
    return utoa((unsigned)value, string, radix);
}
