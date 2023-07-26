# Arduino core for the BouffaloLab chips
[![Release](https://img.shields.io/github/v/release/strongwong/arduino-bl618?style=plastic)](https://github.com/strongwong/arduino-bl618/releases)

Now start to support BL616/8 chips, other chips to be developed later.

The first version can be compiled and run, but the repository is still in the process of development, most of the APIs are not yet adapted, welcome more people in the community to participate.

## Installing with Boards Manager

Currently it supports windows and is tested on windows. Other systems have not been tested yet.
Install the latest [Arduino IDE](https://www.arduino.cc/en/software), 2.x or higher is recommended.
Start Arduino and add the package URL address to the development board manager. into "**File->Preferences->Additional Boards Manager URLs**".
Enter this URL：`https://github.com/strongwong/arduino-bl618/releases/latest/download/package_bouffalolab_index.json`.

Open Boards Manager from  "**Tools->Board->Boards Manager**" menu; Find the bouffalolab boards and install bouffalolab platform (and don't forget to select your BL618G0 board from Tools->Board menu after installation).

## Build test

After the installation is complete and the correct board is selected, you can write a simple code test in Arduino, such as the following code that outputs "*Hello World*".

```c
#include <stdio.h>
#include <Arduino.h>
#define LED_BUILTIN (32)
unsigned long time = 0;

void setup() {
    // put your setup code here, to run once:
    // initialize digital pin LED_BUILTIN as an output.
    pinMode(LED_BUILTIN, OUTPUT);
}

void loop() {
    // put your main code here, to run repeatedly:
    time = millis();
    digitalWrite(LED_BUILTIN, HIGH);  // turn the LED on (HIGH is the voltage level)
    delay(1000);
    digitalWrite(LED_BUILTIN, LOW);   // turn the LED off by making the voltage LOW
    delay(1000);
    printf("hellow world! Timer:%ld\r\n", time);
}
```

After writing and clicking Verify, if it doesn't report an error it means the installation is working.

# Other


