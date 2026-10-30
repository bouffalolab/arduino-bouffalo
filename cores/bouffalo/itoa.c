/*
 * newlib supplies itoa/utoa but api/itoa.h also requires ltoa/ultoa. ILP32 makes long and int
 * equally wide, so delegate to newlib.
 */
#include <stdlib.h>

#include "api/itoa.h"

_Static_assert(sizeof(long) == sizeof(int), "ltoa/ultoa assume ILP32");

/**
 * @brief Convert a signed long to text through the ILP32-compatible newlib integer converter.
 */
char *ltoa(long value, char *string, int radix)
{
    return itoa((int)value, string, radix);
}

/**
 * @brief Convert an unsigned long to text through the ILP32-compatible newlib integer
 * converter.
 */
char *ultoa(unsigned long value, char *string, int radix)
{
    return utoa((unsigned)value, string, radix);
}
