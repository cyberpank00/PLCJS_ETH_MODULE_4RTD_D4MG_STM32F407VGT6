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
  *  Two pure-resistance modes (0..200 Ω, 0..2 kΩ) report the measured
  *  resistance directly (temperature is reported as NaN).
  *
  *  Every type belongs to a *gain class* (RTD_GCLASS_*) that selects the
  *  ADS1220 PGA gain and therefore the full-scale resistance
  *  (FS = 2·RREF / gain = 4000 Ω / gain with RREF = 2 kΩ). Calibration is
  *  stored per channel and gain class.
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
    RTD_TYPE_RES_200,   /* resistance 0..200 Ω    */
    RTD_TYPE_RES_2K,    /* resistance 0..2 kΩ     */
    RTD_TYPE_COUNT
} rtd_type_t;

/* Gain classes (ADS1220 PGA gain per sensor nominal). FS_R = 4000 Ω / gain. */
#define RTD_GCLASS_50       0u   /* gain 16, FS 250 Ω  — 50 Ω sensors            */
#define RTD_GCLASS_100      1u   /* gain 8,  FS 500 Ω  — 100 Ω sensors, R 0..200 */
#define RTD_GCLASS_500      2u   /* gain 2,  FS 2000 Ω — 500 Ω sensors           */
#define RTD_GCLASS_1000     3u   /* gain 1,  FS 4000 Ω — 1000 Ω sensors          */
#define RTD_GCLASS_2000     4u   /* gain 1,  FS 4000 Ω — R 0..2000               */
#define RTD_GCLASS_COUNT    5u

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
    uint8_t        gclass;        /* RTD_GCLASS_*                           */
} rtd_type_info_t;

/** Get static info for a sensor type (NULL if out of range). */
const rtd_type_info_t* rtd_type_info(uint8_t type);

/** Gain class required by a sensor type. */
uint8_t rtd_type_gclass(uint8_t type);

/** ADS1220 gain code (ADS1220_GAIN_*) for a gain class. */
uint8_t rtd_gclass_gain_code(uint8_t gclass);

/** Numeric PGA gain (1, 2, 8, 16 ...) for a gain class. */
float rtd_gclass_gain(uint8_t gclass);

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
