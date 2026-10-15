/*
  protocol.h - controls Grbl execution protocol and procedures
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

#ifndef protocol_h
#define protocol_h

// Line buffer size from the serial input stream to be executed.
// NOTE: Not a problem except for extreme cases, but the line buffer size can be too small
// and g-code blocks can get truncated. Officially, the g-code standards support up to 256
// characters. In future versions, this will be increased, when we know how much extra
// memory space we can invest into here or we re-write the g-code parser not to have this
// buffer.
#ifndef LINE_BUFFER_SIZE
  #define LINE_BUFFER_SIZE 80
#endif

// Define line flags. Includes comment type tracking and line overflow detection.
#define LINE_FLAG_OVERFLOW bit(0)
#define LINE_FLAG_COMMENT_PARENTHESES bit(1)
#define LINE_FLAG_COMMENT_SEMICOLON bit(2)
#define LINE_FLAG_VERBATIM bit(3)   // port: the argument of a $F command, kept as it comes

// Port: Grbl's input filter, shared by the two line sources (the PC and a
// program from the USB stick). Feeds one character into line[] (*count chars
// so far, *flags); returns true at the end of a line, line[] then terminated.
uint8_t protocol_line_char(char *line, uint8_t *count, uint8_t *flags, uint8_t c);

// Port: runs one filtered line - a $-command or G-code - and returns its
// status. from_job: the line comes from the USB stick (src/job.c).
uint8_t protocol_execute_line(char *line, uint8_t flags, uint8_t from_job);

// Port: hal_millis() of the last line received from the PC.
extern uint32_t protocol_last_line_ms;

// Starts Grbl main loop. It handles all incoming characters from the serial port and executes
// them as they complete. It is also responsible for finishing the initialization procedures.
void protocol_main_loop();

// Checks and executes a realtime command at various stop points in main program
void protocol_execute_realtime();
void protocol_exec_rt_system();

// Keeps the USB link, the display and status reports going from loops that
// cannot run protocol_execute_realtime() - the homing cycle and the critical
// alarm loop. Acts on nothing else: reset, door and cycle stop stay with the
// loop that calls it.
void protocol_service_link();

// Executes the auto cycle feature, if enabled.
void protocol_auto_cycle_start();

// Block until all buffered steps are executed
void protocol_buffer_synchronize();

#endif
