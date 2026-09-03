/**
  ******************************************************************************
  * @file    spi_bus.h
  * @brief   Minimal bare-register SPI1 master driver for the ADS1220 chain.
  *
  *  SPI1 is shared by all four ADS1220 converters (individual chip-selects are
  *  driven by the ADS1220 driver). The bus is configured as:
  *    - Master, 8-bit, MSB first
  *    - SPI mode 1 (CPOL = 0, CPHA = 1) as required by the ADS1220
  *    - ~2.6 MHz (PCLK2 / 32) — conservative, tolerant of the digital isolators
  *
  *  Pins (AF5):
  *    - PA5  SPI1_SCK
  *    - PA6  SPI1_MISO
  *    - PB5  SPI1_MOSI
  *
  *  The driver uses direct register access to avoid pulling in the STM32 HAL
  *  SPI module (kept consistent with temp_module's bare-register approach).
  ******************************************************************************
  */
#ifndef APPLICATION_SPI_BUS_H
#define APPLICATION_SPI_BUS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Configure the SPI1 peripheral and its GPIO pins. Idempotent. */
void spi_bus_init(void);

/**
 * Full-duplex 8-bit transfer: shift @p out on MOSI while capturing MISO.
 * Blocking, polled. Chip-select management is the caller's responsibility.
 */
uint8_t spi_bus_transfer(uint8_t out);

#ifdef __cplusplus
}
#endif

#endif /* APPLICATION_SPI_BUS_H */
