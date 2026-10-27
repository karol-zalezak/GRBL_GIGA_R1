/*
  panel.c - operator panel of shield v4: two potentiometers, START, HOLD, STOP

  Part of Grbl-Giga, a port of Grbl 1.1 to the Arduino GIGA R1 WiFi.

  Grbl is free software: you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation, either version 3 of the License, or (at your option) any later
  version. Distributed WITHOUT ANY WARRANTY; see the GNU GPL for details.
*/

#include "grbl.h"

// Potentiometers: read every PANEL_POT_PERIOD_MS, smoothed; a reading that
// moved more than PANEL_POT_DEADBAND counts from the one last taken is a turn,
// and after a turn the override follows the knob until it has rested (moved
// no more than PANEL_POT_NOISE a period) for PANEL_POT_FOLLOW_MS - so it ends
// where the knob stopped, not one dead band short of it. Around the middle,
// PANEL_POT_DETENT counts either side read as 100 %.
#define PANEL_POT_PERIOD_MS   20
#define PANEL_POT_DEADBAND    60     // of 4095, ~1.5 %
#define PANEL_POT_NOISE       8
#define PANEL_POT_FOLLOW_MS   300
#define PANEL_POT_DETENT      100    // of 4095, ~2.5 % either side

static uint8_t pnl_on = false;                    // $39
static volatile const uint32_t *pnl_idr = 0;      // port C, all three buttons
static uint16_t m_start, m_hold, m_stop;
static volatile uint8_t test_pressed = 0;         // $TPNL
#define PANEL_TEST_QUEUE 6
static struct { uint8_t used, mask; uint32_t at; } test_queue[PANEL_TEST_QUEUE];   // $TPNL=mask,ms

static uint8_t stop_candidate = 0, stop_settle = 0, stop_stable = 0;
static volatile uint8_t stop_request = false;     // set by the scan, taken by panel_service()
static uint8_t stop_state = 0;                    // 1: holding, the reset follows once it stands

static uint16_t pot_filt[2];
static int32_t pot_taken[2] = { -1, -1 };         // -1: take the next reading whatever it is
static int16_t pot_test[2] = { -1, -1 };          // $TPOT
static uint8_t pot_target[2];
static uint8_t pot_pending[2] = { false, false }; // still moving the override to pot_target
static uint32_t pot_follow[2];                    // hal_millis() until which a turn is followed
static uint32_t pot_t = 0;


void panel_init()
{
  pnl_on = settings.panel_buttons;
  if (pnl_on) {
    // Pull-ups as well: a bare board with nothing wired reads released.
    hal_pin_input(PANEL_START_PIN, HAL_PIN_INPUT_PULLUP);
    hal_pin_input(PANEL_HOLD_PIN, HAL_PIN_INPUT_PULLUP);
    hal_pin_input(PANEL_STOP_PIN, HAL_PIN_INPUT_PULLUP);
    m_start = hal_pin_mask(PANEL_START_PIN);
    m_hold = hal_pin_mask(PANEL_HOLD_PIN);
    m_stop = hal_pin_mask(PANEL_STOP_PIN);
    pnl_idr = hal_pin_idr(PANEL_START_PIN);       // the same port for all three (cpu_map.h)
  }
  // A reset: the potentiometers set their overrides again (Grbl has just put
  // them back to 100 %), and a STOP in progress has had its reset.
  pot_taken[0] = pot_taken[1] = -1;
  pot_pending[0] = pot_pending[1] = false;
  stop_state = 0;
}


// Pressed buttons, PANEL_BTN_* bits. Fast: one port read.
static uint8_t panel_read()
{
  if (!pnl_on || !pnl_idr) { return 0; }
  uint32_t v = *pnl_idr;
  uint8_t b = test_pressed;
  if (!(v & m_start)) { b |= PANEL_BTN_START; }
  if (!(v & m_hold)) { b |= PANEL_BTN_HOLD; }
  if (!(v & m_stop)) { b |= PANEL_BTN_STOP; }
  return b;
}

static uint32_t panel_fn(uint8_t b)
{
  uint32_t fn = 0;
  if (b & PANEL_BTN_START) { fn |= bit(IN_FN_START); }
  if (b & PANEL_BTN_HOLD) { fn |= bit(IN_FN_HOLD); }
  return fn;
}

uint8_t panel_buttons() { return panel_read(); }
uint32_t panel_functions() { return panel_fn(panel_read()); }


// From the input scan. STOP is debounced like the control inputs and acts on
// its press only.
uint32_t panel_scan()
{
  uint8_t b = panel_read();
  uint8_t stop = (b & PANEL_BTN_STOP) ? 1 : 0;
  if (stop != stop_candidate) {
    stop_candidate = stop;
    stop_settle = 0;
  } else if (stop_settle < CONTROL_DEBOUNCE_SCANS) {
    if (++stop_settle >= CONTROL_DEBOUNCE_SCANS) {
      if (stop && !stop_stable) { stop_request = true; }
      stop_stable = stop;
    }
  }
  return panel_fn(b);
}


// ---- STOP ----

static uint8_t panel_machine_moving()
{
  if (sys.state & (STATE_CYCLE | STATE_JOG | STATE_HOMING)) { return(true); }
  if ((sys.state == STATE_HOLD) && !(sys.suspend & SUSPEND_HOLD_COMPLETE)) { return(true); }
  if ((sys.state == STATE_SAFETY_DOOR) && !(sys.suspend & SUSPEND_RETRACT_COMPLETE)) { return(true); }
  return(false);
}

// Resets Grbl with the machine declared stopped: the reset does not take it
// for an aborted cycle, so the position is kept and no alarm follows.
static void panel_stopped_reset()
{
  stop_state = 0;
  if (sys.state != STATE_ALARM) {
    sys.step_control = STEP_CONTROL_NORMAL_OP;
    sys.suspend = SUSPEND_DISABLE;
    sys.state = STATE_IDLE;
  }
  mc_reset();
}

static void panel_stop()
{
  report_feedback_message(MESSAGE_PANEL_STOP);
  if (job_active()) { job_stop(0); return; }          // the USB stick's program: its own STOP
  if (sys.state == STATE_HOMING) { mc_reset(); return; }   // homing knows no hold
  if (panel_machine_moving()) {
    stop_state = 1;
    system_set_exec_state_flag(EXEC_FEED_HOLD);
    return;
  }
  panel_stopped_reset();                              // standing: the reset at once
}


// ---- potentiometers ----

// The override a reading stands for.
static uint8_t pot_percent(uint8_t f, uint16_t raw)
{
  if (f == PANEL_POT_RAPID) {
    return (raw < 1365) ? RAPID_OVERRIDE_LOW : ((raw < 2730) ? RAPID_OVERRIDE_MEDIUM : DEFAULT_RAPID_OVERRIDE);
  }
  const uint16_t lo = 2048 - PANEL_POT_DETENT, hi = 2048 + PANEL_POT_DETENT;
  uint8_t min = (f == PANEL_POT_FEED) ? MIN_FEED_RATE_OVERRIDE : MIN_SPINDLE_SPEED_OVERRIDE;
  uint8_t max = (f == PANEL_POT_FEED) ? MAX_FEED_RATE_OVERRIDE : MAX_SPINDLE_SPEED_OVERRIDE;
  if (raw <= lo) { return min + (uint8_t)(((uint32_t)raw * (100 - min) + lo / 2) / lo); }
  if (raw < hi) { return 100; }
  return 100 + (uint8_t)(((uint32_t)(raw - hi) * (max - 100) + (4095 - hi) / 2) / (4095 - hi));
}

// One of Grbl's override steps towards the target, the way the buttons of a
// sender do it. False once the override is there.
static uint8_t pot_step(uint8_t f, uint8_t target)
{
  int16_t diff;
  switch (f) {
    case PANEL_POT_FEED:
      if (sys_rt_exec_motion_override) { return(true); }      // the last step is not taken yet
      diff = (int16_t)target - sys.f_override;
      if (!diff) { return(false); }
      if (diff >= FEED_OVERRIDE_COARSE_INCREMENT) { system_set_exec_motion_override_flag(EXEC_FEED_OVR_COARSE_PLUS); }
      else if (diff <= -FEED_OVERRIDE_COARSE_INCREMENT) { system_set_exec_motion_override_flag(EXEC_FEED_OVR_COARSE_MINUS); }
      else if (diff > 0) { system_set_exec_motion_override_flag(EXEC_FEED_OVR_FINE_PLUS); }
      else { system_set_exec_motion_override_flag(EXEC_FEED_OVR_FINE_MINUS); }
      return(true);
    case PANEL_POT_SPINDLE:
      if (sys_rt_exec_accessory_override) { return(true); }
      diff = (int16_t)target - sys.spindle_speed_ovr;
      if (!diff) { return(false); }
      if (diff >= SPINDLE_OVERRIDE_COARSE_INCREMENT) { system_set_exec_accessory_override_flag(EXEC_SPINDLE_OVR_COARSE_PLUS); }
      else if (diff <= -SPINDLE_OVERRIDE_COARSE_INCREMENT) { system_set_exec_accessory_override_flag(EXEC_SPINDLE_OVR_COARSE_MINUS); }
      else if (diff > 0) { system_set_exec_accessory_override_flag(EXEC_SPINDLE_OVR_FINE_PLUS); }
      else { system_set_exec_accessory_override_flag(EXEC_SPINDLE_OVR_FINE_MINUS); }
      return(true);
    case PANEL_POT_RAPID:
      if (sys_rt_exec_motion_override) { return(true); }
      if (sys.r_override == target) { return(false); }
      system_set_exec_motion_override_flag((target == RAPID_OVERRIDE_LOW) ? EXEC_RAPID_OVR_LOW :
                                           ((target == RAPID_OVERRIDE_MEDIUM) ? EXEC_RAPID_OVR_MEDIUM : EXEC_RAPID_OVR_RESET));
      return(true);
  }
  return(false);
}

static void panel_pots()
{
  uint32_t now = hal_millis();
  uint8_t n;
  for (n = 0; n < 2; n++) {
    uint8_t f = settings.pot_func[n];
    if ((f == PANEL_POT_NONE) || (f >= PANEL_N_POT_FUNCS)) { pot_taken[n] = -1; pot_pending[n] = false; continue; }
    if (pot_pending[n]) { pot_pending[n] = pot_step(f, pot_target[n]); }
  }
  if ((uint32_t)(now - pot_t) < PANEL_POT_PERIOD_MS) { return; }
  pot_t = now;
  for (n = 0; n < 2; n++) {
    uint8_t f = settings.pot_func[n];
    if ((f == PANEL_POT_NONE) || (f >= PANEL_N_POT_FUNCS)) { continue; }
    uint16_t raw = (pot_test[n] >= 0) ? (uint16_t)pot_test[n] : hal_analog_read(n ? PANEL_POT2_PIN : PANEL_POT1_PIN);
    pot_filt[n] = (pot_taken[n] < 0) ? raw : (uint16_t)(((uint32_t)pot_filt[n] + raw + 1) / 2);
    int32_t moved = (int32_t)pot_filt[n] - pot_taken[n];
    uint8_t following = ((int32_t)(pot_follow[n] - now) > 0);
    if ((pot_taken[n] < 0) || (moved > PANEL_POT_DEADBAND) || (moved < -PANEL_POT_DEADBAND) ||
        (following && ((moved > PANEL_POT_NOISE) || (moved < -PANEL_POT_NOISE)))) {
      pot_follow[n] = now + PANEL_POT_FOLLOW_MS;      // a turn, still turning, or the first reading
    }
    if (((int32_t)(pot_follow[n] - now) > 0) && (pot_filt[n] != pot_taken[n])) {
      pot_taken[n] = pot_filt[n];
      pot_target[n] = pot_percent(f, pot_filt[n]);
      pot_pending[n] = true;
    }
  }
}


void panel_service()
{
  uint8_t i;
  for (i = 0; i < PANEL_TEST_QUEUE; i++) {
    if (test_queue[i].used && ((int32_t)(hal_millis() - test_queue[i].at) >= 0)) {
      test_queue[i].used = false;
      test_pressed = test_queue[i].mask;
    }
  }
  if (stop_request) {
    stop_request = false;
    panel_stop();
  }
  if (stop_state == 1) {
    if (sys.state == STATE_ALARM) { stop_state = 0; }          // something else stopped it harder
    else if (!panel_machine_moving()) { panel_stopped_reset(); }
  }
  panel_pots();
}


uint16_t panel_pot(uint8_t n)
{
  if ((n > 1) || (settings.pot_func[n] == PANEL_POT_NONE) || (pot_taken[n] < 0)) { return 0; }
  return pot_filt[n];
}


uint8_t panel_test(char *line)
{
  uint8_t char_counter = 6;
  float a, b = 0.0f;
  bool pot = (line[3] == 'O');                      // $TPOT=n,v ; $TPNL=mask[,ms]
  if (!read_float(line, &char_counter, &a)) { return(STATUS_BAD_NUMBER_FORMAT); }
  if (pot || (line[char_counter] == ',')) {
    if (line[char_counter++] != ',') { return(STATUS_INVALID_STATEMENT); }
    if (!read_float(line, &char_counter, &b)) { return(STATUS_BAD_NUMBER_FORMAT); }
  }
  if (line[char_counter] != 0) { return(STATUS_INVALID_STATEMENT); }
  if (!pot) {
    if ((a < 0) || (a > 7) || (b < 0) || (b > 600000)) { return(STATUS_INVALID_STATEMENT); }
    if (b == 0) { test_pressed = (uint8_t)a; return(STATUS_OK); }
    uint8_t i;
    for (i = 0; i < PANEL_TEST_QUEUE; i++) {
      if (!test_queue[i].used) {
        test_queue[i].mask = (uint8_t)a;
        test_queue[i].at = hal_millis() + (uint32_t)b;
        test_queue[i].used = true;
        return(STATUS_OK);
      }
    }
    return(STATUS_INVALID_STATEMENT);            // queue full
  }
  if (((a != 1.0f) && (a != 2.0f)) || (b < -1.0f) || (b > 4095.0f)) { return(STATUS_INVALID_STATEMENT); }
  pot_test[(uint8_t)a - 1] = (b < 0.0f) ? -1 : (int16_t)b;
  return(STATUS_OK);
}
