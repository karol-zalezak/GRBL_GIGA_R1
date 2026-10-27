/*
  outputs.h - the switched output functions (spindle, coolant, AUX0-AUX5,
              collet, reset pulses) on the universal channels OUT1-OUT14

  Part of Grbl-Giga, a port of Grbl 1.1 to the Arduino GIGA R1 WiFi.

  Grbl is free software: you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation, either version 3 of the License, or (at your option) any later
  version. Distributed WITHOUT ANY WARRANTY; see the GNU GPL for details.

  -------------------------------------------------------------------------------

  Upstream Grbl drives the spindle and coolant pins straight from
  spindle_control.c and coolant_control.c, each with its own compile-time
  invert option. Here every output is a *function* (io.h, OUT_FN_*) that a
  setting puts on one of the fourteen channels ($80-$92), with one run-time
  polarity mask ($93). This module remembers what every function was last
  told to do - which is what the display shows and what spindle_get_state()
  and coolant_get_state() report, instead of reading pins back - and writes
  the channel the function is assigned to, if any.

  An aux output is on when M62/M64 switched it on, or when the spindle runs and
  its bit is set in $46 (default: AUX0, so the extraction starts and stops with
  the spindle without any M-code in the program).
*/

#ifndef outputs_h
#define outputs_h

#define N_OUTPUTS           N_OUT_FUNCS

#define OUTPUT_SPINDLE_FWD  OUT_FN_SPINDLE_FWD
#define OUTPUT_SPINDLE_REV  OUT_FN_SPINDLE_REV
#define OUTPUT_FLOOD        OUT_FN_FLOOD
#define OUTPUT_MIST         OUT_FN_MIST
#define OUTPUT_AUX0         OUT_FN_AUX0      // AUX1..AUX5 follow
#define OUTPUT_COLLET       OUT_FN_COLLET
#define OUTPUT_SERVO_RESET  OUT_FN_SERVO_RESET
#define OUTPUT_VFD_RESET    OUT_FN_VFD_RESET

// Switches every function off. Called on every reset.
void outputs_init();

// Central STOP: everything off at once, from its interrupt.
void outputs_all_off();

// Sets function fn (OUTPUT_*) to the logical state `on`. Safe from interrupts.
void outputs_set(uint8_t fn, uint8_t on);

// Logical state of one function, or of all of them as a mask (bit = OUTPUT_*).
uint8_t outputs_get(uint8_t fn);
uint16_t outputs_get_mask();

// M62-M65: manual state of aux output n (0-5).
void outputs_aux_set(uint8_t aux, uint8_t on);
void outputs_aux_all_off();

// Tells the module whether the spindle runs, for the $46 auto outputs and for
// the collet's run-down time.
void outputs_spindle_running(uint8_t running);
// Milliseconds since the spindle outputs went off; 0 while the spindle runs.
uint32_t outputs_spindle_off_ms();

// Re-applies every channel after $46 or an assignment changed.
void outputs_refresh();

// Switches function fn off, before its assignment changes.
void outputs_function_off(uint8_t fn);

// A pulse of `ms` on function fn (the reset outputs). outputs_service(),
// called from the main loop, ends it.
void outputs_pulse(uint8_t fn, uint16_t ms);
void outputs_service();

#endif
