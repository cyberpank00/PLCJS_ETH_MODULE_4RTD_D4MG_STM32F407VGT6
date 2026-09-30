/**
  ******************************************************************************
  * @file    ads1220.c
  * @brief   ADS1220 x4 driver implementation (see ads1220.h).
  ******************************************************************************
  */

#include "ads1220.h"

#include <stddef.h>

#include "main.h"
#include "spi_bus.h"
#include "stm32f4xx_hal.h"

typedef struct {
    GPIO_TypeDef* port;
    uint16_t      pin;
} gpio_ref_t;

static const gpio_ref_t s_cs[ADS1220_CHANNEL_COUNT] = {
    { RTD0_CS_GPIO_Port, RTD0_CS_Pin },
    { RTD1_CS_GPIO_Port, RTD1_CS_Pin },
    { RTD2_CS_GPIO_Port, RTD2_CS_Pin },
    { RTD3_CS_GPIO_Port, RTD3_CS_Pin },
};

/* Last config register 0 written per channel — used as a liveness check. */
static uint8_t s_cfg0[ADS1220_CHANNEL_COUNT];

static inline void short_delay(uint32_t loops)
{
    for (volatile uint32_t i = 0; i < loops; i++) { __NOP(); }
}

static inline void cs_assert(uint8_t ch)
{
    HAL_GPIO_WritePin(s_cs[ch].port, s_cs[ch].pin, GPIO_PIN_RESET);
    short_delay(8u);   /* t_CSSC */
}

static inline void cs_release(uint8_t ch)
{
    short_delay(8u);   /* t_SCCS */
    HAL_GPIO_WritePin(s_cs[ch].port, s_cs[ch].pin, GPIO_PIN_SET);
    short_delay(8u);   /* t_CSH */
}

static void send_cmd(uint8_t ch, uint8_t cmd)
{
    cs_assert(ch);
    (void)spi_bus_transfer(cmd);
    cs_release(ch);
}

static void write_regs(uint8_t ch, uint8_t first, const uint8_t* vals, uint8_t count)
{
    cs_assert(ch);
    (void)spi_bus_transfer((uint8_t)(ADS1220_CMD_WREG | (first << 2) | (count - 1u)));
    for (uint8_t i = 0; i < count; i++) {
        (void)spi_bus_transfer(vals[i]);
    }
    cs_release(ch);
}

uint8_t ads1220_read_reg(uint8_t ch, uint8_t reg)
{
    if (ch >= ADS1220_CHANNEL_COUNT || reg > 3u) {
        return 0u;
    }
    cs_assert(ch);
    (void)spi_bus_transfer((uint8_t)(ADS1220_CMD_RREG | (reg << 2)));
    const uint8_t v = spi_bus_transfer(0x00u);
    cs_release(ch);
    return v;
}

static void configure(uint8_t ch, uint8_t gain_code)
{
    const uint8_t regs[4] = {
        (uint8_t)(ADS1220_MUX_AIN0_AIN1 | (gain_code & 0x0Eu)),
        ADS1220_CFG1_DEFAULT,
        ADS1220_CFG2_DEFAULT,
        ADS1220_CFG3_DEFAULT,
    };
    s_cfg0[ch] = regs[0];
    write_regs(ch, 0u, regs, 4u);
    send_cmd(ch, ADS1220_CMD_START);
}

void ads1220_init(const uint8_t gain_code[ADS1220_CHANNEL_COUNT])
{
    spi_bus_init();

    for (uint8_t ch = 0; ch < ADS1220_CHANNEL_COUNT; ch++) {
        send_cmd(ch, ADS1220_CMD_RESET);
    }
    /* t_RESET: >= 50 us + 32 modulator clocks before the next access. */
    short_delay(20000u);

    for (uint8_t ch = 0; ch < ADS1220_CHANNEL_COUNT; ch++) {
        configure(ch, gain_code[ch]);
    }
}

void ads1220_set_gain(uint8_t ch, uint8_t gain_code)
{
    if (ch >= ADS1220_CHANNEL_COUNT) {
        return;
    }
    /* Rewriting config register 0 restarts the running conversion; a START
     * afterwards keeps the sequence identical to init. */
    configure(ch, gain_code);
}

void ads1220_enter_ref_monitor(uint8_t ch)
{
    if (ch >= ADS1220_CHANNEL_COUNT) {
        return;
    }
    const uint8_t regs[4] = {
        ADS1220_MUX_REF_MON | ADS1220_GAIN_1,
        ADS1220_CFG1_DEFAULT,
        ADS1220_CFG2_DEFAULT,   /* VREF = REF0 selects WHICH pair the monitor measures */
        ADS1220_CFG3_DEFAULT,
    };
    s_cfg0[ch] = regs[0];
    write_regs(ch, 0u, regs, 4u);
    send_cmd(ch, ADS1220_CMD_START);
}

bool ads1220_read_data(uint8_t ch, int32_t* code_out)
{
    if (ch >= ADS1220_CHANNEL_COUNT || code_out == NULL) {
        return false;
    }

    cs_assert(ch);
    (void)spi_bus_transfer(ADS1220_CMD_RDATA);
    const uint8_t b2 = spi_bus_transfer(0x00u);
    const uint8_t b1 = spi_bus_transfer(0x00u);
    const uint8_t b0 = spi_bus_transfer(0x00u);
    cs_release(ch);

    /* Sign-extend the 24-bit two's-complement result. */
    int32_t code = (int32_t)(((uint32_t)b2 << 16) | ((uint32_t)b1 << 8) | b0);
    if (code & 0x00800000) {
        code -= 0x01000000;
    }
    *code_out = code;

    /* Liveness: an absent / unpowered converter returns a constant bus level
     * for every byte, so a mismatching config readback flags the channel. */
    return ads1220_read_reg(ch, 0u) == s_cfg0[ch];
}
