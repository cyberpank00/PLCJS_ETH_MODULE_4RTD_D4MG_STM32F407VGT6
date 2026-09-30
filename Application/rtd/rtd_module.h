/**
  ******************************************************************************
  * @file    rtd_module.h
  * @brief   Four-channel RTD acquisition on top of the ADS1220 driver.
  *
  *  Each channel:
  *    - selects its gain class (ADS1220 PGA gain) from the configured sensor
  *      type (or a calibration override),
  *    - reads the 24-bit ratiometric code and converts it to an uncalibrated
  *      resistance using the nominal RREF:  R = code/2^23 · 2·RREF / gain,
  *    - applies the per-channel/per-gain-class 2-point calibration
  *      (R = gain·R_raw + offset) to remove RREF and PGA gain error,
  *    - optionally smooths the calibrated resistance with a per-channel EMA
  *      (level 0 = off, 1..3 => alpha 1/4, 1/8, 1/16); the raw resistance
  *      and the ADC code stay unfiltered,
  *    - converts the calibrated resistance to temperature for the selected
  *      scale (unless it is a pure-resistance mode),
  *    - flags faults from the code itself (the ADS1220 has no fault register):
  *      open sensor / over-range, shorted sensor, converter not responding,
  *    - drives its status LED (solid = active, off = inactive, fast blink =
  *      fault).
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

/* Calibration gain-class override (runtime only, not persisted):
 * 0 = from sensor type, 1..5 = force RTD_GCLASS_0..4. */
#define RTD_CAL_OVERRIDE_AUTO       0u
#define RTD_CAL_OVERRIDE_MAX        5u

/* Fault codes reported in rtd_channel_status_t.fault_code. */
#define RTD_FAULT_NONE              0u
#define RTD_FAULT_OPEN              1u   /* open sensor / over-range (code ≈ +FS) */
#define RTD_FAULT_SHORT             2u   /* shorted sensor / under-range          */
#define RTD_FAULT_ADC               3u   /* converter not responding on SPI       */
#define RTD_FAULT_REVERSED          4u   /* negative code: excitation not flowing S+ -> S-
                                            (leads swapped, sensor on S-/E only, no return) */

/* int16 view of a channel reading (holding registers 0..3). */
#define RTD_I16_FAULT               ((int16_t)-32768)
#define RTD_I16_DISABLED            ((int16_t)0)

typedef struct {
    bool     enabled;
    bool     valid;         /* last conversion produced a usable value      */
    bool     fault;
    uint8_t  fault_code;    /* RTD_FAULT_*                                  */
    uint8_t  gclass;        /* active gain class (RTD_GCLASS_*)             */
    int32_t  adc_code;      /* 24-bit signed conversion result              */
    float    r_raw;         /* uncalibrated resistance, Ω                   */
    float    r_cal;         /* calibrated resistance, Ω                     */
    float    temperature;   /* °C, NaN for resistance modes / faults        */
} rtd_channel_status_t;

/** Initialise SPI, the ADS1220 converters and the runtime configuration. */
void rtd_module_init(void);

/** Re-read settings into the runtime state and apply gains / config. */
void rtd_module_apply_config(void);

/** Acquire all four channels once (call periodically from the RTD task). */
void rtd_module_tick(void);

/** Drive the channel status LEDs; call from a fast (e.g. 10 ms) tick. */
void rtd_module_led_tick(uint16_t period_ms);

/** Read-only access to the latest per-channel status (NULL if out of range). */
const rtd_channel_status_t* rtd_module_get_status(uint8_t ch);

/**
 * int16 representation of a channel for the compact register map:
 *   RTD types      : temperature °C × 100 (−327.68 … +327.67)
 *   resistance     : 0..32767 scaled from the mode full-scale (200 / 2000 Ω)
 *   disabled       : 0,   fault / not yet valid : −32768
 */
int16_t rtd_module_int16_view(uint8_t ch);

/** Configured full-scan period, ms (clamped). */
uint16_t rtd_module_scan_period_ms(void);

/** Set the runtime gain-class override for a channel (not persisted). */
void rtd_module_set_cal_override(uint8_t ch, uint8_t override);

/** Get the runtime gain-class override for a channel. */
uint8_t rtd_module_get_cal_override(uint8_t ch);

#ifdef __cplusplus
}
#endif

#endif /* APPLICATION_RTD_MODULE_H */
