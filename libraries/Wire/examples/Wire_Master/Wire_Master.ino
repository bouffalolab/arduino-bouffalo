
#include <Wire.h>

unsigned char count;
unsigned char array[34] = {0,0,3,4,5,6,7,8,9,10,11,12,13};
void setup() {
  // put your setup code here, to run once:
  Wire.begin();
}

void loop() {

  // put your main code here, to run repeatedly:
  Wire.beginTransmission(0x50);
  Wire.write((unsigned char)0);
  Wire.endTransmission();

  Wire.beginTransmission(0x50);
  Wire.write(array,12);
  Wire.endTransmission();

  delay(200);

  Wire.beginTransmission(0x50);
  Wire.write(array,2);
  Wire.endTransmission();

  Wire.requestFrom(0x50, 10);    // Request 6 bytes from slave device number two

  // Slave may send less than requested
  while(Wire.available()) {
      char c = Wire.read();    // Receive a byte as character
      printf("%02x ",c);         // Print the character
  }
  printf("done!\r\n");

  while(1);
}
