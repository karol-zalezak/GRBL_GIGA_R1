/*
  estop.h - central STOP input (signal contact of the safety relay)

  Part of Grbl-Giga, a port of Grbl 1.1 to the Arduino GIGA R1 WiFi.

  Grbl is free software: you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation, either version 3 of the License, or (at your option) any later
  version. Distributed WITHOUT ANY WARRANTY; see the GNU GPL for details.

  -------------------------------------------------------------------------------

  The safety relay does the safety function: it cuts the power of the drives,
  the VFD, the brake and the valves. This input only makes Grbl agree with
  it, at once: no further step pulse (drives may still run for a moment on
  their capacitors), brake engaged, enable, spindle, coolant, aux and collet
  outputs off, ALARM:18 - and the machine stays in alarm until a reset finds
  the input released. See PLAN.md part 2.

  The pin is fixed (ESTOP_PIN, D49 = PE4) and has an interrupt of its own,
  EXTI4, at NVIC priority 0: above the stepper, the input scan and all of
  mbed. The 10 kHz scan watches it as well, which catches the state at power
  up and any edge the interrupt could have missed. $57 switches it on, bit 20
  of $79 says whether the contact is closed or open for STOP.
*/

#ifndef estop_h
#define estop_h

// Set by the trip, cleared only by a reset that finds STOP released. While it
// is set no step pulse goes out and the drives stay disabled.
extern volatile uint8_t estop_latched;

// Configures the interrupt (first call) and re-reads $57/$79. Called on every
// reset and when those settings change. Clears the latch if STOP is released.
void estop_init();

// STOP input active now, or released less than 100 ms ago. Always false with $57=0.
uint8_t estop_active();

// The scan's path: channels = io_read_channels() (bit IO_ESTOP_BIT = STOP).
void estop_scan(uint32_t channels);

// For $I: trips since power-up, and how long the last trip routine took from
// its start until every output was off, in 0.1 us.
uint32_t estop_trip_count();
uint32_t estop_trip_time_tenth_us();
// Which path tripped last: 1 = the STOP interrupt, 2 = the input scan.
uint8_t estop_trip_source();

#endif
