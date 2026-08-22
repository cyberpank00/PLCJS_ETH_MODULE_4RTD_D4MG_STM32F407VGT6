/**
  ******************************************************************************
  * @file    rtd_module.h
  * @brief   Four-channel RTD acquisition on top of the MAX31865 driver.
  *
  *  Each channel:
  *    - selects its reference-resistor range (RANG pin) from the configured
  *      sensor type (or a calibration override),
  *    - reads the MAX31865 ratiometric code and converts it to an
  *      uncalibrated resistance using the nominal RREF,
  *    - applies the per-channel/per-range 2-point calibration
  *      (R = gain·R_raw + offset) to remove RREF error and switch Ron,
  *    - optionally smooths the calibrated resistance with a per-channel EMA
  *      (level 0 = off, 1..3 => alpha 1/4, 1/8, 1/16); the raw resistance
  *      and the ADC code stay unfiltered,
  *    - converts the calibrated resistance to temperature for the selected
  *      scale (unless it is a pure-resistance mode),
  *    - drives its status LED (solid = active, off = inactive, fast blink =
  *      converter fault).
  ******************************************************************************
  */
#ifndef APPLICATION_RTD_MODULE_H
#define APPLICATION_RTD_MODULE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RTD_MODULE_CHANNEL_COUNT    4u

/* Calibration range override (runtime only, not persisted). */
#define RTD_CAL_OVERRIDE_AUTO       0u   /* range from sensor type       */
#define RTD_CAL_OVERRIDE_LOW        1u   /* force low RREF range         */
#define RTD_CAL_OVERRIDE_HIGH       2u   /* force high RREF range        */

typedef struct {
    bool     enabled;
    bool     valid;         /* last conversion produced a usable value      */
    bool     fault;         /* MAX31865 flagged a fault                     */
    uint8_t  fault_code;    /* MAX31865 fault-status register (0x07)        */
    uint8_t  range;         /* resolved reference-resistor range            */
    uint16_t adc_code;      /* 15-bit ratiometric code                      */
    float    r_raw;         /* uncalibrated resistance, Ω                   */
    float    r_cal;         /* calibrated resistance, Ω                     */
    float    temperature;   /* °C, NaN for resistance modes / faults        */
} rtd_channel_status_t;

/** Initialise SPI, the MAX31865 converters and the runtime configuration. */
void rtd_module_init(void);

/** Re-read settings into the runtime state and apply RANG / config. */
void rtd_module_apply_config(void);

/** Acquire all four channels once (call periodically from the RTD task). */
void rtd_module_tick(void);

/** Drive the channel status LEDs; call from a fast (e.g. 10 ms) tick. */
void rtd_module_led_tick(uint16_t period_ms);

/** Read-only access to the latest per-channel status (NULL if out of range). */
const rtd_channel_status_t* rtd_module_get_status(uint8_t ch);

/** Configured full-scan period, ms (clamped). */
uint16_t rtd_module_scan_period_ms(void);

/** Set the runtime calibration range override for a channel (not persisted). */
void rtd_module_set_cal_override(uint8_t ch, uint8_t override);

/** Get the runtime calibration range override for a channel. */
uint8_t rtd_module_get_cal_override(uint8_t ch);

#ifdef __cplusplus
}
#endif

#endif /* APPLICATION_RTD_MODULE_H */
