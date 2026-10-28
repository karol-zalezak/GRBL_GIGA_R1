/*
  settings.h - eeprom configuration handling
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

#ifndef settings_h
#define settings_h

#include "grbl.h"


// Version of the EEPROM data. Will be used to migrate existing data from older versions of Grbl
// when firmware is upgraded. Always stored in byte 0 of eeprom
// 11 adds settings.kinematics ($40); 12 adds settings.limits_head_space
// ($41); 20 is the four-axis Giga layout with $42-$47; 21 the universal I/O
// channels ($48-$59, $60-$93; $5, $6, $44, $45 and $47 gone); 22 appends
// the driver power input ($94, $95) and takes a version 21 image over (see
// read_global_settings()); 23 appends the handwheel ($96-$98) and takes
// version 21 and 22 images over; 24 appends the spindle VFD over RS485
// ($33-$37) and takes versions 21-23 over; 25 appends the PWM on D6 ($38)
// and the operator panel ($39, $44, $45) and takes versions 21-24 over; 26
// renumbers OUT7-OUT13 to shield v4's terminal order (D6 OUT13 -> OUT7, D5-D15
// OUT7-OUT12 -> OUT8-OUT13) and takes versions 21-25 over with every output
// function and inversion kept on its pin.
// Bumped from upstream Grbl's 10 so a mismatched image is rejected and
// defaults are written instead - back up $$ and $#.
#define SETTINGS_VERSION 26  // NOTE: Check settings_reset() when moving to next version.

// Define bit flag masks for the boolean settings in settings.flag.
#define BIT_REPORT_INCHES      0
#define BIT_LASER_MODE         1
#define BIT_INVERT_ST_ENABLE   2
#define BIT_HARD_LIMIT_ENABLE  3
#define BIT_HOMING_ENABLE      4
#define BIT_SOFT_LIMIT_ENABLE  5
// Bits 6 and 7 were the limit and probe invert flags ($5, $6). On this port
// every input has its own bit in $79 instead.

#define BITFLAG_REPORT_INCHES      bit(BIT_REPORT_INCHES)
#define BITFLAG_LASER_MODE         bit(BIT_LASER_MODE)
#define BITFLAG_INVERT_ST_ENABLE   bit(BIT_INVERT_ST_ENABLE)
#define BITFLAG_HARD_LIMIT_ENABLE  bit(BIT_HARD_LIMIT_ENABLE)
#define BITFLAG_HOMING_ENABLE      bit(BIT_HOMING_ENABLE)
#define BITFLAG_SOFT_LIMIT_ENABLE  bit(BIT_SOFT_LIMIT_ENABLE)

// Define status reporting boolean enable bit flags in settings.status_report_mask
#define BITFLAG_RT_STATUS_POSITION_TYPE     bit(0)
#define BITFLAG_RT_STATUS_BUFFER_STATE      bit(1)

// Define settings restore bitflags.
#define SETTINGS_RESTORE_DEFAULTS bit(0)
#define SETTINGS_RESTORE_PARAMETERS bit(1)
#define SETTINGS_RESTORE_STARTUP_LINES bit(2)
#define SETTINGS_RESTORE_BUILD_INFO bit(3)
#ifndef SETTINGS_RESTORE_ALL
  #define SETTINGS_RESTORE_ALL 0xFF // All bitflags
#endif

// Define EEPROM memory address location values for Grbl settings and parameters
// NOTE: The Atmega328p has 1KB EEPROM. The upper half is reserved for parameters and
// the startup script. The lower half contains the global settings and space for future
// developments.
#define EEPROM_ADDR_GLOBAL         1U
#define EEPROM_ADDR_PARAMETERS     512U
#define EEPROM_ADDR_STARTUP_BLOCK  768U
#define EEPROM_ADDR_BUILD_INFO     942U

// Define EEPROM address indexing for coordinate parameters
#define N_COORDINATE_SYSTEM 6  // Number of supported work coordinate systems (from index 1)
#define SETTING_INDEX_NCOORD N_COORDINATE_SYSTEM+1 // Total number of system stored (from index 0)
// NOTE: Work coordinate indices are (0=G54, 1=G55, ... , 6=G59)
#define SETTING_INDEX_G28    N_COORDINATE_SYSTEM    // Home position 1
#define SETTING_INDEX_G30    N_COORDINATE_SYSTEM+1  // Home position 2
#define SETTING_INDEX_G92    N_COORDINATE_SYSTEM+2  // G92 coordinate offset; only used with G92_PERSISTENT (G92.2,G92.3 not supported)

// Define Grbl axis settings numbering scheme. Starts at START_VAL, every INCREMENT, over N_SETTINGS.
#define AXIS_N_SETTINGS          4
#define AXIS_SETTINGS_START_VAL  100 // NOTE: Reserving settings values >= 100 for axis settings. Up to 255.
#define AXIS_SETTINGS_INCREMENT  10  // Must be greater than the number of axis settings

// Global persistent settings (Stored from byte EEPROM_ADDR_GLOBAL onwards)
typedef struct {
  // Axis settings
  float steps_per_mm[N_AXIS];
  float max_rate[N_AXIS];
  float acceleration[N_AXIS];
  float max_travel[N_AXIS];

  // Remaining Grbl settings
  uint8_t pulse_microseconds;
  uint8_t step_invert_mask;
  uint8_t dir_invert_mask;
  uint8_t stepper_idle_lock_time; // If max value 255, steppers do not disable.
  uint8_t status_report_mask; // Mask to indicate desired report data.
  float junction_deviation;
  float arc_tolerance;

  float rpm_max;
  float rpm_min;

  uint8_t flags;  // Contains default boolean settings

  uint8_t homing_dir_mask;
  float homing_feed_rate;
  float homing_seek_rate;
  uint16_t homing_debounce_delay;
  float homing_pulloff;

  uint8_t kinematics;        // $40: KINEMATICS_CARTESIAN or KINEMATICS_COREXY
  uint8_t limits_head_space; // $41: 0 = per-motor limits, 1 = head-axis limits

  uint8_t a_mode;             // $42: A_MODE_* (rotary, linear, slave of X, slave of Y)
  uint8_t a_homing;           // $43: home an independent A axis in $H
  uint8_t aux_follow_spindle; // $46: AUX0-AUX5 switched together with the spindle
  uint8_t at_speed_loss;      // $53: at-speed lost while cutting: 0 ignore, 1 hold, 2 alarm
  uint8_t estop_enable;       // $57: central STOP input in use
  uint8_t m6_mode;            // $58: M6 = 0 error (upstream), 1 pause for a manual change
  uint8_t brake_mode;         // $59: 0 the drive works the brake, 1 the BRAKE output does

  uint16_t brake_on_ms;       // $48: drivers enabled -> brake released
  uint16_t brake_release_ms;  // $49: brake released -> first step
  uint16_t brake_off_ms;      // $50: brake engaged -> drivers disabled
  uint16_t spindle_rundown_ms;  // $51: spindle off this long before the collet may open
  uint16_t at_speed_timeout_ms; // $52: wait for at-speed at most this long (0 = do not wait)
  uint16_t spinup_delay_ms;   // $54: fixed wait after M3/M4 when not waiting for at-speed
  uint16_t reset_pulse_ms;    // $55: servo / VFD reset pulse length
  uint16_t dir_delay_us;      // $56: direction change -> next step

  uint8_t in_func[N_IN_FUNCS];   // $60-$78: channel of each input function, 0 = unused
  uint8_t out_func[N_OUT_FUNCS]; // $80-$92: channel of each output function, 0 = unused
  uint32_t in_invert;            // $79: NC inputs, bit n-1 = INn, bit 20 = STOP, bit 21 = DRV PWR
  uint16_t out_invert;           // $93: inverted outputs, bit n-1 = OUTn

  // Version 22. New settings go after this point only: an older image is the
  // record up to here.
  uint16_t drv_power_delay_ms;   // $95: supply back this long before $H/$X
  uint8_t drv_power_enable;      // $94: driver power input (D53) watched
  // Version 23.
  uint8_t mpg_mode;              // $96: handwheel 0 off, 1 on, 2 on reversed
  float mpg_step;                // $97: distance of one detent at x1
  float mpg_max_feed;            // $98: highest handwheel feed, mm/min
  // Version 24.
  uint8_t vfd_type;              // $33: spindle VFD over RS485: 0 none, 1 Delta (vfd.h)
  uint8_t vfd_address;           // $34: its Modbus address
  uint8_t vfd_format;            // $36: 0 8N1, 1 8N2, 2 8E1, 3 8O1, 4 8E2, 5 8O2
  uint32_t vfd_baud;             // $35: baud rate
  float vfd_max_hz;              // $37: drive frequency at $30, Hz
  // Version 25.
  uint8_t spindle_pwm;           // $38: PWM on OUT7 (D6): 0 off, 1 on, 2 inverted
  uint8_t panel_buttons;         // $39: operator panel START/HOLD/STOP in use
  uint8_t pot_func[2];           // $44, $45: panel potentiometers A0, A1 (panel.h)
} settings_t;
extern settings_t settings;

// Initialize the configuration subsystem (load settings from EEPROM)
void settings_init();

// Helper function to clear and restore EEPROM defaults
void settings_restore(uint8_t restore_flag);

// A helper method to set new settings from command line
uint8_t settings_store_global_setting(uint8_t parameter, float value);

// The global settings in $$ order, the value of any setting as $$ prints it,
// and whether $$ prints it with decimals. The display uses the same three.
#define N_GLOBAL_SETTINGS 85
#define SETTINGS_GLOBAL_NUMBERS { 0, 1, 2, 3, 4, 10, 11, 12, 13, 20, 21, 22, 23, 24, 25, 26, 27, \
                                  30, 31, 32, 33, 34, 35, 36, 37, 38, 39,                    \
                                  40, 41, 42, 43, 44, 45, 46,                                \
                                  48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59,            \
                                  60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72,        \
                                  73, 74, 75, 76, 77, 78, 79,                                \
                                  80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93,    \
                                  94, 95, 96, 97, 98 }
extern const uint8_t settings_global_numbers[N_GLOBAL_SETTINGS];
float settings_value(uint8_t n);
uint8_t settings_is_float(uint8_t n);

// Longest $56 accepted, us. The stepper interrupt period is stretched by it
// at every direction change.
#define DIR_DELAY_MAX_US 1000

// Stores the protocol line variable as a startup line in EEPROM
void settings_store_startup_line(uint8_t n, char *line);

// Reads an EEPROM startup line to the protocol line variable
uint8_t settings_read_startup_line(uint8_t n, char *line);

// Stores build info user-defined string
void settings_store_build_info(char *line);

// Reads build info user-defined string
uint8_t settings_read_build_info(char *line);

// Writes selected coordinate data to EEPROM
void settings_write_coord_data(uint8_t coord_select, float *coord_data);

// Reads selected coordinate data from EEPROM
uint8_t settings_read_coord_data(uint8_t coord_select, float *coord_data);

// Returns the step pin mask according to Grbl's internal axis numbering
uint8_t get_step_pin_mask(uint8_t i);

// Returns the direction pin mask according to Grbl's internal axis numbering
uint8_t get_direction_pin_mask(uint8_t i);

// Returns the limit pin mask according to Grbl's internal axis numbering
uint8_t get_limit_pin_mask(uint8_t i);


#endif
