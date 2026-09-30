/**
  ******************************************************************************
  * @file    chled_pwm.h
  * @brief   Software PWM for the four channel status LEDs (RTDx_STAT, GPIOE).
  *
  *  The pins (PE11/PE12/PE13/PE14) do not all map to independent timer
  *  channels (PE12 is only TIM1_CH3N, and TIM1 is the HAL time base), so the
  *  dimming is done in software: TIM7 fires at CHLED_PWM_LEVELS x 200 Hz and
  *  the ISR compares one counter against the per-LED level and writes all
  *  four pins with a single BSRR access. 64 levels -> 200 Hz frame, no
  *  visible flicker, ~13 kHz ISR of a few instructions.
  ******************************************************************************
  */
#ifndef APPLICATION_CHLED_PWM_H
#define APPLICATION_CHLED_PWM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CHLED_PWM_CHANNELS   4u
#define CHLED_PWM_LEVELS     64u
#define CHLED_PWM_MAX        (CHLED_PWM_LEVELS - 1u)

/** Start TIM7 and its interrupt; all LEDs off. */
void chled_pwm_init(void);

/** Set brightness 0..CHLED_PWM_MAX (clamped) for LED @p ch (0..3). */
void chled_pwm_set(uint8_t ch, uint8_t level);

#ifdef __cplusplus
}
#endif

#endif /* APPLICATION_CHLED_PWM_H */
