/**
  ******************************************************************************
  * @file    modbus_app.h
  * @brief   Modbus register-map adapter for the 4RTD analog input module.
  *
  *  Float values are IEEE-754 32-bit, transmitted as two 16-bit registers with
  *  the HIGH word first (big-endian word order: register[N] = bits 31..16).
  *
  *  ---- Holding Registers (FC03/06/16) — compact per-channel block --------
  *    Grouped by quantity, 4 registers (channels 0..3) per group:
  *      0..3    reading, int16 (RO):
  *                RTD types      temperature °C × 100  (−327.68 … +327.67)
  *                R 0..200 / 2k  0..32767 scaled from the mode full scale
  *                disabled = 0,  fault / not yet valid = −32768 (0x8000)
  *      4..7    sensor type (rtd_type_t: 0..18 RTD scales, 19 R200, 20 R2k)
  *      8..11   enabled (0/1)
  *      12..15  alpha mode (0 default / 1 custom)
  *      16..19  custom W100 × 10000
  *      20..23  gain-class override (0 auto, 1..5 = force class 0..4; runtime)
  *      24..27  smoothing (EMA): 0 off, 1 weak (1/4), 2 medium (1/8), 3 (1/16)
  *
  *  ---- Input Registers (FC04, read-only) — grouped by quantity ----------
  *      300..307 temperature, float32 °C ×4      (NaN: resistance mode / fault)
  *      308..315 calibrated resistance, float32 Ω ×4
  *      316..323 raw (uncalibrated) resistance, float32 Ω ×4   (calibration input)
  *      324..327 status flags ×4: bit0 enabled, bit1 valid, bit2 fault,
  *                 bits15..8 fault code (1 open/over-range, 2 short, 3 ADC dead,
  *                 4 reversed: negative code, excitation not flowing S+ -> S-).
  *                 Faults are debounced: raised after 3 consecutive faulty scans,
  *                 cleared (valid again) after 3 consecutive good ones.
  *      328..335 ADC code, int32 (24-bit signed, high word first) ×4
  *      336..339 active gain class ×4 (0..4)
  *    Global:
  *      120 fw major, 121 fw minor, 122/123 uptime s (lo/hi),
  *      125 module id, 126 on-chip temperature (signed 0.1 °C)
  *      127 calibration lock bitmask bits 0..15, 128 bits 16..19
  *          (bit = CALSTORE_SLOT(ch, class) = ch*5 + class)
  *
  *  ---- Holding Registers (FC03/06/16) — global -----------------------------
  *      100 RTD scan period ms (50..5000)      101 LED mode (0/1/2)
  *      102 Modbus slave id                    103 Modbus TCP port
  *      104..107 static IP octets              108..111 netmask octets
  *      112..115 gateway octets                116 net mode (0 static/1 DHCP/2 LL)
  *      117 SAVE trigger (0xA5A5)              118 REBOOT (0xB00B) / BOOT (0xB007)
  *                                                 / KSZ8863 reset (0x8863)
  *      119 FACTORY RESET trigger (0xDEAD)     130 on-chip temperature (RO)
  *      131 CAL COMMIT trigger (0xCA00|slot)   132 CAL ERASE ARM (0xC1A5)
  *    Calibration coefficients (float32), base 540 + ch*20 (WRITE-ONCE):
  *      + class*4 + 0..1 gain,  + class*4 + 2..3 offset   (class 0..4)
  *      Writes are a live preview and are rejected once the (channel, class)
  *      slot is committed. Commit = write 0xCA00|(ch*5+class) to register 131.
  *    Nominal reference resistor (float32 Ω): 620..621
  ******************************************************************************
  */
#ifndef APPLICATION_MODBUS_APP_H
#define APPLICATION_MODBUS_APP_H

#include <stdbool.h>
#include <stdint.h>

#include "nanomodbus.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Magic write triggers (FC06/FC10 to specific holding registers). */
#define MODBUS_TRIG_SAVE            0xA5A5u
#define MODBUS_TRIG_REBOOT          0xB00Bu
#define MODBUS_TRIG_FACTORY_RESET   0xDEADu
#define MODBUS_TRIG_BOOTLOADER      0xB007u
/* Hardware-reset the KSZ8863 Ethernet switch (operator recovery command).
 * Written to MB_HR_TRIG_REBOOT (118). Interrupts pass-through traffic for
 * the duration of the chip reset + port auto-negotiation — use only when the
 * switch shows no signs of life. */
#define MODBUS_TRIG_SWITCH_RESET    0x8863u

/* Calibration commit: value = MB_CAL_COMMIT_BASE | (ch*5 + class), slot 0..19. */
#define MB_CAL_COMMIT_BASE          0xCA00u
#define MB_CAL_COMMIT_SLOT_MASK     0x00FFu

/* Emergency calibration-erase arming magic (two-factor: this + button). */
#define MODBUS_TRIG_CAL_ERASE_ARM   0xC1A5u

/* Window after arming during which a physical button confirm erases the
 * calibration sector, and the button hold required to confirm (ms). */
#define CAL_ERASE_ARM_WINDOW_MS     30000u
#define CAL_ERASE_CONFIRM_MS        3000u

/* No-init RAM cell shared with the bootloader. */
#define BOOT_REQUEST_FLAG_ADDR      0x2001FFF0u
#define BOOT_REQUEST_MAGIC          0xB007CAFEu

/* ---- Global holding registers ---- */
#define MB_HR_RTD_SCAN_MS           100u
#define MB_HR_LED_MODE              101u
#define MB_HR_SLAVE_ID              102u
#define MB_HR_TCP_PORT              103u
#define MB_HR_IP_BASE               104u
#define MB_HR_NETMASK_BASE          108u
#define MB_HR_GATEWAY_BASE          112u
#define MB_HR_USE_DHCP              116u
#define MB_HR_TRIG_SAVE             117u
#define MB_HR_TRIG_REBOOT           118u
#define MB_HR_TRIG_FACTORY_RESET    119u
#define MB_HR_TEMPERATURE           130u
#define MB_HR_CAL_COMMIT            131u
#define MB_HR_CAL_ERASE_ARM        132u

/* ---- Global input registers ---- */
#define MB_IR_FW_VER_MAJOR          120u
#define MB_IR_FW_VER_MINOR          121u
#define MB_IR_UPTIME_LO             122u
#define MB_IR_UPTIME_HI             123u
#define MB_IR_MODULE_ID             125u
#define MB_IR_TEMPERATURE           126u
#define MB_IR_CAL_LOCK              127u  /* bits 0..15  */
#define MB_IR_CAL_LOCK_HI           128u  /* bits 16..19 */

#define MB_RTD_CHANNELS             4u
#define MB_RTD_GCLASSES             5u

/* ---- Compact per-channel block (holding): address = group*4 + ch ---- */
#define MB_HR_CH_BASE               0u
#define MB_HR_CH_GROUP_READING      0u    /* int16, read-only */
#define MB_HR_CH_GROUP_TYPE         1u
#define MB_HR_CH_GROUP_ENABLED      2u
#define MB_HR_CH_GROUP_ALPHA_MODE   3u
#define MB_HR_CH_GROUP_W100         4u
#define MB_HR_CH_GROUP_CALOVR       5u
#define MB_HR_CH_GROUP_SMOOTH       6u
#define MB_HR_CH_GROUPS             7u    /* registers 0..27 */

/* ---- RTD readings (input registers), grouped by quantity ---- */
#define MB_IR_RTD_BASE              300u
#define MB_IR_RTD_TEMP              300u  /* float32 ×4 -> 300..307 */
#define MB_IR_RTD_RCAL              308u  /* float32 ×4 -> 308..315 */
#define MB_IR_RTD_RRAW              316u  /* float32 ×4 -> 316..323 */
#define MB_IR_RTD_FLAGS             324u  /* u16 ×4     -> 324..327 */
#define MB_IR_RTD_CODE              328u  /* int32 ×4   -> 328..335 */
#define MB_IR_RTD_GCLASS            336u  /* u16 ×4     -> 336..339 */
#define MB_IR_RTD_END               340u  /* first address past the block */

/* Reading flag bits (registers 324..327). */
#define MB_RTD_FLAG_ENABLED         0x0001u
#define MB_RTD_FLAG_VALID           0x0002u
#define MB_RTD_FLAG_FAULT           0x0004u

/* ---- RTD calibration coefficients (holding, float32) ---- */
#define MB_HR_RTD_CAL_BASE          540u
#define MB_HR_RTD_CAL_STRIDE        20u   /* 5 classes × (gain, offset) floats */

/* ---- Nominal reference resistor (holding, float32) ---- */
#define MB_HR_RREF_BASE             620u  /* 620..621 */

/* Module ID (input register 125). */
#define MODULE_ID_04RTD             0x04D1u

/** Initialise the modbus register adapter. Must be called after settings_init(). */
void modbus_app_init(void);

/** Mark a successful modbus transaction (used to drive STAT_LED state). */
void modbus_app_notify_request(void);

/** Get the populated nmbs_callbacks structure for nmbs_server_create(). */
const nmbs_callbacks* modbus_app_get_callbacks(void);

/**
 * Returns 1 if an emergency calibration erase has been armed over Modbus and
 * the arming window (CAL_ERASE_ARM_WINDOW_MS) has not yet expired. Used by the
 * application loop to gate the physical button confirmation. Non-destructive.
 */
uint8_t modbus_app_cal_erase_armed(void);

/** Clear the armed calibration-erase state (e.g. after the action completes). */
void modbus_app_clear_cal_erase_arm(void);

#ifdef __cplusplus
}
#endif

#endif /* APPLICATION_MODBUS_APP_H */
