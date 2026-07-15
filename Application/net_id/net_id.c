/**
  ******************************************************************************
  * @file    net_id.c
  * @brief   Per-device MAC derived from the STM32 96-bit unique device ID.
  ******************************************************************************
  */

#include "net_id.h"

#include "stm32f4xx_hal.h"

/* UID_BASE (0x1FFF7A10 on STM32F407) is provided by the CMSIS device header.
 * Guard with a literal fallback in case a future part header omits it. */
#ifndef UID_BASE
#define UID_BASE    0x1FFF7A10u
#endif

/* CRC32 (IEEE 802.3, software) over the 12 UID bytes. Kept local to avoid a
 * dependency on other modules; the bootloader can compile this file as-is. */
static uint32_t net_id_crc32(const uint8_t* data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint32_t k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return ~crc;
}

void net_id_get_mac(uint8_t mac[6])
{
    const uint8_t* uid = (const uint8_t*)UID_BASE;   /* 96-bit unique ID */
    const uint32_t h   = net_id_crc32(uid, 12u);

    /* Locally administered, unicast (bit0 = 0, bit1 = 1). */
    mac[0] = 0x02u;
    mac[1] = 0x00u;
    mac[2] = (uint8_t)(h >> 24);
    mac[3] = (uint8_t)(h >> 16);
    mac[4] = (uint8_t)(h >> 8);
    mac[5] = (uint8_t)(h);
}
