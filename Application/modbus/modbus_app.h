/**
  ******************************************************************************
  * @file    modbus_app.h
  * @brief   Modbus register-map adapter for the 4RTD analog input module.
  *
  *  Float values are IEEE-754 32-bit, transmitted as two 16-bit registers with
  *  the HIGH word first (big-endian word order: register[N] = bits 31..16).
  *
  *  ---- Input Registers (FC04, read-only) --------------------------------
  *    Per channel, base 300 + ch*20 (ch = 0..3):
  *      +0..1   temperature, float32 °C   (NaN for resistance modes / fault)
  *      +2..3   calibrated resistance, float32 Ω
  *      +4..5   raw (uncalibrated) resistance, float32 Ω   (calibration input)
  *      +6      status flags: bit0 enabled, bit1 valid, bit2 fault,
  *                            bits15..8 MAX31865 fault-status byte
  *      +7      raw 15-bit ADC code
  *      +8      resolved range (0 = low RREF, 1 = high RREF)
  *    Global:
  *      120 fw major, 121 fw minor, 122/123 uptime s (lo/hi),
  *      125 module id, 126 on-chip temperature (signed 0.1 °C)
  *
  *  ---- Holding Registers (FC03/06/16, read/write) -----------------------
  *    Global:
  *      100 RTD scan period ms (50..5000)      101 LED mode (0/1/2)
  *      102 Modbus slave id                    103 Modbus TCP port
  *      104..107 static IP octets              108..111 netmask octets
  *      112..115 gateway octets                116 use DHCP (0/1)
  *      117 SAVE trigger (0xA5A5)              118 REBOOT (0xB00B) / BOOT (0xB007)
  *      119 FACTORY RESET trigger (0xDEAD)     130 on-chip temperature (RO)
  *      131 CAL COMMIT trigger (0xCA00|slot)   132 CAL ERASE ARM (0xC1A5)
  *    Per channel, base 500 + ch*10:
  *      +0 enabled (0/1)      +1 sensor type (rtd_type_t)
  *      +2 alpha mode (0 default / 1 custom)   +3 custom W100 ×10000
  *      +4 calibration range override (0 auto / 1 low / 2 high, runtime only)
  *    Calibration coefficients (float32), base 540 + ch*8 (WRITE-ONCE):
  *      +0..1 gain low range     +2..3 offset low range
  *      +4..5 gain high range    +6..7 offset high range
  *      Writes are a live preview and are rejected once the (channel,range)
  *      slot is committed. Commit = write 0xCA00|(ch*2+range) to register 131.
  *    Nominal reference resistors (float32 Ω):
  *      580..581 RREF low        582..583 RREF high
  *  ---- Input Registers (extra) ------------------------------------------
  *      127 calibration lock bitmask (RO): bit (ch*2+range) = slot committed
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

/* Calibration commit: value = MB_CAL_COMMIT_BASE | (ch*2 + range), slot 0..7. */
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
#define MB_IR_CAL_LOCK              127u

/* ---- RTD readings (input registers) ---- */
#define MB_RTD_CHANNELS             4u
#define MB_IR_RTD_BASE              300u
#define MB_IR_RTD_STRIDE            20u
#define MB_IR_RTD_TEMP_OFF          0u    /* float32 */
#define MB_IR_RTD_RCAL_OFF          2u    /* float32 */
#define MB_IR_RTD_RRAW_OFF          4u    /* float32 */
#define MB_IR_RTD_FLAGS_OFF         6u
#define MB_IR_RTD_CODE_OFF          7u
#define MB_IR_RTD_RANGE_OFF         8u
#define MB_IR_RTD_SPAN              9u    /* used registers per channel */

/* Reading flag bits (register base+6). */
#define MB_RTD_FLAG_ENABLED         0x0001u
#define MB_RTD_FLAG_VALID           0x0002u
#define MB_RTD_FLAG_FAULT           0x0004u

/* ---- RTD config (holding registers) ---- */
#define MB_HR_RTD_CFG_BASE          500u
#define MB_HR_RTD_CFG_STRIDE        10u
#define MB_HR_RTD_CFG_ENABLED       0u
#define MB_HR_RTD_CFG_TYPE          1u
#define MB_HR_RTD_CFG_ALPHA_MODE    2u
#define MB_HR_RTD_CFG_W100          3u
#define MB_HR_RTD_CFG_CALRANGE      4u
#define MB_HR_RTD_CFG_SPAN          5u

/* ---- RTD calibration coefficients (holding, float32) ---- */
#define MB_HR_RTD_CAL_BASE          540u
#define MB_HR_RTD_CAL_STRIDE        8u    /* 4 floats per channel */

/* ---- Nominal reference resistors (holding, float32) ---- */
#define MB_HR_RREF_BASE             580u  /* low: 580..581, high: 582..583 */

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
