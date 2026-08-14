#include "board.h"

/* lwIP's LWIP_RAND() hook (see lwip-port/config/lwipopts.h).  The minimal
 * probe app must provide it so archives such as igmp.c link cleanly. */
int bl_rand(void)
{
    static unsigned int seed = 0x12345678;
    seed = seed * 1664525u + 1013904223u;
    return (int)(seed >> 16);
}

int main(void)
{
    board_init();
    while (1) {
    }
}
