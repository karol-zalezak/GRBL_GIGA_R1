/*
  mpg.h - handwheel pendant (manual pulse generator)

  Part of Grbl-Giga, a port of Grbl 1.1 to the Arduino GIGA R1 WiFi.

  Grbl is free software: you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation, either version 3 of the License, or (at your option) any later
  version. Distributed WITHOUT ANY WARRANTY; see the GNU GPL for details.

  -------------------------------------------------------------------------------

  A handwheel pendant such as the EHDW-BG6L: a quadrature wheel (A/B, 100
  detents a turn), an axis selector OFF/X/Y/Z/4/5/6 and a step selector
  x1/x10/x100, each position a contact to the pendant's 0 V, and an LED. It
  comes in on the camera pins, which the Display Shield passes through its
  socket J3 (PLAN.md part 5; pins in cpu_map.h):

     wheel A  D54 (PI5 = TIM8_CH1)     wheel B  D59 (PI6 = TIM8_CH2)
     axis     X D55, Y D56, Z D57, 4 D58 (the A axis)
     step     x1 D60, x10 D61, x100 D62           LED  D63

  TIM8 counts the wheel in hardware (encoder mode, every edge of both
  inputs, filtered). The selector inputs are active low - the pendant's
  contact, or the shield's optocoupler, pulls the pin against its pull-up -
  and are debounced for MPG_DEBOUNCE_MS. The LED output is active high.

  Every MPG_PERIOD_MS, between two lines from the PC, the detents turned
  become one jog of detents x step on the selected axis ($J=G91 ...), at the
  feed that covers it in one period, at most $98 and the axis's max rate. A
  block holds at most one period of motion at that feed, no more than
  MPG_MAX_BLOCKS blocks are queued and no more than MPG_PENDING_PERIODS wait
  - turning faster than the axis can follow is dropped, not stored up - so
  the axis stops within some 100 ms of the wheel. The wheel moves the
  machine only in Idle or during its own jog: not during a program, a hold,
  an alarm or a jog from elsewhere.
  Selecting another axis, or OFF, cancels its jog at once. At a soft limit
  the axis stops at the limit instead of the jog being refused.

     $96  0 off, 1 on, 2 on with the direction reversed
     $97  step at x1 (mm, degrees for a rotary A); x10 and x100 are 10x, 100x
     $98  highest feed the wheel moves an axis at, mm/min

  LED: on = the wheel moves the selected axis, blinking = an axis is selected
  but the machine cannot jog now, off = OFF (or no pendant, or $96=0).
*/

#ifndef mpg_h
#define mpg_h

// mpg_state()
#define MPG_DISABLED  0   // $96=0
#define MPG_NO_AXIS   1   // selector at OFF (or 5, 6; or nothing plugged in)
#define MPG_READY     2   // an axis is selected and the wheel moves it
#define MPG_BLOCKED   3   // an axis is selected, but the machine cannot jog now

// Pins and TIM8 (first call only). Called on every reset.
void mpg_init();

// From every wait loop (protocol_service_machine): selector debounce, LED,
// and cancelling the wheel's jog when the selection changes.
void mpg_rt_service();

// Main loop, between two lines from the PC: turns the wheel into jogs.
void mpg_service();

// For $I and the display.
uint8_t mpg_state();
uint8_t mpg_axis();          // selected axis 0-3, 0xFF none
uint8_t mpg_mult();          // 1, 10 or 100
uint16_t mpg_counter();      // TIM8 count, raw
uint8_t mpg_inputs();        // selector inputs, active = 1: bit 0-3 X Y Z 4, bit 4-6 x1 x10 x100

// Test hooks.
// $TMPG=axis,mult: the selector as if turned to axis (0 OFF, 1-4 = X Y Z 4)
// and mult (0 none, 1, 10, 100), by pulling those pins down - the same path
// as the pendant's contacts. $TMPG=0,0 releases them.
uint8_t mpg_test_select(uint8_t axis, uint16_t mult);
// $TMPW=detents[,rate]: turns the wheel by detents (negative: backwards) at
// rate detents a second (default 100), by switching the pulls of the A and B
// pins in quadrature - TIM8 counts the real edges. Driven from the input scan.
uint8_t mpg_test_turn(int32_t detents, uint32_t rate);
uint8_t mpg_test_busy();     // the simulated turn has not finished yet
void mpg_test_tick();        // input scan, every tick

#endif
