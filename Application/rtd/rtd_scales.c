/**
  ******************************************************************************
  * @file    rtd_scales.c
  * @brief   RTD scales and resistance-to-temperature conversion (see header).
  ******************************************************************************
  */

#include "rtd_scales.h"

#include <math.h>
#include <stddef.h>

/* Platinum Callendar–Van Dusen (α, δ, β) constants. */
#define PT_IEC_DELTA    1.49990f
#define PT_IEC_BETA     0.10863f
#define PT_GOST_DELTA   1.49830f
#define PT_GOST_BETA    0.10630f

/* Copper sub-zero correction coefficient (ГОСТ form W = 1+αt+β·t(t+6.7)). */
#define CU_SUBZERO_BETA (-6.20e-7f)

/* Nickel DIN 43760 reference (Ni100, W100 = 1.618). */
#define NI_ALPHA_REF    0.00618f
#define NI_A            5.485e-3f
#define NI_B            6.650e-6f
#define NI_C            2.805e-11f
#define NI_D            (-2.0e-17f)

/* ---------------------------------------------------------------------------
 * Type table — order MUST match rtd_type_t.
 * ------------------------------------------------------------------------- */
static const rtd_type_info_t s_types[RTD_TYPE_COUNT] = {
    /* name       material     r0     w100    k1(δ/β)         k2(β)        range */
    { "50M",    RTD_MAT_CU,   50.0f, 1.4280f, CU_SUBZERO_BETA, 0.0f,        RTD_RANGE_LOW  },
    { "Cu50",   RTD_MAT_CU,   50.0f, 1.4260f, CU_SUBZERO_BETA, 0.0f,        RTD_RANGE_LOW  },
    { "50P",    RTD_MAT_PT,   50.0f, 1.3910f, PT_GOST_DELTA,   PT_GOST_BETA, RTD_RANGE_LOW  },
    { "Pt50",   RTD_MAT_PT,   50.0f, 1.3851f, PT_IEC_DELTA,    PT_IEC_BETA,  RTD_RANGE_LOW  },
    { "Ni100",  RTD_MAT_NI,  100.0f, 1.6180f, 0.0f,            0.0f,         RTD_RANGE_LOW  },
    { "100M",   RTD_MAT_CU,  100.0f, 1.4280f, CU_SUBZERO_BETA, 0.0f,        RTD_RANGE_LOW  },
    { "Cu100",  RTD_MAT_CU,  100.0f, 1.4260f, CU_SUBZERO_BETA, 0.0f,        RTD_RANGE_LOW  },
    { "100P",   RTD_MAT_PT,  100.0f, 1.3910f, PT_GOST_DELTA,   PT_GOST_BETA, RTD_RANGE_LOW  },
    { "Pt100",  RTD_MAT_PT,  100.0f, 1.3851f, PT_IEC_DELTA,    PT_IEC_BETA,  RTD_RANGE_LOW  },
    { "Ni500",  RTD_MAT_NI,  500.0f, 1.6180f, 0.0f,            0.0f,         RTD_RANGE_HIGH },
    { "500M",   RTD_MAT_CU,  500.0f, 1.4280f, CU_SUBZERO_BETA, 0.0f,        RTD_RANGE_HIGH },
    { "Cu500",  RTD_MAT_CU,  500.0f, 1.4260f, CU_SUBZERO_BETA, 0.0f,        RTD_RANGE_HIGH },
    { "500P",   RTD_MAT_PT,  500.0f, 1.3910f, PT_GOST_DELTA,   PT_GOST_BETA, RTD_RANGE_HIGH },
    { "Pt500",  RTD_MAT_PT,  500.0f, 1.3851f, PT_IEC_DELTA,    PT_IEC_BETA,  RTD_RANGE_HIGH },
    { "Ni1000", RTD_MAT_NI, 1000.0f, 1.6180f, 0.0f,            0.0f,         RTD_RANGE_HIGH },
    { "1000M",  RTD_MAT_CU, 1000.0f, 1.4280f, CU_SUBZERO_BETA, 0.0f,        RTD_RANGE_HIGH },
    { "Cu1000", RTD_MAT_CU, 1000.0f, 1.4260f, CU_SUBZERO_BETA, 0.0f,        RTD_RANGE_HIGH },
    { "1000P",  RTD_MAT_PT, 1000.0f, 1.3910f, PT_GOST_DELTA,   PT_GOST_BETA, RTD_RANGE_HIGH },
    { "Pt1000", RTD_MAT_PT, 1000.0f, 1.3851f, PT_IEC_DELTA,    PT_IEC_BETA,  RTD_RANGE_HIGH },
    { "R2k",    RTD_MAT_RES, 2000.0f, 1.0f,   0.0f,            0.0f,         RTD_RANGE_HIGH },
    { "R5k",    RTD_MAT_RES, 5000.0f, 1.0f,   0.0f,            0.0f,         RTD_RANGE_HIGH },
};

const rtd_type_info_t* rtd_type_info(uint8_t type)
{
    return (type < RTD_TYPE_COUNT) ? &s_types[type] : NULL;
}

uint8_t rtd_type_range(uint8_t type)
{
    const rtd_type_info_t* t = rtd_type_info(type);
    return (t != NULL) ? t->range : RTD_RANGE_HIGH;
}

bool rtd_type_is_resistance(uint8_t type)
{
    const rtd_type_info_t* t = rtd_type_info(type);
    return (t != NULL) && (t->material == RTD_MAT_RES);
}

uint16_t rtd_type_default_w100_x10000(uint8_t type)
{
    const rtd_type_info_t* t = rtd_type_info(type);
    if (t == NULL) {
        return 0u;
    }
    return (uint16_t)(t->w100_default * 10000.0f + 0.5f);
}

/* ---------------------------------------------------------------------------
 * Forward model R(t) for a given type and W100.
 * ------------------------------------------------------------------------- */
static float forward_resistance(const rtd_type_info_t* t, float w100, float temp)
{
    const float alpha = (w100 - 1.0f) / 100.0f;

    switch (t->material) {
    case RTD_MAT_PT: {
        const float delta = t->k1;
        const float beta  = t->k2;
        const float A = alpha * (1.0f + delta / 100.0f);
        const float B = -alpha * delta / 10000.0f;
        float w;
        if (temp >= 0.0f) {
            w = 1.0f + A * temp + B * temp * temp;
        } else {
            const float C = -alpha * beta / 100000000.0f;
            w = 1.0f + A * temp + B * temp * temp + C * (temp - 100.0f) * temp * temp * temp;
        }
        return t->r0 * w;
    }
    case RTD_MAT_CU: {
        float w;
        if (temp >= 0.0f) {
            w = 1.0f + alpha * temp;
        } else {
            w = 1.0f + alpha * temp + t->k1 * temp * (temp + 6.7f);
        }
        return t->r0 * w;
    }
    case RTD_MAT_NI: {
        const float k  = alpha / NI_ALPHA_REF;   /* scale for custom W100 */
        const float t2 = temp * temp;
        const float t4 = t2 * t2;
        const float t6 = t4 * t2;
        const float w  = 1.0f + k * (NI_A * temp + NI_B * t2 + NI_C * t4 + NI_D * t6);
        return t->r0 * w;
    }
    default:
        return t->r0;
    }
}

/* ---------------------------------------------------------------------------
 * Numerical inversion (bisection).
 * ------------------------------------------------------------------------- */
float rtd_resistance_to_temperature(uint8_t type, float w100, float resistance)
{
    const rtd_type_info_t* t = rtd_type_info(type);
    if (t == NULL || t->material == RTD_MAT_RES) {
        return NAN;
    }
    if (w100 <= 1.0f) {
        w100 = t->w100_default;
    }

    float lo, hi;
    switch (t->material) {
    case RTD_MAT_PT: lo = -200.0f; hi = 850.0f; break;
    case RTD_MAT_CU: lo = -180.0f; hi = 200.0f; break;
    case RTD_MAT_NI: lo =  -60.0f; hi = 250.0f; break;
    default:         return NAN;
    }

    const float r_lo = forward_resistance(t, w100, lo);
    const float r_hi = forward_resistance(t, w100, hi);
    if (resistance <= r_lo) { return lo; }
    if (resistance >= r_hi) { return hi; }

    for (int i = 0; i < 60; i++) {
        const float mid = 0.5f * (lo + hi);
        const float r_mid = forward_resistance(t, w100, mid);
        if (r_mid < resistance) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return 0.5f * (lo + hi);
}
