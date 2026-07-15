/**
  ******************************************************************************
  * @file    rtd_scales.h
  * @brief   RTD sensor scales and resistance-to-temperature conversion.
  *
  *  Supported sensor types (Russian ГОСТ 6651 and IEC/DIN designations). The
  *  name already encodes both the material and the nominal temperature
  *  coefficient, but every type also carries a *default* W100 = R(100)/R(0)
  *  which the user may override per channel to match a specific manufacturer:
  *
  *    М  (медь / copper, ГОСТ)   α = 0.00428   W100 = 1.4280
  *    Cu (copper, foreign)        α = 0.00426   W100 = 1.4260
  *    П  (платина / Pt, ГОСТ)    α = 0.00391   W100 = 1.3910
  *    Pt (platinum, IEC 60751)    α = 0.00385   W100 = 1.3851
  *    Ni (nickel, DIN 43760)      α = 0.00617   W100 = 1.6180
  *
  *  Platinum uses the Callendar–Van Dusen equation expressed via (α, δ, β) so
  *  that a custom W100 only rescales α while keeping the standard δ/β. Copper
  *  uses the ГОСТ linear form with a small sub-zero correction. Nickel uses the
  *  DIN 43760 polynomial scaled by the α ratio. The forward R(t) function is
  *  inverted numerically (bisection) to obtain the temperature.
  *
  *  Two pure-resistance modes (0..2 kΩ, 0..5 kΩ) report the measured
  *  resistance directly (temperature is reported as NaN).
  ******************************************************************************
  */
#ifndef APPLICATION_RTD_SCALES_H
#define APPLICATION_RTD_SCALES_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sensor type codes (Modbus enum). Order follows the datasheet list. */
typedef enum {
    RTD_TYPE_50M = 0,   /* 50М   Cu ГОСТ  R0=50   */
    RTD_TYPE_CU50,      /* Cu50  Cu       R0=50   */
    RTD_TYPE_50P,       /* 50П   Pt ГОСТ  R0=50   */
    RTD_TYPE_PT50,      /* Pt50  Pt IEC   R0=50   */
    RTD_TYPE_NI100,     /* Ni100 Ni DIN   R0=100  */
    RTD_TYPE_100M,      /* 100М  Cu ГОСТ  R0=100  */
    RTD_TYPE_CU100,     /* Cu100 Cu       R0=100  */
    RTD_TYPE_100P,      /* 100П  Pt ГОСТ  R0=100  */
    RTD_TYPE_PT100,     /* Pt100 Pt IEC   R0=100  */
    RTD_TYPE_NI500,     /* Ni500 Ni DIN   R0=500  */
    RTD_TYPE_500M,      /* 500М  Cu ГОСТ  R0=500  */
    RTD_TYPE_CU500,     /* Cu500 Cu       R0=500  */
    RTD_TYPE_500P,      /* 500П  Pt ГОСТ  R0=500  */
    RTD_TYPE_PT500,     /* Pt500 Pt IEC   R0=500  */
    RTD_TYPE_NI1000,    /* Ni1000 Ni DIN  R0=1000 */
    RTD_TYPE_1000M,     /* 1000М Cu ГОСТ  R0=1000 */
    RTD_TYPE_CU1000,    /* Cu1000 Cu      R0=1000 */
    RTD_TYPE_1000P,     /* 1000П Pt ГОСТ  R0=1000 */
    RTD_TYPE_PT1000,    /* Pt1000 Pt IEC  R0=1000 */
    RTD_TYPE_RES_2K,    /* resistance 0..2 kΩ     */
    RTD_TYPE_RES_5K,    /* resistance 0..5 kΩ     */
    RTD_TYPE_COUNT
} rtd_type_t;

/* Reference-resistor range selected via the ADG849 (RANG pin). */
#define RTD_RANGE_LOW   0u   /* low RREF  (~400 Ω)  — 50/100 Ω sensors     */
#define RTD_RANGE_HIGH  1u   /* high RREF (~4000 Ω) — 500/1000 Ω, resistance */

typedef enum {
    RTD_MAT_PT = 0,
    RTD_MAT_CU,
    RTD_MAT_NI,
    RTD_MAT_RES,
} rtd_material_t;

typedef struct {
    const char*    name;
    rtd_material_t material;
    float          r0;            /* R at 0 °C, Ω (RES: full-scale nominal) */
    float          w100_default;  /* default R100/R0                        */
    float          k1;            /* Pt: δ  | Cu: sub-zero β | Ni/RES: n/a  */
    float          k2;            /* Pt: β  | others: n/a                   */
    uint8_t        range;         /* RTD_RANGE_LOW / RTD_RANGE_HIGH         */
} rtd_type_info_t;

/** Get static info for a sensor type (NULL if out of range). */
const rtd_type_info_t* rtd_type_info(uint8_t type);

/** Reference-resistor range required by a sensor type. */
uint8_t rtd_type_range(uint8_t type);

/** true for the pure-resistance output modes. */
bool rtd_type_is_resistance(uint8_t type);

/** Default W100 for a type, encoded as W100 × 10000 (e.g. 1.3851 -> 13851). */
uint16_t rtd_type_default_w100_x10000(uint8_t type);

/**
 * Convert a measured resistance (Ω) to temperature (°C) for a sensor type,
 * using the supplied W100 (R100/R0). Returns NaN for resistance modes or if
 * the value is outside the invertible range.
 */
float rtd_resistance_to_temperature(uint8_t type, float w100, float resistance);

#ifdef __cplusplus
}
#endif

#endif /* APPLICATION_RTD_SCALES_H */
