/*
  settings.c - eeprom configuration handling
  Part of Grbl

  Copyright (c) 2011-2016 Sungeun K. Jeon for Gnea Research LLC
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
#include <stddef.h>

settings_t settings;

const __flash settings_t defaults = {\
    .pulse_microseconds = DEFAULT_STEP_PULSE_MICROSECONDS,
    .stepper_idle_lock_time = DEFAULT_STEPPER_IDLE_LOCK_TIME,
    .step_invert_mask = DEFAULT_STEPPING_INVERT_MASK,
    .dir_invert_mask = DEFAULT_DIRECTION_INVERT_MASK,
    .status_report_mask = DEFAULT_STATUS_REPORT_MASK,
    .junction_deviation = DEFAULT_JUNCTION_DEVIATION,
    .arc_tolerance = DEFAULT_ARC_TOLERANCE,
    .rpm_max = DEFAULT_SPINDLE_RPM_MAX,
    .rpm_min = DEFAULT_SPINDLE_RPM_MIN,
    .homing_dir_mask = DEFAULT_HOMING_DIR_MASK,
    .homing_feed_rate = DEFAULT_HOMING_FEED_RATE,
    .homing_seek_rate = DEFAULT_HOMING_SEEK_RATE,
    .homing_debounce_delay = DEFAULT_HOMING_DEBOUNCE_DELAY,
    .homing_pulloff = DEFAULT_HOMING_PULLOFF,
    .flags = (DEFAULT_REPORT_INCHES << BIT_REPORT_INCHES) | \
             (DEFAULT_LASER_MODE << BIT_LASER_MODE) | \
             (DEFAULT_INVERT_ST_ENABLE << BIT_INVERT_ST_ENABLE) | \
             (DEFAULT_HARD_LIMIT_ENABLE << BIT_HARD_LIMIT_ENABLE) | \
             (DEFAULT_HOMING_ENABLE << BIT_HOMING_ENABLE) | \
             (DEFAULT_SOFT_LIMIT_ENABLE << BIT_SOFT_LIMIT_ENABLE),
    .steps_per_mm[X_AXIS] = DEFAULT_X_STEPS_PER_MM,
    .steps_per_mm[Y_AXIS] = DEFAULT_Y_STEPS_PER_MM,
    .steps_per_mm[Z_AXIS] = DEFAULT_Z_STEPS_PER_MM,
    .steps_per_mm[A_AXIS] = DEFAULT_A_STEPS_PER_MM,
    .max_rate[X_AXIS] = DEFAULT_X_MAX_RATE,
    .max_rate[Y_AXIS] = DEFAULT_Y_MAX_RATE,
    .max_rate[Z_AXIS] = DEFAULT_Z_MAX_RATE,
    .max_rate[A_AXIS] = DEFAULT_A_MAX_RATE,
    .acceleration[X_AXIS] = DEFAULT_X_ACCELERATION,
    .acceleration[Y_AXIS] = DEFAULT_Y_ACCELERATION,
    .acceleration[Z_AXIS] = DEFAULT_Z_ACCELERATION,
    .acceleration[A_AXIS] = DEFAULT_A_ACCELERATION,
    .max_travel[X_AXIS] = (-DEFAULT_X_MAX_TRAVEL),
    .max_travel[Y_AXIS] = (-DEFAULT_Y_MAX_TRAVEL),
    .max_travel[Z_AXIS] = (-DEFAULT_Z_MAX_TRAVEL),
    .max_travel[A_AXIS] = (-DEFAULT_A_MAX_TRAVEL),
    .kinematics = DEFAULT_KINEMATICS,
    .limits_head_space = DEFAULT_LIMITS_HEAD_SPACE,
    .a_mode = DEFAULT_A_MODE,
    .a_homing = DEFAULT_A_HOMING,
    .aux_follow_spindle = DEFAULT_AUX_FOLLOW_SPINDLE,
    .at_speed_loss = DEFAULT_AT_SPEED_LOSS,
    .estop_enable = DEFAULT_ESTOP_ENABLE,
    .m6_mode = DEFAULT_M6_MODE,
    .brake_mode = DEFAULT_BRAKE_MODE,
    .brake_on_ms = DEFAULT_BRAKE_ON_MS,
    .brake_release_ms = DEFAULT_BRAKE_RELEASE_MS,
    .brake_off_ms = DEFAULT_BRAKE_OFF_MS,
    .spindle_rundown_ms = DEFAULT_SPINDLE_RUNDOWN_MS,
    .at_speed_timeout_ms = DEFAULT_AT_SPEED_TIMEOUT_MS,
    .spinup_delay_ms = DEFAULT_SPINUP_DELAY_MS,
    .reset_pulse_ms = DEFAULT_RESET_PULSE_MS,
    .dir_delay_us = DEFAULT_DIR_DELAY_US,
    .in_func = DEFAULT_IN_FUNC,
    .out_func = DEFAULT_OUT_FUNC,
    .in_invert = DEFAULT_IN_INVERT,
    .out_invert = DEFAULT_OUT_INVERT,
    .drv_power_delay_ms = DEFAULT_DRV_POWER_DELAY_MS,
    .drv_power_enable = DEFAULT_DRV_POWER_ENABLE,
    .mpg_mode = DEFAULT_MPG_MODE,
    .mpg_step = DEFAULT_MPG_STEP,
    .mpg_max_feed = DEFAULT_MPG_MAX_FEED,
    .vfd_type = DEFAULT_VFD_TYPE,
    .vfd_address = DEFAULT_VFD_ADDRESS,
    .vfd_format = DEFAULT_VFD_FORMAT,
    .vfd_baud = DEFAULT_VFD_BAUD,
    .vfd_max_hz = DEFAULT_VFD_MAX_HZ,
    .spindle_pwm = DEFAULT_SPINDLE_PWM,
    .panel_buttons = DEFAULT_PANEL_BUTTONS,
    .pot_func = { DEFAULT_POT1_FUNC, DEFAULT_POT2_FUNC }};


// The global (non-axis) settings in the order $$ prints them. The display's
// $$ page has the same list (grbl_shm.h), axis settings appended.
const uint8_t settings_global_numbers[N_GLOBAL_SETTINGS] = SETTINGS_GLOBAL_NUMBERS;


// True for the settings $$ prints with decimals.
uint8_t settings_is_float(uint8_t n)
{
  if (n >= AXIS_SETTINGS_START_VAL) { return(true); }
  switch (n) {
    case 11: case 12: case 24: case 25: case 27: case 30: case 31: case 37: case 97: case 98: return(true);
  }
  return(false);
}


// The value of $n as $$ prints it (axis settings included). 0 for a number
// that does not exist.
float settings_value(uint8_t n)
{
  if (n >= AXIS_SETTINGS_START_VAL) {
    uint8_t axis = (n - AXIS_SETTINGS_START_VAL) % AXIS_SETTINGS_INCREMENT;
    uint8_t set_idx = (n - AXIS_SETTINGS_START_VAL) / AXIS_SETTINGS_INCREMENT;
    if (axis >= N_AXIS) { return 0; }
    switch (set_idx) {
      case 0: return settings.steps_per_mm[axis];
      case 1: return settings.max_rate[axis];
      case 2: return settings.acceleration[axis] / (60 * 60);
      case 3: return -settings.max_travel[axis];
    }
    return 0;
  }
  if ((n >= IN_FN_SETTING) && (n < IN_FN_SETTING + N_IN_FUNCS)) { return settings.in_func[n - IN_FN_SETTING]; }
  if ((n >= OUT_FN_SETTING) && (n < OUT_FN_SETTING + N_OUT_FUNCS)) { return settings.out_func[n - OUT_FN_SETTING]; }
  switch (n) {
    case 0: return settings.pulse_microseconds;
    case 1: return settings.stepper_idle_lock_time;
    case 2: return settings.step_invert_mask;
    case 3: return settings.dir_invert_mask;
    case 4: return bit_istrue(settings.flags,BITFLAG_INVERT_ST_ENABLE) ? 1 : 0;
    case 10: return settings.status_report_mask;
    case 11: return settings.junction_deviation;
    case 12: return settings.arc_tolerance;
    case 13: return bit_istrue(settings.flags,BITFLAG_REPORT_INCHES) ? 1 : 0;
    case 20: return bit_istrue(settings.flags,BITFLAG_SOFT_LIMIT_ENABLE) ? 1 : 0;
    case 21: return bit_istrue(settings.flags,BITFLAG_HARD_LIMIT_ENABLE) ? 1 : 0;
    case 22: return bit_istrue(settings.flags,BITFLAG_HOMING_ENABLE) ? 1 : 0;
    case 23: return settings.homing_dir_mask;
    case 24: return settings.homing_feed_rate;
    case 25: return settings.homing_seek_rate;
    case 26: return settings.homing_debounce_delay;
    case 27: return settings.homing_pulloff;
    case 30: return settings.rpm_max;
    case 31: return settings.rpm_min;
    case 32:
      #ifdef VARIABLE_SPINDLE
        return bit_istrue(settings.flags,BITFLAG_LASER_MODE) ? 1 : 0;
      #else
        return 0;
      #endif
    case 33: return settings.vfd_type;
    case 34: return settings.vfd_address;
    case 35: return settings.vfd_baud;
    case 36: return settings.vfd_format;
    case 37: return settings.vfd_max_hz;
    case 38: return settings.spindle_pwm;
    case 39: return settings.panel_buttons;
    case 44: return settings.pot_func[0];
    case 45: return settings.pot_func[1];
    case 40: return settings.kinematics;
    case 41: return settings.limits_head_space;
    case 42: return settings.a_mode;
    case 43: return settings.a_homing;
    case 46: return settings.aux_follow_spindle;
    case 48: return settings.brake_on_ms;
    case 49: return settings.brake_release_ms;
    case 50: return settings.brake_off_ms;
    case 51: return settings.spindle_rundown_ms;
    case 52: return settings.at_speed_timeout_ms;
    case 53: return settings.at_speed_loss;
    case 54: return settings.spinup_delay_ms;
    case 55: return settings.reset_pulse_ms;
    case 56: return settings.dir_delay_us;
    case 57: return settings.estop_enable;
    case 58: return settings.m6_mode;
    case 59: return settings.brake_mode;
    case IN_INVERT_SETTING: return settings.in_invert;
    case OUT_INVERT_SETTING: return settings.out_invert;
    case 94: return settings.drv_power_enable;
    case 95: return settings.drv_power_delay_ms;
    case 96: return settings.mpg_mode;
    case 97: return settings.mpg_step;
    case 98: return settings.mpg_max_feed;
  }
  return 0;
}


// Method to store startup lines into EEPROM
void settings_store_startup_line(uint8_t n, char *line)
{
  #ifdef FORCE_BUFFER_SYNC_DURING_EEPROM_WRITE
    protocol_buffer_synchronize(); // A startup line may contain a motion and be executing. 
  #endif
  uint32_t addr = n*(LINE_BUFFER_SIZE+1)+EEPROM_ADDR_STARTUP_BLOCK;
  memcpy_to_eeprom_with_checksum(addr,(char*)line, LINE_BUFFER_SIZE);
}


// Method to store build info into EEPROM
// NOTE: This function can only be called in IDLE state.
void settings_store_build_info(char *line)
{
  // Build info can only be stored when state is IDLE.
  memcpy_to_eeprom_with_checksum(EEPROM_ADDR_BUILD_INFO,(char*)line, LINE_BUFFER_SIZE);
}


// Method to store coord data parameters into EEPROM
void settings_write_coord_data(uint8_t coord_select, float *coord_data)
{
  #ifdef FORCE_BUFFER_SYNC_DURING_EEPROM_WRITE
    protocol_buffer_synchronize();
  #endif
  uint32_t addr = coord_select*(sizeof(float)*N_AXIS+1) + EEPROM_ADDR_PARAMETERS;
  memcpy_to_eeprom_with_checksum(addr,(char*)coord_data, sizeof(float)*N_AXIS);
}


// Method to store Grbl global settings struct and version number into EEPROM
// NOTE: This function can only be called in IDLE state.
void write_global_settings()
{
  eeprom_put_char(0, SETTINGS_VERSION);
  memcpy_to_eeprom_with_checksum(EEPROM_ADDR_GLOBAL, (char*)&settings, sizeof(settings_t));
}


// Method to restore EEPROM-saved Grbl global settings back to defaults.
void settings_restore(uint8_t restore_flag) {
  if (restore_flag & SETTINGS_RESTORE_DEFAULTS) {    
    settings = defaults;
    write_global_settings();
  }

  if (restore_flag & SETTINGS_RESTORE_PARAMETERS) {
    uint8_t idx;
    float coord_data[N_AXIS];
    memset(&coord_data, 0, sizeof(coord_data));
    for (idx=0; idx <= SETTING_INDEX_NCOORD; idx++) { settings_write_coord_data(idx, coord_data); }
  }

  if (restore_flag & SETTINGS_RESTORE_STARTUP_LINES) {
    #if N_STARTUP_LINE > 0
      eeprom_put_char(EEPROM_ADDR_STARTUP_BLOCK, 0);
      eeprom_put_char(EEPROM_ADDR_STARTUP_BLOCK+1, 0); // Checksum
    #endif
    #if N_STARTUP_LINE > 1
      eeprom_put_char(EEPROM_ADDR_STARTUP_BLOCK+(LINE_BUFFER_SIZE+1), 0);
      eeprom_put_char(EEPROM_ADDR_STARTUP_BLOCK+(LINE_BUFFER_SIZE+2), 0); // Checksum
    #endif
  }

  if (restore_flag & SETTINGS_RESTORE_BUILD_INFO) {
    eeprom_put_char(EEPROM_ADDR_BUILD_INFO , 0);
    eeprom_put_char(EEPROM_ADDR_BUILD_INFO+1 , 0); // Checksum
  }
}


// Reads startup line from EEPROM. Updated pointed line string data.
uint8_t settings_read_startup_line(uint8_t n, char *line)
{
  uint32_t addr = n*(LINE_BUFFER_SIZE+1)+EEPROM_ADDR_STARTUP_BLOCK;
  if (!(memcpy_from_eeprom_with_checksum((char*)line, addr, LINE_BUFFER_SIZE))) {
    // Reset line with default value
    line[0] = 0; // Empty line
    settings_store_startup_line(n, line);
    return(false);
  }
  return(true);
}


// Reads startup line from EEPROM. Updated pointed line string data.
uint8_t settings_read_build_info(char *line)
{
  if (!(memcpy_from_eeprom_with_checksum((char*)line, EEPROM_ADDR_BUILD_INFO, LINE_BUFFER_SIZE))) {
    // Reset line with default value
    line[0] = 0; // Empty line
    settings_store_build_info(line);
    return(false);
  }
  return(true);
}


// Read selected coordinate data from EEPROM. Updates pointed coord_data value.
uint8_t settings_read_coord_data(uint8_t coord_select, float *coord_data)
{
  uint32_t addr = coord_select*(sizeof(float)*N_AXIS+1) + EEPROM_ADDR_PARAMETERS;
  if (!(memcpy_from_eeprom_with_checksum((char*)coord_data, addr, sizeof(float)*N_AXIS))) {
    // Reset with default zero vector
    clear_vector_float(coord_data);
    settings_write_coord_data(coord_select,coord_data);
    return(false);
  }
  return(true);
}


// Size of the older records: everything before the fields a later version
// appended, padded as those structs were (to 4 bytes: they hold floats).
#define SETTINGS_V21_SIZE  ((offsetof(settings_t, out_invert) + sizeof(uint16_t) + 3) & ~(size_t)3)
#define SETTINGS_V22_SIZE  ((offsetof(settings_t, drv_power_enable) + sizeof(uint8_t) + 3) & ~(size_t)3)
#define SETTINGS_V23_SIZE  ((offsetof(settings_t, mpg_max_feed) + sizeof(float) + 3) & ~(size_t)3)
#define SETTINGS_V24_SIZE  ((offsetof(settings_t, vfd_max_hz) + sizeof(float) + 3) & ~(size_t)3)

// Reads Grbl global settings struct from EEPROM.
uint8_t read_global_settings() {
  // Check version-byte of eeprom
  uint8_t version = eeprom_get_char(0);
  if (version == SETTINGS_VERSION) {
    // Read settings-record and check checksum
    if (!(memcpy_from_eeprom_with_checksum((char*)&settings, EEPROM_ADDR_GLOBAL, sizeof(settings_t)))) {
      return(false);
    }
  } else if ((version >= 21) && (version <= 24)) {
    // An older version: the same record without what later versions appended.
    // Keep it, give the new settings their defaults, store it as current.
    static const uint16_t old_size[4] = { SETTINGS_V21_SIZE, SETTINGS_V22_SIZE, SETTINGS_V23_SIZE, SETTINGS_V24_SIZE };
    size_t size = old_size[version - 21];
    if (!(memcpy_from_eeprom_with_checksum((char*)&settings, EEPROM_ADDR_GLOBAL, size))) {
      return(false);
    }
    if (version == 21) {
      settings.drv_power_delay_ms = DEFAULT_DRV_POWER_DELAY_MS;
      settings.drv_power_enable = DEFAULT_DRV_POWER_ENABLE;
    }
    if (version <= 22) {
      settings.mpg_mode = DEFAULT_MPG_MODE;
      settings.mpg_step = DEFAULT_MPG_STEP;
      settings.mpg_max_feed = DEFAULT_MPG_MAX_FEED;
    }
    if (version <= 23) {
      settings.vfd_type = DEFAULT_VFD_TYPE;
      settings.vfd_address = DEFAULT_VFD_ADDRESS;
      settings.vfd_format = DEFAULT_VFD_FORMAT;
      settings.vfd_baud = DEFAULT_VFD_BAUD;
      settings.vfd_max_hz = DEFAULT_VFD_MAX_HZ;
    }
    // Before 25 D6 always carried the PWM: a laser keeps it.
    settings.spindle_pwm = bit_istrue(settings.flags,BITFLAG_LASER_MODE) ? 1 : DEFAULT_SPINDLE_PWM;
    settings.panel_buttons = DEFAULT_PANEL_BUTTONS;
    settings.pot_func[0] = DEFAULT_POT1_FUNC;
    settings.pot_func[1] = DEFAULT_POT2_FUNC;
    write_global_settings();
  } else {
    return(false);
  }
  return(true);
}


// A helper method to set settings from command line
uint8_t settings_store_global_setting(uint8_t parameter, float value) {
  if (value < 0.0) { return(STATUS_NEGATIVE_VALUE); }
  if (parameter >= AXIS_SETTINGS_START_VAL) {
    // Store axis configuration. Axis numbering sequence set by AXIS_SETTING defines.
    // NOTE: Ensure the setting index corresponds to the report.c settings printout.
    parameter -= AXIS_SETTINGS_START_VAL;
    uint8_t set_idx = 0;
    while (set_idx < AXIS_N_SETTINGS) {
      if (parameter < N_AXIS) {
        // Valid axis setting found.
        switch (set_idx) {
          case 0:
            #ifdef MAX_STEP_RATE_HZ
              if (value*settings.max_rate[parameter] > (MAX_STEP_RATE_HZ*60.0)) { return(STATUS_MAX_STEP_RATE_EXCEEDED); }
            #endif
            settings.steps_per_mm[parameter] = value;
            break;
          case 1:
            #ifdef MAX_STEP_RATE_HZ
              if (value*settings.steps_per_mm[parameter] > (MAX_STEP_RATE_HZ*60.0)) {  return(STATUS_MAX_STEP_RATE_EXCEEDED); }
            #endif
            settings.max_rate[parameter] = value;
            break;
          case 2: settings.acceleration[parameter] = value*60*60; break; // Convert to mm/min^2 for grbl internal use.
          case 3: settings.max_travel[parameter] = -value; break;  // Store as negative for grbl internal use.
        }
        break; // Exit while-loop after setting has been configured and proceed to the EEPROM write call.
      } else {
        set_idx++;
        // If axis index greater than N_AXIS or setting index greater than number of axis settings, error out.
        if ((parameter < AXIS_SETTINGS_INCREMENT) || (set_idx == AXIS_N_SETTINGS)) { return(STATUS_INVALID_STATEMENT); }
        parameter -= AXIS_SETTINGS_INCREMENT;
      }
    }
  } else {
    // Store non-axis Grbl settings
    uint8_t int_value = trunc(value);
    // The port's settings go past 255 (times in ms, the $79 mask).
    float tvalue = trunc(value);
    uint32_t ivalue = (tvalue >= 16777216.0f) ? 16777216UL : (uint32_t)tvalue;

    if ((parameter >= IN_FN_SETTING) && (parameter < IN_FN_SETTING + N_IN_FUNCS)) {
      // Input assignment, $60-$78.
      uint8_t fn = parameter - IN_FN_SETTING;
      if (ivalue > N_IN_CHANNELS) { return(STATUS_INVALID_STATEMENT); }
      uint8_t status = io_check_in_assignment(fn, (uint8_t)ivalue);
      if (status != STATUS_OK) { return(status); }
      settings.in_func[fn] = (uint8_t)ivalue;
      io_rebuild();
      limits_init();   // restart debouncing from the new assignment
      system_init();
    } else if ((parameter >= OUT_FN_SETTING) && (parameter < OUT_FN_SETTING + N_OUT_FUNCS)) {
      // Output assignment, $80-$92. Whatever the function was doing stops:
      // the channel it leaves and the one it takes over both end up off.
      uint8_t fn = parameter - OUT_FN_SETTING;
      if (ivalue > N_OUT_CHANNELS) { return(STATUS_INVALID_STATEMENT); }
      uint8_t status = io_check_out_assignment(fn, (uint8_t)ivalue);
      if (status != STATUS_OK) { return(status); }
      outputs_function_off(fn);
      settings.out_func[fn] = (uint8_t)ivalue;
      outputs_refresh();
    } else switch(parameter) {
      case 0:
        if (int_value < 3) { return(STATUS_SETTING_STEP_PULSE_MIN); }
        settings.pulse_microseconds = int_value; break;
      case 1: settings.stepper_idle_lock_time = int_value; break;
      case 2:
        settings.step_invert_mask = int_value;
        st_generate_step_dir_invert_masks(); // Regenerate step and direction port invert masks.
        break;
      case 3:
        settings.dir_invert_mask = int_value;
        st_generate_step_dir_invert_masks(); // Regenerate step and direction port invert masks.
        break;
      case 4: // Reset to ensure change. Immediate re-init may cause problems.
        if (int_value) { settings.flags |= BITFLAG_INVERT_ST_ENABLE; }
        else { settings.flags &= ~BITFLAG_INVERT_ST_ENABLE; }
        break;
      case 10: settings.status_report_mask = int_value; break;
      case 11: settings.junction_deviation = value; break;
      case 12: settings.arc_tolerance = value; break;
      case 13:
        if (int_value) { settings.flags |= BITFLAG_REPORT_INCHES; }
        else { settings.flags &= ~BITFLAG_REPORT_INCHES; }
        system_flag_wco_change(); // Make sure WCO is immediately updated.
        break;
      case 20:
        if (int_value) {
          if (bit_isfalse(settings.flags, BITFLAG_HOMING_ENABLE)) { return(STATUS_SOFT_LIMIT_ERROR); }
          settings.flags |= BITFLAG_SOFT_LIMIT_ENABLE;
        } else { settings.flags &= ~BITFLAG_SOFT_LIMIT_ENABLE; }
        break;
      case 21:
        if (int_value) { settings.flags |= BITFLAG_HARD_LIMIT_ENABLE; }
        else { settings.flags &= ~BITFLAG_HARD_LIMIT_ENABLE; }
        limits_init(); // Re-init to immediately change. NOTE: Nice to have but could be problematic later.
        break;
      case 22:
        if (int_value) { settings.flags |= BITFLAG_HOMING_ENABLE; }
        else {
          settings.flags &= ~BITFLAG_HOMING_ENABLE;
          settings.flags &= ~BITFLAG_SOFT_LIMIT_ENABLE; // Force disable soft-limits.
        }
        break;
      case 23: settings.homing_dir_mask = int_value; break;
      case 24: settings.homing_feed_rate = value; break;
      case 25: settings.homing_seek_rate = value; break;
      case 26:
        if (ivalue > 65535) { return(STATUS_INVALID_STATEMENT); }
        settings.homing_debounce_delay = ivalue; break;
      case 27: settings.homing_pulloff = value; break;
      case 30: settings.rpm_max = value; spindle_init(); break; // Re-initialize spindle rpm calibration
      case 31: settings.rpm_min = value; spindle_init(); break; // Re-initialize spindle rpm calibration
      case 32:
        #ifdef VARIABLE_SPINDLE
          if (int_value && settings.vfd_type) { return(STATUS_SETTING_DISABLED); }   // a laser has no VFD
          if (int_value && !settings.spindle_pwm) { return(STATUS_SETTING_DISABLED_LASER); }   // nor a laser without PWM ($38)
          if (int_value) { settings.flags |= BITFLAG_LASER_MODE; }
          else { settings.flags &= ~BITFLAG_LASER_MODE; }
        #else
          return(STATUS_SETTING_DISABLED_LASER);
        #endif
        break;
      case 33: case 34: case 35: case 36:
        // Spindle VFD over RS485 (vfd.h). Not while the spindle runs, or may
        // still run: the drive would be left running, or another one addressed.
        if (outputs_get(OUTPUT_SPINDLE_FWD) || outputs_get(OUTPUT_SPINDLE_REV) || vfd_may_run()) {
          return(STATUS_IDLE_ERROR);
        }
        switch (parameter) {
          case 33:
            if (ivalue >= VFD_N_TYPES) { return(STATUS_INVALID_STATEMENT); }
            if (ivalue && bit_istrue(settings.flags,BITFLAG_LASER_MODE)) { return(STATUS_SETTING_DISABLED); }
            settings.vfd_type = ivalue;
            break;
          case 34:
            if ((ivalue < 1) || (ivalue > 247)) { return(STATUS_INVALID_STATEMENT); }
            settings.vfd_address = ivalue;
            break;
          case 35:
            if ((ivalue < 1200) || (ivalue > 115200)) { return(STATUS_INVALID_STATEMENT); }
            settings.vfd_baud = ivalue;
            break;
          default:
            if (ivalue >= HAL_RS485_FORMATS) { return(STATUS_INVALID_STATEMENT); }
            settings.vfd_format = ivalue;
            break;
        }
        vfd_init();
        break;
      case 37:
        if ((value < 1.0f) || (value > VFD_MAX_HZ)) { return(STATUS_INVALID_STATEMENT); }
        settings.vfd_max_hz = value;
        break;
      case 38: {
        // PWM on OUT13. On: the timer takes D6, so no output function may be
        // on OUT13. Off: the laser loses its power control.
        if (ivalue > 2) { return(STATUS_INVALID_STATEMENT); }
        if (!ivalue && bit_istrue(settings.flags,BITFLAG_LASER_MODE)) { return(STATUS_SETTING_DISABLED_LASER); }
        if (ivalue) {
          uint8_t f;
          for (f = 0; f < N_OUT_FUNCS; f++) {
            if (settings.out_func[f] == OUT_PWM_CHANNEL) { return(STATUS_CHANNEL_IN_USE); }
          }
        }
        settings.spindle_pwm = ivalue;
        spindle_init();
        break;
      }
      case 39:
        if (ivalue > 1) { return(STATUS_INVALID_STATEMENT); }
        settings.panel_buttons = ivalue;
        panel_init();
        break;
      case 44: case 45:
        if (ivalue >= PANEL_N_POT_FUNCS) { return(STATUS_INVALID_STATEMENT); }
        settings.pot_func[parameter - 44] = ivalue;
        panel_init();
        break;
      case 40: // Kinematics: 0 = Cartesian, 1 = CoreXY
        if (int_value > KINEMATICS_COREXY) { return(STATUS_INVALID_STATEMENT); }
        if (int_value == KINEMATICS_COREXY) {
          // A second X or Y motor has no meaning on a belt-coupled frame.
          if (axis_a_is_slave()) { return(STATUS_SETTING_DISABLED); }
          // CoreXY drives both belt motors for any X or Y move, so X and Y
          // cannot be homed in the same cycle. Refuse rather than let a user
          // discover this by crashing into a hard stop.
          #if defined(HOMING_CYCLE_0) && ((HOMING_CYCLE_0) & (1<<X_AXIS)) && ((HOMING_CYCLE_0) & (1<<Y_AXIS))
            return(STATUS_SETTING_DISABLED);
          #endif
          #if defined(HOMING_CYCLE_1) && ((HOMING_CYCLE_1) & (1<<X_AXIS)) && ((HOMING_CYCLE_1) & (1<<Y_AXIS))
            return(STATUS_SETTING_DISABLED);
          #endif
          #if defined(HOMING_CYCLE_2) && ((HOMING_CYCLE_2) & (1<<X_AXIS)) && ((HOMING_CYCLE_2) & (1<<Y_AXIS))
            return(STATUS_SETTING_DISABLED);
          #endif
        }
        settings.kinematics = int_value;
        // Machine position is held in motor steps, whose meaning just changed.
        // Force a re-sync so the planner and the reported position agree.
        plan_sync_position();
        gc_sync_position();
        break;
      case 41: // Limit frame: 0 = per-motor, 1 = tool head axes
        if (int_value > 1) { return(STATUS_INVALID_STATEMENT); }
        settings.limits_head_space = int_value;
        break;
      case 42: // A axis mode: 0 rotary, 1 linear, 2 slave of X, 3 slave of Y
        if (int_value > A_MODE_SLAVE_Y) { return(STATUS_INVALID_STATEMENT); }
        if ((int_value >= A_MODE_SLAVE_X) && kinematics_is_corexy()) { return(STATUS_SETTING_DISABLED); }
        // Only while idle or in alarm: the stepper copies the master's pulses
        // to the A motor according to this, so it must not flip mid-motion.
        if (sys.state & ~(STATE_ALARM)) { return(STATUS_IDLE_ERROR); }
        settings.a_mode = int_value;
        // A slave owns no coordinate. Drop whatever A position there was, so the
        // parser, the planner and the reports agree that A sits at zero.
        if (axis_a_is_slave()) { sys_position[A_AXIS] = 0; }
        st_generate_step_dir_invert_masks(); // Re-latch which axis the A motor follows.
        plan_sync_position();
        gc_sync_position();
        break;
      case 43: settings.a_homing = (int_value != 0); break;
      case 46:
        if (ivalue >= (1UL << N_AUX_OUTPUTS)) { return(STATUS_INVALID_STATEMENT); }
        settings.aux_follow_spindle = ivalue;
        outputs_refresh();
        break;
      case 48: case 49: case 50: case 51: case 52: case 54: case 55:
        if (ivalue > 65535) { return(STATUS_INVALID_STATEMENT); }
        switch (parameter) {
          case 48: settings.brake_on_ms = ivalue; break;
          case 49: settings.brake_release_ms = ivalue; break;
          case 50: settings.brake_off_ms = ivalue; break;
          case 51: settings.spindle_rundown_ms = ivalue; break;
          case 52: settings.at_speed_timeout_ms = ivalue; break;
          case 54: settings.spinup_delay_ms = ivalue; break;
          default: settings.reset_pulse_ms = ivalue; break;
        }
        break;
      case 53:
        if (ivalue > 2) { return(STATUS_INVALID_STATEMENT); }
        settings.at_speed_loss = ivalue;
        break;
      case 56:
        if (ivalue > DIR_DELAY_MAX_US) { return(STATUS_INVALID_STATEMENT); }
        settings.dir_delay_us = ivalue;
        break;
      case 57:
        if (ivalue > 1) { return(STATUS_INVALID_STATEMENT); }
        settings.estop_enable = ivalue;
        estop_init();
        break;
      case 58:
        if (ivalue > 1) { return(STATUS_INVALID_STATEMENT); }
        settings.m6_mode = ivalue;
        break;
      case 59:
        if (ivalue > 1) { return(STATUS_INVALID_STATEMENT); }
        settings.brake_mode = ivalue;
        st_power_refresh();
        break;
      case IN_INVERT_SETTING:
        if (ivalue >= (1UL << IO_N_IN_PINS)) { return(STATUS_INVALID_STATEMENT); }
        settings.in_invert = ivalue;
        io_rebuild();
        limits_init();
        system_init();
        estop_init();
        break;
      case 94:
        if (ivalue > 1) { return(STATUS_INVALID_STATEMENT); }
        settings.drv_power_enable = ivalue;
        drvpwr_init();
        break;
      case 95:
        if (ivalue > 65535) { return(STATUS_INVALID_STATEMENT); }
        settings.drv_power_delay_ms = ivalue;
        drvpwr_init();
        break;
      case 96:
        if (ivalue > 2) { return(STATUS_INVALID_STATEMENT); }
        settings.mpg_mode = ivalue;
        break;
      case 97:
        if ((value < 0.00099f) || (value > 10.0f)) { return(STATUS_INVALID_STATEMENT); }   // $$ shows 3 decimals
        settings.mpg_step = value;
        break;
      case 98:
        if (value < 1.0f) { return(STATUS_INVALID_STATEMENT); }
        settings.mpg_max_feed = value;
        break;
      case OUT_INVERT_SETTING:
        if (ivalue >= (1UL << N_OUT_CHANNELS)) { return(STATUS_INVALID_STATEMENT); }
        settings.out_invert = ivalue;
        io_rebuild();
        break;
      default:
        return(STATUS_INVALID_STATEMENT);
    }
  }
  write_global_settings();
  return(STATUS_OK);
}


// Initialize the config subsystem
void settings_init() {
  if(!read_global_settings()) {
    report_status_message(STATUS_SETTING_READ_FAIL);
    settings_restore(SETTINGS_RESTORE_ALL); // Force restore all EEPROM data.
    report_grbl_settings();
  }
}


// Pin masks in this port are logical axis-bit masks, not hardware port bits:
// bit i means axis i. The HAL turns them into real pin writes, which is what
// lets step and direction pins live on any pins of any MCU port.
uint8_t get_step_pin_mask(uint8_t axis_idx)
{
  return((uint8_t)(1 << axis_idx));
}


uint8_t get_direction_pin_mask(uint8_t axis_idx)
{
  return((uint8_t)(1 << axis_idx));
}


uint8_t get_limit_pin_mask(uint8_t axis_idx)
{
  return((uint8_t)(1 << axis_idx));
}
