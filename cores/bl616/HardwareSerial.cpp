/*
  HardwareSerial.cpp - Hardware serial library for Wiring
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

  Modified 23 November 2006 by David A. Mellis
  Modified 28 September 2010 by Mark Sproul
  Modified 14 August 2012 by Alarus
  Modified 3 December 2013 by Matthijs Kooijman
*/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "Arduino.h"

#include "HardwareSerial.h"
#include "pins_arduino.h"

#include "bflb_gpio.h"

// this next line disables the entire HardwareSerial.cpp,
// this is so I can support Attiny series and any other chip without a uart
#if defined(HAVE_HWSERIAL0) || defined(HAVE_HWSERIAL1) || defined(HAVE_HWSERIAL2) || defined(HAVE_HWSERIAL3)

// SerialEvent functions are weak, so when the user doesn't define them,
// the linker just sets their address to 0 (which is checked below).
// The Serialx_available is just a wrapper around Serialx.available(),
// but we can refer to it weakly so we don't pull in the entire
// HardwareSerial instance if the user doesn't also refer to it.
#if defined(HAVE_HWSERIAL0)
  HardwareSerial Serial = HardwareSerial(0);
  void serialEvent() __attribute__((weak));
  bool Serial0_available() __attribute__((weak));
#endif

#if defined(HAVE_HWSERIAL1)
  HardwareSerial Serial1 = HardwareSerial(1);
  void serialEvent1() __attribute__((weak));
  bool Serial1_available() __attribute__((weak));
#endif

#if defined(HAVE_HWSERIAL2)
  HardwareSerial Serial1 = HardwareSerial(2);
  void serialEvent2() __attribute__((weak));
  bool Serial2_available() __attribute__((weak));
#endif

#if defined(HAVE_HWSERIAL3)
  HardwareSerial Serial1 = HardwareSerial(3);
  void serialEvent3() __attribute__((weak));
  bool Serial3_available() __attribute__((weak));
#endif

void serialEventRun(void)
{
#if defined(HAVE_HWSERIAL0)
  if (Serial0_available && serialEvent && Serial0_available()) serialEvent();
#endif
#if defined(HAVE_HWSERIAL1)
  if (Serial1_available && serialEvent1 && Serial1_available()) serialEvent1();
#endif
#if defined(HAVE_HWSERIAL2)
  if (Serial2_available && serialEvent2 && Serial2_available()) serialEvent2();
#endif
#if defined(HAVE_HWSERIAL3)
  if (Serial3_available && serialEvent3 && Serial3_available()) serialEvent3();
#endif
}

// Actual interrupt handlers //////////////////////////////////////////////////////////////

void HardwareSerial::_tx_udr_empty_irq(void)
{
}

// Public Methods //////////////////////////////////////////////////////////////

HardwareSerial::HardwareSerial(uint8_t index)
{
  struct bflb_device_s *gpio;

  gpio = bflb_device_get_by_name("gpio");

  if (index == 0) {
#if defined(HAVE_HWSERIAL0)
    uart = bflb_device_get_by_name("uart0");
    bflb_gpio_uart_init(gpio, UART0_TX, GPIO_UART_FUNC_UART0_TX);
    bflb_gpio_uart_init(gpio, UART0_RX, GPIO_UART_FUNC_UART0_RX);
#endif
  } else if (index == 1) {
#if defined(HAVE_HWSERIAL1)
    uart = bflb_device_get_by_name("uart1");
    bflb_gpio_uart_init(gpio, UART1_TX, GPIO_UART_FUNC_UART1_TX);
    bflb_gpio_uart_init(gpio, UART1_RX, GPIO_UART_FUNC_UART1_RX);
#endif
  } else if (index == 2) {
#if defined(HAVE_HWSERIAL2)
    uart = bflb_device_get_by_name("uart2");
    bflb_gpio_uart_init(gpio, UART2_TX, GPIO_UART_FUNC_UART2_TX);
    bflb_gpio_uart_init(gpio, UART2_RX, GPIO_UART_FUNC_UART2_RX);
#endif
  } else if (index == 3) {
#if defined(HAVE_HWSERIAL3)
    uart = bflb_device_get_by_name("uart3");
    bflb_gpio_uart_init(gpio, UART3_TX, GPIO_UART_FUNC_UART3_TX);
    bflb_gpio_uart_init(gpio, UART3_RX, GPIO_UART_FUNC_UART3_RX);
#endif
  }
}

void HardwareSerial::begin(unsigned long baud, uint8_t config)
{
  struct bflb_uart_config_s cfg;
  cfg.baudrate = baud;
  cfg.data_bits = config & 0x3;
  cfg.stop_bits = (config >> 4) & 0x3;
  cfg.parity = (config >> 6) & 0x3;;
  cfg.flow_ctrl = 0;
  cfg.tx_fifo_threshold = 7;
  cfg.rx_fifo_threshold = 7;

  bflb_uart_init(uart, &cfg);
}

void HardwareSerial::end()
{
  // wait for transmission of outgoing data
  flush();

  bflb_uart_disable(uart);
  bflb_uart_feature_control(uart, UART_CMD_CLR_RX_FIFO, 0);
}

int HardwareSerial::available(void)
{
  return bflb_uart_feature_control(uart, UART_CMD_GET_RX_FIFO_CNT, 0);
}

int peek_data = -1;

int HardwareSerial::peek(void)
{
  if (available() && peek_data == -1) {
      peek_data = bflb_uart_getchar(uart);
  }
  return peek_data;
}

int HardwareSerial::read(void)
{
  int ret;

  if (peek_data == -1) {
      return bflb_uart_getchar(uart);
  } else {
      ret = peek_data;
      peek_data = -1;
      return ret;
  }
}

int HardwareSerial::availableForWrite(void)
{
  return bflb_uart_feature_control(uart, UART_CMD_GET_TX_FIFO_CNT, 0);
}

void HardwareSerial::flush()
{
  while (*(uint32_t*)(uart->reg_base + 0x30) & 1) {
  }
}

size_t HardwareSerial::write(uint8_t c)
{
  bflb_uart_putchar(uart, c);

  return 1;
}

#endif // whole file
