/*
  touch.h - touch controller and RGB LED of the GIGA Display Shield (M4)

  Part of Grbl-Giga, a port of Grbl 1.1 to the Arduino GIGA R1 WiFi.

  Grbl is free software: you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation, either version 3 of the License, or (at your option) any later
  version. Distributed WITHOUT ANY WARRANTY; see the GNU GPL for details.
*/

#ifndef touch_h
#define touch_h

#include <stdint.h>

#define TOUCH_OK            1
#define TOUCH_ERR_NO_ANSWER 0xE1    // nothing at 0x5D or 0x14
#define TOUCH_ERR_CONFIG    0xE2    // answered, but its configuration could not be read

// Resets the controller and checks it answers. Returns TOUCH_OK or an error.
uint32_t touch_init(void);

// Polls the controller. Returns -1 when it has no new report, otherwise the
// number of fingers on the panel (0 = all lifted), with the first one in
// *x, *y in landscape screen pixels.
int touch_read(uint16_t *x, uint16_t *y);

// The resolution the controller's configuration reports (normally 800 x 480).
void touch_resolution(uint16_t *w, uint16_t *h);

// The shield's RGB LED (IS31FL3197, same I2C bus). rgb_init() returns 1 if it
// answered, 0xE1 if not; rgb_set() does nothing then.
uint32_t rgb_init(void);
void rgb_set(uint8_t r, uint8_t g, uint8_t b);

#endif
