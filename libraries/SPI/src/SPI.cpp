/*
###############################################################################
# Copyright (c) 2019, PulseRain Technology LLC
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

#include "Arduino.h"
#include "pins_arduino.h"
#include "SPI.h"

SPIClass::SPIClass()
{
}

void SPIClass::begin(void)
{
    struct bflb_device_s *gpio;

    gpio = bflb_device_get_by_name("gpio");
    /* spi cs */
    bflb_gpio_init(gpio, GPIO_PIN_20, GPIO_FUNC_SPI0 | GPIO_ALTERNATE | GPIO_PULLUP | GPIO_SMT_EN | GPIO_DRV_1);
    /* spi clk */
    bflb_gpio_init(gpio, GPIO_PIN_17, GPIO_FUNC_SPI0 | GPIO_ALTERNATE | GPIO_PULLUP | GPIO_SMT_EN | GPIO_DRV_1);
    /* spi miso */
    bflb_gpio_init(gpio, GPIO_PIN_18, GPIO_FUNC_SPI0 | GPIO_ALTERNATE | GPIO_PULLUP | GPIO_SMT_EN | GPIO_DRV_1);
    /* spi mosi */
    bflb_gpio_init(gpio, GPIO_PIN_19, GPIO_FUNC_SPI0 | GPIO_ALTERNATE | GPIO_PULLUP | GPIO_SMT_EN | GPIO_DRV_1);
}

void SPIClass::beginTransaction(unsigned int speedMaximum, unsigned char dataOrder, unsigned char dataMode)
{
    struct bflb_spi_config_s spi_cfg = {
        .freq = 1*1000*1000,
        .role = SPI_ROLE_MASTER,
        .mode = SPI_MODE3,
        .data_width = SPI_DATA_WIDTH_8BIT,
        .bit_order = SPI_BIT_MSB,
        .byte_order = SPI_BYTE_MSB,
        .tx_fifo_threshold = 0,
        .rx_fifo_threshold = 0,
    };

    GLB_Set_SPI_CLK(ENABLE, GLB_SPI_CLK_MCU_MUXPLL_160M, 0);

    spi_cfg.bit_order = dataOrder;
    spi_cfg.byte_order = dataOrder;
    spi_cfg.mode = dataMode;
    spi0 = bflb_device_get_by_name("spi0");
    bflb_spi_init(spi0, &spi_cfg);
    bflb_spi_feature_control(spi0, SPI_CMD_SET_CS_INTERVAL, 0);
    bflb_spi_feature_control(spi0, SPI_CMD_SET_DATA_WIDTH, SPI_DATA_WIDTH_8BIT);
}

unsigned char SPIClass::transfer(unsigned char val)
{
    unsigned char * p_tx = &val;
    unsigned char rx = 0;
    bflb_spi_poll_exchange(spi0, p_tx, &rx, 1);

    return rx;
}

void SPIClass::endTransaction(void)
{
    bflb_spi_deinit(spi0);
}

void SPIClass::end(void)
{
    struct bflb_device_s *gpio;

    gpio = bflb_device_get_by_name("gpio");
    /* spi cs */
    bflb_gpio_deinit(gpio, GPIO_PIN_20);
    /* spi clk */
    bflb_gpio_deinit(gpio, GPIO_PIN_17);
    /* spi miso */
    bflb_gpio_deinit(gpio, GPIO_PIN_18);
    /* spi mosi */
    bflb_gpio_deinit(gpio, GPIO_PIN_19);
}

void SPIClass::setBitOrder(void)
{

}

void SPIClass::setDataMode(void)
{

}

void SPIClass::setClockDivider(uint8_t divider)
{

}

void SPIClass::usingInterrupt(void)
{

}

SPIClass SPI;


