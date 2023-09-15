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

#include "Arduino.h"

// #include "log.h"

// // Declared weak in Arduino.h to allow user redefinitions.
// int atexit(void (* /*func*/ )()) { return 0; }

// Weak empty variant initialization function.
// May be redefined by variant files.

extern  "C" {
__attribute__((weak)) void wifi_event_handler() { }
}
void initVariant() __attribute__((weak));
void initVariant() { }

static TaskHandle_t loop_task_handle;
#define DBG_TAG "MAIN"

void loopTask(void *pvParameters){

    //+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
    // ARDUINO sketch
    //+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
    setup();

    for(;;){

        loop();
        // bflb_mtimer_delay_ms(1000);
        // delay(1000);
    }
}

// extern "C" {
// // void __dso_handle(void) {
// //     // while(1) {
// //     //     printf("entry!\r\n");
// //     // }
// // }
// void *__dso_handle = NULL;
// }

int main(void)
{
    init();

    //+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

    //freertos
    xTaskCreate(loopTask, (char *)"loop_task", 512, NULL, 1, &loop_task_handle);

    vTaskStartScheduler();

    while (1) {
    }

    return 0;
} // End of main()
