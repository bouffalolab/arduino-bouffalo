/*
  HardwareSerial.h - Hardware serial library for Wiring
  Copyright (c) 2006 Nicholas Zambetti.  All right reserved.

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

  Modified 28 September 2010 by Mark Sproul
  Modified 14 August 2012 by Alarus
  Modified 3 December 2013 by Matthijs Kooijman
*/

#ifndef HardwareSerial_h
#define HardwareSerial_h

#include <inttypes.h>

#include "Stream.h"
// #include "bouffalo_sdk.h"
#include "bflb_uart.h"
#include "bflb_timer.h"

#define HAVE_HWSERIAL0
#define UBRR0H
#define HAVE_HWSERIAL1
#define UBRR1H

// Define constants and variables for buffering incoming serial data.  We're
// using a ring buffer (I think), in which head is the index of the location
// to which to write the next incoming character and tail is the index of the
// location from which to read.
// NOTE: a "power of 2" buffer size is recommended to dramatically
//       optimize all the modulo operations for ring buffers.
// WARNING: When buffer sizes are increased to > 256, the buffer index
// variables are automatically increased in size, but the extra
// atomicity guards needed for that are not implemented. This will
// often work, but occasionally a race condition can occur that makes
// Serial behave erratically. See https://github.com/arduino/Arduino/issues/2405

// Define config for Serial.begin(baud, config);
#define SERIAL_5N1 0x10
#define SERIAL_6N1 0x11
#define SERIAL_7N1 0x12
#define SERIAL_8N1 0x13
#define SERIAL_5N2 0x30
#define SERIAL_6N2 0x31
#define SERIAL_7N2 0x32
#define SERIAL_8N2 0x33
#define SERIAL_5E1 0x90
#define SERIAL_6E1 0x91
#define SERIAL_7E1 0x92
#define SERIAL_8E1 0x93
#define SERIAL_5E2 0xB0
#define SERIAL_6E2 0xB1
#define SERIAL_7E2 0xB2
#define SERIAL_8E2 0xB3
#define SERIAL_5O1 0x50
#define SERIAL_6O1 0x51
#define SERIAL_7O1 0x52
#define SERIAL_8O1 0x53
#define SERIAL_5O2 0x70
#define SERIAL_6O2 0x71
#define SERIAL_7O2 0x72
#define SERIAL_8O2 0x73

class HardwareSerial : public Stream
{
  private:
    struct bflb_device_s *uart;
  public:
    HardwareSerial(uint8_t index);
    void begin(unsigned long baud, uint8_t config);
    inline void begin(unsigned long baud) {
        begin(baud, SERIAL_8N1);
    }
    inline void begin() {
        begin(2000000);
    }
    void end();
    virtual int available(void);
    virtual int peek(void);
    virtual int read(void);
    virtual int availableForWrite(void);
    virtual void flush(void);
    virtual size_t write(uint8_t);
    inline size_t write(unsigned long n) { return write((uint8_t)n); }
    inline size_t write(long n) { return write((uint8_t)n); }
    inline size_t write(unsigned int n) { return write((uint8_t)n); }
    inline size_t write(int n) { return write((uint8_t)n); }
    //using Print::write; // pull in write(str) and write(buf, size) from Print
    operator bool() { return true; }

    // Interrupt handlers - Not intended to be called externally
    inline void _rx_complete_irq(void);
    void _tx_udr_empty_irq(void);
};

#if defined(UBRRH) || defined(UBRR0H)
  extern HardwareSerial Serial;
  #define HAVE_HWSERIAL0
#endif
#if defined(UBRR1H)
  extern HardwareSerial Serial1;
  #define HAVE_HWSERIAL1
#endif
#if defined(UBRR2H)
  extern HardwareSerial Serial2;
  #define HAVE_HWSERIAL2
#endif
#if defined(UBRR3H)
  extern HardwareSerial Serial3;
  #define HAVE_HWSERIAL3
#endif

extern void serialEventRun(void) __attribute__((weak));

#endif
