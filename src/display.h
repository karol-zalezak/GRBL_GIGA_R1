/*
  display.h - status display glue (M7 side)

  Part of Grbl-Giga, a port of Grbl 1.1 to the Arduino GIGA R1 WiFi.

  Grbl is free software: you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation, either version 3 of the License, or (at your option) any later
  version. Distributed WITHOUT ANY WARRANTY; see the GNU GPL for details.
*/

#ifndef display_h
#define display_h

// Sets up the shared memory block. Called once, early in grbl_init().
void display_init(void);

// Starts the M4 and begins publishing. Called once Grbl's pins are configured.
void display_start(void);

// Publishes the machine state for the M4 every DISPLAY_REFRESH_MS and takes
// real-time commands from the touch screen. Called from protocol_exec_rt_system().
void display_service(void);

// Runs a pending touch-screen action (jog, home, unlock, zero) as the command
// it stands for. Called from protocol_main_loop() only between two lines from
// the PC - never with part of a line received - so a touch action can never
// split a line a sender is streaming. Its ok/error goes to the display, not
// the serial port, where it would confuse a sender counting replies.
void display_touch_service(void);

// $TOUCH=x,y[,ms[,delay]]: presses the touch screen at landscape (x, y) for ms
// milliseconds (default 100), delay ms from now (default 0) and after any
// press queued before it, as if a finger did. For automated tests - the
// delay lets a test queue a press for a state in which Grbl reads no lines,
// such as START during a feed hold.
uint8_t display_simulate_touch(char *line);

// $SPLASH[=ms]: the display shows its splash screen again, for at least ms
// (default 3000), as after power-up - for screenshots and tests.
uint8_t display_splash(char *line);

// Remember the alarm / error the display should show. Called from report.c.
void display_note_alarm(uint8_t code);
void display_note_error(uint8_t code);

// $I: " touch=<status>/<presses>@<x>,<y>/<res x>x<res y>" - status 1 means the
// GT911 answered, 0xE0.. that it did not (see m4/touch.cpp).
void display_report_diag(void);

// $SCREEN: writes "[SCREEN:480x800:RGB565:<bytes>]", a CR LF, then the raw
// framebuffer of the panel (portrait, little-endian RGB565). For test tools.
uint8_t display_dump_screen(void);

#endif
