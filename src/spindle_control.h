/*
  spindle_control.h - spindle control methods
  Part of Grbl

  Copyright (c) 2012-2017 Sungeun K. Jeon for Gnea Research LLC
  Copyright (c) 2009-2011 Simen Svale Skogsrud
  Copyright (c) 2026 Grbl-R4 port for the Arduino UNO R4 (Renesas RA4M1)
  Copyright (c) 2026 Grbl-Q port for the Arduino UNO Q (STM32U585)
  Copyright (c) 2026 Grbl-Giga port for the Arduino GIGA R1 WiFi (STM32H747)

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

#ifndef spindle_control_h
#define spindle_control_h

#define SPINDLE_NO_SYNC false
#define SPINDLE_FORCE_SYNC true

#define SPINDLE_STATE_DISABLE  0  // Must be zero.
#define SPINDLE_STATE_CW       bit(0)
#define SPINDLE_STATE_CCW      bit(1)

// Duty value that means "off". Always zero.
#define SPINDLE_PWM_OFF_VALUE  0

// On the ATmega the PWM register was 8 bits, so Grbl's duty values ran 0-255.
// On the GIGA, TIM4 counts at 240 MHz behind a small prescaler, so the number of
// duty steps in one PWM period depends on the chosen frequency: 60000 at the
// 1 kHz default. The resolution is therefore established at run time by
// spindle_init(), which asks the HAL what the timer actually achieved.
extern uint16_t spindle_pwm_max_value;

// Initializes spindle pins and hardware PWM, if enabled.
void spindle_init();

// Returns current spindle output state. Overrides may alter it from programmed states.
uint8_t spindle_get_state();

#ifdef VARIABLE_SPINDLE

  // Called by g-code parser when setting spindle state and requires a buffer sync.
  void spindle_sync(uint8_t state, float rpm);

  // Sets spindle running state with direction, enable, and spindle PWM.
  void spindle_set_state(uint8_t state, float rpm);

  // Sets spindle PWM quickly for stepper ISR. Also called by spindle_set_state().
  void spindle_set_speed(uint16_t pwm_value);

  // Computes the PWM duty value for the given RPM for quick updating.
  uint16_t spindle_compute_pwm_value(float rpm);

#else

  // Called by g-code parser when setting spindle state and requires a buffer sync.
  #define spindle_sync(state, rpm) _spindle_sync(state)
  void _spindle_sync(uint8_t state);

  // Sets spindle running state with direction and enable.
  #define spindle_set_state(state, rpm) _spindle_set_state(state)
  void _spindle_set_state(uint8_t state);

#endif

// Stop and start spindle routines. Called by all spindle routines and stepper ISR.
void spindle_stop();

// Spindle PWM duty last written, in 0.1 % steps (0-1000). For the display.
uint16_t spindle_get_pwm_permille();

// Port: the VFD's at-speed and fault relays and the collet. spindle_scan()
// runs in the input scan, spindle_service() in the main loop.
void spindle_scan(uint32_t fn);
void spindle_service();
uint8_t spindle_fault_active();     // fault input active (debounced)
uint8_t spindle_at_speed();         // at-speed input active (debounced)
#define SPINDLE_STATUS_NONE      0  // off, or not monitored
#define SPINDLE_STATUS_SPINUP    1  // waiting for the at-speed relay
#define SPINDLE_STATUS_AT_SPEED  2
#define SPINDLE_STATUS_LOST      3  // at-speed lost while cutting
#define SPINDLE_STATUS_FAULT     4  // VFD fault
uint8_t spindle_status();
uint8_t spindle_last_ccw();           // the last M3/M4 was M4
void spindle_cancel_wait();           // ends the wait of an M3/M4 for the speed (touch STOP)
uint8_t spindle_resume_wait();        // a hold's cycle start waits while true (protocol.c)
void spindle_door_restore_wait();     // the safety door's restore: at speed, or $54
uint8_t spindle_collet_is_open();
uint8_t spindle_collet_set(uint8_t open);   // returns a status code


#endif
