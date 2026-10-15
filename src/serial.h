/*
  serial.h - Low level functions for sending and receiving bytes via the PC link
  Part of Grbl

  Copyright (c) 2011-2016 Sungeun K. Jeon for Gnea Research LLC
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

#ifndef serial_h
#define serial_h

// The GIGA has RAM to spare, so the receive buffer is eight times larger than
// AVR Grbl's. Senders that use character counting read the size from the $I
// [OPT:] field or the status `Bf:` field and stream correspondingly deeper.
#ifndef RX_BUFFER_SIZE
  #define RX_BUFFER_SIZE 1024
#endif

#define SERIAL_NO_DATA 0xff


// Receive-ring overflows since power-up; see serial.c.
extern uint32_t serial_rx_dropped;

void grbl_serial_init();   // upstream serial_init(); renamed, mbed exports one

// Writes one byte to the TX serial buffer. Called by main program.
void serial_write(uint8_t data);

// Fetches the first byte in the serial read buffer. Called by main program.
uint8_t serial_read();

// Reset and empty data in read buffer. Used by e-stop and reset.
void serial_reset_read_buffer();

// Returns the number of bytes available in the RX serial buffer.
uint16_t serial_get_rx_buffer_available();

// Returns the number of bytes used in the RX serial buffer.
// NOTE: Deprecated. Not used unless classic status reports are enabled in config.h.
uint16_t serial_get_rx_buffer_count();

// Services the transport: pushes queued output and, where the hardware has no
// per-character receive interrupt, pulls in received bytes. Called from
// protocol_execute_realtime().
void serial_pump();

#endif
