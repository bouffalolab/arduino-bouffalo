#ifndef pgmspace_h
#define pgmspace_h

#include <stdint.h>
#include <string.h>

#define PROGMEM
#define PGM_P const char *
#define PSTR(value) (value)
#define pgm_read_byte(address) (*(const uint8_t *)(address))
#define strlen_P(address) strlen(address)
#define strcpy_P(destination, source) strcpy((destination), (source))

#endif
