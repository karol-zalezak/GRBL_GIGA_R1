/*
  drvpwr.h - driver power input (supply voltage of the step and servo drives)

  Part of Grbl-Giga, a port of Grbl 1.1 to the Arduino GIGA R1 WiFi.

  Grbl is free software: you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation, either version 3 of the License, or (at your option) any later
  version. Distributed WITHOUT ANY WARRANTY; see the GNU GPL for details.

  -------------------------------------------------------------------------------

  The GIGA keeps running from USB when the drives lose their supply, and the
  step counter then still claims a position the motors have lost. This input
  watches the drive supply through an optocoupler of its own on the shield
  and makes Grbl agree with the drives:

    - supply gone for 2 ms (DRVPWR_LOSS_SCANS): motion stops at once, the
      planner is flushed and Grbl goes to ALARM:22 - from any state, idle
      included, since an unpowered motor can be turned by hand. Already in
      an alarm, ALARM:22 is reported all the same, so that a position kept
      over an earlier alarm is not trusted any more.
    - while the supply is off, and until it has been back for $95 ms (drives
      take that long to boot), $H and $X are refused with error:50; when it
      is back Grbl says [MSG:Driver power on].

  Not a critical alarm: no reset needed, $H or $X once the drives are on.
  The pin is fixed (DRVPWR_PIN, D53 = PG7) and read by the 10 kHz input scan.
  $94 switches the watch on, bit 21 of $79 inverts the input (for a drive's
  "ready" contact that opens when ready, say).
*/

#ifndef drvpwr_h
#define drvpwr_h

// drvpwr_state()
#define DRVPWR_NOT_WATCHED  0   // $94=0
#define DRVPWR_OFF          1   // no supply: alarm, $H/$X refused
#define DRVPWR_RETURNING    2   // supply back, $95 not yet over
#define DRVPWR_ON           3

// Re-reads $94/$95. Called at power-up and when those settings change.
void drvpwr_init();

// The scan's path: channels = io_read_channels() (bit IO_DRVPWR_BIT = supply present).
void drvpwr_scan(uint32_t channels);

// True when not watched, or when the supply has been present for $95 ms.
uint8_t drvpwr_ok();

uint8_t drvpwr_state();          // DRVPWR_*
uint32_t drvpwr_loss_count();    // losses since power-up, for $I

// Main loop: reports the supply coming back.
void drvpwr_service();

#endif
