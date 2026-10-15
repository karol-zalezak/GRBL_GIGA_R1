/*
  canvas.cpp - drawing for the GIGA Display Shield on the M4

  Part of Grbl-Giga, a port of Grbl 1.1 to the Arduino GIGA R1 WiFi.

  Grbl is free software: you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation, either version 3 of the License, or (at your option) any later
  version. Distributed WITHOUT ANY WARRANTY; see the GNU GPL for details.

  -------------------------------------------------------------------------------

  See canvas.h. The panel setup itself (SDRAM, PLL3, DSI host, LTDC, the
  ST7701 controller's init sequence) is the core's Arduino_H7_Video library;
  this file only uses what it leaves running: one RGB565 layer whose
  framebuffer sits in SDRAM.

  Landscape (lx, ly) maps to panel (479 - ly, lx), the same rotation the
  library applies for an 800x480 Arduino_H7_Video. The scratch buffer stores a
  widget column by column in that orientation, so the DMA2D copies it with a
  plain line offset.
*/

#if defined(CORE_CM4)

#include <Arduino.h>
#include "SDRAM.h"
#include "H7DisplayShield.h"
#include "dsi.h"
extern "C" {
#include "video_modes.h"
}

#include "canvas.h"

#define PANEL_W   480     // physical, portrait
#define PANEL_H   800

#define SCRATCH_PIXELS  48000
static uint16_t scratch[SCRATCH_PIXELS] __attribute__((aligned(32)));

static uint16_t *fb = 0;

// Current widget.
static int wx, wy, ww, wh;
static bool wvalid = false;

// The M4 sees its SRAM at 0x10000000; every other bus master, the DMA2D
// included, sees the same memory at 0x30000000.
static uint32_t bus_addr(const void *p)
{
  uint32_t a = (uint32_t)p;
  if (a >= 0x10000000UL && a < 0x10048000UL) { a += 0x20000000UL; }
  return a;
}

static void dma2d_wait(void)
{
  uint32_t start = millis();
  while (DMA2D->CR & DMA2D_CR_START) {
    if ((uint32_t)(millis() - start) > 50) { DMA2D->CR |= DMA2D_CR_ABORT; break; }
  }
  DMA2D->IFCR = 0x3F;
}

bool cv_init(uint16_t color)
{
  SDRAM.begin(dsi_getFramebufferEnd());
  GigaDisplayShield.init(EDID_MODE_480x800_60Hz);
  // The library leaves its second layer on screen; draw into that one.
  fb = (uint16_t *)dsi_getActiveFrameBuffer();
  if (!fb) { return false; }
  RCC->AHB3ENR |= RCC_AHB3ENR_DMA2DEN;
  (void)RCC->AHB3ENR;
  cv_screen_fill(0, 0, SCREEN_W, SCREEN_H, color);
  return true;
}

uint32_t cv_framebuffer(void) { return (uint32_t)fb; }

void cv_screen_fill(int x, int y, int w, int h, uint16_t c)
{
  if (!fb || w <= 0 || h <= 0) { return; }
  // Landscape rectangle -> panel rectangle: rows x..x+w-1, columns 480-y-h..479-y.
  uint16_t *dst = fb + (uint32_t)x * PANEL_W + (PANEL_W - y - h);
  dma2d_wait();
  DMA2D->CR = (3UL << DMA2D_CR_MODE_Pos);          // register to memory
  DMA2D->OPFCCR = 2;                                // RGB565
  DMA2D->OCOLR = c;
  DMA2D->OMAR = (uint32_t)dst;
  DMA2D->OOR = PANEL_W - h;
  DMA2D->NLR = ((uint32_t)h << DMA2D_NLR_PL_Pos) | (uint32_t)w;
  DMA2D->CR |= DMA2D_CR_START;
  dma2d_wait();
}

bool cv_begin(int x, int y, int w, int h, uint16_t bg)
{
  wvalid = (w > 0 && h > 0 && (uint32_t)w * (uint32_t)h <= SCRATCH_PIXELS &&
            x >= 0 && y >= 0 && x + w <= SCREEN_W && y + h <= SCREEN_H);
  if (!wvalid) { return false; }
  wx = x; wy = y; ww = w; wh = h;
  uint32_t n = (uint32_t)w * (uint32_t)h;
  uint32_t v = ((uint32_t)bg << 16) | bg;
  uint32_t *p = (uint32_t *)scratch;
  for (uint32_t i = 0; i < n / 2; i++) { p[i] = v; }
  if (n & 1) { scratch[n - 1] = bg; }
  return true;
}

uint32_t cv_dma_errors = 0;     // DMA2D ISR flags of the last failed copy, for diagnostics
static bool cpu_copy = false;   // DMA2D could not read the scratch buffer: copy with the CPU

void cv_end(void)
{
  if (!wvalid || !fb) { return; }
  uint16_t *dst = fb + (uint32_t)wx * PANEL_W + (PANEL_W - wy - wh);
  if (!cpu_copy) {
    dma2d_wait();
    DMA2D->CR = (0UL << DMA2D_CR_MODE_Pos);          // memory to memory
    DMA2D->FGPFCCR = 2;                               // RGB565
    DMA2D->OPFCCR = 2;
    DMA2D->FGMAR = bus_addr(scratch);
    DMA2D->FGOR = 0;
    DMA2D->OMAR = (uint32_t)dst;
    DMA2D->OOR = PANEL_W - wh;
    DMA2D->NLR = ((uint32_t)wh << DMA2D_NLR_PL_Pos) | (uint32_t)ww;
    DMA2D->CR |= DMA2D_CR_START;
    uint32_t start = millis();
    while (DMA2D->CR & DMA2D_CR_START) {
      if ((uint32_t)(millis() - start) > 50) { DMA2D->CR |= DMA2D_CR_ABORT; break; }
    }
    uint32_t isr = DMA2D->ISR;
    DMA2D->IFCR = 0x3F;
    if (isr & (DMA2D_ISR_TEIF | DMA2D_ISR_CEIF)) {
      cv_dma_errors = isr | 0x80000000UL;
      cpu_copy = true;
    }
  }
  if (cpu_copy) {
    // One panel row per widget column, h pixels each.
    for (int i = 0; i < ww; i++) {
      memcpy(dst + (uint32_t)i * PANEL_W, &scratch[(uint32_t)i * wh], (uint32_t)wh * 2);
    }
  }
  wvalid = false;
}

// Pixel (x, y) of the current widget, panel orientation.
static inline uint16_t *px(int x, int y) { return &scratch[(uint32_t)x * wh + (wh - 1 - y)]; }

void cv_fill(int x, int y, int w, int h, uint16_t c)
{
  if (!wvalid) { return; }
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > ww) { w = ww - x; }
  if (y + h > wh) { h = wh - y; }
  if (w <= 0 || h <= 0) { return; }
  for (int i = 0; i < w; i++) {
    uint16_t *col = px(x + i, y + h - 1);    // lowest address of this column's run
    for (int j = 0; j < h; j++) { col[j] = c; }
  }
}

void cv_frame(int x, int y, int w, int h, int t, uint16_t c)
{
  cv_fill(x, y, w, t, c);
  cv_fill(x, y + h - t, w, t, c);
  cv_fill(x, y, t, h, c);
  cv_fill(x + w - t, y, t, h, c);
}

static uint16_t blend(uint16_t fg, uint16_t bg, uint32_t a)   // a = 0..15
{
  uint32_t rf = fg >> 11, gf = (fg >> 5) & 63, bf = fg & 31;
  uint32_t rb = bg >> 11, gb = (bg >> 5) & 63, bb = bg & 31;
  uint32_t r = (rf * a + rb * (15 - a) + 7) / 15;
  uint32_t g = (gf * a + gb * (15 - a) + 7) / 15;
  uint32_t b = (bf * a + bb * (15 - a) + 7) / 15;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

void cv_disc(int cx, int cy, int r, uint16_t c, uint16_t bg)
{
  if (!wvalid) { return; }
  // Coverage-sampled disc centred on pixel (cx, cy), 4x4 subsamples per
  // pixel for a smooth edge. Distances in 1/8 pixel.
  int r2 = 64 * r * r;
  for (int y = -r - 1; y <= r + 1; y++) {
    for (int x = -r - 1; x <= r + 1; x++) {
      int X = cx + x, Y = cy + y;
      if (X < 0 || Y < 0 || X >= ww || Y >= wh) { continue; }
      int cover = 0;
      for (int sy = 0; sy < 4; sy++) {
        for (int sx = 0; sx < 4; sx++) {
          int dx = 8 * x + 2 * sx - 3, dy = 8 * y + 2 * sy - 3;
          if (dx * dx + dy * dy <= r2) { cover++; }
        }
      }
      if (cover) { *px(X, Y) = (cover >= 16) ? c : blend(c, bg, (uint32_t)(cover * 15 + 8) / 16); }
    }
  }
}

static int glyph_index(const font_t *f, char ch)
{
  const char *m = f->map;
  for (int i = 0; m[i]; i++) { if (m[i] == ch) { return i; } }
  return -1;
}

int cv_text_width(const font_t *f, const char *s)
{
  int n = 0;
  while (s[n]) { n++; }
  return n * f->adv;
}

void cv_text(int x, int y, const font_t *f, const char *s, uint16_t fg, uint16_t bg)
{
  if (!wvalid) { return; }
  uint16_t lut[16];
  for (uint32_t a = 0; a < 16; a++) { lut[a] = blend(fg, bg, a); }
  int bytes_per_row = (f->adv + 1) / 2;
  for (; *s; s++, x += f->adv) {
    if (*s == ' ') { continue; }
    int gi = glyph_index(f, *s);
    if (gi < 0) { continue; }
    const uint8_t *g = f->bitmap + (uint32_t)gi * f->stride;
    for (int gy = 0; gy < f->h; gy++) {
      int Y = y + gy;
      if (Y < 0 || Y >= wh) { continue; }
      const uint8_t *row = g + gy * bytes_per_row;
      for (int gx = 0; gx < f->adv; gx++) {
        int X = x + gx;
        if (X < 0 || X >= ww) { continue; }
        uint8_t v = row[gx >> 1];
        uint32_t a = (gx & 1) ? (v & 15) : (v >> 4);
        if (a) { *px(X, Y) = lut[a]; }
      }
    }
  }
}

void cv_bitmap4(int x, int y, int w, int h, const uint8_t *data, uint16_t fg, uint16_t bg)
{
  if (!wvalid) { return; }
  uint16_t lut[16];
  for (uint32_t a = 0; a < 16; a++) { lut[a] = blend(fg, bg, a); }
  int bytes_per_row = (w + 1) / 2;
  for (int by = 0; by < h; by++) {
    int Y = y + by;
    if (Y < 0 || Y >= wh) { continue; }
    const uint8_t *row = data + (uint32_t)by * bytes_per_row;
    for (int bx = 0; bx < w; bx++) {
      int X = x + bx;
      if (X < 0 || X >= ww) { continue; }
      uint8_t v = row[bx >> 1];
      uint32_t a = (bx & 1) ? (v & 15) : (v >> 4);
      if (a) { *px(X, Y) = lut[a]; }
    }
  }
}

#endif // CORE_CM4
