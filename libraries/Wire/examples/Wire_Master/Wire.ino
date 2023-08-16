#include <bouffalo_sdk.h>
#include <Wire.h>

Wire wire = Wire();
unsigned char count;
unsigned char array[34] = {0,0,3,4,5,6,7,8,9,10,11,12,13};
void setup() {
  // put your setup code here, to run once:
  wire.begin();  
}

void loop() {
  //set clock api test
  //wire.setClock(80000);
  //set wire timeout api test
  //wire.setWireTimeout(0,true);
  // put your main code here, to run repeatedly:
  wire.beginTransmission(0x50);
  wire.write((unsigned char)0);
  wire.endTransmission();

  wire.beginTransmission(0x50);
  wire.write(array,12);
  wire.endTransmission();


  delay(200);

  wire.beginTransmission(0x50);
  wire.write(array,2);
  wire.endTransmission();

  wire.requestFrom(0x50, 10);    // Request 6 bytes from slave device number two

  // Slave may send less than requested
  while(wire.available()) {
      char c = wire.read();    // Receive a byte as character
      printf("%02x ",c);         // Print the character
  }

  while(1);
}
