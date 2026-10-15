/*
  estop.c - central STOP input (signal contact of the safety relay)

  Part of Grbl-Giga, a port of Grbl 1.1 to the Arduino GIGA R1 WiFi.

  Grbl is free software: you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation, either version 3 of the License, or (at your option) any later
  version. Distributed WITHOUT ANY WARRANTY; see the GNU GPL for details.

  -------------------------------------------------------------------------------

  See estop.h. The trip runs in the EXTI4 interrupt (or in the scan) and does
  everything by register writes: the step timers stop, the step pins go to
  rest, enable and brake drop, every output channel goes off, PWM and DAC go
  to zero. Only then does it hand over to Grbl the usual way - mc_reset() and
  an alarm code - which empties the planner and puts Grbl in ALARM:18.

  No debouncing towards STOP: the first edge acts. Back to OK is accepted only
  after the input has stayed released for ESTOP_RELEASE_SCANS.
*/

#include "grbl.h"

#define ESTOP_RELEASE_SCANS  1000    // 100 ms at the 10 kHz scan

volatile uint8_t estop_latched = 0;

static volatile uint8_t enabled = 0;           // $57, as last read
static volatile uint32_t invert = 0;           // bit IO_ESTOP_BIT of $79
static volatile uint16_t released_scans = ESTOP_RELEASE_SCANS;
static volatile uint32_t trips = 0;
static volatile uint32_t trip_cycles = 0;      // when the last trip started
static volatile uint32_t trip_duration = 0;    // cycles the trip routine took
static volatile uint8_t trip_source = 0;       // 1 = its interrupt, 2 = the scan


// The STOP pin read directly: true while it signals STOP (after $79).
static uint8_t estop_input()
{
  uint8_t on = hal_pin_read(ESTOP_PIN) ? 0 : 1;   // low = optocoupler conducts
  return on ^ (invert ? 1 : 0);
}


static void estop_trip(uint8_t source)
{
  if (estop_latched) { return; }
  trip_source = source;
  uint32_t t0 = hal_cycles();
  estop_latched = 1;
  trip_cycles = t0;
  trips++;

  // 1. Steps: no further pulse. 2. Drives: brake engaged, enable off.
  st_estop_halt();
  // 3. Every output off: spindle (with PWM and DAC), coolant, aux, collet.
  spindle_stop();
  outputs_all_off();

  trip_duration = hal_cycles() - t0;

  // Then Grbl's own way out: flush everything, alarm, wait for a reset.
  mc_reset();
  system_set_exec_alarm(EXEC_ALARM_ESTOP);
}


static void estop_isr(void)
{
  // Both edges interrupt; a release edge has nothing to do here.
  if (enabled && estop_input()) { estop_trip(1); }
}


void estop_init()
{
  static bool irq_ready = false;
  uint32_t sreg = GRBL_IRQ_SAVE();
  invert = settings.in_invert & (1UL << IO_ESTOP_BIT);
  enabled = settings.estop_enable ? 1 : 0;
  GRBL_IRQ_RESTORE(sreg);
  if (!irq_ready) {
    irq_ready = true;
    // io_init() has already made the pin an input with its pull-up.
    hal_estop_init(ESTOP_PIN, estop_isr);
  }
  // A reset ends the latch only if STOP has been released (and stayed so).
  if (estop_latched && !estop_active()) { estop_latched = 0; }
  if (!enabled) { estop_latched = 0; }
}


uint8_t estop_active()
{
  if (!enabled) { return(false); }
  return (estop_input() || (released_scans < ESTOP_RELEASE_SCANS)) ? true : false;
}


void estop_scan(uint32_t channels)
{
  if (!enabled) { released_scans = ESTOP_RELEASE_SCANS; return; }
  if (channels & (1UL << IO_ESTOP_BIT)) {
    released_scans = 0;
    if (!estop_latched) { estop_trip(2); }  // missed edge, or STOP at power-up
  } else if (released_scans < ESTOP_RELEASE_SCANS) {
    released_scans++;
  }
}


uint32_t estop_trip_count() { return trips; }
uint8_t estop_trip_source() { return trip_source; }
uint32_t estop_trip_time_tenth_us() { return (trip_duration * 10UL) / hal_cycles_per_us(); }
