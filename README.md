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
#include "HardwareSerial.h"

unsigned long time = 0;

void setup() {
  // put your setup code here, to run once:
  // delayMicroseconds(1000);
  // printf("12345\r\n");

  delayMicroseconds(1000);
  pinMode(LED_BUILTIN, OUTPUT);

  Serial.begin(2000000);
  // wait 1ms
  delayMicroseconds(1000);
  Serial.write('\n');
  Serial.write('a');
  Serial.write('b');
  Serial.write('c');
  Serial.write('d');
  Serial.write('\n');

  Serial.println("Input something:");
  // wait serial input something
  while(Serial.available() == 0) {
  }
  Serial.write(Serial.read());

  Serial.write('\n');
  Serial.println("hello world!");
}

void loop() {
  time = millis();
  digitalWrite(LED_BUILTIN, HIGH);  // turn the LED on (HIGH is the voltage level)
  delay(1000);
  digitalWrite(LED_BUILTIN, LOW);   // turn the LED off by making the voltage LOW
  delay(1000);
  // printf("loop!\r\n");
  Serial.print("Timer:");
  Serial.print(time);
  Serial.print(" us; ");
  Serial.println("hello world!");
}

```

After writing and clicking Verify, if it doesn't report an error it means the installation is working.

# Other


