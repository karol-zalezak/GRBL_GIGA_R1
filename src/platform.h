/*
  platform.h - AVR compatibility shims for the STM32H747 (GIGA R1) build

  Part of Grbl-Giga, a port of Grbl 1.1 to the Arduino GIGA R1 WiFi.

  Grbl is free software: you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation, either version 3 of the License, or (at your option) any later
  version. Distributed WITHOUT ANY WARRANTY; see the GNU GPL for details.

  -------------------------------------------------------------------------------

  Grbl's core algorithms are portable; only its bindings to the ATmega328P are
  not. This header absorbs the small vocabulary of AVR-isms the core uses, so
  planner.c, gcode.c, protocol.c, report.c and friends stay byte-for-byte
  comparable with upstream Grbl 1.1h and remain easy to re-diff against it.
*/

#ifndef platform_h
#define platform_h

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "hal/hal.h"
#include "hal/hal_nvs_size.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Program memory
// ---------------------------------------------------------------------------
// On AVR, string literals live in a separate address space and need PROGMEM,
// PSTR() and pgm_read_byte_near() to reach them. The Cortex-M7 has one flat
// address space, so all three become identities and printPgmString() is just
// printString().

#ifndef PROGMEM
  #define PROGMEM
#endif
#ifndef PSTR
  #define PSTR(s) (s)
#endif
#ifndef pgm_read_byte_near
  #define pgm_read_byte_near(addr) (*(const char *)(addr))
#endif
// avr-gcc's named address space for read-only data. Const data already lives in
// flash (or, for a loadable extension, in RAM) here, so the qualifier is dropped.
#ifndef __flash
  #define __flash
#endif


// ---------------------------------------------------------------------------
// Clock
// ---------------------------------------------------------------------------
// F_CPU here is the step timer clock, not the core clock: Grbl derives its
// step timing from it (TICKS_PER_MICROSECOND, the AMASS cut-offs). The M7 runs
// at 480 MHz but the timers count at 240 MHz, so F_CPU is 240 MHz. Note that
// F_CPU*60 no longer fits in 32 bits at this rate; stepper.c computes that
// constant in floating point.
#ifdef F_CPU
  #undef F_CPU
#endif
#ifndef F_CPU
  #define F_CPU HAL_TIMER_HZ
#endif


// ---------------------------------------------------------------------------
// Interrupt control
// ---------------------------------------------------------------------------
// AVR Grbl guards read-modify-write of the volatile sys_rt_exec_* flags with
// `uint8_t sreg = SREG; cli(); ...; SREG = sreg;`. The direct equivalent here
// saves and restores PRIMASK, which nests correctly.

#define GRBL_IRQ_SAVE()     hal_irq_disable()
#define GRBL_IRQ_RESTORE(x) hal_irq_restore(x)


// ---------------------------------------------------------------------------
// Delays
// ---------------------------------------------------------------------------

#define _delay_ms(ms) hal_delay_ms((uint32_t)(ms))
#define _delay_us(us) hal_delay_us((uint32_t)(us))

#ifdef __cplusplus
}
#endif

#endif
