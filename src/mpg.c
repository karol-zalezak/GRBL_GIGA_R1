/*
  mpg.c - handwheel pendant (manual pulse generator)

  Part of Grbl-Giga, a port of Grbl 1.1 to the Arduino GIGA R1 WiFi.

  Grbl is free software: you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation, either version 3 of the License, or (at your option) any later
  version. Distributed WITHOUT ANY WARRANTY; see the GNU GPL for details.

  -------------------------------------------------------------------------------

  See mpg.h. The jogs are ordinary "$J=G21G91..." lines run at the same
  point of the main loop as the touch screen's (no line from the PC half
  received), so the parser, the planner and the soft limits treat them like
  any other jog.
*/

#include "grbl.h"

#define MPG_N_SEL    7          // X Y Z 4, x1 x10 x100
#define MPG_NONE     0xFF

static const uint8_t sel_pins[MPG_N_SEL] = MPG_SELECT_PINS;

static bool pins_ready = false;

// Selector, debounced.
static uint8_t sel_cand = 0;        // last reading
static uint8_t sel_stable = 0;      // accepted reading
static uint32_t sel_since = 0;      // when sel_cand appeared
static uint32_t sel_sample_ms = 0;
static uint8_t cur_axis = MPG_NONE;
static uint8_t cur_mult = 1;

// The wheel's jog.
static bool jogging = false;        // the jog running is the wheel's
static uint8_t jog_axis = MPG_NONE; // and on this axis
static uint32_t last_ms = 0;
static uint16_t last_cnt = 0;
static int32_t residue = 0;         // counts short of a whole detent
static float pending = 0.0f;        // distance turned, not yet planned
static uint8_t pending_axis = MPG_NONE;
static bool led_on = false;

// Simulated wheel ($TMPW), stepped by the input scan.
static volatile int32_t test_steps = 0;     // quadrature transitions to go, signed
static volatile uint16_t test_interval = 0; // scans between two transitions, minus one
static uint16_t test_wait = 0;
static uint8_t test_phase = 2;              // A and B high: the rest position, both pulled up


void mpg_init()
{
  if (pins_ready) { return; }
  pins_ready = true;
  hal_mpg_init(MPG_A_PIN, MPG_B_PIN);
  uint8_t i;
  for (i = 0; i < MPG_N_SEL; i++) { hal_pin_input(sel_pins[i], HAL_PIN_INPUT_PULLUP); }
  hal_pin_write(MPG_LED_PIN, false);
  hal_pin_output(MPG_LED_PIN);
  last_cnt = hal_mpg_count();
  last_ms = hal_millis();
}


static uint8_t read_selector()
{
  uint8_t i, v = 0;
  for (i = 0; i < MPG_N_SEL; i++) {
    if (!hal_pin_read(sel_pins[i])) { v |= (uint8_t)(1u << i); }   // active low
  }
  return v;
}


static void decode(uint8_t v)
{
  uint8_t axis = MPG_NONE;
  switch (v & 0x0F) {          // exactly one axis contact; 5, 6 and OFF leave none
    case 0x01: axis = X_AXIS; break;
    case 0x02: axis = Y_AXIS; break;
    case 0x04: axis = Z_AXIS; break;
    case 0x08: axis = A_AXIS; break;
  }
  if ((axis == A_AXIS) && axis_a_is_slave()) { axis = MPG_NONE; }   // a slave has no coordinate
  cur_axis = axis;
  cur_mult = (v & 0x40) ? 100 : ((v & 0x20) ? 10 : 1);
}


// Whether the wheel may move the machine now: Idle with nothing planned, or
// its own jog running. Never during a program, a hold, an alarm, a STOP, or
// a jog from the PC or the touch screen.
static bool machine_ready()
{
  if (job_active() || estop_latched || !drvpwr_ok()) { return(false); }
  if (sys.state == STATE_IDLE) { return(plan_get_current_block() == NULL); }
  return((sys.state == STATE_JOG) && jogging);
}


uint8_t mpg_state()
{
  if (!settings.mpg_mode) { return(MPG_DISABLED); }
  if (cur_axis == MPG_NONE) { return(MPG_NO_AXIS); }
  return(machine_ready() ? MPG_READY : MPG_BLOCKED);
}


void mpg_rt_service()
{
  uint32_t now = hal_millis();
  if ((uint32_t)(now - sel_sample_ms) >= MPG_SAMPLE_MS) {
    sel_sample_ms = now;
    uint8_t raw = read_selector();
    if (raw != sel_cand) {
      sel_cand = raw;
      sel_since = now;
    } else if ((raw != sel_stable) && ((uint32_t)(now - sel_since) >= MPG_DEBOUNCE_MS)) {
      sel_stable = raw;
      decode(raw);
    }
  }
  if (jogging && (sys.state != STATE_JOG)) { jogging = false; }
  // Another axis, OFF or $96=0 ends the wheel's jog at once.
  if (jogging && (!settings.mpg_mode || (cur_axis != jog_axis))) {
    jogging = false;
    pending = 0.0f;
    residue = 0;
    grbl_serial_rx_char(CMD_JOG_CANCEL);
  }
  uint8_t st = mpg_state();
  bool led = (st == MPG_READY) || ((st == MPG_BLOCKED) && ((now / MPG_LED_BLINK_MS) & 1));
  if (led != led_on) {
    led_on = led;
    hal_pin_write(MPG_LED_PIN, led);
  }
}


void mpg_service()
{
  uint32_t now = hal_millis();
  uint32_t dt = now - last_ms;
  if (dt < MPG_PERIOD_MS) { return; }
  last_ms = now;
  uint16_t cnt = hal_mpg_count();
  int16_t d = (int16_t)(cnt - last_cnt);
  last_cnt = cnt;
  // Not here for a while (a hold, a dwell, a line that waited for the
  // planner): what was turned meanwhile is not wanted.
  if (dt > MPG_STALE_MS) { d = 0; residue = 0; }

  uint8_t axis = cur_axis;
  if (!settings.mpg_mode || (axis == MPG_NONE) || !machine_ready()) {
    residue = 0;
    pending = 0.0f;
    return;
  }
  if (axis != pending_axis) { pending = 0.0f; pending_axis = axis; }
  residue += (settings.mpg_mode == 2) ? -d : d;
  int32_t detents = residue / MPG_COUNTS_PER_DETENT;
  residue -= detents * MPG_COUNTS_PER_DETENT;

  float fmax = settings.mpg_max_feed;
  if (fmax > settings.max_rate[axis]) { fmax = settings.max_rate[axis]; }
  pending += (float)detents * settings.mpg_step * (float)cur_mult;
  // One block covers at most one period at the MPG feed, and at most
  // MPG_PENDING_PERIODS of them wait; turning faster than that is dropped.
  float step_max = fmax * (MPG_PERIOD_MS / 60000.0f);
  float cap = step_max * MPG_PENDING_PERIODS;
  if (pending > cap) { pending = cap; } else if (pending < -cap) { pending = -cap; }
  if (fabsf(pending) < 0.00005f) { pending = 0.0f; return; }
  if (plan_get_block_buffer_count() >= MPG_MAX_BLOCKS) { return; }

  int8_t dir = (pending > 0.0f) ? 1 : -1;
  float dist = fabsf(pending);
  if (dist > step_max) { dist = step_max; }
  pending -= (dir > 0) ? dist : -dist;
  float room = jog_soft_limit_room(axis, dir);
  if (dist >= room) {                   // what the soft limit cuts off is dropped
    dist = room;
    pending = 0.0f;
  }
  if (dist < 0.00005f) { return; }
  // The feed that covers the distance in one period: the axis follows the
  // wheel's speed.
  float feed = dist * (60000.0f / MPG_PERIOD_MS);
  if (feed > fmax) { feed = fmax; }
  if (feed < MPG_MIN_FEED) { feed = MPG_MIN_FEED; }
  char cmd[LINE_BUFFER_SIZE];
  jog_line(cmd, axis, (dir < 0) ? -dist : dist, feed);
  uint8_t status = system_execute_line(cmd);
  if (status == STATUS_OK) {
    if (sys.state == STATE_JOG) {
      jogging = true;
      jog_axis = axis;
    }
  } else if (status != STATUS_LIMIT_ACTIVE) {
    display_note_error(status);         // on the screen, not on the USB port
  }
}


uint8_t mpg_axis() { return cur_axis; }
uint8_t mpg_mult() { return cur_mult; }
uint16_t mpg_counter() { return hal_mpg_count(); }
uint8_t mpg_inputs() { return sel_stable; }


uint8_t mpg_test_select(uint8_t axis, uint16_t mult)
{
  int8_t m;
  switch (mult) {
    case 0: m = -1; break;
    case 1: m = 4; break;
    case 10: m = 5; break;
    case 100: m = 6; break;
    default: return(STATUS_INVALID_STATEMENT);
  }
  if (axis > 4) { return(STATUS_INVALID_STATEMENT); }
  uint8_t i;
  for (i = 0; i < MPG_N_SEL; i++) {
    bool on = (axis && (i == axis - 1)) || (i == m);
    if (on) { hal_pin_pull_down(sel_pins[i]); } else { hal_pin_input(sel_pins[i], HAL_PIN_INPUT_PULLUP); }
  }
  return(STATUS_OK);
}


uint8_t mpg_test_turn(int32_t detents, uint32_t rate)
{
  if ((rate < 1) || (rate > INPUT_SCAN_FREQUENCY / 4) || (detents > 100000) || (detents < -100000)) {
    return(STATUS_INVALID_STATEMENT);
  }
  uint32_t interval = INPUT_SCAN_FREQUENCY / (4 * rate);
  if (interval) { interval--; }
  uint32_t sreg = GRBL_IRQ_SAVE();
  test_interval = (uint16_t)interval;
  test_wait = 0;
  test_steps = detents * 4;
  GRBL_IRQ_RESTORE(sreg);
  return(STATUS_OK);
}


uint8_t mpg_test_busy() { return test_steps != 0; }


void mpg_test_tick()
{
  if (!test_steps) { return; }
  if (test_wait) { test_wait--; return; }
  test_wait = test_interval;
  // Quadrature, A leading: 0 = A0 B0, 1 = A1 B0, 2 = A1 B1, 3 = A0 B1.
  if (test_steps > 0) {
    test_phase = (test_phase + 1) & 3;
    test_steps--;
  } else {
    test_phase = (test_phase + 3) & 3;
    test_steps++;
  }
  hal_mpg_test_levels(MPG_A_PIN, MPG_B_PIN, (test_phase == 1) || (test_phase == 2), test_phase >= 2);
}
