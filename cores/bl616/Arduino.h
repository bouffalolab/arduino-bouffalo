/*
###############################################################################
# Copyright (c) 2018, PulseRain Technology LLC
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

// #if !defined(Arduino_h)
#ifndef Arduino_h
#define Arduino_h

#include <stdlib.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include <stdint.h>	//Added for uint_t
#include <stdio.h>

#include "FreeRTOS.h"
#include "FreeRTOSConfig.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "event_groups.h"

#include "bouffalo_misc.h"

#include "stdlib_noniso.h"
// #include "api/ArduinoAPI.h"
// #include "api/Common.h"

#ifdef __cplusplus
extern "C"{
#endif
/* bouffalo lab api */

//============================================================================================
// Constant definition
//============================================================================================

#define HIGH 0x1
#define LOW  0x0

#define INPUT 0x0
#define OUTPUT 0x1
#define INPUT_PULLUP 0x2	// ###check later###
#define INPUT_PULLDOWN 0x3

#define PI 3.1415926535897932384626433832795
#define HALF_PI 1.5707963267948966192313216916398
#define TWO_PI 6.283185307179586476925286766559
#define DEG_TO_RAD 0.017453292519943295769236907684886
#define RAD_TO_DEG 57.295779513082320876798154814105
#define EULER 2.718281828459045235360287471352

#define SERIAL  0x0
#define DISPLAY 0x1

#define LSBFIRST 0
#define MSBFIRST 1

// 0 is low 1 is high (already defined)
#define RISING 2	//modified to be 2 instead of 3 as in AVR
#define FALLING 3	//modified to be 3 instead of 2 as in AVR

#ifndef false
#define false 0
#endif

#ifndef true
#define true 1
#endif

// undefine stdlib's abs if encountered
#ifdef abs
#undef abs
#endif

#define min(a,b) ((a)<(b)?(a):(b))
#define max(a,b) ((a)>(b)?(a):(b))
#define abs(x) ((x)>0?(x):-(x))
#define constrain(amt,low,high) ((amt)<(low)?(low):((amt)>(high)?(high):(amt)))
#define round(x)     ((x)>=0?(int)((x)+0.5):(int)((x)-0.5))
#define radians(deg) ((deg)*DEG_TO_RAD)
#define degrees(rad) ((rad)*RAD_TO_DEG)
#define sq(x) ((x)*(x))

#ifndef _NOP
#define _NOP() __asm__ volatile ("nop");
#endif

typedef bool boolean;
typedef uint8_t byte;
typedef unsigned int word;

#define bit(b) (1UL << (b))

void pinMode(uint8_t, uint8_t);
void digitalWrite(uint8_t, uint8_t);
int digitalRead(uint8_t);
void analogWrite(uint8_t, int);
int analogRead(uint8_t);


void setup(void);
void loop(void);

#ifdef __cplusplus
} // extern "C"

#include "Stream.h"
#include "Printable.h"
#include "Print.h"
#include "Server.h"
#include "WString.h"
#include "bouffalo_misc.h"
#include "HardwareSerial.h"

#endif

#include "pins_arduino.h"

#endif // Arduino_h
