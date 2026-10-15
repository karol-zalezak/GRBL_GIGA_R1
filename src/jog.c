/*
  jog.h - Jogging methods
  Part of Grbl

  Copyright (c) 2016 Sungeun K. Jeon for Gnea Research LLC

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


// Sets up valid jog motion received from g-code parser, checks for soft-limits, and executes the jog.
uint8_t jog_execute(plan_line_data_t *pl_data, parser_block_t *gc_block)
{
  // Initialize planner data struct for jogging motions.
  // NOTE: Spindle and coolant are allowed to fully function with overrides during a jog.
  pl_data->feed_rate = gc_block->values.f;
  pl_data->condition |= PL_COND_FLAG_NO_FEED_OVERRIDE;
  #ifdef USE_LINE_NUMBERS
    pl_data->line_number = gc_block->values.n;
  #endif

  if (bit_istrue(settings.flags,BITFLAG_SOFT_LIMIT_ENABLE)) {
    if (system_check_travel_limits(gc_block->values.xyz)) { return(STATUS_TRAVEL_EXCEEDED); }
  }
  // Port: not deeper into a limit switch that is held (away from it is fine).
  uint8_t status = limits_check_jog(gc_block->values.xyz);
  if (status != STATUS_OK) { return(status); }

  // Valid jog command. Plan, set state, and execute.
  mc_line(gc_block->values.xyz,pl_data);
  if (sys.state == STATE_IDLE) {
    if (plan_get_current_block() != NULL) { // Check if there is a block to execute.
      sys.state = STATE_JOG;
      st_prep_buffer();
      st_wake_up();  // NOTE: Manual start. No state machine required.
    }
  }

  return(STATUS_OK);
}


float jog_soft_limit_room(uint8_t axis, int8_t dir)
{
  if (bit_isfalse(settings.flags,BITFLAG_SOFT_LIMIT_ENABLE)) { return(1e9f); }
  if ((axis == A_AXIS) && !system_axis_a_homed()) { return(1e9f); }
  float lo = settings.max_travel[axis], hi = 0.0f;   // max_travel is negative
  #ifdef HOMING_FORCE_SET_ORIGIN
    if (bit_istrue(settings.homing_dir_mask,bit(axis))) { lo = 0.0f; hi = -settings.max_travel[axis]; }
  #endif
  float pos = gc_state.position[axis];   // where the queued jogs end
  float room = ((dir > 0) ? (hi - pos) : (pos - lo)) - 0.001f;
  return (room > 0.0f) ? room : 0.0f;
}


// Appends v with 4 decimals (no printf float support needed).
static char *jog_append_float(char *s, float v)
{
  if (v < 0) { *s++ = '-'; v = -v; }
  uint64_t f = (uint64_t)(v * 10000.0f + 0.5f);
  uint64_t ip = f / 10000;
  uint32_t fp = (uint32_t)(f % 10000);
  char tmp[24];
  int n = 0;
  do { tmp[n++] = (char)('0' + ip % 10); ip /= 10; } while (ip);
  while (n) { *s++ = tmp[--n]; }
  *s++ = '.';
  *s++ = (char)('0' + fp / 1000);
  *s++ = (char)('0' + (fp / 100) % 10);
  *s++ = (char)('0' + (fp / 10) % 10);
  *s++ = (char)('0' + fp % 10);
  *s = 0;
  return s;
}


void jog_line(char *cmd, uint8_t axis, float dist, float feed)
{
  static const char letter[N_AXIS] = { 'X', 'Y', 'Z', 'A' };
  strcpy(cmd, "$J=G21G91");
  char *s = cmd + strlen(cmd);
  *s++ = letter[axis];
  s = jog_append_float(s, dist);
  *s++ = 'F';
  jog_append_float(s, feed);
}
