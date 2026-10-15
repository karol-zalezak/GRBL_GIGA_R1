/*
  system.c - Handles system level commands and real-time processes
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


// Edge detection for the polled control inputs: Grbl's AVR build acted on the
// pin-change interrupt, so it reacted to a press rather than to a held button.
// Remembering the previous debounced state reproduces that exactly.
static uint8_t control_stable = 0;
static uint8_t control_candidate = 0;
static uint8_t control_settle = 0;
static uint8_t control_previous = 0;

// The last alarm reported, for the reset pulses (see system_reset_pulses()).
uint8_t system_last_alarm = 0;


// Folds the input function word into CONTROL_PIN_INDEX bits.
static uint8_t system_control_from_functions(uint32_t fn)
{
  uint8_t control_state = 0;
  #ifdef ENABLE_SAFETY_DOOR_INPUT_PIN
    if (fn & bit(IN_FN_DOOR)) { control_state |= CONTROL_PIN_INDEX_SAFETY_DOOR; }
  #endif
  if (fn & bit(IN_FN_HOLD)) { control_state |= CONTROL_PIN_INDEX_FEED_HOLD; }
  if (fn & bit(IN_FN_RESET)) { control_state |= CONTROL_PIN_INDEX_RESET; }
  if (fn & bit(IN_FN_START)) { control_state |= CONTROL_PIN_INDEX_CYCLE_START; }
  return(control_state);
}


void system_init()
{
  // The control inputs are channels ($73-$76); io.c owns the pins and the
  // scan timer is started by limits_init(). This only restarts debouncing.
  uint8_t state = system_control_get_state();
  uint32_t sreg = GRBL_IRQ_SAVE();
  control_stable = control_candidate = control_previous = state;
  control_settle = 0;
  GRBL_IRQ_RESTORE(sreg);
}


// Returns control pin state as a uint8 bitfield. Each bit indicates the input pin state, where
// triggered is 1 and not triggered is 0. Invert mask is applied. Bitfield organization is
// defined by the CONTROL_PIN_INDEX in the header file.
uint8_t system_control_get_state()
{
  return system_control_from_functions(io_read_functions());
}


// Polled replacement for AVR Grbl's ISR(CONTROL_INT_vect). Called from the
// input scan interrupt in limits.c. Sets only the real-time command execute
// variables so the main program runs them when it is ready, which works exactly
// like the character-based real-time commands picked off the serial stream.
//
// Only newly asserted inputs act, so holding cycle start down does not re-issue
// the command on every scan.
void system_control_scan(uint32_t fn)
{
  uint8_t pin = system_control_from_functions(fn);

  if (pin != control_candidate) {
    control_candidate = pin;
    control_settle = 0;
    return;
  }
  if (control_settle < CONTROL_DEBOUNCE_SCANS) {
    if (++control_settle < CONTROL_DEBOUNCE_SCANS) { return; }
    control_stable = control_candidate;
  }

  uint8_t rising = (uint8_t)(control_stable & ~control_previous);
  control_previous = control_stable;
  if (!rising) { return; }

  if (bit_istrue(rising,CONTROL_PIN_INDEX_RESET)) {
    mc_reset();
  }
  if (bit_istrue(rising,CONTROL_PIN_INDEX_CYCLE_START)) {
    bit_true(sys_rt_exec_state, EXEC_CYCLE_START);
  }
  if (bit_istrue(rising,CONTROL_PIN_INDEX_FEED_HOLD)) {
    bit_true(sys_rt_exec_state, EXEC_FEED_HOLD);
  }
  #ifdef ENABLE_SAFETY_DOOR_INPUT_PIN
    if (bit_istrue(rising,CONTROL_PIN_INDEX_SAFETY_DOOR)) {
      bit_true(sys_rt_exec_state, EXEC_SAFETY_DOOR);
    }
  #endif
}


// Returns if safety door is ajar(T) or closed(F), based on pin state.
uint8_t system_check_safety_door_ajar()
{
  #ifdef ENABLE_SAFETY_DOOR_INPUT_PIN
    return(system_control_get_state() & CONTROL_PIN_INDEX_SAFETY_DOOR);
  #else
    return(false); // Input pin not enabled, so just return that it's closed.
  #endif
}


// Executes user startup script, if stored.
void system_execute_startup(char *line)
{
  uint8_t n;
  for (n=0; n < N_STARTUP_LINE; n++) {
    if (!(settings_read_startup_line(n, line))) {
      line[0] = 0;
      report_execute_startup_message(line,STATUS_SETTING_READ_FAIL);
    } else {
      if (line[0] != 0) {
        uint8_t status_code = gc_execute_line(line);
        report_execute_startup_message(line,status_code);
      }
    }
  }
}


// Directs and executes one line of formatted input from protocol_process. While mostly
// incoming streaming g-code blocks, this also executes Grbl internal commands, such as
// settings, initiating the homing cycle, and toggling switch states. This differs from
// the realtime command module by being susceptible to when Grbl is ready to execute the
// next line during a cycle, so for switches like block delete, the switch only effects
// the lines that are processed afterward, not necessarily real-time during a cycle,
// since there are motions already stored in the buffer. However, this 'lag' should not
// be an issue, since these commands are not typically used during a cycle.
// Port addition: $TIN=n,v - the test hook for input channel n (io.h).
static uint8_t system_test_input(char *line)
{
  uint8_t char_counter = 5;
  float ch, v, ms = 0;
  if (!read_float(line, &char_counter, &ch)) { return(STATUS_BAD_NUMBER_FORMAT); }
  if (line[char_counter++] != ',') { return(STATUS_INVALID_STATEMENT); }
  if (!read_float(line, &char_counter, &v)) { return(STATUS_BAD_NUMBER_FORMAT); }
  if (line[char_counter] == ',') {
    char_counter++;
    if (!read_float(line, &char_counter, &ms)) { return(STATUS_BAD_NUMBER_FORMAT); }
  }
  if ((line[char_counter] != 0) || (ch < 0) || (ch > IO_N_IN_PINS) || (v < 0) || (v > 1) ||
      (ms < 0) || (ms > 600000)) {
    return(STATUS_INVALID_STATEMENT);
  }
  if (ms > 0) { return(io_test_input_later((uint8_t)ch, (uint8_t)v, (uint32_t)ms)); }
  return(io_test_input((uint8_t)ch, (uint8_t)v));
}


// Port addition: $TMPG=axis,mult (selector) and $TMPW=detents[,rate] (wheel) -
// the handwheel's test hooks (mpg.h).
static uint8_t system_test_mpg(char *line)
{
  uint8_t char_counter = 6;
  float a, b = 0.0f;
  bool turn = (line[4] == 'W');
  if (!read_float(line, &char_counter, &a)) { return(STATUS_BAD_NUMBER_FORMAT); }
  if (line[char_counter] == ',') {
    char_counter++;
    if (!read_float(line, &char_counter, &b)) { return(STATUS_BAD_NUMBER_FORMAT); }
  } else if (!turn) {
    return(STATUS_INVALID_STATEMENT);
  }
  if (line[char_counter] != 0) { return(STATUS_INVALID_STATEMENT); }
  if (turn) { return(mpg_test_turn((int32_t)lroundf(a), (b > 0.0f) ? (uint32_t)b : 100)); }
  if ((a < 0.0f) || (b < 0.0f)) { return(STATUS_INVALID_STATEMENT); }
  return(mpg_test_select((uint8_t)a, (uint16_t)b));
}


// Port addition: $TSW=n,<axis><+|->mm (or $TSW=0) - a virtual switch (io.h).
static uint8_t system_test_switch(char *line)
{
  static const char letters[N_AXIS] = { 'X', 'Y', 'Z', 'A' };
  uint8_t char_counter = 5, axis;
  float ch, mm;
  if (!read_float(line, &char_counter, &ch)) { return(STATUS_BAD_NUMBER_FORMAT); }
  if ((line[char_counter] == 0) && (ch == 0)) { return(io_test_switch(0, 0, 0, 0)); }
  if (line[char_counter++] != ',') { return(STATUS_INVALID_STATEMENT); }
  for (axis = 0; axis < N_AXIS; axis++) { if (line[char_counter] == letters[axis]) { break; } }
  if (axis == N_AXIS) { return(STATUS_INVALID_STATEMENT); }
  char_counter++;
  int8_t dir = 0;
  if (line[char_counter] == '+') { dir = 1; } else if (line[char_counter] == '-') { dir = -1; }
  else { return(STATUS_INVALID_STATEMENT); }
  char_counter++;
  if (!read_float(line, &char_counter, &mm)) { return(STATUS_BAD_NUMBER_FORMAT); }
  if ((line[char_counter] != 0) || (ch < 1) || (ch > N_IN_CHANNELS + 1)) { return(STATUS_INVALID_STATEMENT); }
  return(io_test_switch((uint8_t)ch, axis, dir, mm));
}


// Port addition: pulses on the reset outputs at a reset or an unlock. The
// servos get one after ALARM:17 or while a servo alarm input is active, the
// VFD while its fault input is active (PLAN.md part 1).
void system_reset_pulses()
{
  if ((system_last_alarm == EXEC_ALARM_MOTOR_FAULT) || limits_get_motor_alarm_state()) {
    outputs_pulse(OUTPUT_SERVO_RESET, settings.reset_pulse_ms);
  }
  if (spindle_fault_active()) {
    outputs_pulse(OUTPUT_VFD_RESET, settings.reset_pulse_ms);
  }
}


uint8_t system_execute_line(char *line)
{
  uint8_t char_counter = 1;
  uint8_t helper_var = 0; // Helper variable
  float parameter, value;

  // Port additions with long names, ahead of Grbl's one-letter commands.
  if (strncmp(line, "$TIN=", 5) == 0) { return(system_test_input(line)); }   // any state
  if (strncmp(line, "$TSW=", 5) == 0) { return(system_test_switch(line)); }  // any state
  if (strncmp(line, "$SPLASH", 7) == 0) { return(display_splash(line)); }       // any state
  if ((strncmp(line, "$TMPG=", 6) == 0) || (strncmp(line, "$TMPW=", 6) == 0)) {  // any state
    return(system_test_mpg(line));
  }
  if (line[1] == 'F') { return(job_command(line)); }                        // USB stick, job.c
  if (strcmp(line, "$SCREEN") == 0) {                                       // any state but motion
    if (sys.state & (STATE_CYCLE | STATE_JOG | STATE_HOMING)) { return(STATUS_IDLE_ERROR); }
    return(display_dump_screen());
  }
  if (strncmp(line, "$COLLET=", 8) == 0) {
    if ((line[8] != '0') && (line[8] != '1')) { return(STATUS_INVALID_STATEMENT); }
    if (line[9] != 0) { return(STATUS_INVALID_STATEMENT); }
    return(spindle_collet_set(line[8] == '1'));
  }

  // Port: $I only reads, so it is answered in every state (upstream: Idle and
  // Alarm only) - the diagnostics on its [MSG:] line are wanted during motion.
  if ((line[1] == 'I') && (line[2] == 0)) {
    settings_read_build_info(line);
    report_build_info(line);
    return(STATUS_OK);
  }

  switch( line[char_counter] ) {
    case 0 : report_grbl_help(); break;
    case 'T' : // Port addition: $TOUCH=x,y[,ms] presses the touch screen, for tests. Any state.
      return(display_simulate_touch(line));
    case 'J' : // Jogging
      // Execute only if in IDLE or JOG states.
      if (sys.state != STATE_IDLE && sys.state != STATE_JOG) { return(STATUS_IDLE_ERROR); }
      if(line[2] != '=') { return(STATUS_INVALID_STATEMENT); }
      return(gc_execute_line(line)); // NOTE: $J= is ignored inside g-code parser and used to detect jog motions.
      break;
    case '$': case 'G': case 'C': case 'X':
      if ( line[2] != 0 ) { return(STATUS_INVALID_STATEMENT); }
      switch( line[1] ) {
        case '$' : // Prints Grbl settings
          if ( sys.state & (STATE_CYCLE | STATE_HOLD) ) { return(STATUS_IDLE_ERROR); } // Block during cycle. Takes too long to print.
          else { report_grbl_settings(); }
          break;
        case 'G' : // Prints gcode parser state
          // TODO: Move this to realtime commands for GUIs to request this data during suspend-state.
          report_gcode_modes();
          break;
        case 'C' : // Set check g-code mode [IDLE/CHECK]
          // Perform reset when toggling off. Check g-code mode should only work if Grbl
          // is idle and ready, regardless of alarm locks. This is mainly to keep things
          // simple and consistent.
          if ( sys.state == STATE_CHECK_MODE ) {
            mc_reset();
            report_feedback_message(MESSAGE_DISABLED);
          } else {
            if (sys.state) { return(STATUS_IDLE_ERROR); } // Requires no alarm mode.
            sys.state = STATE_CHECK_MODE;
            report_feedback_message(MESSAGE_ENABLED);
          }
          break;
        case 'X' : // Disable alarm lock [ALARM]
          if (sys.state == STATE_ALARM) {
            // Port: not while the central STOP holds, nor with the VFD in fault
            // (which gets its reset pulse now; unlock again once it cleared).
            if (estop_latched || estop_active()) { return(STATUS_ESTOP_ACTIVE); }
            if (!drvpwr_ok()) { return(STATUS_DRIVER_POWER); }
            system_reset_pulses();
            if (spindle_fault_active()) { return(STATUS_SPINDLE_FAULT); }
            // Block if safety door is ajar.
            if (system_check_safety_door_ajar()) { return(STATUS_CHECK_DOOR); }
            report_feedback_message(MESSAGE_ALARM_UNLOCK);
            system_last_alarm = 0;
            sys.state = STATE_IDLE;
            // Don't run startup script. Prevents stored moves in startup from causing accidents.
          } // Otherwise, no effect.
          break;
      }
      break;
    default :
      // Block any system command that requires the state as IDLE/ALARM. (i.e. EEPROM, homing)
      if ( !(sys.state == STATE_IDLE || sys.state == STATE_ALARM) ) { return(STATUS_IDLE_ERROR); }
      switch( line[1] ) {
        case '#' : // Print Grbl NGC parameters
          if ( line[2] != 0 ) { return(STATUS_INVALID_STATEMENT); }
          else { report_ngc_parameters(); }
          break;
        case 'H' : // Perform homing cycle [IDLE/ALARM]
          if (bit_isfalse(settings.flags,BITFLAG_HOMING_ENABLE)) {return(STATUS_SETTING_DISABLED); }
          if (system_check_safety_door_ajar()) { return(STATUS_CHECK_DOOR); } // Block if safety door is ajar.
          // Work out which cycle before entering the homing state, so that a
          // mistyped $H command leaves the state (alarm lock included) alone.
          helper_var = HOMING_CYCLE_ALL;
          if (line[2] != 0) {
            #ifdef HOMING_SINGLE_AXIS_COMMANDS
              if (line[3] != 0) { return(STATUS_INVALID_STATEMENT); }
              switch (line[2]) {
                case 'X': helper_var = HOMING_CYCLE_X; break;
                case 'Y': helper_var = HOMING_CYCLE_Y; break;
                case 'Z': helper_var = HOMING_CYCLE_Z; break;
                case 'A':
                  // A slave is homed together with its master ($HX / $HY).
                  if (axis_a_is_slave()) { return(STATUS_SETTING_DISABLED); }
                  helper_var = HOMING_CYCLE_A;
                  break;
                default: return(STATUS_INVALID_STATEMENT);
              }
            #else
              return(STATUS_INVALID_STATEMENT);
            #endif
          }
          // Port: not while the central STOP holds or the VFD is in fault, and
          // only if every axis has a switch at the end $23 homes it to.
          if (estop_latched || estop_active()) { return(STATUS_ESTOP_ACTIVE); }
          if (!drvpwr_ok()) { return(STATUS_DRIVER_POWER); }
          if (spindle_fault_active()) { return(STATUS_SPINDLE_FAULT); }
          {
            uint8_t cycles = helper_var;
            if (cycles == HOMING_CYCLE_ALL) {
              cycles = HOMING_CYCLE_0;
              #ifdef HOMING_CYCLE_1
                cycles |= HOMING_CYCLE_1;
              #endif
              #ifdef HOMING_CYCLE_2
                cycles |= HOMING_CYCLE_2;
              #endif
              if (system_axis_a_homed()) { cycles |= HOMING_CYCLE_A; }
            }
            uint8_t status = limits_check_homing(cycles);
            if (status != STATUS_OK) { return(status); }
          }
          sys.state = STATE_HOMING; // Set system state variable
          mc_homing_cycle(helper_var);
          if (!sys.abort) {  // Execute startup scripts after successful homing.
            system_last_alarm = 0;
            sys.state = STATE_IDLE; // Set to IDLE when complete.
            st_go_idle(); // Set steppers to the settings idle state before returning.
            if (line[2] == 0) { system_execute_startup(line); }
          }
          break;
        case 'S' : // Puts Grbl to sleep [IDLE/ALARM]
          if ((line[2] != 'L') || (line[3] != 'P') || (line[4] != 0)) { return(STATUS_INVALID_STATEMENT); }
          system_set_exec_state_flag(EXEC_SLEEP); // Set to execute sleep mode immediately
          break;
        case 'I' : // Print or store build info. [IDLE/ALARM]
          if ( line[++char_counter] == 0 ) {
            settings_read_build_info(line);
            report_build_info(line);
          #ifdef ENABLE_BUILD_INFO_WRITE_COMMAND
            } else { // Store startup line [IDLE/ALARM]
              if(line[char_counter++] != '=') { return(STATUS_INVALID_STATEMENT); }
              helper_var = char_counter; // Set helper variable as counter to start of user info line.
              do {
                line[char_counter-helper_var] = line[char_counter];
              } while (line[char_counter++] != 0);
              settings_store_build_info(line);
          #endif
          }
          break;
        case 'R' : // Restore defaults [IDLE/ALARM]
          if ((line[2] != 'S') || (line[3] != 'T') || (line[4] != '=') || (line[6] != 0)) { return(STATUS_INVALID_STATEMENT); }
          switch (line[5]) {
            #ifdef ENABLE_RESTORE_EEPROM_DEFAULT_SETTINGS
              case '$': settings_restore(SETTINGS_RESTORE_DEFAULTS); break;
            #endif
            #ifdef ENABLE_RESTORE_EEPROM_CLEAR_PARAMETERS
              case '#': settings_restore(SETTINGS_RESTORE_PARAMETERS); break;
            #endif
            #ifdef ENABLE_RESTORE_EEPROM_WIPE_ALL
              case '*': settings_restore(SETTINGS_RESTORE_ALL); break;
            #endif
            default: return(STATUS_INVALID_STATEMENT);
          }
          report_feedback_message(MESSAGE_RESTORE_DEFAULTS);
          mc_reset(); // Force reset to ensure settings are initialized correctly.
          break;
        case 'N' : // Startup lines. [IDLE/ALARM]
          if ( line[++char_counter] == 0 ) { // Print startup lines
            for (helper_var=0; helper_var < N_STARTUP_LINE; helper_var++) {
              if (!(settings_read_startup_line(helper_var, line))) {
                report_status_message(STATUS_SETTING_READ_FAIL);
              } else {
                report_startup_line(helper_var,line);
              }
            }
            break;
          } else { // Store startup line [IDLE Only] Prevents motion during ALARM.
            if (sys.state != STATE_IDLE) { return(STATUS_IDLE_ERROR); } // Store only when idle.
            helper_var = true;  // Set helper_var to flag storing method.
            // No break. Continues into default: to read remaining command characters.
          }
        default :  // Storing setting methods [IDLE/ALARM]
          if(!read_float(line, &char_counter, &parameter)) { return(STATUS_BAD_NUMBER_FORMAT); }
          if(line[char_counter++] != '=') { return(STATUS_INVALID_STATEMENT); }
          if (helper_var) { // Store startup line
            // Prepare sending gcode block to gcode parser by shifting all characters
            helper_var = char_counter; // Set helper variable as counter to start of gcode block
            do {
              line[char_counter-helper_var] = line[char_counter];
            } while (line[char_counter++] != 0);
            // Execute gcode block to ensure block is valid.
            helper_var = gc_execute_line(line); // Set helper_var to returned status code.
            if (helper_var) { return(helper_var); }
            else {
              helper_var = trunc(parameter); // Set helper_var to int value of parameter
              settings_store_startup_line(helper_var,line);
            }
          } else { // Store global setting.
            if(!read_float(line, &char_counter, &value)) { return(STATUS_BAD_NUMBER_FORMAT); }
            if((line[char_counter] != 0) || (parameter > 255)) { return(STATUS_INVALID_STATEMENT); }
            return(settings_store_global_setting((uint8_t)parameter, value));
          }
      }
  }
  return(STATUS_OK); // If '$' command makes it to here, then everything's ok.
}



void system_flag_wco_change()
{
  #ifdef FORCE_BUFFER_SYNC_DURING_WCO_CHANGE
    protocol_buffer_synchronize();
  #endif
  sys.report_wco_counter = 0;
}


// Returns machine position of axis 'idx'. Must be sent a 'step' array.
// NOTE: If motor steps and machine position are not in the same coordinate frame, this function
//   serves as a central place to compute the transformation.
float system_convert_axis_steps_to_mpos(int32_t *steps, uint8_t idx)
{
  float pos;
  if (kinematics_is_corexy()) {
    if (idx==X_AXIS) {
      pos = (float)kinematics_motors_to_x_steps(steps) / settings.steps_per_mm[idx];
    } else if (idx==Y_AXIS) {
      pos = (float)kinematics_motors_to_y_steps(steps) / settings.steps_per_mm[idx];
    } else {
      pos = steps[idx]/settings.steps_per_mm[idx];
    }
  } else {
    pos = steps[idx]/settings.steps_per_mm[idx];
  }
  return(pos);
}


void system_convert_array_steps_to_mpos(float *position, int32_t *steps)
{
  uint8_t idx;
  for (idx=0; idx<N_AXIS; idx++) {
    position[idx] = system_convert_axis_steps_to_mpos(steps, idx);
  }
  return;
}


// CoreXY step conversions now live in kinematics.c, since they are selected at
// run time by the $40 setting rather than at compile time.


// Checks and reports if target array exceeds machine travel limits.
uint8_t system_axis_a_homed()
{
  return(!axis_a_is_slave() && settings.a_homing);
}


uint8_t system_check_travel_limits(float *target)
{
  uint8_t idx;
  for (idx=0; idx<N_AXIS; idx++) {
    // An A axis that $H does not home has no known machine position (and a
    // slave has no position at all), so there is nothing to check it against.
    if ((idx == A_AXIS) && !system_axis_a_homed()) { continue; }
    #ifdef HOMING_FORCE_SET_ORIGIN
      // When homing forced set origin is enabled, soft limits checks need to account for directionality.
      // NOTE: max_travel is stored as negative
      if (bit_istrue(settings.homing_dir_mask,bit(idx))) {
        if (target[idx] < 0 || target[idx] > -settings.max_travel[idx]) { return(true); }
      } else {
        if (target[idx] > 0 || target[idx] < settings.max_travel[idx]) { return(true); }
      }
    #else
      // NOTE: max_travel is stored as negative
      if (target[idx] > 0 || target[idx] < settings.max_travel[idx]) { return(true); }
    #endif
  }
  return(false);
}


// Special handlers for setting and clearing Grbl's real-time execution flags.
void system_set_exec_state_flag(uint8_t mask) {
  uint32_t sreg = GRBL_IRQ_SAVE();
  sys_rt_exec_state |= (mask);
  GRBL_IRQ_RESTORE(sreg);
}

void system_clear_exec_state_flag(uint8_t mask) {
  uint32_t sreg = GRBL_IRQ_SAVE();
  sys_rt_exec_state &= ~(mask);
  GRBL_IRQ_RESTORE(sreg);
}

void system_set_exec_alarm(uint8_t code) {
  uint32_t sreg = GRBL_IRQ_SAVE();
  sys_rt_exec_alarm = code;
  GRBL_IRQ_RESTORE(sreg);
}

void system_clear_exec_alarm() {
  uint32_t sreg = GRBL_IRQ_SAVE();
  sys_rt_exec_alarm = 0;
  GRBL_IRQ_RESTORE(sreg);
}

void system_set_exec_motion_override_flag(uint8_t mask) {
  uint32_t sreg = GRBL_IRQ_SAVE();
  sys_rt_exec_motion_override |= (mask);
  GRBL_IRQ_RESTORE(sreg);
}

void system_set_exec_accessory_override_flag(uint8_t mask) {
  uint32_t sreg = GRBL_IRQ_SAVE();
  sys_rt_exec_accessory_override |= (mask);
  GRBL_IRQ_RESTORE(sreg);
}

void system_clear_exec_motion_overrides() {
  uint32_t sreg = GRBL_IRQ_SAVE();
  sys_rt_exec_motion_override = 0;
  GRBL_IRQ_RESTORE(sreg);
}

void system_clear_exec_accessory_overrides() {
  uint32_t sreg = GRBL_IRQ_SAVE();
  sys_rt_exec_accessory_override = 0;
  GRBL_IRQ_RESTORE(sreg);
}
