/**
  ******************************************************************************
  * @file    max31865.c
  * @brief   MAX31865 x4 driver implementation (see max31865.h).
  ******************************************************************************
  */

#include "max31865.h"

#include "main.h"
#include "spi_bus.h"
#include "stm32f4xx_hal.h"

typedef struct {
    GPIO_TypeDef* port;
    uint16_t      pin;
} gpio_ref_t;

static const gpio_ref_t s_cs[MAX31865_CHANNEL_COUNT] = {
    { RTD0_CS_GPIO_Port, RTD0_CS_Pin },
    { RTD1_CS_GPIO_Port, RTD1_CS_Pin },
    { RTD2_CS_GPIO_Port, RTD2_CS_Pin },
    { RTD3_CS_GPIO_Port, RTD3_CS_Pin },
};

/* Default operating configuration: 3-wire, continuous auto-conversion,
 * VBIAS on, 50 Hz filter. */
#define MAX31865_DEFAULT_CONFIG \
    (MAX31865_CFG_VBIAS | MAX31865_CFG_CONV_AUTO | MAX31865_CFG_3WIRE | MAX31865_CFG_FILT_50HZ)

static inline void cs_assert(uint8_t ch)
{
    HAL_GPIO_WritePin(s_cs[ch].port, s_cs[ch].pin, GPIO_PIN_RESET);
    /* t_CSS setup: a few hundred ns is plenty at this clock rate. */
    for (volatile uint32_t i = 0; i < 8u; i++) { __NOP(); }
}

static inline void cs_release(uint8_t ch)
{
    for (volatile uint32_t i = 0; i < 8u; i++) { __NOP(); }
    HAL_GPIO_WritePin(s_cs[ch].port, s_cs[ch].pin, GPIO_PIN_SET);
}

static uint8_t reg_read8(uint8_t ch, uint8_t addr)
{
    cs_assert(ch);
    (void)spi_bus_transfer(addr & 0x7Fu);
    const uint8_t v = spi_bus_transfer(0xFFu);
    cs_release(ch);
    return v;
}

static uint16_t reg_read16(uint8_t ch, uint8_t addr)
{
    cs_assert(ch);
    (void)spi_bus_transfer(addr & 0x7Fu);
    const uint8_t hi = spi_bus_transfer(0xFFu);
    const uint8_t lo = spi_bus_transfer(0xFFu);
    cs_release(ch);
    return (uint16_t)(((uint16_t)hi << 8) | lo);
}

void max31865_write_config(uint8_t ch, uint8_t config)
{
    if (ch >= MAX31865_CHANNEL_COUNT) {
        return;
    }
    cs_assert(ch);
    (void)spi_bus_transfer(MAX31865_REG_CONFIG | 0x80u);
    (void)spi_bus_transfer(config);
    cs_release(ch);
}

void max31865_init(void)
{
    spi_bus_init();
    for (uint8_t ch = 0; ch < MAX31865_CHANNEL_COUNT; ch++) {
        max31865_write_config(ch, MAX31865_DEFAULT_CONFIG);
    }
}

bool max31865_read_rtd(uint8_t ch, uint16_t* code_out)
{
    if (ch >= MAX31865_CHANNEL_COUNT) {
        if (code_out != NULL) { *code_out = 0u; }
        return true;
    }
    const uint16_t raw = reg_read16(ch, MAX31865_REG_RTD_MSB);
    if (code_out != NULL) {
        *code_out = (uint16_t)(raw >> 1);   /* drop the fault flag (bit0) */
    }
    return (raw & 0x0001u) != 0u;
}

uint8_t max31865_read_fault(uint8_t ch)
{
    if (ch >= MAX31865_CHANNEL_COUNT) {
        return 0u;
    }
    return reg_read8(ch, MAX31865_REG_FAULT_STATUS);
}

void max31865_clear_fault(uint8_t ch, uint8_t base_config)
{
    /* Writing FAULT_CLEAR (bit1) with 1-shot/auto bits masked clears the
     * latched fault status; keep the operating bits otherwise intact. */
    const uint8_t cfg = (uint8_t)((base_config & ~MAX31865_CFG_1SHOT) | MAX31865_CFG_FAULT_CLEAR);
    max31865_write_config(ch, cfg);
}
