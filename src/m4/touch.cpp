/*
  touch.cpp - the GT911 touch controller of the GIGA Display Shield (M4)

  Part of Grbl-Giga, a port of Grbl 1.1 to the Arduino GIGA R1 WiFi.

  Grbl is free software: you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation, either version 3 of the License, or (at your option) any later
  version. Distributed WITHOUT ANY WARRANTY; see the GNU GPL for details.

  -------------------------------------------------------------------------------

  The shield's Goodix GT911 sits on Wire1 (I2C4: SDA PH12, SCL PB6), with its
  reset line on PI2 and its interrupt line on PI1. Holding INT low while
  reset is released selects I2C address 0x5D - the power-on sequence of the
  GT911 datasheet, as Arduino_GigaDisplayTouch does it.

  The controller is polled rather than interrupt driven: the display loop
  reads the status register every few milliseconds, which is all a button
  needs and keeps the M4 free of interrupt handlers. While a finger is down
  the GT911 posts a new report every scan (about 100 Hz); a report with no
  points means the finger lifted. Each report must be acknowledged by
  writing 0 to the status register.

  The GT911 on the shield is configured for 800 x 480, the same landscape
  orientation the display uses, so its coordinates are screen coordinates. If
  a controller reports a portrait configuration instead, the points are
  rotated the way canvas.cpp rotates the picture.
*/

#if defined(CORE_CM4)

#include <Arduino.h>
#include <Wire.h>
#include "pinDefinitions.h"
#include "touch.h"

#define GT911_ADDR          0x5D
#define GT911_ADDR_ALT      0x14
#define REG_CONFIG_VERSION  0x8047      // then X max (2 bytes), Y max (2 bytes)
#define REG_STATUS          0x814E      // bit 7 = report ready, bits 0-3 = points
#define REG_POINT1          0x814F      // track id, x (2), y (2), size (2), reserved

static uint8_t addr = GT911_ADDR;
static uint16_t res_x = 800, res_y = 480;
static bool ok = false;

static bool gt_read(uint16_t reg, uint8_t *buf, uint8_t n)
{
  Wire1.beginTransmission(addr);
  Wire1.write((uint8_t)(reg >> 8));
  Wire1.write((uint8_t)(reg & 0xFF));
  if (Wire1.endTransmission() != 0) { return false; }
  if (Wire1.requestFrom(addr, n) != n) { return false; }
  for (uint8_t i = 0; i < n; i++) { buf[i] = (uint8_t)Wire1.read(); }
  return true;
}

static bool gt_write(uint16_t reg, uint8_t v)
{
  Wire1.beginTransmission(addr);
  Wire1.write((uint8_t)(reg >> 8));
  Wire1.write((uint8_t)(reg & 0xFF));
  Wire1.write(v);
  return Wire1.endTransmission() == 0;
}

uint32_t touch_init(void)
{
  const int pin_int = PinNameToIndex(PI_1);
  const int pin_rst = PinNameToIndex(PI_2);

  Wire1.begin();
  Wire1.setClock(400000);     // only takes effect once begin() has made the bus

  // Power-on sequence, GT911 datasheet: reset low with INT low, INT stays low
  // when reset is released (address 0x5D), then INT becomes an input.
  pinMode(pin_rst, OUTPUT);
  pinMode(pin_int, OUTPUT);
  digitalWrite(pin_rst, LOW);
  digitalWrite(pin_int, LOW);
  delay(11);
  digitalWrite(pin_int, LOW);
  delayMicroseconds(110);
  digitalWrite(pin_rst, HIGH);
  delay(6);
  digitalWrite(pin_int, LOW);
  delay(51);
  pinMode(pin_int, INPUT);

  uint8_t cfg[5];
  addr = GT911_ADDR;
  if (!gt_read(REG_CONFIG_VERSION, cfg, 5)) {
    addr = GT911_ADDR_ALT;
    if (!gt_read(REG_CONFIG_VERSION, cfg, 5)) { addr = GT911_ADDR; return TOUCH_ERR_NO_ANSWER; }
  }
  uint16_t rx = (uint16_t)(cfg[1] | (cfg[2] << 8));
  uint16_t ry = (uint16_t)(cfg[3] | (cfg[4] << 8));
  if (rx == 0 || ry == 0 || rx > 4096 || ry > 4096) { return TOUCH_ERR_CONFIG; }
  res_x = rx;
  res_y = ry;
  gt_write(REG_STATUS, 0);
  ok = true;
  return TOUCH_OK;
}

// The shield's RGB LED: an IS31FL3197 at 0x50 on the same bus, set up the way
// Arduino's GigaDisplayRGB does it.
#define RGB_ADDR  0x50
static bool rgb_ok = false;

static bool rgb_write(uint8_t reg, uint8_t v)
{
  Wire1.beginTransmission(RGB_ADDR);
  Wire1.write(reg);
  Wire1.write(v);
  return Wire1.endTransmission() == 0;
}

uint32_t rgb_init(void)
{
  rgb_ok = rgb_write(0x01, 0xF1) && rgb_write(0x02, 0xFF);
  if (rgb_ok) { rgb_set(0, 0, 0); }
  return rgb_ok ? 1 : 0xE1;
}

void rgb_set(uint8_t r, uint8_t g, uint8_t b)
{
  if (!rgb_ok) { return; }
  rgb_write(0x10, r);
  rgb_write(0x11, g);
  rgb_write(0x12, b);
  rgb_write(0x2B, 0xC5);      // latch the new colour
}

void touch_resolution(uint16_t *w, uint16_t *h)
{
  *w = res_x;
  *h = res_y;
}

int touch_read(uint16_t *x, uint16_t *y)
{
  if (!ok) { return -1; }
  uint8_t b[9];
  if (!gt_read(REG_STATUS, b, sizeof(b))) { return -1; }
  if (!(b[0] & 0x80)) { return -1; }      // no new report since the last one
  gt_write(REG_STATUS, 0);                // acknowledge it
  int n = b[0] & 0x0F;
  if (n > 5) { return -1; }               // not a valid report
  if (n == 0) { return 0; }
  uint32_t px = (uint32_t)(b[2] | (b[3] << 8));
  uint32_t py = (uint32_t)(b[4] | (b[5] << 8));
  uint32_t lx, ly;
  if (res_x >= res_y) {
    // Landscape configuration: scale to the screen if it differs.
    lx = px * 800 / res_x;
    ly = py * 480 / res_y;
  } else {
    // Portrait configuration: rotate as canvas.cpp maps the picture,
    // panel (px, py) = (479 - ly, lx).
    lx = py * 800 / res_y;
    uint32_t t = px * 480 / res_x;
    ly = (t > 479) ? 0 : 479 - t;
  }
  if (lx > 799) { lx = 799; }
  if (ly > 479) { ly = 479; }
  *x = (uint16_t)lx;
  *y = (uint16_t)ly;
  return n;
}

#endif // CORE_CM4
