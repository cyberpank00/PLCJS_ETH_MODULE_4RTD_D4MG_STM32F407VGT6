/**
  ******************************************************************************
  * @file    calstore.c
  * @brief   Write-once calibration store (see calstore.h).
  *
  * Flash layout — Sector 11 of the STM32F407VG, 128 KiB starting at
  * 0x080E0000. Only the first 8 * sizeof(cal_slot_t) bytes are used; the slot
  * for (channel, range) is at fixed index (ch * CALSTORE_RANGES + range). A
  * slot is "empty" while its magic reads 0xFFFFFFFF (erased Flash) and
  * "locked" once a valid record with the correct magic and CRC is programmed.
  ******************************************************************************
  */

#include "calstore.h"

#include <math.h>
#include <string.h>

#include "stm32f4xx_hal.h"

/* ---------------------------------------------------------------------------
 * Storage location
 * ------------------------------------------------------------------------- */
#define CALSTORE_FLASH_SECTOR   FLASH_SECTOR_11
#define CALSTORE_FLASH_ADDR     0x080E0000u
#define CALSTORE_SLOT_MAGIC     0xCA11B004u

#define CALSTORE_SLOT_TOTAL     (CALSTORE_CHANNELS * CALSTORE_RANGES)  /* 8 */

/* Persistent per-slot record. Fixed 20-byte layout, word-aligned so it can be
 * programmed with 32-bit Flash writes. Do not reorder. */
typedef struct {
    uint32_t magic;     /* 0xFFFFFFFF = empty; CALSTORE_SLOT_MAGIC = written */
    uint8_t  channel;
    uint8_t  range;
    uint16_t reserved;
    float    gain;
    float    offset;
    uint32_t crc32;     /* CRC32 over the preceding 16 bytes */
} cal_slot_t;

/* ---------------------------------------------------------------------------
 * Live (RAM) state
 * ------------------------------------------------------------------------- */
static float s_gain[CALSTORE_CHANNELS][CALSTORE_RANGES];
static float s_offset[CALSTORE_CHANNELS][CALSTORE_RANGES];
static bool  s_locked[CALSTORE_CHANNELS][CALSTORE_RANGES];

/* ---------------------------------------------------------------------------
 * CRC32 (IEEE 802.3, software) — same polynomial as settings.c.
 * ------------------------------------------------------------------------- */
static uint32_t calstore_crc32(const void* data, uint32_t len)
{
    const uint8_t* p = (const uint8_t*)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (uint32_t k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return ~crc;
}

static uint32_t slot_crc(const cal_slot_t* s)
{
    return calstore_crc32(s, (uint32_t)((const uint8_t*)&s->crc32 - (const uint8_t*)s));
}

static const cal_slot_t* slot_at(uint8_t index)
{
    return &((const cal_slot_t*)CALSTORE_FLASH_ADDR)[index];
}

static bool slot_valid(const cal_slot_t* nv, uint8_t ch, uint8_t range)
{
    return nv->magic == CALSTORE_SLOT_MAGIC &&
           nv->channel == ch && nv->range == range &&
           nv->crc32 == slot_crc(nv);
}

static void set_defaults(uint8_t ch, uint8_t range)
{
    s_gain[ch][range]   = 1.0f;
    s_offset[ch][range] = 0.0f;
    s_locked[ch][range] = false;
}

/* ---------------------------------------------------------------------------
 * Init
 * ------------------------------------------------------------------------- */
void calstore_init(void)
{
    for (uint8_t ch = 0; ch < CALSTORE_CHANNELS; ch++) {
        for (uint8_t r = 0; r < CALSTORE_RANGES; r++) {
            const cal_slot_t* nv = slot_at((uint8_t)(ch * CALSTORE_RANGES + r));
            if (slot_valid(nv, ch, r)) {
                s_gain[ch][r]   = nv->gain;
                s_offset[ch][r] = nv->offset;
                s_locked[ch][r] = true;
            } else {
                set_defaults(ch, r);
            }
        }
    }
}

/* ---------------------------------------------------------------------------
 * Accessors
 * ------------------------------------------------------------------------- */
bool calstore_is_locked(uint8_t ch, uint8_t range)
{
    if (ch >= CALSTORE_CHANNELS || range >= CALSTORE_RANGES) { return false; }
    return s_locked[ch][range];
}

uint16_t calstore_lock_mask(void)
{
    uint16_t m = 0u;
    for (uint8_t ch = 0; ch < CALSTORE_CHANNELS; ch++) {
        for (uint8_t r = 0; r < CALSTORE_RANGES; r++) {
            if (s_locked[ch][r]) { m |= (uint16_t)(1u << (ch * CALSTORE_RANGES + r)); }
        }
    }
    return m;
}

float calstore_gain(uint8_t ch, uint8_t range)
{
    if (ch >= CALSTORE_CHANNELS || range >= CALSTORE_RANGES) { return 1.0f; }
    return s_gain[ch][range];
}

float calstore_offset(uint8_t ch, uint8_t range)
{
    if (ch >= CALSTORE_CHANNELS || range >= CALSTORE_RANGES) { return 0.0f; }
    return s_offset[ch][range];
}

/* slot code -> (range, is_offset) */
static bool slot_decode(uint8_t slot, uint8_t* range, bool* is_offset)
{
    if (slot >= CALSTORE_SLOT_COUNT) { return false; }
    *range     = (uint8_t)(slot >> 1);   /* 0,1 -> low ; 2,3 -> high */
    *is_offset = (slot & 1u) != 0u;
    return true;
}

float calstore_get_coeff(uint8_t ch, uint8_t slot)
{
    uint8_t range; bool is_offset;
    if (ch >= CALSTORE_CHANNELS || !slot_decode(slot, &range, &is_offset)) {
        return 0.0f;
    }
    return is_offset ? s_offset[ch][range] : s_gain[ch][range];
}

bool calstore_set_coeff(uint8_t ch, uint8_t slot, float value)
{
    uint8_t range; bool is_offset;
    if (ch >= CALSTORE_CHANNELS || !slot_decode(slot, &range, &is_offset)) {
        return false;
    }
    if (s_locked[ch][range]) { return false; }
    if (is_offset) { s_offset[ch][range] = value; }
    else           { s_gain[ch][range]   = value; }
    return true;
}

/* ---------------------------------------------------------------------------
 * Commit — program a single slot, then lock it.
 * ------------------------------------------------------------------------- */
bool calstore_commit(uint8_t ch, uint8_t range)
{
    if (ch >= CALSTORE_CHANNELS || range >= CALSTORE_RANGES) { return false; }
    if (s_locked[ch][range]) { return false; }

    const uint8_t index = (uint8_t)(ch * CALSTORE_RANGES + range);
    const cal_slot_t* nv = slot_at(index);

    /* The slot must be fully erased before programming (write-once guard). */
    const uint32_t* w = (const uint32_t*)nv;
    for (uint32_t i = 0; i < sizeof(cal_slot_t) / 4u; i++) {
        if (w[i] != 0xFFFFFFFFu) { return false; }
    }

    cal_slot_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.magic   = CALSTORE_SLOT_MAGIC;
    rec.channel = ch;
    rec.range   = range;
    rec.gain    = s_gain[ch][range];
    rec.offset  = s_offset[ch][range];
    rec.crc32   = slot_crc(&rec);

    if (HAL_FLASH_Unlock() != HAL_OK) { return false; }
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR |
                           FLASH_FLAG_WRPERR | FLASH_FLAG_PGAERR |
                           FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);

    const uint32_t* src = (const uint32_t*)&rec;
    uint32_t addr = CALSTORE_FLASH_ADDR + (uint32_t)index * sizeof(cal_slot_t);
    bool ok = true;
    for (uint32_t i = 0; i < sizeof(cal_slot_t) / 4u; i++) {
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr, src[i]) != HAL_OK) {
            ok = false;
            break;
        }
        addr += 4u;
    }
    HAL_FLASH_Lock();

    if (!ok || !slot_valid(slot_at(index), ch, range)) { return false; }

    s_locked[ch][range] = true;
    return true;
}

/* ---------------------------------------------------------------------------
 * Erase — protected service action. Erases the whole sector; blocks the CPU
 * for ~1-2 s. Caller is responsible for the watchdog and any indication.
 * ------------------------------------------------------------------------- */
bool calstore_erase(void)
{
    if (HAL_FLASH_Unlock() != HAL_OK) { return false; }
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR |
                           FLASH_FLAG_WRPERR | FLASH_FLAG_PGAERR |
                           FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);

    FLASH_EraseInitTypeDef erase = {
        .TypeErase    = FLASH_TYPEERASE_SECTORS,
        .Banks        = FLASH_BANK_1,
        .Sector       = CALSTORE_FLASH_SECTOR,
        .NbSectors    = 1u,
        .VoltageRange = FLASH_VOLTAGE_RANGE_3,
    };
    uint32_t sector_error = 0u;
    const HAL_StatusTypeDef hs = HAL_FLASHEx_Erase(&erase, &sector_error);
    HAL_FLASH_Lock();

    for (uint8_t ch = 0; ch < CALSTORE_CHANNELS; ch++) {
        for (uint8_t r = 0; r < CALSTORE_RANGES; r++) {
            set_defaults(ch, r);
        }
    }
    return hs == HAL_OK;
}
