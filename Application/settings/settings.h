/**
  ******************************************************************************
  * @file    settings.h
  * @brief   Persistent settings stored in internal Flash with CRC32 protection.
  *          4RTD variant: network + per-channel RTD configuration + per-channel
  *          per-range 2-point calibration coefficients.
  ******************************************************************************
  */
#ifndef APPLICATION_SETTINGS_H
#define APPLICATION_SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Magic and version --------------------------------------------------------- */
#define SETTINGS_MAGIC          0x04D14A57u
/* v2: calibration coefficients moved to the dedicated write-once calstore
 * (see calstore.h); cal_gain/cal_offset removed from this structure. */
#define SETTINGS_VERSION        2u

#define SETTINGS_RTD_CHANNELS   4u
#define SETTINGS_RTD_RANGES     2u

/* Defaults ------------------------------------------------------------------ */
#define SETTINGS_DEF_SCAN_MS       250u
#define SETTINGS_SCAN_MS_MIN       50u
#define SETTINGS_SCAN_MS_MAX       5000u

#define SETTINGS_DEF_LED_MODE       2u    /* STATE_MACHINE */

#define SETTINGS_DEF_SLAVE_ID       1u
#define SETTINGS_DEF_TCP_PORT       502u

#define SETTINGS_DEF_USE_DHCP       1u

#define SETTINGS_DEF_IP0            192u
#define SETTINGS_DEF_IP1            168u
#define SETTINGS_DEF_IP2            142u
#define SETTINGS_DEF_IP3            150u

#define SETTINGS_DEF_MASK0          255u
#define SETTINGS_DEF_MASK1          255u
#define SETTINGS_DEF_MASK2          255u
#define SETTINGS_DEF_MASK3          0u

#define SETTINGS_DEF_GW0            192u
#define SETTINGS_DEF_GW1            168u
#define SETTINGS_DEF_GW2            142u
#define SETTINGS_DEF_GW3            1u

/* Per-channel RTD defaults. */
#define SETTINGS_DEF_CH_TYPE        8u    /* RTD_TYPE_PT100 */

/* Nominal reference resistors (Ω) for the two RANG positions. These are only
 * starting estimates; the per-channel/per-range calibration removes the real
 * RREF error and the analog-switch on-resistance. */
#define SETTINGS_DEF_RREF_LOW       400.0f
#define SETTINGS_DEF_RREF_HIGH      4000.0f

/* LED mode codes ------------------------------------------------------------ */
typedef enum {
    LED_MODE_ALW_OFF       = 0,
    LED_MODE_ALW_ON        = 1,
    LED_MODE_STATE_MACHINE = 2,
} led_mode_t;

/**
 * Persistent settings structure. Layout is fixed and naturally aligned. Do not
 * reorder without bumping SETTINGS_VERSION.
 */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved0;

    uint16_t rtd_scan_ms;           /* full 4-channel scan period, ms         */
    uint16_t led_mode;              /* led_mode_t                             */

    uint16_t modbus_tcp_port;       /* default 502                            */
    uint8_t  modbus_slave_id;       /* default 1                              */
    uint8_t  use_dhcp;              /* 0 = static, 1 = DHCP                   */

    uint8_t  ip[4];
    uint8_t  netmask[4];
    uint8_t  gateway[4];

    /* Per-channel RTD configuration. */
    uint8_t  ch_enabled[SETTINGS_RTD_CHANNELS];
    uint8_t  ch_type[SETTINGS_RTD_CHANNELS];       /* rtd_type_t              */
    uint8_t  ch_alpha_mode[SETTINGS_RTD_CHANNELS]; /* 0 = default, 1 = custom */
    uint8_t  reserved_a[SETTINGS_RTD_CHANNELS];
    uint16_t ch_custom_w100[SETTINGS_RTD_CHANNELS];/* W100 × 10000            */

    /* NOTE: the 2-point linear calibration (R_true = gain·R_raw + offset) is
     * NOT stored here. It lives in the write-once calstore (calstore.h) in its
     * own Flash sector so it survives factory reset and can be locked. */

    /* Nominal RREF (Ω) per range (low, high). */
    float    rref_nominal[SETTINGS_RTD_RANGES];

    uint8_t  reserved1[16];

    uint32_t crc32;                 /* CRC32 over all preceding bytes         */
} settings_t;

/* API ----------------------------------------------------------------------- */

/**
 * Initialise the settings subsystem. Loads settings from Flash; if the stored
 * image is invalid the structure is filled with defaults.
 *
 * @return true if the stored image was valid, false if defaults were applied.
 */
bool settings_init(void);

/** Reload defaults into the in-memory settings (does not write to flash). */
void settings_reset_to_defaults(void);

/** Persist the current in-memory settings to internal Flash. */
bool settings_save(void);

/** Get a pointer to the live in-memory settings. */
settings_t* settings_get(void);

#ifdef __cplusplus
}
#endif

#endif /* APPLICATION_SETTINGS_H */
