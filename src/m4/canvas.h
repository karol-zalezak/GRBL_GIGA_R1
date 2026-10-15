/*
  canvas.h - drawing for the GIGA Display Shield on the M4

  Part of Grbl-Giga, a port of Grbl 1.1 to the Arduino GIGA R1 WiFi.

  Grbl is free software: you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation, either version 3 of the License, or (at your option) any later
  version. Distributed WITHOUT ANY WARRANTY; see the GNU GPL for details.

  -------------------------------------------------------------------------------

  The panel is 480x800 portrait; the UI is 800x480 landscape. Every widget is
  drawn into a scratch buffer in the M4's own SRAM, already laid out in the
  panel's orientation, and then copied into the framebuffer in SDRAM with one
  DMA2D transfer. Each pixel of the framebuffer is written once per update,
  with its final colour, so nothing ever flickers and no second framebuffer is
  needed.

  Coordinates passed to the cv_* calls are landscape and relative to the
  widget opened with cv_begin().
*/

#ifndef canvas_h
#define canvas_h

#include <stdint.h>
#include "fonts.h"

#define SCREEN_W  800
#define SCREEN_H  480

// RGB565 from 8-bit components.
#define RGB(r, g, b)  ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

// Brings up SDRAM, the DSI host and the panel; clears the screen. Returns
// false if the display did not come up.
bool cv_init(uint16_t color);

// Starts a widget at (x, y) of w x h, filled with bg. Returns false if it does
// not fit the scratch buffer.
bool cv_begin(int x, int y, int w, int h, uint16_t bg);
// Copies the widget to the screen.
void cv_end(void);

void cv_fill(int x, int y, int w, int h, uint16_t c);
void cv_frame(int x, int y, int w, int h, int t, uint16_t c);
void cv_disc(int cx, int cy, int r, uint16_t c, uint16_t bg);
void cv_text(int x, int y, const font_t *f, const char *s, uint16_t fg, uint16_t bg);
int  cv_text_width(const font_t *f, const char *s);
// A w x h coverage bitmap in the fonts' format (4 bits a pixel, two a byte,
// high nibble first, rows of (w + 1) / 2 bytes), drawn in fg over bg.
void cv_bitmap4(int x, int y, int w, int h, const uint8_t *data, uint16_t fg, uint16_t bg);

// Fills a whole screen rectangle straight in the framebuffer (DMA2D).
void cv_screen_fill(int x, int y, int w, int h, uint16_t c);

// The framebuffer, for the M7's screenshot command.
uint32_t cv_framebuffer(void);

#endif
