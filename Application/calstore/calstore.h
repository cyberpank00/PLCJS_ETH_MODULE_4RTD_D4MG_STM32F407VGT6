/**
  ******************************************************************************
  * @file    calstore.h
  * @brief   Write-once calibration store for the 4RTD module.
  *
  * Per-channel / per-range 2-point linear calibration (R_true = gain·R_raw +
  * offset) is kept in a dedicated internal-Flash sector (Sector 11) that the
  * firmware NEVER erases during normal operation — not on settings save, not on
  * factory reset. Each of the 8 (channel × range) slots can be committed
  * exactly once: once a slot is programmed it cannot be rewritten, because
  * internal Flash can only clear bits (1 → 0) without a full sector erase.
  *
  * The live (working) coefficients live in RAM here. Modbus writes update the
  * live values as a preview (rejected if the slot is already locked); a
  * separate commit persists the live values into the slot and locks it.
  *
  * The only way to un-lock is calstore_erase(), which erases the whole sector
  * and reverts every slot to the neutral defaults (gain = 1.0, offset = 0.0).
  * That path is intended for a protected, physically-confirmed service action.
  ******************************************************************************
  */
#ifndef APPLICATION_CALSTORE_H
#define APPLICATION_CALSTORE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CALSTORE_CHANNELS   4u
#define CALSTORE_RANGES     2u

/* Coefficient slot codes used by the Modbus adapter (matches the 540+ register
 * layout: gain low, offset low, gain high, offset high). */
#define CALSTORE_SLOT_GAIN_LOW    0u
#define CALSTORE_SLOT_OFF_LOW     1u
#define CALSTORE_SLOT_GAIN_HIGH   2u
#define CALSTORE_SLOT_OFF_HIGH    3u
#define CALSTORE_SLOT_COUNT       4u

/** Load committed slots from Flash into the live coefficients. Un-committed
 *  (channel, range) pairs are initialised to the neutral defaults. */
void calstore_init(void);

/** True if the (channel, range) slot has been committed (write-locked). */
bool calstore_is_locked(uint8_t ch, uint8_t range);

/** Bitmask of locked slots: bit (ch*2 + range) set = locked. */
uint16_t calstore_lock_mask(void);

/** Live gain / offset used by the acquisition path. NAN-safe defaults. */
float calstore_gain(uint8_t ch, uint8_t range);
float calstore_offset(uint8_t ch, uint8_t range);

/** Read a live coefficient by slot code (CALSTORE_SLOT_*). */
float calstore_get_coeff(uint8_t ch, uint8_t slot);

/** Update a live coefficient (preview). Returns false if the slot is locked
 *  or the arguments are out of range. Does NOT touch Flash. */
bool calstore_set_coeff(uint8_t ch, uint8_t slot, float value);

/** Persist the live gain/offset of (channel, range) into its Flash slot and
 *  lock it. Returns false if already locked, out of range, or on Flash error.
 *  One-shot: a locked slot cannot be re-committed. */
bool calstore_commit(uint8_t ch, uint8_t range);

/** Emergency service action: erase the calibration sector, unlock every slot
 *  and revert the live coefficients to defaults. Returns false on Flash error. */
bool calstore_erase(void);

#ifdef __cplusplus
}
#endif

#endif /* APPLICATION_CALSTORE_H */
