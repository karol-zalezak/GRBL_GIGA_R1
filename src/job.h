/*
  job.h - running G-code from the USB stick (M7 side)

  Part of Grbl-Giga, a port of Grbl 1.1 to the Arduino GIGA R1 WiFi.

  Grbl is free software: you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation, either version 3 of the License, or (at your option) any later
  version. Distributed WITHOUT ANY WARRANTY; see the GNU GPL for details.
*/

#ifndef job_h
#define job_h

// Clears the shared job block. Once at power-up, before the M4 starts.
void job_boot();

// A program from the stick is being checked, run or stopped.
uint8_t job_active();
// It is in its run (not the check pass).
uint8_t job_running();
// The serial $ command `line` is allowed while a program runs.
uint8_t job_allows(char *line);

// Starts the file `name` (as listed). from_display: also wants the PC quiet.
uint8_t job_start(const char *name, uint8_t from_display);
// STOP: hold, then a reset that keeps the position; alarm = 0 or the alarm
// to end in. Safe from any context.
void job_stop(uint8_t alarm);

// Main loop: feeds the program's lines. Every wait loop: finishes a STOP,
// notices an alarm. Every reset: ends a job the reset was not for.
void job_service();
void job_rt_service();
void job_reset();

// M2/M30 parsed while a program runs.
void job_program_end();

// |SD:... in the status report while a program runs.
void job_report_status();

// Formats the stick (FORMAT on the display, $FF=ERASE; $FF=EXFAT: exFAT
// whatever its size).
uint8_t job_format(uint8_t exfat);

// $F commands: $F lists the stick and the job, $F=name runs a file, $FS
// stops it, $FR reads the directory again, $FT=name is a read test, $FW
// uploads a file to the stick, $FD deletes one, $FF=ERASE formats the
// stick, $FX=lba shows a sector.
uint8_t job_command(char *line);

#endif
