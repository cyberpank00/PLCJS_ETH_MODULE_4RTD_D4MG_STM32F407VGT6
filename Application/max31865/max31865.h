/**
  ******************************************************************************
  * @file    max31865.h
  * @brief   Driver for four MAX31865 RTD-to-digital converters on a shared
  *          SPI1 bus with per-channel chip-select.
  *
  *  All sensors are wired in 3-wire mode on the board (a physical jumper adds
  *  the 2-wire option), so the driver always enables the 3-wire configuration
  *  bit. Conversions run continuously (auto mode) with VBIAS permanently on
  *  and the 50 Hz notch filter enabled; the RTD register is simply polled.
  ******************************************************************************
  */
#ifndef APPLICATION_MAX31865_H
#define APPLICATION_MAX31865_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX31865_CHANNEL_COUNT      4u

/* Register addresses (read address; OR 0x80 for writes). */
#define MAX31865_REG_CONFIG         0x00u
#define MAX31865_REG_RTD_MSB        0x01u
#define MAX31865_REG_RTD_LSB        0x02u
#define MAX31865_REG_HFAULT_MSB     0x03u
#define MAX31865_REG_LFAULT_MSB     0x05u
#define MAX31865_REG_FAULT_STATUS   0x07u

/* Configuration register bits. */
#define MAX31865_CFG_VBIAS          0x80u   /* bias voltage on                 */
#define MAX31865_CFG_CONV_AUTO      0x40u   /* automatic (continuous) mode     */
#define MAX31865_CFG_1SHOT          0x20u
#define MAX31865_CFG_3WIRE          0x10u   /* 3-wire RTD                      */
#define MAX31865_CFG_FAULT_CLEAR    0x02u
#define MAX31865_CFG_FILT_50HZ      0x01u   /* 50 Hz notch (0 => 60 Hz)        */

/* Fault-status register bits (register 0x07). */
#define MAX31865_FAULT_RTD_HIGH     0x80u
#define MAX31865_FAULT_RTD_LOW      0x40u
#define MAX31865_FAULT_REFIN_HIGH   0x20u
#define MAX31865_FAULT_REFIN_LOW    0x10u
#define MAX31865_FAULT_RTDIN_LOW    0x08u
#define MAX31865_FAULT_OV_UV        0x04u

/** Initialise SPI + all four converters (3-wire, auto, VBIAS, 50 Hz). */
void max31865_init(void);

/** (Re)write the configuration byte of a single channel. */
void max31865_write_config(uint8_t ch, uint8_t config);

/**
 * Read the 15-bit RTD ADC code of a channel.
 * @param ch        channel 0..3
 * @param code_out  receives the 15-bit ratio code (0..32767)
 * @return true if the MAX31865 flagged a fault (RTD LSB bit0 set).
 */
bool max31865_read_rtd(uint8_t ch, uint16_t* code_out);

/** Read the fault-status register (0x07). */
uint8_t max31865_read_fault(uint8_t ch);

/** Clear a latched fault (writes FAULT_CLEAR while preserving the config). */
void max31865_clear_fault(uint8_t ch, uint8_t base_config);

#ifdef __cplusplus
}
#endif

#endif /* APPLICATION_MAX31865_H */
