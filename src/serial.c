/*
  serial.c - Low level functions for sending and receiving bytes via the PC link
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

  -------------------------------------------------------------------------------

  Only the receive path lives here now. Transmit buffering moved into the HAL,
  which owns the transport-specific ring and its completion interrupt, so this
  file no longer needs to know whether the PC link is a UART or USB CDC.

  grbl_serial_rx_char() is the direct descendant of AVR Grbl's ISR(SERIAL_RX):
  it picks real-time command characters out of the stream before they can reach
  the line buffer. On the GIGA it runs in the USB receive callback, so reset
  and feed hold act with the same immediacy as on AVR.
*/

#include "grbl.h"

#define RX_RING_BUFFER (RX_BUFFER_SIZE+1)

uint8_t serial_rx_buffer[RX_RING_BUFFER];
// Bytes discarded because the receive ring was full. A sender that honours the
// buffer size Grbl reports should never cause this; if it climbs, either the
// sender is over-filling or something downstream is not draining.
uint32_t serial_rx_dropped = 0;
uint16_t serial_rx_buffer_head = 0;
volatile uint16_t serial_rx_buffer_tail = 0;


// Returns the number of bytes available in the RX serial buffer.
uint16_t serial_get_rx_buffer_available()
{
  uint16_t rtail = serial_rx_buffer_tail; // Copy to limit multiple calls to volatile
  if (serial_rx_buffer_head >= rtail) { return(RX_BUFFER_SIZE - (serial_rx_buffer_head-rtail)); }
  return((rtail-serial_rx_buffer_head-1));
}


// Returns the number of bytes used in the RX serial buffer.
// NOTE: Deprecated. Not used unless classic status reports are enabled in config.h.
uint16_t serial_get_rx_buffer_count()
{
  uint16_t rtail = serial_rx_buffer_tail; // Copy to limit multiple calls to volatile
  if (serial_rx_buffer_head >= rtail) { return(serial_rx_buffer_head-rtail); }
  return (RX_BUFFER_SIZE - (rtail-serial_rx_buffer_head));
}


void grbl_serial_init()
{
  hal_serial_init(BAUD_RATE);
}


// Writes one byte to the TX buffer. Called by main program.
void serial_write(uint8_t data)
{
  // Wait until there is space in the transport's ring. The HAL never blocks, so
  // spinning here is the only place output can stall - and this loop still lets
  // an abort break out, exactly as the AVR version did.
  while (hal_serial_tx_free() == 0) {
    hal_serial_pump();
    if (sys_rt_exec_state & EXEC_RESET) { return; } // Only check for abort to avoid an endless loop.
  }
  hal_serial_write_byte(data);
}


// Fetches the first byte in the serial read buffer. Called by main program.
uint8_t serial_read()
{
  uint16_t tail = serial_rx_buffer_tail; // Temporary serial_rx_buffer_tail (to optimize for volatile)
  if (serial_rx_buffer_head == tail) {
    return SERIAL_NO_DATA;
  } else {
    uint8_t data = serial_rx_buffer[tail];

    tail++;
    if (tail == RX_RING_BUFFER) { tail = 0; }
    serial_rx_buffer_tail = tail;

    return data;
  }
}


// Handles one received character. Called from the receive interrupt on
// transports that have one, otherwise from serial_pump().
void grbl_serial_rx_char(uint8_t data)
{
  uint16_t next_head;

  // Pick off realtime command characters directly from the serial stream. These characters are
  // not passed into the main buffer, but these set system state flag bits for realtime execution.
  switch (data) {
    case CMD_RESET:         mc_reset(); break; // Call motion control reset routine.
    case CMD_STATUS_REPORT: system_set_exec_state_flag(EXEC_STATUS_REPORT); break; // Set as true
    case CMD_CYCLE_START:   system_set_exec_state_flag(EXEC_CYCLE_START); break; // Set as true
    case CMD_FEED_HOLD:     system_set_exec_state_flag(EXEC_FEED_HOLD); break; // Set as true
    default :
      if (data > 0x7F) { // Real-time control characters are extended ACSII only.
        switch(data) {
          case CMD_SAFETY_DOOR:   system_set_exec_state_flag(EXEC_SAFETY_DOOR); break; // Set as true
          case CMD_JOG_CANCEL:
            if (sys.state & STATE_JOG) { // Block all other states from invoking motion cancel.
              system_set_exec_state_flag(EXEC_MOTION_CANCEL);
            }
            break;
          #ifdef DEBUG
            case CMD_DEBUG_REPORT: {uint32_t sreg = GRBL_IRQ_SAVE(); bit_true(sys_rt_exec_debug,EXEC_DEBUG_REPORT); GRBL_IRQ_RESTORE(sreg);} break;
          #endif
          case CMD_FEED_OVR_RESET: system_set_exec_motion_override_flag(EXEC_FEED_OVR_RESET); break;
          case CMD_FEED_OVR_COARSE_PLUS: system_set_exec_motion_override_flag(EXEC_FEED_OVR_COARSE_PLUS); break;
          case CMD_FEED_OVR_COARSE_MINUS: system_set_exec_motion_override_flag(EXEC_FEED_OVR_COARSE_MINUS); break;
          case CMD_FEED_OVR_FINE_PLUS: system_set_exec_motion_override_flag(EXEC_FEED_OVR_FINE_PLUS); break;
          case CMD_FEED_OVR_FINE_MINUS: system_set_exec_motion_override_flag(EXEC_FEED_OVR_FINE_MINUS); break;
          case CMD_RAPID_OVR_RESET: system_set_exec_motion_override_flag(EXEC_RAPID_OVR_RESET); break;
          case CMD_RAPID_OVR_MEDIUM: system_set_exec_motion_override_flag(EXEC_RAPID_OVR_MEDIUM); break;
          case CMD_RAPID_OVR_LOW: system_set_exec_motion_override_flag(EXEC_RAPID_OVR_LOW); break;
          case CMD_SPINDLE_OVR_RESET: system_set_exec_accessory_override_flag(EXEC_SPINDLE_OVR_RESET); break;
          case CMD_SPINDLE_OVR_COARSE_PLUS: system_set_exec_accessory_override_flag(EXEC_SPINDLE_OVR_COARSE_PLUS); break;
          case CMD_SPINDLE_OVR_COARSE_MINUS: system_set_exec_accessory_override_flag(EXEC_SPINDLE_OVR_COARSE_MINUS); break;
          case CMD_SPINDLE_OVR_FINE_PLUS: system_set_exec_accessory_override_flag(EXEC_SPINDLE_OVR_FINE_PLUS); break;
          case CMD_SPINDLE_OVR_FINE_MINUS: system_set_exec_accessory_override_flag(EXEC_SPINDLE_OVR_FINE_MINUS); break;
          case CMD_SPINDLE_OVR_STOP: system_set_exec_accessory_override_flag(EXEC_SPINDLE_OVR_STOP); break;
          case CMD_COOLANT_FLOOD_OVR_TOGGLE: system_set_exec_accessory_override_flag(EXEC_COOLANT_FLOOD_OVR_TOGGLE); break;
          #ifdef ENABLE_M7
            case CMD_COOLANT_MIST_OVR_TOGGLE: system_set_exec_accessory_override_flag(EXEC_COOLANT_MIST_OVR_TOGGLE); break;
          #endif
        }
        // Throw away any unfound extended-ASCII character by not passing it to the serial buffer.
      } else { // Write character to buffer
        next_head = serial_rx_buffer_head + 1;
        if (next_head == RX_RING_BUFFER) { next_head = 0; }

        // Write data to buffer unless it is full.
        if (next_head != serial_rx_buffer_tail) {
          serial_rx_buffer[serial_rx_buffer_head] = data;
          serial_rx_buffer_head = next_head;
        } else {
          serial_rx_dropped++;
        }
      }
  }
}


void serial_pump()
{
  hal_serial_pump();
  hal_actled_service(sys.state == STATE_ALARM);
  hal_housekeeping();
  // An AVR board resets when a sender opens the port, so senders expect the
  // welcome banner then. Native USB does not reset; print it anyway.
  if (hal_serial_connect_event()) { report_init_message(); }
}


void serial_reset_read_buffer()
{
  serial_rx_buffer_tail = serial_rx_buffer_head;
}
