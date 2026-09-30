/**
  ******************************************************************************
  * @file    chled_pwm.c
  * @brief   Software PWM for the channel status LEDs, see chled_pwm.h.
  ******************************************************************************
  */
#include "chled_pwm.h"

#include "main.h"
#include "stm32f4xx_hal.h"

/* TIM7 is on APB1 (42 MHz); timer clock is doubled to 84 MHz because the APB1
 * prescaler is not 1. 84 MHz / 84 = 1 MHz tick, / 78 = 12.82 kHz update
 * -> 64-step PWM frame at ~200 Hz. */
#define CHLED_TIM_PRESCALER   (84u - 1u)
#define CHLED_TIM_PERIOD      (78u - 1u)

static const uint16_t s_pin[CHLED_PWM_CHANNELS] = {
    RTD0_STAT_Pin, RTD1_STAT_Pin, RTD2_STAT_Pin, RTD3_STAT_Pin,
};

static volatile uint8_t s_level[CHLED_PWM_CHANNELS];
static uint8_t          s_counter;

void chled_pwm_init(void)
{
    for (uint8_t i = 0; i < CHLED_PWM_CHANNELS; i++) { s_level[i] = 0u; }

    __HAL_RCC_TIM7_CLK_ENABLE();
    TIM7->CR1  = 0u;
    TIM7->PSC  = CHLED_TIM_PRESCALER;
    TIM7->ARR  = CHLED_TIM_PERIOD;
    TIM7->EGR  = TIM_EGR_UG;            /* load PSC/ARR */
    TIM7->SR   = 0u;
    TIM7->DIER = TIM_DIER_UIE;

    /* No RTOS calls from the ISR, so any priority below the HAL time base is
     * fine; keep it modest so it never delays SysTick/ETH. */
    HAL_NVIC_SetPriority(TIM7_IRQn, 6u, 0u);
    HAL_NVIC_EnableIRQ(TIM7_IRQn);

    TIM7->CR1 = TIM_CR1_CEN;
}

void chled_pwm_set(uint8_t ch, uint8_t level)
{
    if (ch >= CHLED_PWM_CHANNELS) { return; }
    s_level[ch] = (level > CHLED_PWM_MAX) ? (uint8_t)CHLED_PWM_MAX : level;
}

void TIM7_IRQHandler(void)
{
    TIM7->SR = 0u;                      /* clear UIF */

    const uint8_t c = s_counter;
    s_counter = (uint8_t)((c + 1u) % CHLED_PWM_LEVELS);

    uint32_t set = 0u, reset = 0u;
    for (uint8_t i = 0; i < CHLED_PWM_CHANNELS; i++) {
        /* level 0 -> never on; level MAX -> always on (c never exceeds MAX). */
        if (s_level[i] > c) { set |= s_pin[i]; } else { reset |= s_pin[i]; }
    }
    GPIOE->BSRR = set | (reset << 16);
}
