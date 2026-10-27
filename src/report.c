/*
  report.c - reporting and messaging methods
  Part of Grbl

  Copyright (c) 2012-2016 Sungeun K. Jeon for Gnea Research LLC

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

/*
  This file functions as the primary feedback interface for Grbl. Any outgoing data, such
  as the protocol status messages, feedback messages, and status reports, are stored here.
  For the most part, these functions primarily are called from protocol.c methods. If a
  different style feedback is desired (i.e. JSON), then a user can change these following
  methods to accomodate their needs.
*/

#include "grbl.h"


// Internal report utilities to reduce flash with repetitive tasks turned into functions.
void report_util_setting_prefix(uint8_t n) { serial_write('$'); print_uint8_base10(n); serial_write('='); }
static void report_util_line_feed() { printPgmString(PSTR("\r\n")); }
static void report_util_feedback_line_feed() { serial_write(']'); report_util_line_feed(); }
static void report_util_gcode_modes_G() { printPgmString(PSTR(" G")); }
static void report_util_gcode_modes_M() { printPgmString(PSTR(" M")); }
// static void report_util_comment_line_feed() { serial_write(')'); report_util_line_feed(); }
static void report_util_axis_values(float *axis_value) {
  uint8_t idx;
  for (idx=0; idx<N_AXIS; idx++) {
    printFloat_CoordValue(axis_value[idx]);
    if (idx < (N_AXIS-1)) { serial_write(','); }
  }
}

/*
static void report_util_setting_string(uint8_t n) {
  serial_write(' ');
  serial_write('(');
  switch(n) {
    case 0: printPgmString(PSTR("stp pulse")); break;
    case 1: printPgmString(PSTR("idl delay")); break;
    case 2: printPgmString(PSTR("stp inv")); break;
    case 3: printPgmString(PSTR("dir inv")); break;
    case 4: printPgmString(PSTR("stp en inv")); break;
    case 5: printPgmString(PSTR("lim inv")); break;
    case 6: printPgmString(PSTR("prb inv")); break;
    case 10: printPgmString(PSTR("rpt")); break;
    case 11: printPgmString(PSTR("jnc dev")); break;
    case 12: printPgmString(PSTR("arc tol")); break;
    case 13: printPgmString(PSTR("rpt inch")); break;
    case 20: printPgmString(PSTR("sft lim")); break;
    case 21: printPgmString(PSTR("hrd lim")); break;
    case 22: printPgmString(PSTR("hm cyc")); break;
    case 23: printPgmString(PSTR("hm dir inv")); break;
    case 24: printPgmString(PSTR("hm feed")); break;
    case 25: printPgmString(PSTR("hm seek")); break;
    case 26: printPgmString(PSTR("hm delay")); break;
    case 27: printPgmString(PSTR("hm pulloff")); break;
    case 30: printPgmString(PSTR("rpm max")); break;
    case 31: printPgmString(PSTR("rpm min")); break;
    case 32: printPgmString(PSTR("laser")); break;
    default:
      n -= AXIS_SETTINGS_START_VAL;
      uint8_t idx = 0;
      while (n >= AXIS_SETTINGS_INCREMENT) {
        n -= AXIS_SETTINGS_INCREMENT;
        idx++;
      }
      serial_write(n+'x');
      switch (idx) {
        case 0: printPgmString(PSTR(":stp/mm")); break;
        case 1: printPgmString(PSTR(":mm/min")); break;
        case 2: printPgmString(PSTR(":mm/s^2")); break;
        case 3: printPgmString(PSTR(":mm max")); break;
      }
      break;
  }
  report_util_comment_line_feed();
}
*/

static void report_util_float_setting(uint8_t n, float val, uint8_t n_decimal) {
  report_util_setting_prefix(n);
  printFloat(val,n_decimal);
  report_util_line_feed(); // report_util_setting_string(n);
}


// Handles the primary confirmation protocol response for streaming interfaces and human-feedback.
// For every incoming line, this method responds with an 'ok' for a successful command or an
// 'error:'  to indicate some error event with the line or some critical system error during
// operation. Errors events can originate from the g-code parser, settings module, or asynchronously
// from a critical error, such as a triggered hard limit. Interface should always monitor for these
// responses.
void report_status_message(uint8_t status_code)
{
  switch(status_code) {
    case STATUS_OK: // STATUS_OK
      printPgmString(PSTR("ok\r\n")); break;
    default:
      display_note_error(status_code);
      printPgmString(PSTR("error:"));
      print_uint8_base10(status_code);
      report_util_line_feed();
  }
}

// Prints alarm messages.
void report_alarm_message(uint8_t alarm_code)
{
  display_note_alarm(alarm_code);
  system_last_alarm = alarm_code;
  printPgmString(PSTR("ALARM:"));
  print_uint8_base10(alarm_code);
  report_util_line_feed();
  // Port: which switch, where Grbl knows ([MSG:Limit X+]; X alone for two
  // switches on one input), and the STOP.
  if (alarm_code == EXEC_ALARM_HARD_LIMIT) { report_feedback_message(MESSAGE_LIMIT); }
  if (alarm_code == EXEC_ALARM_ESTOP) { report_feedback_message(MESSAGE_ESTOP); }
  // Upstream waits 500 ms here so the UART can drain before a possible reset.
  // USB output is pumped by the main loop, so drain it explicitly instead.
  hal_serial_flush();
}

// Prints feedback messages. This serves as a centralized method to provide additional
// user feedback for things that are not of the status/alarm message protocol. These are
// messages such as setup warnings, switch toggling, and how to exit alarms.
// NOTE: For interfaces, messages are always placed within brackets. And if silent mode
// is installed, the message number codes are less than zero.
void report_feedback_message(uint8_t message_code)
{
  printPgmString(PSTR("[MSG:"));
  switch(message_code) {
    case MESSAGE_CRITICAL_EVENT:
      printPgmString(PSTR("Reset to continue")); break;
    case MESSAGE_ALARM_LOCK:
      printPgmString(PSTR("'$H'|'$X' to unlock")); break;
    case MESSAGE_ALARM_UNLOCK:
      printPgmString(PSTR("Caution: Unlocked")); break;
    case MESSAGE_ENABLED:
      printPgmString(PSTR("Enabled")); break;
    case MESSAGE_DISABLED:
      printPgmString(PSTR("Disabled")); break;
    case MESSAGE_SAFETY_DOOR_AJAR:
      printPgmString(PSTR("Check Door")); break;
    case MESSAGE_CHECK_LIMITS:
      printPgmString(PSTR("Check Limits")); break;
    case MESSAGE_PROGRAM_END:
      printPgmString(PSTR("Pgm End")); break;
    case MESSAGE_RESTORE_DEFAULTS:
      printPgmString(PSTR("Restoring defaults")); break;
    case MESSAGE_SPINDLE_RESTORE:
      printPgmString(PSTR("Restoring spindle")); break;
    case MESSAGE_SLEEP_MODE:
      printPgmString(PSTR("Sleeping")); break;
    case MESSAGE_ESTOP:
      printPgmString(PSTR("E-stop")); break;
    case MESSAGE_DRIVER_POWER_ON:
      printPgmString(PSTR("Driver power on")); break;
    case MESSAGE_PANEL_STOP:
      printPgmString(PSTR("Panel STOP")); break;
    case MESSAGE_LIMIT: {
      static const char axis_letter[N_AXIS] = { 'X', 'Y', 'Z', 'A' };
      uint8_t sw = limits_last_trigger(), axis, first = true;
      printPgmString(PSTR("Limit"));
      for (axis = 0; axis < N_AXIS; axis++) {
        uint8_t pair = (sw >> (2*axis)) & 3;
        if (!pair) { continue; }
        serial_write(first ? ' ' : ',');
        first = false;
        serial_write(axis_letter[axis]);
        if (pair == 1) { serial_write('-'); }
        else if (pair == 2) { serial_write('+'); }
      }
      break;
    }
    case MESSAGE_TOOL_CHANGE:
      printPgmString(PSTR("Tool change T"));
      print_uint8_base10(gc_state.tool);
      printPgmString(PSTR(", cycle start to resume"));
      break;
  }
  report_util_feedback_line_feed();
}


// Welcome message
void report_init_message()
{
  // Keep this line byte-identical to AVR Grbl: senders match on it to detect
  // the controller. The platform goes on its own [MSG:] line, which every
  // sender already knows how to treat as informational.
  printPgmString(PSTR("\r\nGrbl " GRBL_VERSION " ['$' for help]\r\n"));
  // Platform line, printed at every reset: the A axis role, whether the
  // settings flash is usable, and whether the display core is running.
  printPgmString(PSTR("[MSG:" GRBL_PLATFORM " A="));
  switch (settings.a_mode) {
    case A_MODE_ROTARY:  printPgmString(PSTR("rotary")); break;
    case A_MODE_LINEAR:  printPgmString(PSTR("linear")); break;
    case A_MODE_SLAVE_X: printPgmString(PSTR("slave-X")); break;
    default:             printPgmString(PSTR("slave-Y")); break;
  }
  printPgmString(PSTR(" nvs="));
  print_uint8_base10(hal_nvs_ok() ? 1 : 0);
  printPgmString(PSTR(" display="));
  print_uint8_base10(hal_display_present() ? 1 : 0);
  printPgmString(PSTR("]\r\n"));
}

// Grbl help message
void report_grbl_help() {
  printPgmString(PSTR("[HLP:$$ $# $G $I $N $x=val $Nx=line $J=line $SLP $C $X $H $HX $HY $HZ $HA ~ ! ? ctrl-x]\r\n"));
}


// Grbl global settings print out.
// NOTE: The numbering scheme here must correlate to storing in settings.c
void report_grbl_settings() {
  // Print the global settings in the order of settings_global_numbers[], then
  // the axis settings. Values come from settings_value(), which the display's
  // $$ page uses as well.
  uint8_t i;
  for (i = 0; i < N_GLOBAL_SETTINGS; i++) {
    uint8_t n = settings_global_numbers[i];
    if ((n == 30) || (n == 31)) {
      report_util_float_setting(n, settings_value(n), N_DECIMAL_RPMVALUE);
    } else if (settings_is_float(n)) {
      report_util_float_setting(n, settings_value(n), N_DECIMAL_SETTINGVALUE);
    } else {
      report_util_setting_prefix(n);
      print_uint32_base10((uint32_t)settings_value(n));
      report_util_line_feed();
    }
  }
  // Print axis settings
  uint8_t idx, set_idx;
  uint8_t val = AXIS_SETTINGS_START_VAL;
  for (set_idx=0; set_idx<AXIS_N_SETTINGS; set_idx++) {
    for (idx=0; idx<N_AXIS; idx++) {
      report_util_float_setting(val+idx, settings_value(val+idx), N_DECIMAL_SETTINGVALUE);
    }
    val += AXIS_SETTINGS_INCREMENT;
  }
}


// Prints current probe parameters. Upon a probe command, these parameters are updated upon a
// successful probe or upon a failed probe with the G38.3 without errors command (if supported).
// These values are retained until Grbl is power-cycled, whereby they will be re-zeroed.
void report_probe_parameters()
{
  // Report in terms of machine position.
  printPgmString(PSTR("[PRB:"));
  float print_position[N_AXIS];
  system_convert_array_steps_to_mpos(print_position,sys_probe_position);
  report_util_axis_values(print_position);
  serial_write(':');
  print_uint8_base10(sys.probe_succeeded);
  report_util_feedback_line_feed();
}


// Prints Grbl NGC parameters (coordinate offsets, probing)
void report_ngc_parameters()
{
  float coord_data[N_AXIS];
  uint8_t coord_select;
  for (coord_select = 0; coord_select <= SETTING_INDEX_NCOORD; coord_select++) {
    if (!(settings_read_coord_data(coord_select,coord_data))) {
      report_status_message(STATUS_SETTING_READ_FAIL);
      return;
    }
    printPgmString(PSTR("[G"));
    switch (coord_select) {
      case 6: printPgmString(PSTR("28")); break;
      case 7: printPgmString(PSTR("30")); break;
      default: print_uint8_base10(coord_select+54); break; // G54-G59
    }
    serial_write(':');
    report_util_axis_values(coord_data);
    report_util_feedback_line_feed();
  }
  printPgmString(PSTR("[G92:")); // Print G92,G92.1 which are not persistent in memory
  report_util_axis_values(gc_state.coord_offset);
  report_util_feedback_line_feed();
  printPgmString(PSTR("[TLO:")); // Print tool length offset value
  printFloat_CoordValue(gc_state.tool_length_offset);
  report_util_feedback_line_feed();
  report_probe_parameters(); // Print probe parameters. Not persistent in memory.
}


// Print current gcode parser mode state
void report_gcode_modes()
{
  printPgmString(PSTR("[GC:G"));
  if (gc_state.modal.motion >= MOTION_MODE_PROBE_TOWARD) {
    printPgmString(PSTR("38."));
    print_uint8_base10(gc_state.modal.motion - (MOTION_MODE_PROBE_TOWARD-2));
  } else {
    print_uint8_base10(gc_state.modal.motion);
  }

  report_util_gcode_modes_G();
  print_uint8_base10(gc_state.modal.coord_select+54);

  report_util_gcode_modes_G();
  print_uint8_base10(gc_state.modal.plane_select+17);

  report_util_gcode_modes_G();
  print_uint8_base10(21-gc_state.modal.units);

  report_util_gcode_modes_G();
  print_uint8_base10(gc_state.modal.distance+90);

  report_util_gcode_modes_G();
  print_uint8_base10(94-gc_state.modal.feed_rate);

  if (gc_state.modal.program_flow) {
    report_util_gcode_modes_M();
    switch (gc_state.modal.program_flow) {
      case PROGRAM_FLOW_PAUSED : serial_write('0'); break;
      // case PROGRAM_FLOW_OPTIONAL_STOP : serial_write('1'); break; // M1 is ignored and not supported.
      case PROGRAM_FLOW_COMPLETED_M2 :
      case PROGRAM_FLOW_COMPLETED_M30 :
        print_uint8_base10(gc_state.modal.program_flow);
        break;
    }
  }

  report_util_gcode_modes_M();
  switch (gc_state.modal.spindle) {
    case SPINDLE_ENABLE_CW : serial_write('3'); break;
    case SPINDLE_ENABLE_CCW : serial_write('4'); break;
    case SPINDLE_DISABLE : serial_write('5'); break;
  }

  #ifdef ENABLE_M7
    if (gc_state.modal.coolant) { // Note: Multiple coolant states may be active at the same time.
      if (gc_state.modal.coolant & PL_COND_FLAG_COOLANT_MIST) { report_util_gcode_modes_M(); serial_write('7'); }
      if (gc_state.modal.coolant & PL_COND_FLAG_COOLANT_FLOOD) { report_util_gcode_modes_M(); serial_write('8'); }
    } else { report_util_gcode_modes_M(); serial_write('9'); }
  #else
    report_util_gcode_modes_M();
    if (gc_state.modal.coolant) { serial_write('8'); }
    else { serial_write('9'); }
  #endif

  #ifdef ENABLE_PARKING_OVERRIDE_CONTROL
    if (sys.override_ctrl == OVERRIDE_PARKING_MOTION) {
      report_util_gcode_modes_M();
      print_uint8_base10(56);
    }
  #endif

  printPgmString(PSTR(" T"));
  print_uint8_base10(gc_state.tool);

  printPgmString(PSTR(" F"));
  printFloat_RateValue(gc_state.feed_rate);

  #ifdef VARIABLE_SPINDLE
    printPgmString(PSTR(" S"));
    printFloat(gc_state.spindle_speed,N_DECIMAL_RPMVALUE);
  #endif

  report_util_feedback_line_feed();
}

// Prints specified startup line
void report_startup_line(uint8_t n, char *line)
{
  printPgmString(PSTR("$N"));
  print_uint8_base10(n);
  serial_write('=');
  printString(line);
  report_util_line_feed();
}

void report_execute_startup_message(char *line, uint8_t status_code)
{
  serial_write('>');
  printString(line);
  serial_write(':');
  report_status_message(status_code);
}

// Prints build info line
void report_build_info(char *line)
{
  printPgmString(PSTR("[VER:" GRBL_VERSION "." GRBL_VERSION_BUILD ":"));
  printString(line);
  report_util_feedback_line_feed();
  printPgmString(PSTR("[OPT:")); // Generate compile-time build option list
  #ifdef VARIABLE_SPINDLE
    serial_write('V');
  #endif
  #ifdef USE_LINE_NUMBERS
    serial_write('N');
  #endif
  #ifdef ENABLE_M7
    serial_write('M');
  #endif
  if (kinematics_is_corexy()) { serial_write('C'); }
  #ifdef PARKING_ENABLE
    serial_write('P');
  #endif
  #ifdef HOMING_FORCE_SET_ORIGIN
    serial_write('Z');
  #endif
  #ifdef HOMING_SINGLE_AXIS_COMMANDS
    serial_write('H');
  #endif
  #ifdef LIMITS_TWO_SWITCHES_ON_AXES
    serial_write('T');
  #endif
  #ifdef ALLOW_FEED_OVERRIDE_DURING_PROBE_CYCLES
    serial_write('A');
  #endif
  #ifdef USE_SPINDLE_DIR_AS_ENABLE_PIN
    serial_write('D');
  #endif
  #ifdef SPINDLE_ENABLE_OFF_WITH_ZERO_SPEED
    serial_write('0');
  #endif
  #ifdef ENABLE_SOFTWARE_DEBOUNCE
    serial_write('S');
  #endif
  #ifdef ENABLE_PARKING_OVERRIDE_CONTROL
    serial_write('R');
  #endif
  #ifndef HOMING_INIT_LOCK
    serial_write('L');
  #endif
  #ifdef ENABLE_SAFETY_DOOR_INPUT_PIN
    serial_write('+');
  #endif
  #ifndef ENABLE_RESTORE_EEPROM_WIPE_ALL // NOTE: Shown when disabled.
    serial_write('*');
  #endif
  #ifndef ENABLE_RESTORE_EEPROM_DEFAULT_SETTINGS // NOTE: Shown when disabled.
    serial_write('$');
  #endif
  #ifndef ENABLE_RESTORE_EEPROM_CLEAR_PARAMETERS // NOTE: Shown when disabled.
    serial_write('#');
  #endif
  #ifndef ENABLE_BUILD_INFO_WRITE_COMMAND // NOTE: Shown when disabled.
    serial_write('I');
  #endif
  #ifndef FORCE_BUFFER_SYNC_DURING_EEPROM_WRITE // NOTE: Shown when disabled.
    serial_write('E');
  #endif
  #ifndef FORCE_BUFFER_SYNC_DURING_WCO_CHANGE // NOTE: Shown when disabled.
    serial_write('W');
  #endif
  #ifdef ENABLE_DUAL_AXIS
    serial_write('2');
  #endif
  // NOTE: Compiled values, like override increments/max/min values, may be added at some point later.
  serial_write(',');
  print_uint8_base10(BLOCK_BUFFER_SIZE-1);
  serial_write(',');
  print_uint32_base10(RX_BUFFER_SIZE);

  report_util_feedback_line_feed();

  // Port diagnostics, on their own [MSG:] line so senders ignore it. Counters
  // since power-up: uptime in ms, input scans performed (should track
  // uptime x INPUT_SCAN_FREQUENCY), stepper interrupts, pulse terminations
  // (should equal stepper interrupts while stepping), and the transport
  // health counters from the banner.
  printPgmString(PSTR("[MSG:" GRBL_PLATFORM " up="));
  print_uint32_base10(hal_millis());
  printPgmString(PSTR(" scan="));
  print_uint32_base10(hal_scan_ticks);
  printPgmString(PSTR(" step="));
  print_uint32_base10(hal_diag_stepper_ticks);
  printPgmString(PSTR(" pulse="));
  print_uint32_base10(hal_diag_pulse_ticks);
  printPgmString(PSTR(" merged="));
  print_uint32_base10(hal_diag_pulse_overrun);
  printPgmString(PSTR(" floor="));
  print_uint32_base10(hal_diag_period_floor);
  printPgmString(PSTR(" spur="));
  print_uint32_base10(hal_diag_stepper_spurious);
  printPgmString(PSTR(" lat="));
  print_uint32_base10(hal_diag_isr_lat_max / HAL_TICKS_PER_US);
  printPgmString(PSTR("/"));
  print_uint32_base10(hal_diag_isr_lat_over);
  printPgmString(PSTR(" tx="));
  print_uint32_base10(hal_serial_tx_stall_count());
  printPgmString(PSTR(" drop="));
  print_uint32_base10(serial_rx_dropped);
  printPgmString(PSTR(" motor="));
  { uint8_t idx;
    for (idx = 0; idx < N_AXIS; idx++) {
      if (idx) { serial_write(','); }
      int32_t v = st_diag_motor[idx];
      if (v < 0) { serial_write('-'); v = -v; }
      print_uint32_base10((uint32_t)v);
    }
  }
  // Input channels (bit n-1 = INn after $79, bit 20 = STOP), the input
  // functions active (bit = $60 + n), the output channels (bit n-1 = OUTn,
  // logical, before $93) and the output functions on (bit = $80 + n), as the
  // display shows them; then drives, brake, STOP and the $56 counters.
  printPgmString(PSTR(" in="));
  { uint32_t ch = io_read_channels();
    print_uint32_base10(ch);
    serial_write('/');
    print_uint32_base10(io_functions(ch)); }
  printPgmString(PSTR(" out="));
  print_uint32_base10(io_out_state());
  serial_write('/');
  print_uint32_base10(outputs_get_mask());
  printPgmString(PSTR(" en="));
  print_uint8_base10(st_drivers_enabled());
  printPgmString(PSTR(" brake="));
  print_uint8_base10(st_brake_released());
  printPgmString(PSTR(" estop="));
  print_uint8_base10(estop_latched ? 2 : (estop_active() ? 1 : 0));
  serial_write('/');
  print_uint32_base10(estop_trip_count());
  serial_write('/');
  print_uint32_base10(estop_trip_time_tenth_us());
  serial_write('/');
  print_uint8_base10(estop_trip_source());
  // Driver power: drvpwr_state() (0 not watched, 1 off, 2 back but $95 not
  // over, 3 on) and the losses since power-up.
  printPgmString(PSTR(" drv="));
  print_uint8_base10(drvpwr_state());
  serial_write('/');
  print_uint32_base10(drvpwr_loss_count());
  // Handwheel: mpg_state() (0 off, 1 no axis, 2 ready, 3 blocked), axis
  // (0 none, 1-4 = X Y Z A), step multiplier, TIM8 count, selector inputs,
  // the LED output.
  printPgmString(PSTR(" mpg="));
  print_uint8_base10(mpg_state());
  serial_write('/');
  print_uint8_base10((mpg_axis() < N_AXIS) ? mpg_axis() + 1 : 0);
  serial_write('/');
  print_uint8_base10(mpg_mult());
  serial_write('/');
  print_uint32_base10(mpg_counter());
  serial_write('/');
  print_uint8_base10(mpg_inputs());
  serial_write('/');
  print_uint8_base10(hal_pin_read(MPG_LED_PIN) ? 1 : 0);
  printPgmString(PSTR(" spindle="));
  print_uint8_base10(spindle_status());
  vfd_report_diag();
  // Operator panel: buttons pressed (bit 0 START, 1 HOLD, 2 STOP) and the
  // potentiometers' readings (0-4095, 0 when off).
  printPgmString(PSTR(" pnl="));
  print_uint8_base10(panel_buttons());
  serial_write('/');
  print_uint32_base10(panel_pot(0));
  serial_write('/');
  print_uint32_base10(panel_pot(1));
  printPgmString(PSTR(" dir="));
  print_uint32_base10(st_diag_dir_delays);
  serial_write('/');
  print_uint32_base10((st_diag_dir_gap_min == 0xFFFFFFFFUL) ? 0 : (st_diag_dir_gap_min * 10UL) / hal_cycles_per_us());
  printPgmString(PSTR(" tin="));
  print_uint32_base10(io_test_mask());
  // Longest scan / stepper interrupt in 0.1 us; a read resets them.
  printPgmString(PSTR(" isr="));
  print_uint32_base10(hal_diag_scan_cycles_max * 10UL / hal_cycles_per_us());
  serial_write('/');
  print_uint32_base10(hal_diag_stepper_cycles_max * 10UL / hal_cycles_per_us());
  hal_diag_scan_cycles_max = 0;
  hal_diag_stepper_cycles_max = 0;
  printPgmString(PSTR(" pwm="));
  print_uint32_base10(hal_spindle_pwm_readback());
  printPgmString(PSTR(" dac="));
  print_uint32_base10(hal_spindle_dac_readback());
  // The level of D6 (OUT13 or the PWM) as the pin reads back.
  printPgmString(PSTR(" d6="));
  print_uint8_base10(hal_pin_read(SPINDLE_PWM_PIN) ? 1 : 0);
  printPgmString(PSTR(" tim="));
  print_uint32_base10(hal_timer_clock_hz() / 1000000UL);
  serial_write('/');
  print_uint32_base10(hal_timer2_clock_hz() / 1000000UL);
  printPgmString(PSTR(" m4="));
  print_uint32_base10(hal_display_frames());
  serial_write('/');
  print_uint32_base10(hal_display_status());
  display_report_diag();
  printPgmString(PSTR("]\r\n"));
}


// Prints the character string line Grbl has received from the user, which has been pre-parsed,
// and has been sent into protocol_execute_line() routine to be executed by Grbl.
void report_echo_line_received(char *line)
{
  printPgmString(PSTR("[echo: ")); printString(line);
  report_util_feedback_line_feed();
}


 // Prints real-time data. This function grabs a real-time snapshot of the stepper subprogram
 // and the actual location of the CNC machine. Users may change the following function to their
 // specific needs, but the desired real-time data report must be as short as possible. This is
 // requires as it minimizes the computational overhead and allows grbl to keep running smoothly,
 // especially during g-code programs with fast, short line segments and high frequency reports (5-20Hz).
void report_realtime_status()
{
  uint8_t idx;
  int32_t current_position[N_AXIS]; // Copy current state of the system position variable
  memcpy(current_position,sys_position,sizeof(sys_position));
  float print_position[N_AXIS];
  system_convert_array_steps_to_mpos(print_position,current_position);

  // Report current machine state and sub-states
  serial_write('<');
  switch (sys.state) {
    case STATE_IDLE: printPgmString(PSTR("Idle")); break;
    case STATE_CYCLE: printPgmString(PSTR("Run")); break;
    case STATE_HOLD:
      if (!(sys.suspend & SUSPEND_JOG_CANCEL)) {
        printPgmString(PSTR("Hold:"));
        if (sys.suspend & SUSPEND_HOLD_COMPLETE) { serial_write('0'); } // Ready to resume
        else { serial_write('1'); } // Actively holding
        break;
      } // Continues to print jog state during jog cancel.
    case STATE_JOG: printPgmString(PSTR("Jog")); break;
    case STATE_HOMING: printPgmString(PSTR("Home")); break;
    case STATE_ALARM: printPgmString(PSTR("Alarm")); break;
    case STATE_CHECK_MODE: printPgmString(PSTR("Check")); break;
    case STATE_SAFETY_DOOR:
      printPgmString(PSTR("Door:"));
      if (sys.suspend & SUSPEND_INITIATE_RESTORE) {
        serial_write('3'); // Restoring
      } else {
        if (sys.suspend & SUSPEND_RETRACT_COMPLETE) {
          if (sys.suspend & SUSPEND_SAFETY_DOOR_AJAR) {
            serial_write('1'); // Door ajar
          } else {
            serial_write('0');
          } // Door closed and ready to resume
        } else {
          serial_write('2'); // Retracting
        }
      }
      break;
    case STATE_SLEEP: printPgmString(PSTR("Sleep")); break;
  }

  float wco[N_AXIS];
  if (bit_isfalse(settings.status_report_mask,BITFLAG_RT_STATUS_POSITION_TYPE) ||
      (sys.report_wco_counter == 0) ) {
    for (idx=0; idx< N_AXIS; idx++) {
      // Apply work coordinate offsets and tool length offset to current position.
      wco[idx] = gc_state.coord_system[idx]+gc_state.coord_offset[idx];
      if (idx == TOOL_LENGTH_OFFSET_AXIS) { wco[idx] += gc_state.tool_length_offset; }
      if (bit_isfalse(settings.status_report_mask,BITFLAG_RT_STATUS_POSITION_TYPE)) {
        print_position[idx] -= wco[idx];
      }
    }
  }

  // Report machine position
  if (bit_istrue(settings.status_report_mask,BITFLAG_RT_STATUS_POSITION_TYPE)) {
    printPgmString(PSTR("|MPos:"));
  } else {
    printPgmString(PSTR("|WPos:"));
  }
  report_util_axis_values(print_position);

  // Returns planner and serial read buffer states.
  #ifdef REPORT_FIELD_BUFFER_STATE
    if (bit_istrue(settings.status_report_mask,BITFLAG_RT_STATUS_BUFFER_STATE)) {
      printPgmString(PSTR("|Bf:"));
      print_uint8_base10(plan_get_block_buffer_available());
      serial_write(',');
      print_uint32_base10(serial_get_rx_buffer_available());
    }
  #endif

  #ifdef USE_LINE_NUMBERS
    #ifdef REPORT_FIELD_LINE_NUMBERS
      // Report current line number
      plan_block_t * cur_block = plan_get_current_block();
      if (cur_block != NULL) {
        uint32_t ln = cur_block->line_number;
        if (ln > 0) {
          printPgmString(PSTR("|Ln:"));
          printInteger(ln);
        }
      }
    #endif
  #endif

  // Report realtime feed speed
  #ifdef REPORT_FIELD_CURRENT_FEED_SPEED
    #ifdef VARIABLE_SPINDLE
      printPgmString(PSTR("|FS:"));
      printFloat_RateValue(st_get_realtime_rate());
      serial_write(',');
      printFloat(sys.spindle_speed,N_DECIMAL_RPMVALUE);
    #else
      printPgmString(PSTR("|F:"));
      printFloat_RateValue(st_get_realtime_rate());
    #endif
  #endif

  // Port: progress of a program from the USB stick, as grblHAL reports SD card jobs.
  job_report_status();

  #ifdef REPORT_FIELD_PIN_STATE
    uint8_t lim_pin_state = limits_get_state();
    uint8_t ctrl_pin_state = system_control_get_state();
    uint8_t prb_pin_state = probe_get_state();
    uint8_t motor_alarm_state = limits_get_motor_alarm_state();
    uint8_t estop_state = estop_active();
    if (lim_pin_state | ctrl_pin_state | prb_pin_state | motor_alarm_state | estop_state) {
      printPgmString(PSTR("|Pn:"));
      if (estop_state) { serial_write('E'); }   // central STOP (grblHAL's letter)
      if (prb_pin_state) { serial_write('P'); }
      if (lim_pin_state) {
        if (bit_istrue(lim_pin_state,bit(X_AXIS))) { serial_write('X'); }
        if (bit_istrue(lim_pin_state,bit(Y_AXIS))) { serial_write('Y'); }
        if (bit_istrue(lim_pin_state,bit(Z_AXIS))) { serial_write('Z'); }
        // With A as a slave this is the second X/Y switch; reported as A all
        // the same, so the two switches of a squared gantry can be told apart.
        if (bit_istrue(lim_pin_state,bit(A_AXIS))) { serial_write('A'); }
      }
      // Any driver alarm input (grblHAL's letter for a motor fault).
      if (motor_alarm_state) { serial_write('M'); }
      if (ctrl_pin_state) {
        #ifdef ENABLE_SAFETY_DOOR_INPUT_PIN
          if (bit_istrue(ctrl_pin_state,CONTROL_PIN_INDEX_SAFETY_DOOR)) { serial_write('D'); }
        #endif
        if (bit_istrue(ctrl_pin_state,CONTROL_PIN_INDEX_RESET)) { serial_write('R'); }
        if (bit_istrue(ctrl_pin_state,CONTROL_PIN_INDEX_FEED_HOLD)) { serial_write('H'); }
        if (bit_istrue(ctrl_pin_state,CONTROL_PIN_INDEX_CYCLE_START)) { serial_write('S'); }
      }
    }
  #endif

  #ifdef REPORT_FIELD_WORK_COORD_OFFSET
    bool print_wco;
    #ifdef REPORT_VERBOSE_STATUS
      print_wco = true;
    #else
      print_wco = false;
      if (sys.report_wco_counter > 0) { sys.report_wco_counter--; }
      else {
        if (sys.state & (STATE_HOMING | STATE_CYCLE | STATE_HOLD | STATE_JOG | STATE_SAFETY_DOOR)) {
          sys.report_wco_counter = (REPORT_WCO_REFRESH_BUSY_COUNT-1); // Reset counter for slow refresh
        } else { sys.report_wco_counter = (REPORT_WCO_REFRESH_IDLE_COUNT-1); }
        if (sys.report_ovr_counter == 0) { sys.report_ovr_counter = 1; } // Set override on next report.
        print_wco = true;
      }
    #endif
    if (print_wco) {
      printPgmString(PSTR("|WCO:"));
      report_util_axis_values(wco);
    }
  #endif

  #ifdef REPORT_FIELD_OVERRIDES
    bool print_ovr;
    #ifdef REPORT_VERBOSE_STATUS
      print_ovr = true;
    #else
      print_ovr = false;
      if (sys.report_ovr_counter > 0) { sys.report_ovr_counter--; }
      else {
        if (sys.state & (STATE_HOMING | STATE_CYCLE | STATE_HOLD | STATE_JOG | STATE_SAFETY_DOOR)) {
          sys.report_ovr_counter = (REPORT_OVR_REFRESH_BUSY_COUNT-1); // Reset counter for slow refresh
        } else { sys.report_ovr_counter = (REPORT_OVR_REFRESH_IDLE_COUNT-1); }
        print_ovr = true;
      }
    #endif
    if (print_ovr) {
      printPgmString(PSTR("|Ov:"));
      print_uint8_base10(sys.f_override);
      serial_write(',');
      print_uint8_base10(sys.r_override);
      serial_write(',');
      print_uint8_base10(sys.spindle_speed_ovr);

      uint8_t sp_state = spindle_get_state();
      uint8_t cl_state = coolant_get_state();
      if (sp_state || cl_state) {
        printPgmString(PSTR("|A:"));
        if (sp_state) { // != SPINDLE_STATE_DISABLE
          #ifdef VARIABLE_SPINDLE
            #ifdef USE_SPINDLE_DIR_AS_ENABLE_PIN
              serial_write('S'); // CW
            #else
              if (sp_state == SPINDLE_STATE_CW) { serial_write('S'); } // CW
              else { serial_write('C'); } // CCW
            #endif
          #else
            if (sp_state & SPINDLE_STATE_CW) { serial_write('S'); } // CW
            else { serial_write('C'); } // CCW
          #endif
        }
        if (cl_state & COOLANT_STATE_FLOOD) { serial_write('F'); }
        #ifdef ENABLE_M7
          if (cl_state & COOLANT_STATE_MIST) { serial_write('M'); }
        #endif
      }
    }
  #endif

  serial_write('>');
  report_util_line_feed();
}


#ifdef DEBUG
  void report_realtime_debug()
  {

  }
#endif
