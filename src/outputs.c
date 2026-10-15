/*
  outputs.c - the switched output functions on the universal channels

  Part of Grbl-Giga, a port of Grbl 1.1 to the Arduino GIGA R1 WiFi.

  Grbl is free software: you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation, either version 3 of the License, or (at your option) any later
  version. Distributed WITHOUT ANY WARRANTY; see the GNU GPL for details.

  -------------------------------------------------------------------------------

  See outputs.h. outputs_set() is reached from interrupt context - mc_reset()
  stops spindle and coolant from the USB receive callback and from the input
  scan - so the state word is updated with interrupts masked and the channel
  itself is one BSRR store in the HAL.
*/

#include "grbl.h"

static volatile uint16_t fn_state = 0;     // bit fn = function on
static volatile uint8_t aux_manual = 0;    // M62-M65 state, bit n = AUXn
static volatile uint8_t spindle_on = 0;
static volatile uint32_t spindle_off_at = 0;   // hal_millis() when the spindle went off

static uint16_t pulse_active = 0;          // functions in a timed pulse
static uint32_t pulse_start[N_OUTPUTS];
static uint16_t pulse_len[N_OUTPUTS];


static void outputs_drive(uint8_t fn)
{
  io_out_write(io_out_channel(fn), (fn_state >> fn) & 1);
}


// Recomputes the aux outputs from the manual state and the spindle.
static void outputs_update_aux()
{
  uint8_t i;
  for (i = 0; i < N_AUX_OUTPUTS; i++) {
    uint8_t on = ((aux_manual >> i) & 1) || (spindle_on && (settings.aux_follow_spindle & bit(i)));
    outputs_set(OUTPUT_AUX0 + i, on);
  }
}


void outputs_init()
{
  uint8_t i;
  uint32_t sreg = GRBL_IRQ_SAVE();
  fn_state = 0;
  aux_manual = 0;
  spindle_on = 0;
  pulse_active = 0;
  GRBL_IRQ_RESTORE(sreg);
  // Every channel off, also one whose function was just unassigned.
  for (i = 1; i <= N_OUT_CHANNELS; i++) { io_out_write(i, false); }
  // The run-down clock keeps running across a reset: a reset does not make a
  // spinning spindle stop any sooner.
}


// Central STOP: every function and every channel off at once. Safe from the
// STOP interrupt (priority 0); the reset that follows re-initialises the rest.
void outputs_all_off()
{
  uint8_t i;
  uint32_t sreg = GRBL_IRQ_SAVE();
  fn_state = 0;
  aux_manual = 0;
  pulse_active = 0;
  if (spindle_on) { spindle_off_at = hal_millis(); }
  spindle_on = 0;
  for (i = 1; i <= N_OUT_CHANNELS; i++) { io_out_write(i, false); }
  GRBL_IRQ_RESTORE(sreg);
}


void outputs_set(uint8_t fn, uint8_t on)
{
  if (fn >= N_OUTPUTS) { return; }
  uint32_t sreg = GRBL_IRQ_SAVE();
  if (on) { fn_state |= (uint16_t)bit(fn); } else { fn_state &= (uint16_t)~bit(fn); }
  outputs_drive(fn);
  GRBL_IRQ_RESTORE(sreg);
}


uint8_t outputs_get(uint8_t fn) { return (fn < N_OUTPUTS) ? ((fn_state >> fn) & 1) : 0; }
uint16_t outputs_get_mask() { return fn_state; }


void outputs_aux_set(uint8_t aux, uint8_t on)
{
  if (aux >= N_AUX_OUTPUTS) { return; }
  uint32_t sreg = GRBL_IRQ_SAVE();
  if (on) { aux_manual |= bit(aux); } else { aux_manual &= ~bit(aux); }
  GRBL_IRQ_RESTORE(sreg);
  outputs_update_aux();
}


void outputs_aux_all_off()
{
  aux_manual = 0;
  outputs_update_aux();
}


void outputs_spindle_running(uint8_t running)
{
  uint32_t sreg = GRBL_IRQ_SAVE();
  if (spindle_on && !running) { spindle_off_at = hal_millis(); }
  spindle_on = running ? 1 : 0;
  GRBL_IRQ_RESTORE(sreg);
  outputs_update_aux();
}


uint32_t outputs_spindle_off_ms()
{
  if (spindle_on) { return 0; }
  uint32_t t = (uint32_t)(hal_millis() - spindle_off_at);
  return t ? t : 1;
}


void outputs_refresh()
{
  uint8_t i;
  outputs_update_aux();
  // Channels without a function are off; the others follow their function.
  for (i = 1; i <= N_OUT_CHANNELS; i++) {
    uint8_t f, on = 0;
    for (f = 0; f < N_OUTPUTS; f++) {
      if ((io_out_channel(f) == i) && ((fn_state >> f) & 1)) { on = 1; }
    }
    io_out_write(i, on);
  }
}


void outputs_function_off(uint8_t fn)
{
  if (fn >= N_OUTPUTS) { return; }
  switch (fn) {
    case OUTPUT_SPINDLE_FWD: case OUTPUT_SPINDLE_REV: spindle_stop(); break;
    case OUTPUT_FLOOD: case OUTPUT_MIST: coolant_stop(); break;
    default:
      if ((fn >= OUTPUT_AUX0) && (fn < OUTPUT_AUX0 + N_AUX_OUTPUTS)) {
        aux_manual &= ~bit(fn - OUTPUT_AUX0);
      }
      pulse_active &= (uint16_t)~bit(fn);
      outputs_set(fn, false);
  }
}


void outputs_pulse(uint8_t fn, uint16_t ms)
{
  if ((fn >= N_OUTPUTS) || (ms == 0)) { return; }
  pulse_start[fn] = hal_millis();
  pulse_len[fn] = ms;
  pulse_active |= (uint16_t)bit(fn);
  outputs_set(fn, true);
}


void outputs_service()
{
  if (!pulse_active) { return; }
  uint8_t f;
  uint32_t now = hal_millis();
  for (f = 0; f < N_OUTPUTS; f++) {
    if ((pulse_active & bit(f)) && ((uint32_t)(now - pulse_start[f]) >= pulse_len[f])) {
      pulse_active &= (uint16_t)~bit(f);
      outputs_set(f, false);
    }
  }
}
