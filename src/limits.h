/*
  limits.h - code pertaining to limit-switches and performing the homing cycle
  Part of Grbl

  Copyright (c) 2012-2016 Sungeun K. Jeon for Gnea Research LLC
  Copyright (c) 2009-2011 Simen Svale Skogsrud

  Grbl is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Grbl is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Grbl.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef limits_h
#define limits_h


// Initialize the limits module
void limits_init();

// Disables hard limits.
void limits_disable();

// Returns limit state as a bit-wise uint8 variable: axis bit set if either of
// its switches is active.
uint8_t limits_get_state();

// Limit switches one by one: bit 2*axis = minus, bit 2*axis+1 = plus.
uint8_t limits_get_switches();
uint8_t limits_switches_to_axes(uint8_t switches);
// Switches with a channel of their own (Grbl knows which end they are at).
uint8_t limits_get_known_side();
// The switches behind the last ALARM:1.
uint8_t limits_last_trigger();

// Servo alarm inputs ($68-$71), one bit per axis, 1 = fault. Raw and debounced.
uint8_t limits_get_motor_alarm_state();
uint8_t limits_get_motor_alarm_stable();

// STATUS_LIMIT_ACTIVE if a jog to target would drive into a held switch.
uint8_t limits_check_jog(float *target);

// STATUS_HOMING_NO_SWITCH if an axis of the cycle (0 = the whole $H) has no
// switch at the end $23 homes it to.
uint8_t limits_check_homing(uint8_t cycle_mask);

// Perform one portion of the homing cycle based on the input settings.
void limits_go_home(uint8_t cycle_mask);

// Check for soft limit violations
void limits_soft_check(float *target);

#endif
