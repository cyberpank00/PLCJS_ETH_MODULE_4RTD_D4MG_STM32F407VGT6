/**
  ******************************************************************************
  * @file    settings.c
  * @brief   Persistent settings stored in the last sector of internal Flash.
  *
  * The STM32F407VG has 12 sectors. The last one (Sector 11) starts at
  * 0x080E0000 and is 128 KiB long. We use it as a single-page NV store.
  * On boot the structure is read and validated by CRC32; on failure the
  * defaults are applied. Saving erases the whole sector and rewrites the
  * fresh image.
  ******************************************************************************
  */

#include "settings.h"

#include <string.h>

#include "stm32f4xx_hal.h"
#include "rtd_scales.h"

/* ---------------------------------------------------------------------------
 * Storage location
 * ------------------------------------------------------------------------- */
#define SETTINGS_FLASH_SECTOR   FLASH_SECTOR_10
#define SETTINGS_FLASH_ADDR     0x080C0000u

/* ---------------------------------------------------------------------------
 * Static state
 * ------------------------------------------------------------------------- */
static settings_t s_settings;

/* ---------------------------------------------------------------------------
 * CRC32 (IEEE 802.3, software)
 * ------------------------------------------------------------------------- */
static uint32_t crc32_update(uint32_t crc, const uint8_t* data, uint32_t len)
{
    crc = ~crc;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint32_t k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (-(int32_t)(crc & 1u)));
        }
    }
    return ~crc;
}

static uint32_t settings_crc(const settings_t* s)
{
    /* CRC over everything except the trailing crc32 field. */
    const uint32_t len = (uint32_t)((const uint8_t*)&s->crc32 - (const uint8_t*)s);
    return crc32_update(0u, (const uint8_t*)s, len);
}

/* ---------------------------------------------------------------------------
 * Defaults
 * ------------------------------------------------------------------------- */
void settings_reset_to_defaults(void)
{
    memset(&s_settings, 0, sizeof(s_settings));

    s_settings.magic            = SETTINGS_MAGIC;
    s_settings.version          = SETTINGS_VERSION;

    s_settings.rtd_scan_ms      = SETTINGS_DEF_SCAN_MS;
    s_settings.led_mode         = SETTINGS_DEF_LED_MODE;

    s_settings.modbus_tcp_port  = SETTINGS_DEF_TCP_PORT;
    s_settings.modbus_slave_id  = SETTINGS_DEF_SLAVE_ID;
    s_settings.use_dhcp         = SETTINGS_DEF_USE_DHCP;

    s_settings.ip[0]            = SETTINGS_DEF_IP0;
    s_settings.ip[1]            = SETTINGS_DEF_IP1;
    s_settings.ip[2]            = SETTINGS_DEF_IP2;
    s_settings.ip[3]            = SETTINGS_DEF_IP3;

    s_settings.netmask[0]       = SETTINGS_DEF_MASK0;
    s_settings.netmask[1]       = SETTINGS_DEF_MASK1;
    s_settings.netmask[2]       = SETTINGS_DEF_MASK2;
    s_settings.netmask[3]       = SETTINGS_DEF_MASK3;

    s_settings.gateway[0]       = SETTINGS_DEF_GW0;
    s_settings.gateway[1]       = SETTINGS_DEF_GW1;
    s_settings.gateway[2]       = SETTINGS_DEF_GW2;
    s_settings.gateway[3]       = SETTINGS_DEF_GW3;

    /* Per-channel RTD defaults: disabled, Pt100, default (ГОСТ/IEC) α. */
    for (uint8_t ch = 0; ch < SETTINGS_RTD_CHANNELS; ch++) {
        s_settings.ch_enabled[ch]     = 0u;
        s_settings.ch_type[ch]        = SETTINGS_DEF_CH_TYPE;
        s_settings.ch_alpha_mode[ch]  = 0u;
        s_settings.reserved_a[ch]     = 0u;
        s_settings.ch_custom_w100[ch] =
            rtd_type_default_w100_x10000(SETTINGS_DEF_CH_TYPE);
    }

    s_settings.rref_nominal[RTD_RANGE_LOW]  = SETTINGS_DEF_RREF_LOW;
    s_settings.rref_nominal[RTD_RANGE_HIGH] = SETTINGS_DEF_RREF_HIGH;

    s_settings.crc32            = settings_crc(&s_settings);
}

/* ---------------------------------------------------------------------------
 * Init / accessors
 * ------------------------------------------------------------------------- */
bool settings_init(void)
{
    const settings_t* nv = (const settings_t*)SETTINGS_FLASH_ADDR;

    if (nv->magic == SETTINGS_MAGIC &&
        nv->version == SETTINGS_VERSION &&
        nv->crc32   == settings_crc(nv)) {
        memcpy(&s_settings, nv, sizeof(s_settings));
        return true;
    }

    settings_reset_to_defaults();
    return false;
}

settings_t* settings_get(void)
{
    return &s_settings;
}

/* ---------------------------------------------------------------------------
 * Save: erase sector, then program 32-bit words, then verify CRC
 * ------------------------------------------------------------------------- */
bool settings_save(void)
{
    s_settings.crc32 = settings_crc(&s_settings);

    HAL_StatusTypeDef hs = HAL_FLASH_Unlock();
    if (hs != HAL_OK) {
        return false;
    }

    /* Clear any leftover error flags from a previous interrupted operation
     * (e.g. IWDG reset during erase). Without this, HAL_FLASHEx_Erase()
     * finds stale flags in FLASH_SR and returns HAL_ERROR immediately. */
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR |
                           FLASH_FLAG_WRPERR | FLASH_FLAG_PGAERR |
                           FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);

    FLASH_EraseInitTypeDef erase = {
        .TypeErase    = FLASH_TYPEERASE_SECTORS,
        .Banks        = FLASH_BANK_1,
        .Sector       = SETTINGS_FLASH_SECTOR,
        .NbSectors    = 1u,
        .VoltageRange = FLASH_VOLTAGE_RANGE_3,
    };
    uint32_t sector_error = 0u;

    hs = HAL_FLASHEx_Erase(&erase, &sector_error);
    if (hs != HAL_OK) {
        HAL_FLASH_Lock();
        return false;
    }

    /* Program word-by-word. settings_t must be 4-byte aligned (it is). */
    const uint32_t words = (sizeof(s_settings) + 3u) / 4u;
    const uint32_t* src  = (const uint32_t*)&s_settings;
    uint32_t addr        = SETTINGS_FLASH_ADDR;

    for (uint32_t i = 0; i < words; i++) {
        hs = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr, src[i]);
        if (hs != HAL_OK) {
            HAL_FLASH_Lock();
            return false;
        }
        addr += 4u;
    }

    HAL_FLASH_Lock();

    /* Verify */
    return memcmp((const void*)SETTINGS_FLASH_ADDR, &s_settings, sizeof(s_settings)) == 0;
}
