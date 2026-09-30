/**
  ******************************************************************************
  * @file    ads1220.h
  * @brief   Driver for four TI ADS1220 24-bit delta-sigma ADCs on a shared
  *          SPI1 bus with per-channel chip-select (one ADS1220 per RTD input).
  *
  *  Board wiring (HW2.1, identical for all four channels, 3-wire RTD with two
  *  matched excitation currents — the TI reference topology):
  *
  *    RTD lead 1 (S+) -> AIN2 (IDAC1 output)  and via 10k -> AIN0 (AINP)
  *    RTD lead 2 (S-) -> AIN3 (IDAC2 output)  and via 10k -> AIN1 (AINN)
  *    RTD lead 3 (E)  -> RREF (2 kOhm, 0.1 %) -> GND;  REFP0/REFN0 across RREF
  *
  *  Both IDACs return through lead 3 and RREF, so V_REF = 2*I*RREF and the
  *  differential input is I*R_RTD (lead resistances cancel when I1 = I2):
  *
  *      R_RTD = code / 2^23 * (2 * RREF) / gain
  *
  *  The measuring range is selected purely by the PGA gain (no external range
  *  switch on this board). Operating configuration:
  *    - MUX AIN0/AIN1, PGA enabled, gain per channel (1..16)
  *    - 20 SPS, normal mode, continuous conversion, simultaneous 50/60 Hz FIR
  *    - external reference REFP0/REFN0, IDAC1/IDAC2 = 500 uA -> AIN2 / AIN3
  *    - DRDY pin is not wired: data are read with the RDATA command from the
  *      periodic RTD scan (>= 50 ms), which is slower than the 20 SPS rate.
  ******************************************************************************
  */
#ifndef APPLICATION_ADS1220_H
#define APPLICATION_ADS1220_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ADS1220_CHANNEL_COUNT       4u

/* SPI commands (Table 14). */
#define ADS1220_CMD_RESET           0x06u
#define ADS1220_CMD_START           0x08u
#define ADS1220_CMD_POWERDOWN       0x02u
#define ADS1220_CMD_RDATA           0x10u
#define ADS1220_CMD_RREG            0x20u   /* | (reg << 2) | (count - 1) */
#define ADS1220_CMD_WREG            0x40u   /* | (reg << 2) | (count - 1) */

/* Config register 0: MUX[7:4] GAIN[3:1] PGA_BYPASS[0]. */
#define ADS1220_MUX_AIN0_AIN1       (0x0u << 4)
#define ADS1220_MUX_REF_MON         (0xCu << 4)   /* (V(REFPx) - V(REFNx)) / 4, system monitor */
#define ADS1220_GAIN_1              (0x0u << 1)
#define ADS1220_GAIN_2              (0x1u << 1)
#define ADS1220_GAIN_4              (0x2u << 1)
#define ADS1220_GAIN_8              (0x3u << 1)
#define ADS1220_GAIN_16             (0x4u << 1)
#define ADS1220_GAIN_32             (0x5u << 1)
#define ADS1220_GAIN_64             (0x6u << 1)
#define ADS1220_GAIN_128            (0x7u << 1)
#define ADS1220_PGA_BYPASS          0x01u

/* Config register 1: DR[7:5] MODE[4:3] CM[2] TS[1] BCS[0]. */
#define ADS1220_DR_20SPS            (0x0u << 5)
#define ADS1220_MODE_NORMAL         (0x0u << 3)
#define ADS1220_CM_CONTINUOUS       0x04u
#define ADS1220_TS_ENABLE           0x02u
#define ADS1220_BCS_ENABLE          0x01u

/* Config register 2: VREF[7:6] 50/60[5:4] PSW[3] IDAC[2:0]. */
#define ADS1220_VREF_EXT_REF0       (0x1u << 6)   /* REFP0 / REFN0 */
#define ADS1220_FIR_50_60HZ         (0x1u << 4)
#define ADS1220_FIR_50HZ            (0x2u << 4)
#define ADS1220_PSW_AUTO            0x08u
#define ADS1220_IDAC_OFF            0x0u
#define ADS1220_IDAC_250UA          0x4u
#define ADS1220_IDAC_500UA          0x5u
#define ADS1220_IDAC_1000UA         0x6u

/* Config register 3: I1MUX[7:5] I2MUX[4:2] DRDYM[1]. */
#define ADS1220_I1MUX_AIN2          (0x3u << 5)
#define ADS1220_I2MUX_AIN3          (0x4u << 2)
#define ADS1220_DRDYM               0x02u

/* Operating configuration used on this board (see file header). */
#define ADS1220_CFG1_DEFAULT        (ADS1220_DR_20SPS | ADS1220_MODE_NORMAL | ADS1220_CM_CONTINUOUS)
#define ADS1220_CFG2_DEFAULT        (ADS1220_VREF_EXT_REF0 | ADS1220_FIR_50_60HZ | ADS1220_IDAC_500UA)
#define ADS1220_CFG3_DEFAULT        (ADS1220_I1MUX_AIN2 | ADS1220_I2MUX_AIN3)

/* Full-scale code of the 24-bit two's-complement result. */
#define ADS1220_FULL_SCALE          8388608.0f   /* 2^23 */
#define ADS1220_CODE_MAX            8388607     /* 0x7FFFFF */

/** Bring the SPI bus up and reset + configure all four converters with the
 *  given per-channel gain codes (ADS1220_GAIN_*). Starts continuous conversion. */
void ads1220_init(const uint8_t gain_code[ADS1220_CHANNEL_COUNT]);

/** Re-write config register 0 with a new gain (ADS1220_GAIN_*) and restart
 *  conversion. The next results need a settle period (see rtd_module). */
void ads1220_set_gain(uint8_t ch, uint8_t gain_code);

/** Switch the channel to the excitation-loop monitor: MUX = (REFPx-REFNx)/4.
 *  The device converts the monitor against its internal 2.048 V reference
 *  automatically; VREF must stay = REF0 because it selects WHICH reference
 *  pair is monitored (with VREF = internal the monitor reads 0 — verified on
 *  hardware). IDACs keep running. A healthy loop (2·I·RREF ≈ 2.0 V) reads
 *  ≈ 0.244 FS; an open sensor (no current through RREF) reads ≈ 0. Restore
 *  with ads1220_set_gain(). */
void ads1220_enter_ref_monitor(uint8_t ch);

/**
 * Read the latest conversion result with the RDATA command.
 * @param ch        channel 0..3
 * @param code_out  receives the sign-extended 24-bit code
 * @return false if the channel is out of range or the bus looks dead (the
 *         readback of config register 0 does not match what was written).
 */
bool ads1220_read_data(uint8_t ch, int32_t* code_out);

/** Read back config register @p reg (0..3), for diagnostics. */
uint8_t ads1220_read_reg(uint8_t ch, uint8_t reg);

#ifdef __cplusplus
}
#endif

#endif /* APPLICATION_ADS1220_H */
