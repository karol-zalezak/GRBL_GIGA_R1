/*
  eeprom.h - EEPROM methods
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
*/

#ifndef eeprom_h
#define eeprom_h

unsigned char eeprom_get_char(unsigned int addr);
void eeprom_put_char(unsigned int addr, unsigned char new_value);
void memcpy_to_eeprom_with_checksum(unsigned int destination, char *source, unsigned int size);
int memcpy_from_eeprom_with_checksum(char *destination, unsigned int source, unsigned int size);

// Writes the RAM shadow back to flash when the machine is idle and
// something has changed. Polled by protocol_main_loop().
void eeprom_flush_pending();

#endif
