/* mbed Microcontroller Library
 * Copyright (c) 2006-2013 ARM Limited
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "mbed_assert.h"

#include "spi_api.h"
#include "cmsis.h"
#include "pinmap.h"
#include "mbed_error.h"

#if DEVICE_SPI

static const SWM_Map SWM_SPI_SSEL[] = {
    {4, 16},
    {5, 16},
};

static const SWM_Map SWM_SPI_SCLK[] = {
    {3, 24},
    {4, 24},
};

static const SWM_Map SWM_SPI_MOSI[] = {
    {4, 0},
    {5, 0},
};

static const SWM_Map SWM_SPI_MISO[] = {
    {4, 8},
    {5, 16},
};

// bit flags for used SPIs
static unsigned char spi_used = 0;

static int get_available_spi(void)
{
    int i;
    for (i=0; i<2; i++) {
        if ((spi_used & (1 << i)) == 0)
            return i;
    }
    return -1;
}

static inline int ssp_disable(spi_t *obj);
static inline int ssp_enable(spi_t *obj);

void spi_init(spi_t *obj, PinName mosi, PinName miso, PinName sclk, PinName ssel)
{
    int spi_n = get_available_spi();
    if (spi_n == -1) {
        error("No available SPI");
    }
    obj->spi_n = spi_n;
    spi_used |= (1 << spi_n);

    obj->spi = (spi_n) ? (LPC_SPI0_Type *)(LPC_SPI1_BASE) : (LPC_SPI0_Type *)(LPC_SPI0_BASE);

    const SWM_Map *swm;
    uint32_t regVal;

    if (sclk != (PinName)NC) {
        swm = &SWM_SPI_SCLK[obj->spi_n];
        regVal = LPC_SWM->PINASSIGN[swm->n] & ~(0xFF << swm->offset);
        LPC_SWM->PINASSIGN[swm->n] = regVal |  ((sclk >> PIN_SHIFT) << swm->offset);
    }

    if (mosi != (PinName)NC) {
        swm = &SWM_SPI_MOSI[obj->spi_n];
        regVal = LPC_SWM->PINASSIGN[swm->n] & ~(0xFF << swm->offset);
        LPC_SWM->PINASSIGN[swm->n] = regVal |  ((mosi >> PIN_SHIFT) << swm->offset);
    }

    if (miso != (PinName)NC) {
        swm = &SWM_SPI_MISO[obj->spi_n];
        regVal = LPC_SWM->PINASSIGN[swm->n] & ~(0xFF << swm->offset);
        LPC_SWM->PINASSIGN[swm->n] = regVal |  ((miso >> PIN_SHIFT) << swm->offset);
    }

    if (ssel != (PinName)NC) {
        swm = &SWM_SPI_SSEL[obj->spi_n];
        regVal = LPC_SWM->PINASSIGN[swm->n] & ~(0xFF << swm->offset);
        LPC_SWM->PINASSIGN[swm->n] = regVal |  ((ssel >> PIN_SHIFT) << swm->offset);
    }

    // clear interrupts
    obj->spi->INTENCLR = 0x3f;

    LPC_SYSCON->SYSAHBCLKCTRL |=  (1 << (11 + obj->spi_n));
    LPC_SYSCON->PRESETCTRL    &= ~(1 << obj->spi_n);
    LPC_SYSCON->PRESETCTRL    |=  (1 << obj->spi_n);

    // set default format and frequency
    if (ssel == NC) {
        spi_format(obj, 8, 0, 0);  // 8 bits, mode 0, master
    } else {
        spi_format(obj, 8, 0, 1);  // 8 bits, mode 0, slave
    }
    spi_frequency(obj, 1000000);

    // enable the ssp channel
    ssp_enable(obj);
}

void spi_free(spi_t *obj)
{
}

void spi_format(spi_t *obj, int bits, int mode, int slave)
{
    MBED_ASSERT(((bits >= 1) && (bits <= 16)) && ((mode >= 0) && (mode <= 3)));
    ssp_disable(obj);

    obj->spi->CFG &= ~((0x3 << 4) | (1 << 2));
    obj->spi->CFG |=  ((mode & 0x3) << 4) | ((slave ? 0 : 1) << 2);

    obj->spi->TXDATCTL &= ~( 0xF << 24);
    obj->spi->TXDATCTL |=  (((bits & 0xF) - 1) << 24);

    ssp_enable(obj);
}

void spi_frequency(spi_t *obj, int hz)
{
    ssp_disable(obj);

    // rise DIV value if it cannot be divided
    obj->spi->DIV = (SystemCoreClock + (hz - 1))/hz - 1;
    obj->spi->DLY = 0;

    ssp_enable(obj);
}

static inline int ssp_disable(spi_t *obj)
{
    return obj->spi->CFG &= ~(1 << 0);
}

static inline int ssp_enable(spi_t *obj)
{
    return obj->spi->CFG |= (1 << 0);
}

static inline int ssp_readable(spi_t *obj)
{
    return obj->spi->STAT & (1 << 0);
}

static inline int ssp_writeable(spi_t *obj)
{
    return obj->spi->STAT & (1 << 1);
}

static inline void ssp_write(spi_t *obj, int value)
{
    while (!ssp_writeable(obj));
    // end of transfer
    obj->spi->TXDATCTL |= (1 << 20);
    obj->spi->TXDAT = value;
}

static inline int ssp_read(spi_t *obj)
{
    while (!ssp_readable(obj));
    return obj->spi->RXDAT;
}

static inline int ssp_busy(spi_t *obj)
{
    // checking RXOV(Receiver Overrun interrupt flag)
    return obj->spi->STAT & (1 << 2);
}

int spi_master_write(spi_t *obj, int value)
{
    ssp_write(obj, value);
    return ssp_read(obj);
}

int spi_slave_receive(spi_t *obj)
{
    return (ssp_readable(obj) && !ssp_busy(obj)) ? (1) : (0);
}

int spi_slave_read(spi_t *obj)
{
    return obj->spi->RXDAT;
}

void spi_slave_write(spi_t *obj, int value)
{
    while (ssp_writeable(obj) == 0);
    obj->spi->TXDAT = value;
}

int spi_busy(spi_t *obj)
{
    return ssp_busy(obj);
}

#endif
