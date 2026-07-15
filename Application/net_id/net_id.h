/**
  ******************************************************************************
  * @file    net_id.h
  * @brief   Per-device network identity derived from the STM32 96-bit unique
  *          device ID. Produces a stable, locally-administered unicast MAC that
  *          is unique per chip and identical between the application and the
  *          bootloader (both run on the same silicon and use this same code).
  ******************************************************************************
  */
#ifndef APPLICATION_NET_ID_H
#define APPLICATION_NET_ID_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Fill @p mac (6 bytes) with a MAC address derived from the chip unique ID.
 *
 * mac[0] = 0x02 (locally administered, unicast: bit0 = 0, bit1 = 1). The
 * remaining octets are a hash of the 96-bit UID. The result is deterministic:
 * calling this on the same chip always yields the same MAC, so the application
 * and the bootloader present an identical address without any stored value.
 */
void net_id_get_mac(uint8_t mac[6]);

#ifdef __cplusplus
}
#endif

#endif /* APPLICATION_NET_ID_H */
