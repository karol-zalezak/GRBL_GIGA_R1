/*
  stepper.h - stepper motor driver: executes motion plans of planner.c using the stepper motors
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

#ifndef stepper_h
#define stepper_h

#ifndef SEGMENT_BUFFER_SIZE
  #define SEGMENT_BUFFER_SIZE 6
#endif

// Initialize and setup the stepper motor subsystem
void stepper_init();

// Enable steppers, but cycle does not start unless called by motion control or realtime command.
void st_wake_up();

// Immediately disables steppers
void st_go_idle();

// Generate the step and direction port invert masks.
void st_generate_step_dir_invert_masks();

// Reset the stepper subsystem variables
void st_reset();

// Changes the run state of the step segment buffer to execute the special parking motion.
void st_parking_setup_buffer();

// Restores the step segment buffer to the normal run state after a parking motion.
void st_parking_restore_buffer();

// Reloads step segment buffer. Called continuously by realtime execution system.
void st_prep_buffer();

// Called by planner_recalculate() when the executing block is updated by the new plan.
void st_update_plan_block_parameters();

// Called by realtime status reporting if realtime rate reporting is enabled in config.h.
float st_get_realtime_rate();

// Step pulses put out on each driver since power-up, +1 / -1 by direction,
// slave pulses included (bit n of the step port = motor n).
extern volatile int32_t st_diag_motor[N_AXIS];

// Limit switch sides the running block heads for (bit 2*axis = minus,
// 2*axis+1 = plus); 0 while stopped. Read by the input scan.
extern volatile uint8_t st_axis_dirs;

// Direction setup ($56): steps that were held back after a direction change,
// and the shortest direction-to-step time since the last reset (core cycles).
extern volatile uint32_t st_diag_dir_delays;
extern volatile uint32_t st_diag_dir_gap_min;

// Driver enable and Z brake sequencing ($1, $48-$50, $59). The service runs
// from the main loop; refresh after $59 changed.
void st_power_service();
void st_power_refresh();
uint8_t st_drivers_enabled();
uint8_t st_brake_released();

// Central STOP: stop the steps, brake on, drives off. From its interrupt.
void st_estop_halt();

#endif
