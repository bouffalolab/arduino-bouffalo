/*
###############################################################################
# Copyright (c) 2019, PulseRain Technology LLC
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU Lesser General Public License (LGPL) as
# published by the Free Software Foundation, either version 3 of the License,
# or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
# or FITNESS FOR A PARTICULAR PURPOSE.
# See the GNU Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.
#
###############################################################################
*/

// #include <Arduino.h>
#include "Arduino.h"
#include "bouffalo_sdk.h"
// // Declared weak in Arduino.h to allow user redefinitions.
// int atexit(void (* /*func*/ )()) { return 0; }

// Weak empty variant initialization function.
// May be redefined by variant files.
void initVariant() __attribute__((weak));
void initVariant() { }

int main(void)
{
    init();
    //+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
    // ARDUINO sketch
    //+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
    setup();

    for (;;) {
        loop();
        bflb_mtimer_delay_ms(1000);
    } // End of while loop

    //+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

    return 0;
} // End of main()