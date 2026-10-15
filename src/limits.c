/*
  limits.c - code pertaining to limit-switches and performing the homing cycle
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

#include "grbl.h"


// Homing axis search distance multiplier. Computed by this value times the cycle travel.
#ifndef HOMING_AXIS_SEARCH_SCALAR
  #define HOMING_AXIS_SEARCH_SCALAR  1.5 // Must be > 1 to ensure limit switch will be engaged.
#endif
#ifndef HOMING_AXIS_LOCATE_SCALAR
  #define HOMING_AXIS_LOCATE_SCALAR  5.0 // Must be > 1 to ensure limit switch is cleared.
#endif


// Limit, driver alarm, control and probe inputs are sampled by a periodic timer
// rather than by pin-change interrupts. On the STM32 every EXTI line is shared
// by the same pin number of all ports, so pin-change interrupts would dictate
// the pin map; scanning at INPUT_SCAN_FREQUENCY makes every pin usable for
// every function and gives debouncing for free - a switch must read the same
// way on consecutive scans before Grbl acts on it, which is what
// ENABLE_SOFTWARE_DEBOUNCE used the watchdog for on AVR.
//
// Each axis has a minus and a plus limit function ($60-$67, io.h). In the
// words below bit 2*axis is the minus switch and bit 2*axis+1 the plus switch:
//
//   - two channels: Grbl knows which end of the axis is touched;
//   - both on one channel (switches in series): it does not - like upstream;
//   - only the minus switch assigned: the axis has a switch at that end only.
//
// A hard limit trips on a switch that *becomes* active (as upstream's
// pin-change interrupt did). A switch that is already held trips it only when
// the axis moves towards it, and only where Grbl knows the side - so after
// ALARM:1 and $X the axis can be jogged off the switch with hard limits on.
// With switches in series the side is unknown; as in upstream, the axis can
// then move either way while the switch is held.
static volatile bool hard_limits_armed = false;

// Debounce state, in scan ticks.
static uint8_t limit_stable = 0;      // switch bits, debounced
static uint8_t limit_candidate = 0;
static uint8_t limit_settle = 0;
static uint8_t limit_previous = 0;    // limit_stable as the previous scan saw it
static uint8_t motor_alarm_stable = 0;
static uint8_t motor_alarm_candidate = 0;
static uint8_t motor_alarm_settle = 0;

static uint8_t known_side = 0;        // switches whose end of the axis is known
static volatile uint8_t last_trigger = 0;   // switches behind the last ALARM:1

static void limits_scan_isr(void);


// Switches with a channel of their own: Grbl knows which end they are at.
static uint8_t limits_known_side()
{
  uint8_t known = 0, axis;
  for (axis = 0; axis < N_AXIS; axis++) {
    uint8_t cn = io_in_channel(IN_FN_LIM_XN + 2*axis), cp = io_in_channel(IN_FN_LIM_XN + 2*axis + 1);
    if (cn && (cn != cp)) { known |= bit(2*axis); }
    if (cp && (cp != cn)) { known |= bit(2*axis + 1); }
  }
  return known;
}


void limits_init()
{
  uint32_t fn = io_read_functions();
  uint8_t known = limits_known_side();

  // The scan interrupt reads these, so they are updated with it masked.
  uint32_t sreg = GRBL_IRQ_SAVE();
  limit_stable = limit_candidate = limit_previous = fn & IN_FN_LIMIT_MASK;
  limit_settle = 0;
  motor_alarm_stable = motor_alarm_candidate = (fn & IN_FN_ALARM_MASK) >> IN_FN_ALM_X;
  motor_alarm_settle = 0;
  known_side = known;
  GRBL_IRQ_RESTORE(sreg);

  // One scan timer serves limits, controls and probe for the life of the
  // program; hard limit response is armed and disarmed by a flag rather than by
  // reconfiguring the interrupt.
  static bool scan_started = false;
  if (!scan_started) {
    hal_scan_timer_init(limits_scan_isr, INPUT_SCAN_FREQUENCY);
    scan_started = true;
  }

  if (bit_istrue(settings.flags,BITFLAG_HARD_LIMIT_ENABLE)) {
    hard_limits_armed = true;
  } else {
    limits_disable();
  }
}


// Disables hard limits.
void limits_disable()
{
  hard_limits_armed = false;
}


// Limit switches, one bit per switch (bit 2*axis = minus, 2*axis+1 = plus),
// 1 = active after $79. Read now, not debounced.
uint8_t limits_get_switches()
{
  return (uint8_t)(io_read_functions() & IN_FN_LIMIT_MASK);
}


// Folds a switch word into Grbl's axis bits: axis bit set if either of its
// switches is.
uint8_t limits_switches_to_axes(uint8_t switches)
{
  uint8_t axes = 0, axis;
  for (axis = 0; axis < N_AXIS; axis++) {
    if (switches & (3 << (2*axis))) { axes |= bit(axis); }
  }
  return axes;
}


// Returns limit state as a bit-wise uint8 variable. Each bit indicates an axis limit, where
// triggered is 1 and not triggered is 0. Invert mask is applied. Axes are defined by their
// number in bit position, i.e. Z_AXIS is (1<<2) or bit 2, and Y_AXIS is (1<<1) or bit 1.
uint8_t limits_get_state()
{
  return limits_switches_to_axes(limits_get_switches());
}


// The switches behind the last hard limit alarm (for [MSG:Limit X+]).
uint8_t limits_last_trigger() { return last_trigger; }

// Switches whose side Grbl knows (see above).
uint8_t limits_get_known_side() { return known_side; }


// Servo alarm inputs ($68-$71), one bit per axis like limits_get_state(), 1 =
// the drive reports a fault. An axis without an assigned input reads 0.
uint8_t limits_get_motor_alarm_state()
{
  return (uint8_t)((io_read_functions() & IN_FN_ALARM_MASK) >> IN_FN_ALM_X);
}


// Debounced servo alarm state, as the scan last confirmed it.
uint8_t limits_get_motor_alarm_stable()
{
  return motor_alarm_stable;
}


// A jog that would drive an axis further into a limit switch that is held is
// refused before it is planned (the scan would trip ALARM:1 as soon as it
// started). Only where Grbl knows the switch's side, and only with $21=1.
uint8_t limits_check_jog(float *target)
{
  if (bit_isfalse(settings.flags,BITFLAG_HARD_LIMIT_ENABLE)) { return(STATUS_OK); }
  uint8_t held = limits_get_switches() & known_side;
  if (!held) { return(STATUS_OK); }
  uint8_t dirs = 0, axis;
  for (axis = 0; axis < N_AXIS; axis++) {
    float d = target[axis] - gc_state.position[axis];
    if (d < 0.0f) { dirs |= bit(2*axis); }
    else if (d > 0.0f) { dirs |= bit(2*axis + 1); }
  }
  if (axis_a_is_slave()) { dirs = (dirs & 0x3F) | (((dirs >> (2*axis_a_master())) & 3) << 6); }
  return (held & dirs) ? STATUS_LIMIT_ACTIVE : STATUS_OK;
}


// The input scan interrupt. Runs at INPUT_SCAN_FREQUENCY from TIM17, just
// below the stepper timer (see hal_h7.cpp): step generation delays it by one
// stepper interrupt at most and can never starve it.
//
// Unlike AVR Grbl's pin-change ISR, this sees the actual pin level rather than
// only the fact that something changed, so a bouncing switch cannot be missed
// or misread: a reading has to persist for LIMIT_DEBOUNCE_SCANS consecutive
// scans before it counts. At the 10 kHz default that is a 300 us confirmation
// delay, during which even a 10000 mm/min rapid travels 0.05 mm.
static void limits_scan_isr(void)
{
  uint32_t channels = io_read_channels();
  uint32_t fn = io_functions(channels);

  // The central STOP's second path (its own interrupt is the first), and the
  // supply of the drives.
  estop_scan(channels);
  drvpwr_scan(channels);
  mpg_test_tick();          // the simulated handwheel ($TMPW), if turning

  uint8_t state = fn & IN_FN_LIMIT_MASK;
  if (state != limit_candidate) {
    limit_candidate = state;
    limit_settle = 0;
  } else if (limit_settle < LIMIT_DEBOUNCE_SCANS) {
    if (++limit_settle >= LIMIT_DEBOUNCE_SCANS) { limit_stable = limit_candidate; }
  }

  // Ignore limit switches if already in an alarm state or in-process of executing an alarm.
  // When in the alarm state, Grbl should have been reset or will force a reset, so any pending
  // moves in the planner and serial buffers are all cleared and newly sent blocks will be
  // locked out until a homing cycle or a kill lock command.
  if (hard_limits_armed && (sys.state != STATE_ALARM) && !sys_rt_exec_alarm) {
    uint8_t rising = limit_stable & ~limit_previous;
    uint8_t into = limit_stable & known_side & st_axis_dirs;   // moving into a held switch
    if (rising | into) {
      last_trigger = rising | into;
      mc_reset(); // Initiate system kill.
      system_set_exec_alarm(EXEC_ALARM_HARD_LIMIT); // Indicate hard limit critical event
    }
  }
  limit_previous = limit_stable;

  // Servo alarms. Unlike a limit switch these are watched in every state but
  // alarm, idle included: a servo drive that faults while holding position has
  // lost it just as surely. A fault is a critical event like a hard limit -
  // reset required - and while the input stays active, every unlock lands
  // straight back here. Only assigned inputs ($68-$71) ever read active.
  state = (uint8_t)((fn & IN_FN_ALARM_MASK) >> IN_FN_ALM_X);
  if (state != motor_alarm_candidate) {
    motor_alarm_candidate = state;
    motor_alarm_settle = 0;
  } else if (motor_alarm_settle < MOTOR_ALARM_DEBOUNCE_SCANS) {
    if (++motor_alarm_settle >= MOTOR_ALARM_DEBOUNCE_SCANS) { motor_alarm_stable = motor_alarm_candidate; }
  }
  if (motor_alarm_stable && (sys.state != STATE_ALARM) && !sys_rt_exec_alarm) {
    mc_reset();
    system_set_exec_alarm(EXEC_ALARM_MOTOR_FAULT);
  }

  // The control inputs (reset, feed hold, cycle start, safety door) and the
  // spindle's at-speed and fault inputs ride the same scan.
  system_control_scan(fn);
  spindle_scan(fn);
}


// The limit function (switch bit) axis idx homes on: the switch at the end
// $23 sends it to. With both switches on one channel that is the shared input.
// 0xFF if no switch is assigned at that end.
static uint8_t limits_homing_switch(uint8_t idx)
{
  uint8_t dir_axis = idx;
  // A slave motor homes towards the same end as its master.
  if ((idx == A_AXIS) && axis_a_is_slave()) { dir_axis = axis_a_master(); }
  uint8_t sw = 2*idx + (bit_istrue(settings.homing_dir_mask,bit(dir_axis)) ? 0 : 1);
  return io_in_channel(IN_FN_LIM_XN + sw) ? sw : 0xFF;
}


// $H checks this first: every axis of the cycle needs a switch at the end it
// homes towards. STATUS_HOMING_NO_SWITCH otherwise.
uint8_t limits_check_homing(uint8_t cycle_mask)
{
  uint8_t idx;
  if (axis_a_is_slave() && (cycle_mask & bit(axis_a_master()))) { cycle_mask |= bit(A_AXIS); }
  for (idx = 0; idx < N_AXIS; idx++) {
    if ((cycle_mask & bit(idx)) && (limits_homing_switch(idx) == 0xFF)) { return(STATUS_HOMING_NO_SWITCH); }
  }
  return(STATUS_OK);
}


// Homes the specified cycle axes, sets the machine position, and performs a pull-off motion after
// completing. Homing is a special motion case, which involves rapid uncontrolled stops to locate
// the trigger point of the limit switches. The rapid stops are handled by a system level axis lock
// mask, which prevents the stepper algorithm from executing step pulses. Homing motions typically
// circumvent the processes for executing motions in normal operation.
// NOTE: Only the abort realtime command can interrupt this process.
// TODO: Move limit pin-specific calls to a general function for portability.
void limits_go_home(uint8_t cycle_mask)
{
  if (sys.abort) { return; } // Block if system reset has been issued.

  // Initialize plan data struct for homing motion. Spindle and coolant are disabled.
  plan_line_data_t plan_data;
  plan_line_data_t *pl_data = &plan_data;
  memset(pl_data,0,sizeof(plan_line_data_t));
  pl_data->condition = (PL_COND_FLAG_SYSTEM_MOTION|PL_COND_FLAG_NO_FEED_OVERRIDE);
  #ifdef USE_LINE_NUMBERS
    pl_data->line_number = HOMING_CYCLE_LINE_NUMBER;
  #endif

  // Initialize variables used for homing computations.
  uint8_t n_cycle = (2*N_HOMING_LOCATE_CYCLE+1);
  uint8_t step_pin[N_AXIS];

  // Squaring a gantry whose second motor is the A driver ($42=2/3). When the
  // cycle homes the master axis, the A motor joins it with its own lock bit
  // and its own switch (the A limit input), so each motor stops on its own
  // switch and the gantry ends up square. Upstream's ENABLE_DUAL_AXIS fail
  // check is kept: if one switch triggers and the other has not within a few
  // percent of the other axis's travel, something is broken - stop rather than
  // rack the gantry.
  uint8_t slave_bit = 0;
  uint8_t slave_master = X_AXIS;
  uint8_t dual_check = false;
  int32_t dual_trigger_position = 0;
  int32_t dual_fail_distance = 0;
  if (axis_a_is_slave() && (cycle_mask & bit(axis_a_master()))) {
    slave_bit = bit(A_AXIS);
    slave_master = axis_a_master();
    uint8_t other = (slave_master == X_AXIS) ? Y_AXIS : X_AXIS;
    float fail_distance = (-DUAL_AXIS_HOMING_FAIL_AXIS_LENGTH_PERCENT/100.0)*settings.max_travel[other];
    fail_distance = min(fail_distance, DUAL_AXIS_HOMING_FAIL_DISTANCE_MAX);
    fail_distance = max(fail_distance, DUAL_AXIS_HOMING_FAIL_DISTANCE_MIN);
    dual_fail_distance = trunc(fail_distance*settings.steps_per_mm[slave_master]);
  }
  float target[N_AXIS];
  float max_travel = 0.0;
  uint8_t idx;
  for (idx=0; idx<N_AXIS; idx++) {
    // Initialize step pin masks
    step_pin[idx] = get_step_pin_mask(idx);
    // On CoreXY both belt motors participate in any X or Y move, so the axis
    // lock has to hold or release them as a pair.
    if (kinematics_is_corexy()) {
      if ((idx==A_MOTOR)||(idx==B_MOTOR)) { step_pin[idx] = (get_step_pin_mask(X_AXIS)|get_step_pin_mask(Y_AXIS)); }
    }

    if (bit_istrue(cycle_mask,bit(idx))) {
      // Set target based on max_travel setting. Ensure homing switches engaged with search scalar.
      // NOTE: settings.max_travel[] is stored as a negative value.
      max_travel = max(max_travel,(-HOMING_AXIS_SEARCH_SCALAR)*settings.max_travel[idx]);
    }
  }
  // Set search mode with approach at seek rate to quickly engage the specified cycle_mask limit switches.
  bool approach = true;
  float homing_rate = settings.homing_seek_rate;

  // The switch each homing axis stops on, and - where an axis has two inputs -
  // the one at the far end, which must not trip during homing: that would mean
  // a wrong $23 or swapped wires.
  uint8_t home_bit[N_AXIS];
  uint8_t home_switches = 0, far_switches = 0;
  for (idx=0; idx<N_AXIS; idx++) {
    home_bit[idx] = 0;
    if ((cycle_mask & bit(idx)) || (slave_bit && (idx == A_AXIS))) {
      uint8_t sw = limits_homing_switch(idx);
      if (sw == 0xFF) {   // checked by $H already; a safety net only
        system_set_exec_alarm(EXEC_ALARM_HOMING_FAIL_APPROACH);
        mc_reset();
        protocol_execute_realtime();
        return;
      }
      home_bit[idx] = bit(sw);
      home_switches |= bit(sw);
      far_switches |= (bit((sw ^ 1)) & known_side);
    }
  }

  uint8_t limit_state, axislock, n_active_axis, far_at_start;
  do {
    // A far switch already held when a phase starts (the axis sat on it) is
    // not an error; one that closes during the phase is.
    far_at_start = limits_get_switches() & far_switches;

    system_convert_array_steps_to_mpos(target,sys_position);

    // Initialize and declare variables needed for homing routine.
    axislock = 0;
    dual_check = false;
    n_active_axis = 0;
    for (idx=0; idx<N_AXIS; idx++) {
      // Set target location for active axes and setup computation for homing rate.
      if (bit_istrue(cycle_mask,bit(idx))) {
        n_active_axis++;
        if (kinematics_is_corexy()) {
          // Zero the homing axis while preserving the off-axis position, which
          // is shared between both motors on a CoreXY frame.
          if (idx == X_AXIS) {
            int32_t axis_position = kinematics_motors_to_y_steps(sys_position);
            sys_position[A_MOTOR] = axis_position;
            sys_position[B_MOTOR] = -axis_position;
          } else if (idx == Y_AXIS) {
            int32_t axis_position = kinematics_motors_to_x_steps(sys_position);
            sys_position[A_MOTOR] = sys_position[B_MOTOR] = axis_position;
          } else {
            sys_position[idx] = 0;
          }
        } else {
          sys_position[idx] = 0;
        }
        // Set target direction based on cycle mask and homing cycle approach state.
        // NOTE: This happens to compile smaller than any other implementation tried.
        if (bit_istrue(settings.homing_dir_mask,bit(idx))) {
          if (approach) { target[idx] = -max_travel; }
          else { target[idx] = max_travel; }
        } else {
          if (approach) { target[idx] = max_travel; }
          else { target[idx] = -max_travel; }
        }
        // Apply axislock to the step port pins active in this cycle.
        axislock |= step_pin[idx];
        if (slave_bit && (idx == slave_master)) { axislock |= slave_bit; } // The slave moves with it.
      }

    }
    homing_rate *= sqrt(n_active_axis); // [sqrt(N_AXIS)] Adjust so individual axes all move at homing rate.
    sys.homing_axis_lock = axislock;

    // Perform homing cycle. Planner buffer should be empty, as required to initiate the homing cycle.
    pl_data->feed_rate = homing_rate; // Set current homing rate.
    plan_buffer_line(target, pl_data); // Bypass mc_line(). Directly plan homing motion.

    sys.step_control = STEP_CONTROL_EXECUTE_SYS_MOTION; // Set to execute homing motion and clear existing flags.
    st_prep_buffer(); // Prep and fill segment buffer from newly planned block.
    st_wake_up(); // Initiate motion
    do {
      if (approach) {
        // Check limit state. Lock out cycle axes when they change.
        limit_state = limits_get_switches();
        for (idx=0; idx<N_AXIS; idx++) {
          if (axislock & step_pin[idx]) {
            if (limit_state & home_bit[idx]) {
              if (kinematics_is_corexy() && ((idx == X_AXIS) || (idx == Y_AXIS))) {
                axislock &= ~(step_pin[A_MOTOR]|step_pin[B_MOTOR]);
              } else {
                axislock &= ~(step_pin[idx]);
              }
            }
          }
        }
        // The slave motor stops on its own switch, the A limit input.
        if (slave_bit && (axislock & slave_bit) && (limit_state & home_bit[A_AXIS])) { axislock &= ~slave_bit; }
        sys.homing_axis_lock = axislock;

        // One of the pair has stopped and the other has not: measure how far
        // the running one gets, and give up if its switch never comes.
        if (slave_bit) {
          uint8_t pair = step_pin[slave_master] | slave_bit;
          uint8_t stopped = pair & ~axislock;
          if (stopped && (stopped != pair)) {
            if (!dual_check) {
              dual_check = true;
              dual_trigger_position = sys_position[slave_master];
            } else if (labs(dual_trigger_position - sys_position[slave_master]) > dual_fail_distance) {
              system_set_exec_alarm(EXEC_ALARM_HOMING_FAIL_DUAL_APPROACH);
              mc_reset();
              protocol_execute_realtime();
              return;
            }
          }
        }
      }

      // The switch at the far end closed: wrong $23 or swapped wires. Stop at
      // once rather than drive the axis on into its end stop.
      if (limits_get_switches() & far_switches & ~far_at_start) {
        system_set_exec_alarm(EXEC_ALARM_HOMING_WRONG_SWITCH);
        mc_reset();
        protocol_execute_realtime();
        return;
      }

      st_prep_buffer(); // Check and prep segment buffer. NOTE: Should take no longer than 200us.

      // Port addition: upstream services nothing in this loop, which with a
      // native USB link and a display on the other core meant that for the
      // whole search - up to 1.5 x $13x at $25, half a minute or more with no
      // switch connected - nothing reached the PC, the display went blank and
      // the touch screen's RESET was not read. This keeps the link and the
      // display going and answers '?' (state Home); a reset from the PC, the
      // touch screen or the reset pin is acted on just below, as upstream does.
      protocol_service_link();

      // Exit routines: No time to run protocol_execute_realtime() in this loop.
      if (sys_rt_exec_state & (EXEC_SAFETY_DOOR | EXEC_RESET | EXEC_CYCLE_STOP)) {
        uint8_t rt_exec = sys_rt_exec_state;
        // Homing failure condition: Reset issued during cycle. Not over the
        // alarm of whatever issued it (a servo alarm, the driver supply).
        if ((rt_exec & EXEC_RESET) && !sys_rt_exec_alarm) { system_set_exec_alarm(EXEC_ALARM_HOMING_FAIL_RESET); }
        // Homing failure condition: Safety door was opened.
        if (rt_exec & EXEC_SAFETY_DOOR) { system_set_exec_alarm(EXEC_ALARM_HOMING_FAIL_DOOR); }
        // Homing failure condition: Limit switch still engaged after pull-off motion
        if (!approach && (limits_get_switches() & home_switches)) { system_set_exec_alarm(EXEC_ALARM_HOMING_FAIL_PULLOFF); }
        // Homing failure condition: Limit switch not found during approach.
        if (approach && (rt_exec & EXEC_CYCLE_STOP)) { system_set_exec_alarm(EXEC_ALARM_HOMING_FAIL_APPROACH); }
        if (sys_rt_exec_alarm) {
          mc_reset(); // Stop motors, if they are running.
          protocol_execute_realtime();
          return;
        } else {
          // Pull-off motion complete. Disable CYCLE_STOP from executing.
          system_clear_exec_state_flag(EXEC_CYCLE_STOP);
          break;
        }
      }

    // AXIS_STEP_MASK is the logical all-axes bit mask; axislock holds the same
    // logical bits, so the loop runs until every homing axis has released.
    } while (AXIS_STEP_MASK & axislock);

    st_reset(); // Immediately force kill steppers and reset step segment buffer.
    delay_ms(settings.homing_debounce_delay); // Delay to allow transient dynamics to dissipate.

    // Reverse direction and reset homing rate for locate cycle(s).
    approach = !approach;

    // After first cycle, homing enters locating phase. Shorten search to pull-off distance.
    if (approach) {
      max_travel = settings.homing_pulloff*HOMING_AXIS_LOCATE_SCALAR;
      homing_rate = settings.homing_feed_rate;
    } else {
      max_travel = settings.homing_pulloff;
      homing_rate = settings.homing_seek_rate;
    }

  } while (n_cycle-- > 0);

  // The active cycle axes should now be homed and machine limits have been located. By
  // default, Grbl defines machine space as all negative, as do most CNCs. Since limit switches
  // can be on either side of an axes, check and set axes machine zero appropriately. Also,
  // set up pull-off maneuver from axes limit switches that have been homed. This provides
  // some initial clearance off the switches and should also help prevent them from falsely
  // triggering when hard limits are enabled or when more than one axes shares a limit pin.
  int32_t set_axis_position;
  // Set machine positions for homed limit switches. Don't update non-homed axes.
  for (idx=0; idx<N_AXIS; idx++) {
    // NOTE: settings.max_travel[] is stored as a negative value.
    if (cycle_mask & bit(idx)) {
      #ifdef HOMING_FORCE_SET_ORIGIN
        set_axis_position = 0;
      #else
        if ( bit_istrue(settings.homing_dir_mask,bit(idx)) ) {
          set_axis_position = lround((settings.max_travel[idx]+settings.homing_pulloff)*settings.steps_per_mm[idx]);
        } else {
          set_axis_position = lround(-settings.homing_pulloff*settings.steps_per_mm[idx]);
        }
      #endif

      if (kinematics_is_corexy()) {
        if (idx==X_AXIS) {
          int32_t off_axis_position = kinematics_motors_to_y_steps(sys_position);
          sys_position[A_MOTOR] = set_axis_position + off_axis_position;
          sys_position[B_MOTOR] = set_axis_position - off_axis_position;
        } else if (idx==Y_AXIS) {
          int32_t off_axis_position = kinematics_motors_to_x_steps(sys_position);
          sys_position[A_MOTOR] = off_axis_position + set_axis_position;
          sys_position[B_MOTOR] = off_axis_position - set_axis_position;
        } else {
          sys_position[idx] = set_axis_position;
        }
      } else {
        sys_position[idx] = set_axis_position;
      }

    }
  }
  sys.step_control = STEP_CONTROL_NORMAL_OP; // Return step control to normal operation.
}


// Performs a soft limit check. Called from mc_line() only. Assumes the machine has been homed,
// the workspace volume is in all negative space, and the system is in normal operation.
// NOTE: Used by jogging to limit travel within soft-limit volume.
void limits_soft_check(float *target)
{
  if (system_check_travel_limits(target)) {
    sys.soft_limit = true;
    // Force feed hold if cycle is active. All buffered blocks are guaranteed to be within
    // workspace volume so just come to a controlled stop so position is not lost. When complete
    // enter alarm mode.
    if (sys.state == STATE_CYCLE) {
      system_set_exec_state_flag(EXEC_FEED_HOLD);
      do {
        protocol_execute_realtime();
        if (sys.abort) { return; }
      } while ( sys.state != STATE_IDLE );
    }
    mc_reset(); // Issue system reset and ensure spindle and coolant are shutdown.
    system_set_exec_alarm(EXEC_ALARM_SOFT_LIMIT); // Indicate soft limit critical event
    protocol_execute_realtime(); // Execute to enter critical event loop and system abort
    return;
  }
}
