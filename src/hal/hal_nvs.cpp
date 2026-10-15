/*
  hal_nvs.cpp - Grbl's EEPROM, emulated in the STM32H747's internal flash

  Part of Grbl-Giga, a port of Grbl 1.1 to the Arduino GIGA R1 WiFi.

  Grbl is free software: you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation, either version 3 of the License, or (at your option) any later
  version. Distributed WITHOUT ANY WARRANTY; see the GNU GPL for details.

  -------------------------------------------------------------------------------

  The H747 has no EEPROM and its flash is erased in 128 kB sectors that take
  a second or two, so a 1 kB settings image is not rewritten in place. It is
  appended, as a log:

  - Two sectors in bank 2, 0x08140000 and 0x08160000, inside the region the
    "1.5MB M7 + 0.5MB M4" flash split gives the M7 but far above where its
    program ends (checked at start-up). Bank 2 also holds the M4 program, so
    while a sector is erased or programmed the M4 stalls - the display freezes
    for that moment; Grbl, executing from bank 1, does not.

  - Each record is a 32-byte header (magic, sequence number, CRC) and the
    1024-byte image, 33 flash words. A sector holds 124 of them. A commit
    writes the next free slot, data first and header last, so a record only
    becomes valid once it is complete. The newest valid record wins.

  - When the active sector is full, the other one is erased and the record
    starts it. The full sector still holds a valid record until the new one
    is complete, so losing power at any point leaves the last good settings
    readable. An erase happens once every 124 commits.

  Uploading a sketch does not touch these sectors (DFU erases only what it
  writes), so settings survive re-uploads.
*/

#if defined(CORE_CM7)

#include <Arduino.h>
#include <string.h>
#include "stm32h7xx.h"
#include "stm32h7xx_hal.h"
#include "platform/mbed_mpu_mgmt.h"

#include "hal.h"
#include "hal_nvs_size.h"

#define NVS_SECTOR_SIZE     0x20000UL
#define NVS_BASE0           0x08140000UL     // bank 2, sector 2
#define NVS_BASE1           0x08160000UL     // bank 2, sector 3
#define NVS_SECTOR0         2
#define NVS_SECTOR1         3

#define NVS_WORD            32               // flash programming unit (256 bits)
#define NVS_HDR_SIZE        32
#define NVS_REC_SIZE        (NVS_HDR_SIZE + GRBL_NVS_SIZE)
#define NVS_SLOTS           (NVS_SECTOR_SIZE / NVS_REC_SIZE)
#define NVS_MAGIC           0x4E565331UL     // "NVS1"

typedef struct {
  uint32_t magic;
  uint32_t seq;
  uint32_t crc;
  uint32_t len;
  uint32_t magic_inv;
  uint32_t reserved[3];
} nvs_hdr_t;

static uint8_t shadow[GRBL_NVS_SIZE] __attribute__((aligned(32)));
static bool dirty = false;
static bool usable = false;

static uint8_t  active = 0;      // sector holding the newest record
static uint32_t next_slot = 0;   // first free slot in the active sector
static uint32_t last_seq = 0;

extern "C" uint32_t __etext;     // end of the M7 program's code (linker script)

static uint32_t sector_base(uint8_t s) { return s ? NVS_BASE1 : NVS_BASE0; }

static uint32_t crc32(const uint8_t *p, uint32_t n)
{
  uint32_t c = 0xFFFFFFFFUL;
  while (n--) {
    c ^= *p++;
    for (int k = 0; k < 8; k++) { c = (c >> 1) ^ (0xEDB88320UL & (0UL - (c & 1))); }
  }
  return ~c;
}

static bool is_blank(uint32_t addr, uint32_t len)
{
  const uint32_t *w = (const uint32_t *)addr;
  for (uint32_t i = 0; i < len / 4; i++) { if (w[i] != 0xFFFFFFFFUL) { return false; } }
  return true;
}

static bool record_valid(uint32_t addr, uint32_t *seq)
{
  const nvs_hdr_t *h = (const nvs_hdr_t *)addr;
  if (h->magic != NVS_MAGIC || h->magic_inv != ~NVS_MAGIC || h->len != GRBL_NVS_SIZE) { return false; }
  if (crc32((const uint8_t *)(addr + NVS_HDR_SIZE), GRBL_NVS_SIZE) != h->crc) { return false; }
  *seq = h->seq;
  return true;
}

// Scans one sector: newest valid record and the first slot after every used one.
static void scan_sector(uint8_t s, bool *found, uint32_t *best_seq, uint32_t *best_addr, uint32_t *free_slot)
{
  uint32_t base = sector_base(s);
  *free_slot = NVS_SLOTS;
  for (uint32_t i = NVS_SLOTS; i > 0; i--) {
    uint32_t addr = base + (i - 1) * NVS_REC_SIZE;
    if (!is_blank(addr, NVS_REC_SIZE)) { *free_slot = i; break; }
    if (i == 1) { *free_slot = 0; }
  }
  for (uint32_t i = 0; i < NVS_SLOTS; i++) {
    uint32_t addr = base + i * NVS_REC_SIZE;
    uint32_t seq;
    if (record_valid(addr, &seq)) {
      if (!*found || (int32_t)(seq - *best_seq) > 0) {
        *found = true;
        *best_seq = seq;
        *best_addr = addr;
      }
    }
  }
}


extern "C" bool hal_nvs_init(void)
{
  memset(shadow, 0xFF, sizeof(shadow));
  dirty = false;

  // The settings sectors must lie beyond the M7 program. With the default
  // "2MB M7" split or an enormous sketch they would not; keep settings in RAM
  // only then, and say so in the banner (nvs=0).
  usable = ((uint32_t)&__etext < NVS_BASE0 - 0x10000UL) && (CM4_BINARY_START >= NVS_BASE1 + NVS_SECTOR_SIZE);
  if (!usable) { return false; }

  SCB_InvalidateDCache_by_Addr((uint32_t *)NVS_BASE0, 2 * NVS_SECTOR_SIZE);

  bool found0 = false, found1 = false;
  uint32_t seq0 = 0, seq1 = 0, addr0 = 0, addr1 = 0, free0 = 0, free1 = 0;
  scan_sector(0, &found0, &seq0, &addr0, &free0);
  scan_sector(1, &found1, &seq1, &addr1, &free1);

  uint32_t addr = 0;
  if (found0 && (!found1 || (int32_t)(seq0 - seq1) > 0)) {
    active = 0; addr = addr0; last_seq = seq0; next_slot = free0;
  } else if (found1) {
    active = 1; addr = addr1; last_seq = seq1; next_slot = free1;
  } else {
    // Nothing stored yet. Start in sector 0; commit erases it if it is not blank.
    active = 0; last_seq = 0; next_slot = free0;
    return false;
  }
  memcpy(shadow, (const void *)(addr + NVS_HDR_SIZE), GRBL_NVS_SIZE);
  return true;
}

extern "C" bool hal_nvs_ok(void) { return usable; }

extern "C" uint8_t hal_nvs_read(uint16_t addr)
{
  return (addr < GRBL_NVS_SIZE) ? shadow[addr] : 0xFF;
}

extern "C" void hal_nvs_write(uint16_t addr, uint8_t value)
{
  if (addr >= GRBL_NVS_SIZE) { return; }
  if (shadow[addr] != value) { shadow[addr] = value; dirty = true; }
}

extern "C" bool hal_nvs_dirty(void) { return dirty && usable; }


static bool erase_sector(uint8_t s)
{
  FLASH_EraseInitTypeDef e;
  uint32_t err = 0;
  e.TypeErase = FLASH_TYPEERASE_SECTORS;
  e.Banks = FLASH_BANK_2;
  e.Sector = s ? NVS_SECTOR1 : NVS_SECTOR0;
  e.NbSectors = 1;
  e.VoltageRange = FLASH_VOLTAGE_RANGE_3;
  bool ok = (HAL_FLASHEx_Erase(&e, &err) == HAL_OK);
  SCB_InvalidateDCache_by_Addr((uint32_t *)sector_base(s), NVS_SECTOR_SIZE);
  return ok;
}

static bool program(uint32_t addr, const uint8_t *src, uint32_t len)
{
  static uint32_t word[NVS_WORD / 4] __attribute__((aligned(32)));
  for (uint32_t off = 0; off < len; off += NVS_WORD) {
    memcpy(word, src + off, NVS_WORD);
    if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_FLASHWORD, addr + off, (uint32_t)word) != HAL_OK) { return false; }
  }
  return true;
}

extern "C" bool hal_nvs_commit(void)
{
  if (!usable || !dirty) { return true; }

  // mbed's MPU set-up makes the whole code region read-only, so that a stray
  // write faults instead of corrupting flash; its own FlashIAP lifts that
  // around every program operation, and so must this. Without it the first
  // flash word written is a MemManage fault. Mind the naming: "lock" turns
  // write protection OFF (it locks the writable state, as a counter) and
  // "unlock" turns it back on; an unlock without a lock is a fatal error.
  mbed_mpu_manager_lock_rom_write();
  HAL_FLASH_Unlock();
  __HAL_FLASH_CLEAR_FLAG_BANK2(FLASH_FLAG_ALL_ERRORS_BANK2);

  bool ok = true;
  uint32_t base = sector_base(active);
  if (next_slot >= NVS_SLOTS || !is_blank(base + next_slot * NVS_REC_SIZE, NVS_REC_SIZE)) {
    // Sector full (or something foreign in the way): continue in the other one.
    uint8_t other = active ^ 1;
    if (last_seq == 0 && next_slot == 0) { other = active; }   // very first commit
    ok = erase_sector(other);
    active = other;
    next_slot = 0;
    base = sector_base(active);
  }

  if (ok) {
    nvs_hdr_t h;
    memset(&h, 0xFF, sizeof(h));
    h.magic = NVS_MAGIC;
    h.magic_inv = ~NVS_MAGIC;
    h.seq = last_seq + 1;
    h.len = GRBL_NVS_SIZE;
    h.crc = crc32(shadow, GRBL_NVS_SIZE);
    uint32_t addr = base + next_slot * NVS_REC_SIZE;
    // Image first, header last: the record turns valid only when complete.
    ok = program(addr + NVS_HDR_SIZE, shadow, GRBL_NVS_SIZE) &&
         program(addr, (const uint8_t *)&h, NVS_HDR_SIZE);
    SCB_InvalidateDCache_by_Addr((uint32_t *)addr, NVS_REC_SIZE);
    uint32_t seq;
    if (ok && record_valid(addr, &seq) && memcmp((const void *)(addr + NVS_HDR_SIZE), shadow, GRBL_NVS_SIZE) == 0) {
      last_seq = h.seq;
      dirty = false;
    } else {
      ok = false;
    }
    next_slot++;   // a failed slot is skipped, not retried in place
  }

  HAL_FLASH_Lock();
  mbed_mpu_manager_unlock_rom_write();

  // Grbl retries a dirty shadow on every pass of its main loop. Flash that
  // keeps failing would be erased over and over, so stop after a few tries;
  // settings then stay in RAM and the banner reports nvs=0.
  static uint8_t failures = 0;
  if (ok) { failures = 0; }
  else if (++failures >= 3) { usable = false; }
  return ok;
}

#endif // CORE_CM7
