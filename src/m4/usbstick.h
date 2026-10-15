/*
  usbstick.h - the USB stick on the USB-A port (M4 core)

  Part of Grbl-Giga, a port of Grbl 1.1 to the Arduino GIGA R1 WiFi.

  Grbl is free software: you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation, either version 3 of the License, or (at your option) any later
  version. Distributed WITHOUT ANY WARRANTY; see the GNU GPL for details.
*/

#ifndef usbstick_h
#define usbstick_h

// Clears the shared job block and starts the USB stick thread.
void usbstick_start(void);

#endif
