/**
  ******************************************************************************
  * @file    spi_bus.c
  * @brief   Bare-register SPI1 master driver (see spi_bus.h).
  ******************************************************************************
  */

#include "spi_bus.h"

#include "stm32f4xx_hal.h"

static volatile uint8_t s_initialised = 0u;

void spi_bus_init(void)
{
    if (s_initialised) {
        return;
    }

    /* Peripheral + GPIO clocks. */
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_SPI1_CLK_ENABLE();

    /* --- GPIO alternate-function setup (AF5 = SPI1) --------------------- */
    /* PA5 SCK, PA6 MISO -> AF mode, AF5, very-high speed. */
    GPIOA->MODER   &= ~((3u << (5u * 2u)) | (3u << (6u * 2u)));
    GPIOA->MODER   |=  ((2u << (5u * 2u)) | (2u << (6u * 2u)));   /* AF */
    GPIOA->OSPEEDR |=  ((3u << (5u * 2u)) | (3u << (6u * 2u)));
    GPIOA->AFR[0]  &= ~((0xFu << (5u * 4u)) | (0xFu << (6u * 4u)));
    GPIOA->AFR[0]  |=  ((5u << (5u * 4u)) | (5u << (6u * 4u)));

    /* PB5 MOSI -> AF mode, AF5, very-high speed. */
    GPIOB->MODER   &= ~(3u << (5u * 2u));
    GPIOB->MODER   |=  (2u << (5u * 2u));
    GPIOB->OSPEEDR |=  (3u << (5u * 2u));
    GPIOB->AFR[0]  &= ~(0xFu << (5u * 4u));
    GPIOB->AFR[0]  |=  (5u << (5u * 4u));

    /* --- SPI1 configuration -------------------------------------------- */
    SPI1->CR1 = 0u;
    SPI1->CR1 =  SPI_CR1_MSTR          /* master                            */
               | (4u << SPI_CR1_BR_Pos)/* PCLK2 / 32 (~2.6 MHz)             */
               | SPI_CR1_CPHA          /* CPOL = 0, CPHA = 1 -> SPI mode 1  */
               | SPI_CR1_SSM           /* software slave management         */
               | SPI_CR1_SSI;          /* internal NSS high (master)        */
    SPI1->CR2 = 0u;
    SPI1->CR1 |= SPI_CR1_SPE;          /* enable                            */

    s_initialised = 1u;
}

uint8_t spi_bus_transfer(uint8_t out)
{
    /* 8-bit access to DR so the peripheral clocks out exactly one byte. */
    while ((SPI1->SR & SPI_SR_TXE) == 0u) { }
    *(volatile uint8_t*)&SPI1->DR = out;

    while ((SPI1->SR & SPI_SR_RXNE) == 0u) { }
    return *(volatile uint8_t*)&SPI1->DR;
}
