/*
  probe.c - code pertaining to probing methods
  Part of Grbl

  Copyright (c) 2014-2016 Sungeun K. Jeon for Gnea Research LLC

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

#include "grbl.h"


// Inverts the probe input for the probe-away cycles (G38.4, G38.5). The NC/NO
// choice itself is the probe channel's bit in $79, applied by io.c.
uint8_t probe_invert_mask;


// The probe is whatever channel $72 names; io.c owns the pin.
void probe_init()
{
  probe_configure_invert_mask(false); // Initialize invert mask.
}


// Called by probe_init() and the mc_probe() routines. Sets up the probe pin invert mask to
// appropriately set the pin logic according to the probing cycle modes for
// toward-workpiece/away-from-workpiece.
void probe_configure_invert_mask(uint8_t is_probe_away)
{
  probe_invert_mask = is_probe_away ? PROBE_MASK : 0;
}


// Returns the probe pin state. Triggered = true. Called by gcode parser and probe state monitor.
// A probe without a channel ($72=0) never triggers.
uint8_t probe_get_state() { return(io_probe_raw() ^ probe_invert_mask); }


// Monitors probe pin state and records the system position when detected. Called by the
// stepper ISR per ISR tick.
// NOTE: This function must be extremely efficient as to not bog down the stepper ISR.
void probe_state_monitor()
{
  if (probe_get_state()) {
    sys_probe_state = PROBE_OFF;
    memcpy(sys_probe_position, sys_position, sizeof(sys_position));
    bit_true(sys_rt_exec_state, EXEC_MOTION_CANCEL);
  }
}
