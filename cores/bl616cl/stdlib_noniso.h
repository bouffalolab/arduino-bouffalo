#ifndef stdlib_noniso_h
#define stdlib_noniso_h

#ifdef __cplusplus
extern "C" {
#endif

char *itoa(int value, char *buffer, int base);
char *utoa(unsigned int value, char *buffer, int base);
char *ltoa(long value, char *buffer, int base);
char *ultoa(unsigned long value, char *buffer, int base);
char *dtostrf(double value, signed char width, unsigned char precision, char *buffer);

#ifdef __cplusplus
}
#endif

#endif
