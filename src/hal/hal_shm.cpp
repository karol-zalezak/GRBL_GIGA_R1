/*
  hal_shm.cpp - the M4 display co-processor, seen from the M7

  Part of Grbl-Giga, a port of Grbl 1.1 to the Arduino GIGA R1 WiFi.

  Grbl is free software: you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation, either version 3 of the License, or (at your option) any later
  version. Distributed WITHOUT ANY WARRANTY; see the GNU GPL for details.

  -------------------------------------------------------------------------------

  Three jobs:

  - Map the shared block (grbl_shm.h, in SRAM4) non-cacheable on the M7, so
    status writes reach memory without cache maintenance. MPU region 15 is the
    one the core's RPC library uses for the same SRAM; this sketch does not use
    RPC.

  - Start the M4. The STM32H747 leaves the factory with the BCM4 option bit
    set, which boots the M4 by itself at reset from 0x08100000 - the start of
    flash bank 2, which with the "1.5MB M7 + 0.5MB M4" split belongs to the
    M7 and holds nothing runnable. The core's RPC library handles this by
    clearing BCM4 once and resetting; this does the same. After that the M4
    sits in reset until bootM4() (variant.cpp) points it at CM4_BINARY_START.
    The M4 is only started if a plausible vector table is found there, so a
    board without the display program simply runs Grbl without a display.

  - Watch the M4's heartbeat and pass on the touch screen's commands.
*/

#if defined(CORE_CM7)

#include <Arduino.h>
#include "stm32h7xx.h"
#include "stm32h7xx_hal.h"

#include "hal.h"
#include "../grbl_shm.h"

static bool m4_started = false;
static uint32_t m4_seen_ms = 0;      // last M4 uptime value seen
static uint32_t m4_seen_at = 0;      // our time when it last changed


static void mpu_map_shared(void)
{
  MPU_Region_InitTypeDef r;
  HAL_MPU_Disable();
  r.Enable = MPU_REGION_ENABLE;
  r.BaseAddress = D3_SRAM_BASE;                  // 0x38000000, all 64 kB of SRAM4
  r.Size = MPU_REGION_SIZE_64KB;
  r.AccessPermission = MPU_REGION_FULL_ACCESS;
  r.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;
  r.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
  r.IsShareable = MPU_ACCESS_SHAREABLE;
  r.Number = MPU_REGION_NUMBER15;
  r.TypeExtField = MPU_TEX_LEVEL1;               // normal memory, non-cacheable
  r.SubRegionDisable = 0x00;
  r.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  HAL_MPU_ConfigRegion(&r);
  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);
}


// Clears BCM4 in the option bytes if it is set, then resets. Runs once in the
// life of a board; the core's RPC library does exactly this (cm4_kick()).
static void m4_autoboot_off(void)
{
  FLASH_OBProgramInitTypeDef ob;
  ob.Banks = FLASH_BANK_1;
  HAL_FLASHEx_OBGetConfig(&ob);
  if (!(ob.USERConfig & FLASH_OPTSR_BCM4)) { return; }
  ob.OptionType = OPTIONBYTE_USER;
  ob.USERType = OB_USER_BCM4;
  ob.USERConfig = 0;
  if (HAL_FLASH_OB_Unlock() != HAL_OK) { return; }
  if (HAL_FLASH_Unlock() == HAL_OK) {
    if (HAL_FLASHEx_OBProgram(&ob) == HAL_OK) { HAL_FLASH_OB_Launch(); }
    HAL_FLASH_Lock();
  }
  HAL_FLASH_OB_Lock();
  NVIC_SystemReset();
}


// A program built for the M4 starts with its initial stack pointer (in the
// M4's D2 SRAM) and a Thumb reset vector inside its own flash area.
static bool m4_image_valid(void)
{
  const uint32_t *vt = (const uint32_t *)CM4_BINARY_START;
  SCB_InvalidateDCache_by_Addr((uint32_t *)vt, 32);
  uint32_t sp = vt[0], pc = vt[1];
  bool sp_ok = (sp >= 0x10000000UL && sp <= 0x10048000UL) || (sp >= 0x30000000UL && sp <= 0x30048000UL);
  bool pc_ok = (pc & 1) && (pc >= CM4_BINARY_START) && (pc < CM4_BINARY_START + 0x80000UL);
  return sp_ok && pc_ok;
}


extern "C" void hal_display_init(void)
{
  mpu_map_shared();
  m4_autoboot_off();

  grbl_shm_t *m = GRBL_SHM;
  memset((void *)m, 0, sizeof(grbl_shm_t));
  m->version = GRBL_SHM_VERSION;
  m->seq = 0;                 // even; m7_ms stays 0 until the first status is written
  __DMB();
  m->magic = GRBL_SHM_MAGIC;
  __DMB();
}


// Called from the first display_service(), i.e. after Grbl has configured all
// its pins: the M4 sets up its own pins while starting, and the two cores
// share the GPIO mode registers.
extern "C" void hal_display_start(void)
{
  if (m4_started) { return; }
  m4_started = true;
  if (!m4_image_valid()) { return; }
  bootM4();
}


extern "C" bool hal_display_present(void)
{
  grbl_shm_t *m = GRBL_SHM;
  if (m->m4_magic != GRBL_SHM_M4_MAGIC) { return false; }
  uint32_t now = hal_millis();
  uint32_t ms = m->m4_ms;
  if (ms != m4_seen_ms) { m4_seen_ms = ms; m4_seen_at = now; }
  return (uint32_t)(now - m4_seen_at) < 1000;
}


extern "C" uint32_t hal_display_status(void)
{
  grbl_shm_t *m = GRBL_SHM;
  return (m->m4_magic == GRBL_SHM_M4_MAGIC) ? m->m4_status : 0;
}


extern "C" uint32_t hal_display_frames(void)
{
  grbl_shm_t *m = GRBL_SHM;
  return (m->m4_magic == GRBL_SHM_M4_MAGIC) ? m->m4_frames : 0;
}


extern "C" int hal_display_cmd_read(void)
{
  grbl_shm_t *m = GRBL_SHM;
  if (m->m4_magic != GRBL_SHM_M4_MAGIC) { return -1; }
  uint8_t tail = m->cmd_tail;
  if (tail == m->cmd_head || tail >= sizeof(m->cmd)) { return -1; }
  __DMB();
  uint8_t c = m->cmd[tail];
  __DMB();
  m->cmd_tail = (uint8_t)((tail + 1) % sizeof(m->cmd));
  return c;
}


extern "C" const uint8_t *hal_display_framebuffer(void)
{
  grbl_shm_t *m = GRBL_SHM;
  if (!hal_display_present()) { return 0; }
  uint32_t fb = m->m4_fb;
  // SDRAM, swapped to 0x60000000 by the M4's FMC set-up, cacheable on the M7.
  if (fb < 0x60000000UL || fb >= 0x60800000UL - 480UL * 800UL * 2UL) { return 0; }
  SCB_InvalidateDCache_by_Addr((uint32_t *)fb, 480 * 800 * 2);
  return (const uint8_t *)fb;
}


extern "C" void hal_memory_barrier(void) { __DMB(); }

#endif // CORE_CM7
