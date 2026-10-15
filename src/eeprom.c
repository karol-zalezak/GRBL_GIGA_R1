/*
  eeprom.c - EEPROM emulation in the STM32H747 internal flash
  Part of Grbl

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

  The STM32H747 has no EEPROM, so Grbl's 1 kB byte-addressable store is emulated in
  internal flash by hal_nvs.cpp. Reads and writes here hit a RAM shadow, which keeps
  them free and keeps the flash from wearing out; the shadow is written back to
  flash by eeprom_flush_pending().

  Writing back is deferred rather than immediate for two reasons. Flash is
  written a record at a time, so a $-command that touches a dozen bytes would
  otherwise cost a dozen records instead of one. And programming takes a few
  milliseconds (a sector erase, once every 124 commits, a second or two),
  which is not something to run while the machine is moving - so the flush waits for an idle machine with
  an empty planner. protocol_main_loop() polls it, so in practice the write
  lands within microseconds of the "ok" for a settings command.

  The checksum functions are kept bit-for-bit identical to upstream Grbl,
  including the well-known `||` typo in the rotate, so that settings blobs stay
  compatible with what the rest of the Grbl codebase expects.
*/

#include "grbl.h"


unsigned char eeprom_get_char(unsigned int addr)
{
  return hal_nvs_read((uint16_t)addr);
}


void eeprom_put_char(unsigned int addr, unsigned char new_value)
{
  hal_nvs_write((uint16_t)addr, new_value);
}


// Commits the shadow to flash if anything changed and the machine is in a
// state where a multi-millisecond flash operation is harmless. Called from
// protocol_main_loop().
void eeprom_flush_pending()
{
  if (!hal_nvs_dirty()) { return; }
  if (sys.state != STATE_IDLE) { return; }
  if (plan_get_current_block() != NULL) { return; }

  hal_nvs_commit();
}


// Extensions added as part of Grbl


void memcpy_to_eeprom_with_checksum(unsigned int destination, char *source, unsigned int size) {
  unsigned char checksum = 0;
  for(; size > 0; size--) {
    checksum = (checksum << 1) || (checksum >> 7);
    checksum += *source;
    eeprom_put_char(destination++, *(source++));
  }
  eeprom_put_char(destination, checksum);
}

int memcpy_from_eeprom_with_checksum(char *destination, unsigned int source, unsigned int size) {
  unsigned char data, checksum = 0;
  for(; size > 0; size--) {
    data = eeprom_get_char(source++);
    checksum = (checksum << 1) || (checksum >> 7);
    checksum += data;
    *(destination++) = data;
  }
  return(checksum == eeprom_get_char(source));
}

// end of file
