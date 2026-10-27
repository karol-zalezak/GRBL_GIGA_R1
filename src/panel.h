/*
  panel.h - operator panel of shield v4: two potentiometers, START, HOLD, STOP

  Part of Grbl-Giga, a port of Grbl 1.1 to the Arduino GIGA R1 WiFi.

  Grbl is free software: you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation, either version 3 of the License, or (at your option) any later
  version. Distributed WITHOUT ANY WARRANTY; see the GNU GPL for details.

  -------------------------------------------------------------------------------

  Straight to the GIGA's analog header, not isolated - short wires to a panel
  at the control cabinet (PINOUT.md, "Panel obsluhy"; pins in cpu_map.h):

     A4 (PC3) START   A5 (PC2) HOLD   A6 (PC0) STOP   buttons to GND, active low
     A0 (PC4) pot 1   A1 (PC5) pot 2                  ratiometric to 3.3 V

  Buttons ($39): START and HOLD are the cycle start and feed hold control
  inputs, OR-ed into the input functions, so they take the same debounce and
  edges as those functions on an input channel. STOP ends the program with a
  controlled stop: a feed hold, and once the machine stands Grbl is reset
  with the machine declared stopped - the position is kept, no alarm, the
  spindle and coolant go off with the reset. A program from the USB stick is
  stopped the way its own STOP does it; homing, which knows no hold, is
  reset at once. Not the safety STOP on D49.

  Potentiometers ($44 for A0, $45 for A1): 0 none, 1 feed override, 2 spindle
  override, 3 rapid override. A potentiometer sets its override when it is
  turned - and once at start-up and after every reset - through Grbl's own
  override steps, so the PC and the touch screen can still change the
  override in between. Feed and spindle: 10-100 % over the left half,
  100 % in a band around the middle, 100-200 % over the right half. Rapid:
  thirds, 25 / 50 / 100 %.
*/

#ifndef panel_h
#define panel_h

// $44, $45
#define PANEL_POT_NONE      0
#define PANEL_POT_FEED      1
#define PANEL_POT_SPINDLE   2
#define PANEL_POT_RAPID     3
#define PANEL_N_POT_FUNCS   4

// Panel button bits (panel_buttons(), $TPNL).
#define PANEL_BTN_START     bit(0)
#define PANEL_BTN_HOLD      bit(1)
#define PANEL_BTN_STOP      bit(2)

// Pins and state from $39/$44/$45. Every reset and every change of those.
void panel_init();

// The input scan: the input functions the pressed buttons stand for
// (IN_FN_START, IN_FN_HOLD) - and the STOP button's debounce and edge.
uint32_t panel_scan();
// The same functions, read now, for the reports.
uint32_t panel_functions();

// From every wait loop (protocol_service_machine): the controlled stop and
// the potentiometers.
void panel_service();

// For $I: pressed buttons (PANEL_BTN_*), the potentiometers' readings 0-4095.
uint8_t panel_buttons();
uint16_t panel_pot(uint8_t n);

// Test hooks. $TPNL=mask presses buttons (PANEL_BTN_* bits) as if their pins
// were low; $TPNL=0 releases them; $TPNL=mask,ms does it ms from now (Grbl
// parses no line during a hold). $TPOT=n,value gives potentiometer n (1, 2)
// the reading value (0-4095) instead of its pin; $TPOT=n,-1 hands it back.
uint8_t panel_test(char *line);

#endif
