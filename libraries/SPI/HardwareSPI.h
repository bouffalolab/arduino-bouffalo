#ifndef Hardware_SPI_h
#define Hardware_SPI_h

#include "bouffalo_sdk.h"


#define SPI_CLOCK_DIV2   2
#define SPI_CLOCK_DIV4   4
#define SPI_CLOCK_DIV8   8
#define SPI_CLOCK_DIV32  32
#define SPI_CLOCK_DIV64  64
#define SPI_CLOCK_DIV128 128

#define MSBFIRST         0
#define LSBFIRST         1

#define SPI_MODE0        0
#define SPI_MODE1        1
#define SPI_MODE2        2
#define SPI_MODE3        3


class SPI
{
public:
    SPI();

    struct bflb_device_s *spi0;
    
    void begin(void);
    void beginTransaction(unsigned int speedMaximum, unsigned char dataOrder, unsigned char dataMode);
    unsigned char transfer(unsigned char val);
    void endTransaction(void);
    void end(void);
    void setBitOrder(void);
    void setDataMode(void);
    void setClockDivider(uint8_t divider);
    void usingInterrupt(void);
};

#endif