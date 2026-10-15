/*
  io.h - universal input and output channels and their functions

  Part of Grbl-Giga, a port of Grbl 1.1 to the Arduino GIGA R1 WiFi.

  Grbl is free software: you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation, either version 3 of the License, or (at your option) any later
  version. Distributed WITHOUT ANY WARRANTY; see the GNU GPL for details.

  -------------------------------------------------------------------------------

  The peripheral shield has twenty optocoupled inputs IN1-IN20 and twelve
  transistor outputs OUT1-OUT12 (pins in cpu_map.h). None of them has a fixed
  job: every input function and every output function is a setting holding
  the number of the channel that carries it, 0 for "not used".

      $60-$67  limit X-, X+, Y-, Y+, Z-, Z+, A-, A+         IN1-IN8
      $68-$71  servo alarm X, Y, Z, A                       IN9-IN12
      $72      probe                                        IN13
      $73-$76  reset, feed hold, cycle start, safety door   IN14-IN17
      $77      spindle at speed                             IN18
      $78      spindle fault                                IN19
      $79      NC mask: bit n-1 = INn, bit 20 = STOP, bit 21 = DRV PWR

      $80-$81  spindle FWD, spindle REV                     OUT1-OUT2
      $82-$83  flood (M8), mist (M7)                        OUT3-OUT4
      $84-$89  AUX0-AUX5 (M62-M65 P0-P5)                    OUT5-OUT8, OUT12, -
      $90      collet open                                  OUT9
      $91      servo alarm reset (pulse)                    OUT10
      $92      VFD fault reset (pulse)                      OUT11
      $93      inverted outputs: bit n-1 = OUTn

  An input channel is "on" when its optocoupler conducts, which pulls the pin
  low against the internal pull-up. A channel's bit in $79 says the contact
  behind it is normally closed: the channel is then active while the
  optocoupler is OFF - which also makes a broken wire read as active.

  Limit inputs may share a channel: X+ on the same channel as X- is one input
  for two switches in series, as in upstream Grbl. Any other sharing, and any
  output on two functions, is refused when the setting is stored.

  The tables built from the settings are read by the 10 kHz input scan and,
  for the probe, by the stepper interrupt, so each pin is turned into a port
  and a bit mask once, when a setting changes, not on every read.
*/

#ifndef io_h
#define io_h

#define N_IN_CHANNELS     20
#define N_OUT_CHANNELS    12
#define IO_ESTOP_BIT      20     // the STOP input's bit in channel words and in $79
#define IO_DRVPWR_BIT     21     // the driver power input's bit (drvpwr.h)
#define IO_N_IN_PINS      (N_IN_CHANNELS + 2)   // channels, STOP, DRV PWR

// Input functions, in the order of their settings.
#define IN_FN_SETTING     60
#define IN_FN_LIM_XN      0      // $60 ... the eight limits are 2*axis + (0 = minus, 1 = plus)
#define IN_FN_LIM_AP      7      // $67
#define IN_FN_ALM_X       8      // $68 ... servo alarm of axis n is IN_FN_ALM_X + n
#define IN_FN_PROBE       12     // $72
#define IN_FN_RESET       13     // $73
#define IN_FN_HOLD        14     // $74
#define IN_FN_START       15     // $75
#define IN_FN_DOOR        16     // $76
#define IN_FN_AT_SPEED    17     // $77
#define IN_FN_SPIN_FAULT  18     // $78
#define N_IN_FUNCS        19
#define IN_INVERT_SETTING 79

// Output functions, in the order of their settings.
#define OUT_FN_SETTING    80
#define OUT_FN_SPINDLE_FWD  0    // $80
#define OUT_FN_SPINDLE_REV  1    // $81
#define OUT_FN_FLOOD        2    // $82
#define OUT_FN_MIST         3    // $83
#define OUT_FN_AUX0         4    // $84 ... AUXn is OUT_FN_AUX0 + n
#define N_AUX_OUTPUTS       6
#define OUT_FN_COLLET       10   // $90
#define OUT_FN_SERVO_RESET  11   // $91
#define OUT_FN_VFD_RESET    12   // $92
#define N_OUT_FUNCS         13
#define OUT_INVERT_SETTING  93

// Function-word bit masks (bit f = input function f active).
#define IN_FN_LIMIT_MASK    0x000000FFUL
#define IN_FN_ALARM_MASK    0x00000F00UL
#define IN_FN_CONTROL_MASK  (bit(IN_FN_RESET)|bit(IN_FN_HOLD)|bit(IN_FN_START)|bit(IN_FN_DOOR))
#define IN_FN_SPINDLE_MASK  (bit(IN_FN_AT_SPEED)|bit(IN_FN_SPIN_FAULT))

// Configures the pins (first call only) and builds the tables from settings.
void io_init();

// Rebuilds the tables after a mapping or polarity setting changed.
void io_rebuild();

// Every input channel at once, after $79: bit n-1 = INn active, bit 20 = STOP,
// bit 21 = driver supply present.
uint32_t io_read_channels();

// The input functions those channels carry: bit f = function f active. A
// function without a channel is never active.
uint32_t io_functions(uint32_t channels);

// Shorthand for io_functions(io_read_channels()).
uint32_t io_read_functions();

// Channel (1-20) of input function f, 0 if unused.
uint8_t io_in_channel(uint8_t fn);

// Probe input, after $79. Fast: the stepper interrupt calls it on every tick
// while probing. Always false when no channel carries the probe.
uint8_t io_probe_raw();

// Output channels: logical state (before $93), bit n-1 = OUTn.
void io_out_write(uint8_t channel, uint8_t on);   // channel 1-12; 0 is ignored
uint16_t io_out_state();

// Channel (1-12) of output function f, 0 if unused.
uint8_t io_out_channel(uint8_t fn);

// Checks a new channel number for input / output function fn against the
// other assignments. Returns a status code (STATUS_OK if it may be stored).
uint8_t io_check_in_assignment(uint8_t fn, uint8_t channel);
uint8_t io_check_out_assignment(uint8_t fn, uint8_t channel);

// Test hook ($TIN=n,v): v=1 pulls input channel n (1-20, 21 = STOP, 22 =
// DRV PWR) down,
// which reads exactly like its optocoupler conducting; v=0 restores the
// pull-up. $TIN=0,0 restores all. The inputs then take their normal path -
// debouncing, the STOP interrupt, everything.
uint8_t io_test_input(uint8_t channel, uint8_t on);
uint32_t io_test_mask();   // channels currently pulled down by the test hook
// $TIN=n,v,ms: the same, ms from now (Grbl parses no line during a hold or a
// homing; the change still lands). io_test_service() runs from the main loop.
uint8_t io_test_input_later(uint8_t channel, uint8_t on, uint32_t delay_ms);
void io_test_service();

// Test hook ($TSW=n,X+10): input channel n reads as active while the machine
// position of the axis is at or beyond mm in that direction - a virtual
// limit switch that closes and opens with the motion, for homing tests.
// Cartesian positions only (it compares motor steps). $TSW=0 removes all;
// a reset does not.
uint8_t io_test_switch(uint8_t channel, uint8_t axis, int8_t dir, float mm);

#endif
