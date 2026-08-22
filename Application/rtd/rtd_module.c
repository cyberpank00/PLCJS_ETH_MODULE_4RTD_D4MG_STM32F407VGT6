/**
  ******************************************************************************
  * @file    rtd_module.c
  * @brief   Four-channel RTD acquisition (see rtd_module.h).
  ******************************************************************************
  */

#include "rtd_module.h"

#include <math.h>

#include "calstore.h"
#include "main.h"
#include "max31865.h"
#include "rtd_scales.h"
#include "settings.h"
#include "stm32f4xx_hal.h"

/* MAX31865 operating configuration (mirror of the driver default). */
#define RTD_MAX_CONFIG \
    (MAX31865_CFG_VBIAS | MAX31865_CFG_CONV_AUTO | MAX31865_CFG_3WIRE | MAX31865_CFG_FILT_50HZ)

/* MAX31865 full-scale code (15-bit ratiometric). */
#define RTD_ADC_FULL_SCALE      32768.0f

/* Ticks (scan cycles) to ignore validity after a RANG range change. */
#define RTD_SETTLE_TICKS        3u

/* RANG pin level that selects the HIGH reference-resistor range. If the board
 * turns out to have the opposite polarity, the calibration will read ~10x off
 * and this single constant is the only thing to flip. */
#define RTD_RANG_LEVEL_HIGH     GPIO_PIN_SET

/* Channel status LED blink half-period on fault, ms. */
#define RTD_FAULT_BLINK_MS      100u

typedef struct {
    GPIO_TypeDef* port;
    uint16_t      pin;
} gpio_ref_t;

static const gpio_ref_t s_rang[RTD_MODULE_CHANNEL_COUNT] = {
    { RTD0_RANG_GPIO_Port, RTD0_RANG_Pin },
    { RTD1_RANG_GPIO_Port, RTD1_RANG_Pin },
    { RTD2_RANG_GPIO_Port, RTD2_RANG_Pin },
    { RTD3_RANG_GPIO_Port, RTD3_RANG_Pin },
};

static const gpio_ref_t s_stat[RTD_MODULE_CHANNEL_COUNT] = {
    { RTD0_STAT_GPIO_Port, RTD0_STAT_Pin },
    { RTD1_STAT_GPIO_Port, RTD1_STAT_Pin },
    { RTD2_STAT_GPIO_Port, RTD2_STAT_Pin },
    { RTD3_STAT_GPIO_Port, RTD3_STAT_Pin },
};

/* Runtime, derived from settings on rtd_module_apply_config(). */
static rtd_channel_status_t s_status[RTD_MODULE_CHANNEL_COUNT];
static uint8_t  s_type[RTD_MODULE_CHANNEL_COUNT];
static bool     s_enabled[RTD_MODULE_CHANNEL_COUNT];
static float    s_w100[RTD_MODULE_CHANNEL_COUNT];
static uint8_t  s_range[RTD_MODULE_CHANNEL_COUNT];
static uint8_t  s_cal_override[RTD_MODULE_CHANNEL_COUNT];
static uint8_t  s_settle[RTD_MODULE_CHANNEL_COUNT];
static uint16_t s_scan_ms = SETTINGS_DEF_SCAN_MS;

/* Software smoothing (EMA): y += alpha * (x - y), alpha = 1 / 2^(level+1),
 * i.e. level 1/2/3 => 1/4, 1/8, 1/16. Applied to the calibrated resistance.
 * The filter state is seeded with the first valid sample and reset on fault,
 * settle and any configuration change, so it never mixes readings taken with
 * different ranges/types or drags a stale value after a sensor fault. */
static uint8_t  s_smooth[RTD_MODULE_CHANNEL_COUNT];
static float    s_ema[RTD_MODULE_CHANNEL_COUNT];
static bool     s_ema_seeded[RTD_MODULE_CHANNEL_COUNT];

/* LED blink state. */
static uint16_t s_blink_timer;
static uint8_t  s_blink_on;

static inline void rang_set(uint8_t ch, uint8_t range)
{
    const GPIO_PinState lvl = (range == RTD_RANGE_HIGH)
                                ? RTD_RANG_LEVEL_HIGH
                                : ((RTD_RANG_LEVEL_HIGH == GPIO_PIN_SET) ? GPIO_PIN_RESET : GPIO_PIN_SET);
    HAL_GPIO_WritePin(s_rang[ch].port, s_rang[ch].pin, lvl);
}

static inline void stat_set(uint8_t ch, bool on)
{
    HAL_GPIO_WritePin(s_stat[ch].port, s_stat[ch].pin,
                      on ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

void rtd_module_init(void)
{
    max31865_init();

    for (uint8_t ch = 0; ch < RTD_MODULE_CHANNEL_COUNT; ch++) {
        s_cal_override[ch] = RTD_CAL_OVERRIDE_AUTO;
        s_range[ch]        = 0xFFu;   /* force RANG write on first apply */
        s_settle[ch]       = RTD_SETTLE_TICKS;
        s_status[ch].temperature = NAN;
        s_status[ch].r_raw = NAN;
        s_status[ch].r_cal = NAN;
        stat_set(ch, false);
    }

    rtd_module_apply_config();
}

static uint8_t resolve_range(uint8_t ch)
{
    switch (s_cal_override[ch]) {
    case RTD_CAL_OVERRIDE_LOW:  return RTD_RANGE_LOW;
    case RTD_CAL_OVERRIDE_HIGH: return RTD_RANGE_HIGH;
    default:                    return rtd_type_range(s_type[ch]);
    }
}

void rtd_module_apply_config(void)
{
    const settings_t* s = settings_get();

    s_scan_ms = s->rtd_scan_ms;
    if (s_scan_ms < SETTINGS_SCAN_MS_MIN) { s_scan_ms = SETTINGS_SCAN_MS_MIN; }
    if (s_scan_ms > SETTINGS_SCAN_MS_MAX) { s_scan_ms = SETTINGS_SCAN_MS_MAX; }

    for (uint8_t ch = 0; ch < RTD_MODULE_CHANNEL_COUNT; ch++) {
        s_enabled[ch] = (s->ch_enabled[ch] != 0u);

        uint8_t type = s->ch_type[ch];
        if (type >= RTD_TYPE_COUNT) { type = SETTINGS_DEF_CH_TYPE; }
        s_type[ch] = type;

        const uint16_t w100_x = (s->ch_alpha_mode[ch] != 0u)
                                    ? s->ch_custom_w100[ch]
                                    : rtd_type_default_w100_x10000(type);
        s_w100[ch] = (float)w100_x / 10000.0f;

        uint8_t smooth = s->ch_smooth[ch];
        if (smooth > SETTINGS_SMOOTH_MAX) { smooth = SETTINGS_SMOOTH_OFF; }
        s_smooth[ch] = smooth;

        const uint8_t range = resolve_range(ch);
        if (range != s_range[ch]) {
            rang_set(ch, range);
            s_range[ch]  = range;
            s_settle[ch] = RTD_SETTLE_TICKS;
        }

        /* Any (re)configuration restarts the filter: the next valid sample
         * seeds it, avoiding a slow crawl from a value taken under the old
         * configuration. */
        s_ema_seeded[ch] = false;
    }
}

void rtd_module_tick(void)
{
    const settings_t* s = settings_get();

    for (uint8_t ch = 0; ch < RTD_MODULE_CHANNEL_COUNT; ch++) {
        rtd_channel_status_t* st = &s_status[ch];
        const uint8_t range = s_range[ch];

        st->enabled = s_enabled[ch];
        st->range   = range;

        uint16_t code = 0u;
        const bool fault = max31865_read_rtd(ch, &code);
        st->adc_code = code;

        if (fault) {
            st->fault      = true;
            st->fault_code = max31865_read_fault(ch);
            max31865_clear_fault(ch, RTD_MAX_CONFIG);
            st->valid        = false;
            st->r_raw        = NAN;
            st->r_cal        = NAN;
            st->temperature  = NAN;
            s_ema_seeded[ch] = false;   /* restart smoothing after the fault */
            continue;
        }

        st->fault      = false;
        st->fault_code = 0u;

        const float ratio = (float)code / RTD_ADC_FULL_SCALE;
        const float rref  = s->rref_nominal[range];
        const float r_raw = ratio * rref;
        float       r_cal = calstore_gain(ch, range) * r_raw + calstore_offset(ch, range);

        if (s_settle[ch] > 0u) {
            s_settle[ch]--;
            st->valid        = false;
            s_ema_seeded[ch] = false;   /* do not feed settling samples in    */
        } else {
            st->valid = true;

            /* Software smoothing (EMA) of the calibrated resistance; the raw
             * resistance and the ADC code stay unfiltered for diagnostics. */
            if (s_smooth[ch] != SETTINGS_SMOOTH_OFF) {
                if (!s_ema_seeded[ch]) {
                    s_ema[ch]        = r_cal;
                    s_ema_seeded[ch] = true;
                } else {
                    const float alpha = 1.0f / (float)(1u << (s_smooth[ch] + 1u));
                    s_ema[ch] += alpha * (r_cal - s_ema[ch]);
                }
                r_cal = s_ema[ch];
            }
        }

        st->r_raw = r_raw;
        st->r_cal = r_cal;

        /* Temperature is only meaningful for RTD scales in normal (non-cal)
         * operation. Resistance modes and calibration overrides report NaN. */
        if (s_cal_override[ch] != RTD_CAL_OVERRIDE_AUTO ||
            rtd_type_is_resistance(s_type[ch])) {
            st->temperature = NAN;
        } else {
            st->temperature = rtd_resistance_to_temperature(s_type[ch], s_w100[ch], r_cal);
        }
    }
}

void rtd_module_led_tick(uint16_t period_ms)
{
    s_blink_timer = (uint16_t)(s_blink_timer + period_ms);
    if (s_blink_timer >= RTD_FAULT_BLINK_MS) {
        s_blink_timer = 0u;
        s_blink_on    = (uint8_t)(!s_blink_on);
    }

    for (uint8_t ch = 0; ch < RTD_MODULE_CHANNEL_COUNT; ch++) {
        if (!s_enabled[ch]) {
            stat_set(ch, false);
        } else if (s_status[ch].fault) {
            stat_set(ch, s_blink_on != 0u);
        } else {
            stat_set(ch, true);
        }
    }
}

const rtd_channel_status_t* rtd_module_get_status(uint8_t ch)
{
    return (ch < RTD_MODULE_CHANNEL_COUNT) ? &s_status[ch] : NULL;
}

uint16_t rtd_module_scan_period_ms(void)
{
    return s_scan_ms;
}

void rtd_module_set_cal_override(uint8_t ch, uint8_t override)
{
    if (ch >= RTD_MODULE_CHANNEL_COUNT || override > RTD_CAL_OVERRIDE_HIGH) {
        return;
    }
    s_cal_override[ch] = override;
    rtd_module_apply_config();   /* re-resolve range / RANG pin */
}

uint8_t rtd_module_get_cal_override(uint8_t ch)
{
    return (ch < RTD_MODULE_CHANNEL_COUNT) ? s_cal_override[ch] : 0u;
}
