/**
  ******************************************************************************
  * @file    rtd_module.c
  * @brief   Four-channel RTD acquisition (see rtd_module.h).
  ******************************************************************************
  */

#include "rtd_module.h"

#include <math.h>
#include <stddef.h>

#include "ads1220.h"
#include "calstore.h"
#include "main.h"
#include "rtd_scales.h"
#include "settings.h"
#include "stm32f4xx_hal.h"

/* Ticks (scan cycles) to ignore validity after a gain change / (re)start. */
#define RTD_SETTLE_TICKS        3u

/* Codes at or above this are treated as an open sensor / over-range: the
 * IDAC has no return path, AIN0 is pulled to the supply and the PGA clips. */
#define RTD_CODE_OPEN           0x7FF000

/* An RTD reading below this fraction of R0 is a shorted sensor (Pt at −200 °C
 * is still ~0.185·R0, copper at −180 °C ~0.22·R0). */
#define RTD_SHORT_FRACTION      0.10f

/* Channel status LED blink half-period on fault, ms. */
#define RTD_FAULT_BLINK_MS      100u

typedef struct {
    GPIO_TypeDef* port;
    uint16_t      pin;
} gpio_ref_t;

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
static uint8_t  s_gclass[RTD_MODULE_CHANNEL_COUNT];
static uint8_t  s_cal_override[RTD_MODULE_CHANNEL_COUNT];
static uint8_t  s_settle[RTD_MODULE_CHANNEL_COUNT];
static uint16_t s_scan_ms = SETTINGS_DEF_SCAN_MS;

/* Software smoothing (EMA): y += alpha * (x - y), alpha = 1 / 2^(level+1),
 * i.e. level 1/2/3 => 1/4, 1/8, 1/16. Applied to the calibrated resistance.
 * The filter state is seeded with the first valid sample and reset on fault,
 * settle and any configuration change, so it never mixes readings taken with
 * different gains/types or drags a stale value after a sensor fault. */
static uint8_t  s_smooth[RTD_MODULE_CHANNEL_COUNT];
static float    s_ema[RTD_MODULE_CHANNEL_COUNT];
static bool     s_ema_seeded[RTD_MODULE_CHANNEL_COUNT];

/* LED blink state. */
static uint16_t s_blink_timer;
static uint8_t  s_blink_on;

static inline void stat_set(uint8_t ch, bool on)
{
    HAL_GPIO_WritePin(s_stat[ch].port, s_stat[ch].pin,
                      on ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static uint8_t resolve_gclass(uint8_t ch)
{
    const uint8_t ov = s_cal_override[ch];
    if (ov >= 1u && ov <= RTD_CAL_OVERRIDE_MAX) {
        return (uint8_t)(ov - 1u);
    }
    return rtd_type_gclass(s_type[ch]);
}

static void load_settings(void)
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
    }
}

void rtd_module_init(void)
{
    uint8_t gain_code[RTD_MODULE_CHANNEL_COUNT];

    for (uint8_t ch = 0; ch < RTD_MODULE_CHANNEL_COUNT; ch++) {
        s_cal_override[ch] = RTD_CAL_OVERRIDE_AUTO;
        s_status[ch].temperature = NAN;
        s_status[ch].r_raw = NAN;
        s_status[ch].r_cal = NAN;
        stat_set(ch, false);
    }

    load_settings();
    for (uint8_t ch = 0; ch < RTD_MODULE_CHANNEL_COUNT; ch++) {
        s_gclass[ch]     = resolve_gclass(ch);
        s_settle[ch]     = RTD_SETTLE_TICKS;
        s_ema_seeded[ch] = false;
        gain_code[ch]    = rtd_gclass_gain_code(s_gclass[ch]);
    }

    ads1220_init(gain_code);
}

void rtd_module_apply_config(void)
{
    load_settings();

    for (uint8_t ch = 0; ch < RTD_MODULE_CHANNEL_COUNT; ch++) {
        const uint8_t gclass = resolve_gclass(ch);
        if (gclass != s_gclass[ch]) {
            ads1220_set_gain(ch, rtd_gclass_gain_code(gclass));
            s_gclass[ch] = gclass;
            s_settle[ch] = RTD_SETTLE_TICKS;
        }

        /* Any (re)configuration restarts the filter: the next valid sample
         * seeds it, avoiding a slow crawl from a value taken under the old
         * configuration. */
        s_ema_seeded[ch] = false;
    }
}

static void set_fault(rtd_channel_status_t* st, uint8_t ch, uint8_t code)
{
    st->fault        = true;
    st->fault_code   = code;
    st->valid        = false;
    st->r_raw        = NAN;
    st->r_cal        = NAN;
    st->temperature  = NAN;
    s_ema_seeded[ch] = false;   /* restart smoothing after the fault */
}

void rtd_module_tick(void)
{
    const settings_t* s = settings_get();

    for (uint8_t ch = 0; ch < RTD_MODULE_CHANNEL_COUNT; ch++) {
        rtd_channel_status_t* st = &s_status[ch];
        const uint8_t gclass = s_gclass[ch];

        st->enabled = s_enabled[ch];
        st->gclass  = gclass;

        if (!st->enabled) {
            st->fault       = false;
            st->fault_code  = RTD_FAULT_NONE;
            st->valid       = false;
            st->adc_code    = 0;
            st->r_raw       = NAN;
            st->r_cal       = NAN;
            st->temperature = NAN;
            s_ema_seeded[ch] = false;
            continue;
        }

        int32_t code = 0;
        const bool alive = ads1220_read_data(ch, &code);
        st->adc_code = code;

        if (!alive) {
            set_fault(st, ch, RTD_FAULT_ADC);
            continue;
        }
        if (code >= RTD_CODE_OPEN) {
            set_fault(st, ch, RTD_FAULT_OPEN);
            continue;
        }

        /* Ratiometric: V_in = I·R, V_ref = 2·I·RREF  =>  R = ratio·2·RREF/gain. */
        const float ratio = (float)code / ADS1220_FULL_SCALE;
        const float r_raw = ratio * 2.0f * s->rref_nominal / rtd_gclass_gain(gclass);
        float       r_cal = calstore_gain(ch, gclass) * r_raw + calstore_offset(ch, gclass);

        const rtd_type_info_t* ti = rtd_type_info(s_type[ch]);
        const bool is_res = (ti != NULL) && (ti->material == RTD_MAT_RES);
        if (!is_res && ti != NULL && r_cal < RTD_SHORT_FRACTION * ti->r0) {
            set_fault(st, ch, RTD_FAULT_SHORT);
            continue;
        }
        if (is_res && r_cal < 0.0f) {
            r_cal = 0.0f;   /* noise around a genuine 0 Ω input */
        }

        st->fault      = false;
        st->fault_code = RTD_FAULT_NONE;

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
        if (s_cal_override[ch] != RTD_CAL_OVERRIDE_AUTO || is_res) {
            st->temperature = NAN;
        } else {
            st->temperature = rtd_resistance_to_temperature(s_type[ch], s_w100[ch], r_cal);
        }
    }
}

int16_t rtd_module_int16_view(uint8_t ch)
{
    if (ch >= RTD_MODULE_CHANNEL_COUNT) { return RTD_I16_DISABLED; }
    const rtd_channel_status_t* st = &s_status[ch];

    if (!st->enabled) { return RTD_I16_DISABLED; }
    if (st->fault || !st->valid) { return RTD_I16_FAULT; }

    const rtd_type_info_t* ti = rtd_type_info(s_type[ch]);
    float v;
    if (ti != NULL && ti->material == RTD_MAT_RES) {
        v = st->r_cal / ti->r0 * 32768.0f;      /* r0 = mode full scale */
    } else {
        if (isnan(st->temperature)) { return RTD_I16_FAULT; }
        v = st->temperature * 100.0f;
    }
    if (v >  32767.0f) { v =  32767.0f; }
    if (v < -32767.0f) { v = -32767.0f; }
    return (int16_t)lroundf(v);
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
    if (ch >= RTD_MODULE_CHANNEL_COUNT || override > RTD_CAL_OVERRIDE_MAX) {
        return;
    }
    s_cal_override[ch] = override;
    rtd_module_apply_config();   /* re-resolve the gain class */
}

uint8_t rtd_module_get_cal_override(uint8_t ch)
{
    return (ch < RTD_MODULE_CHANNEL_COUNT) ? s_cal_override[ch] : 0u;
}
