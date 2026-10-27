/*
  spindle_control.c - spindle control methods
  Part of Grbl

  Copyright (c) 2012-2017 Sungeun K. Jeon for Gnea Research LLC
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

  On the GIGA the spindle has three signals instead of AVR Grbl's two:

  - FWD and REV, the run commands of a VFD: M3 switches FWD on, M4 switches
    REV on, never both. They are output functions ($80, $81; OUT1 and OUT2 by
    default) through outputs.c, so $93 can invert them.
  - The speed is a PWM carrier on SPINDLE_PWM_PIN (TIM4, 1 kHz by default,
    60000 duty steps) and, at the same time, a 0-3.3 V level on the DAC pin
    (A12), which the shield's isolated amplifier turns into 0-10 V.

  So unlike upstream, VARIABLE_SPINDLE does not take over the enable pin: the
  PWM pin carries speed only and FWD/REV say whether and which way the spindle
  runs. In laser mode the run output stays on for the whole M3/M4 block and
  the stepper ISR modulates only the PWM, as upstream does with the PWM pin.

  With $33 > 0 the same commands also go to the VFD over RS485 (vfd.h), and
  its status takes the place of the at-speed relay (and adds to the fault
  relay). Laser mode and $33 exclude each other.
*/

#include "grbl.h"

// Duty value corresponding to 100%, established by spindle_init().
uint16_t spindle_pwm_max_value = 0;

#ifdef VARIABLE_SPINDLE
  static float pwm_gradient;      // Precalculated to speed up rpm to PWM conversions.
  static uint16_t pwm_current;    // Last duty written.
#endif
static uint8_t spindle_dir_ccw = 0;   // direction of the last M3/M4
static uint8_t spindle_spinup_pending = false;   // started since the last wait ($54)
static void spindle_at_speed_restart(uint8_t was_running);


void spindle_init()
{
  #ifdef VARIABLE_SPINDLE
    // $38: the PWM on OUT13 (D6), or OUT13 a plain output. The duty value is
    // worked out either way - the DAC's 0-10 V follows it.
    uint8_t pwm = (settings.spindle_pwm <= HAL_PWM_INVERTED) ? settings.spindle_pwm : HAL_PWM_OFF;
    if (pwm) { io_out_reserve(OUT_PWM_CHANNEL, true); }
    uint32_t period = hal_spindle_pwm_init(SPINDLE_PWM_PIN, SPINDLE_PWM_FREQUENCY, pwm);
    if (!pwm) { io_out_reserve(OUT_PWM_CHANNEL, false); }
    if (period == 0) { period = 1; }               // PWM unavailable; degrade to on/off.
    if (period > 0xFFFFUL) { period = 0xFFFFUL; }
    spindle_pwm_max_value = (uint16_t)period;
    #ifdef SPINDLE_DAC_PIN
      hal_spindle_dac_init(SPINDLE_DAC_PIN);
    #endif

    pwm_gradient = (float)(spindle_pwm_max_value - SPINDLE_PWM_MIN_VALUE) /
                   (settings.rpm_max - settings.rpm_min);
  #endif

  spindle_stop();
}


uint16_t spindle_get_pwm_permille()
{
  #ifdef VARIABLE_SPINDLE
    if (spindle_pwm_max_value == 0) { return 0; }
    return (uint16_t)(((uint32_t)pwm_current * 1000UL) / spindle_pwm_max_value);
  #else
    return (outputs_get(OUTPUT_SPINDLE_FWD) || outputs_get(OUTPUT_SPINDLE_REV)) ? 1000 : 0;
  #endif
}


uint8_t spindle_get_state()
{
  uint8_t fwd = outputs_get(OUTPUT_SPINDLE_FWD), rev = outputs_get(OUTPUT_SPINDLE_REV);
  if (fwd || rev) {
    #ifdef VARIABLE_SPINDLE
      // A running spindle at S0 is reported as off, as upstream does by
      // looking at the PWM pin.
      if (pwm_current == SPINDLE_PWM_OFF_VALUE && bit_isfalse(settings.flags,BITFLAG_LASER_MODE)) {
        return(SPINDLE_STATE_DISABLE);
      }
    #endif
    if (rev) { return(SPINDLE_STATE_CCW); }
    return(SPINDLE_STATE_CW);
  }
  return(SPINDLE_STATE_DISABLE);
}


// Disables the spindle and sets PWM output to zero when PWM variable spindle speed is enabled.
// Called by various main program and ISR routines. Keep routine small, fast, and efficient.
// Called by spindle_init(), spindle_set_speed(), spindle_set_state(), and mc_reset().
void spindle_stop()
{
  #ifdef VARIABLE_SPINDLE
    pwm_current = SPINDLE_PWM_OFF_VALUE;
    hal_spindle_pwm_off();      // Duty zero: the pin idles low, as on AVR.
    #ifdef SPINDLE_DAC_PIN
      hal_spindle_dac_set(0);
    #endif
  #endif
  outputs_set(OUTPUT_SPINDLE_FWD, false);
  outputs_set(OUTPUT_SPINDLE_REV, false);
  outputs_spindle_running(false);
  vfd_spindle(VFD_STOP, 0.0f);
  spindle_at_speed_restart(false);   // off: nothing to wait for or watch
}


#ifdef VARIABLE_SPINDLE
  // Sets spindle speed PWM output. Called by spindle_set_state() and the stepper ISR (laser
  // mode, once per segment). Keep routine small and efficient.
  void spindle_set_speed(uint16_t pwm_value)
  {
    // A running spindle changing speed has to reach it again (an S word goes
    // through spindle_sync() and waits; an override only re-arms the watch).
    if ((pwm_value != pwm_current) && (outputs_get(OUTPUT_SPINDLE_FWD) || outputs_get(OUTPUT_SPINDLE_REV))) {
      pwm_current = pwm_value;
      vfd_speed(sys.spindle_speed);   // the override while cutting: the drive follows
      spindle_at_speed_restart(true);
    }
    pwm_current = pwm_value;
    hal_spindle_pwm_set(pwm_value);
    #ifdef SPINDLE_DAC_PIN
      hal_spindle_dac_set(spindle_pwm_max_value ? (uint32_t)pwm_value * 4095UL / spindle_pwm_max_value : 0);
    #endif
    #ifdef SPINDLE_ENABLE_OFF_WITH_ZERO_SPEED
      if (pwm_value == SPINDLE_PWM_OFF_VALUE) {
        outputs_set(OUTPUT_SPINDLE_FWD, false);
        outputs_set(OUTPUT_SPINDLE_REV, false);
      } else {
        outputs_set(spindle_dir_ccw ? OUTPUT_SPINDLE_REV : OUTPUT_SPINDLE_FWD, true);
      }
    #endif
  }


  #ifdef ENABLE_PIECEWISE_LINEAR_SPINDLE

    // Called by spindle_set_state() and step segment generator. Keep routine small and efficient.
    uint16_t spindle_compute_pwm_value(float rpm)
    {
      // The piecewise fit constants in config.h are expressed against the AVR's
      // 0-255 register, so the model is evaluated in that domain and the result
      // is scaled up to this port's wider duty range. Existing RPM_LINE_* tuning
      // therefore carries over unchanged.
      float pwm8;
      rpm *= (0.010*sys.spindle_speed_ovr); // Scale by spindle speed override value.
      if ((settings.rpm_min >= settings.rpm_max) || (rpm >= RPM_MAX)) {
        rpm = RPM_MAX;
        sys.spindle_speed = rpm;
        return(spindle_pwm_max_value);
      } else if (rpm <= RPM_MIN) {
        if (rpm == 0.0) { // S0 disables spindle
          sys.spindle_speed = rpm;
          return(SPINDLE_PWM_OFF_VALUE);
        }
        rpm = RPM_MIN;
        sys.spindle_speed = rpm;
        return(SPINDLE_PWM_MIN_VALUE);
      }

      #if (N_PIECES > 3)
        if (rpm > RPM_POINT34) {
          pwm8 = floor(RPM_LINE_A4*rpm - RPM_LINE_B4);
        } else
      #endif
      #if (N_PIECES > 2)
        if (rpm > RPM_POINT23) {
          pwm8 = floor(RPM_LINE_A3*rpm - RPM_LINE_B3);
        } else
      #endif
      #if (N_PIECES > 1)
        if (rpm > RPM_POINT12) {
          pwm8 = floor(RPM_LINE_A2*rpm - RPM_LINE_B2);
        } else
      #endif
      {
        pwm8 = floor(RPM_LINE_A1*rpm - RPM_LINE_B1);
      }
      sys.spindle_speed = rpm;
      if (pwm8 < 0.0) { pwm8 = 0.0; }
      if (pwm8 > 255.0) { pwm8 = 255.0; }
      return((uint16_t)((pwm8 * (float)spindle_pwm_max_value) / 255.0));
    }

  #else

    // Called by spindle_set_state() and step segment generator. Keep routine small and efficient.
    uint16_t spindle_compute_pwm_value(float rpm)
    {
      uint16_t pwm_value;
      rpm *= (0.010*sys.spindle_speed_ovr); // Scale by spindle speed override value.
      // Calculate PWM register value based on rpm max/min settings and programmed rpm.
      if ((settings.rpm_min >= settings.rpm_max) || (rpm >= settings.rpm_max)) {
        // No PWM range possible. Set simple on/off spindle control pin state.
        sys.spindle_speed = settings.rpm_max;
        pwm_value = spindle_pwm_max_value;
      } else if (rpm <= settings.rpm_min) {
        if (rpm == 0.0) { // S0 disables spindle
          sys.spindle_speed = 0.0;
          pwm_value = SPINDLE_PWM_OFF_VALUE;
        } else { // Set minimum PWM output
          sys.spindle_speed = settings.rpm_min;
          pwm_value = SPINDLE_PWM_MIN_VALUE;
        }
      } else {
        // Compute intermediate PWM value with linear spindle speed model.
        // NOTE: A nonlinear model could be installed here, if required, but keep it VERY light-weight.
        sys.spindle_speed = rpm;
        pwm_value = (uint16_t)(floor((rpm-settings.rpm_min)*pwm_gradient) + SPINDLE_PWM_MIN_VALUE);
      }
      return(pwm_value);
    }

  #endif
#endif


// Immediately sets spindle running state with direction and spindle rpm via PWM, if enabled.
// Called by g-code parser spindle_sync(), parking retract and restore, g-code program end,
// sleep, and spindle stop override.
#ifdef VARIABLE_SPINDLE
  void spindle_set_state(uint8_t state, float rpm)
#else
  void _spindle_set_state(uint8_t state)
#endif
{
  if (sys.abort) { return; } // Block during abort.

  if (state == SPINDLE_DISABLE) { // Halt or set spindle direction and rpm.

    #ifdef VARIABLE_SPINDLE
      sys.spindle_speed = 0.0;
    #endif
    spindle_stop();

  } else {

    // An open collet and a spinning spindle never go together. The parser
    // refuses M3/M4 then; this also covers the other ways in (a resume after
    // a hold, the spindle restore of a safety door, a startup line).
    if (spindle_collet_is_open()) { spindle_stop(); return; }

    // FWD and REV are exclusive: a VFD given both stops or faults.
    uint8_t new_ccw = (state != SPINDLE_ENABLE_CW);
    uint8_t was_on = outputs_get(OUTPUT_SPINDLE_FWD) || outputs_get(OUTPUT_SPINDLE_REV);
    uint8_t was_running = was_on && (new_ccw == spindle_dir_ccw);
    spindle_dir_ccw = new_ccw;
    outputs_set(spindle_dir_ccw ? OUTPUT_SPINDLE_FWD : OUTPUT_SPINDLE_REV, false);

    #ifdef VARIABLE_SPINDLE
      // NOTE: Assumes all calls to this function is when Grbl is not moving or must remain off.
      if (settings.flags & BITFLAG_LASER_MODE) {
        if (state == SPINDLE_ENABLE_CCW) { rpm = 0.0; } // TODO: May need to be rpm_min*(100/MAX_SPINDLE_SPEED_OVERRIDE);
      }
      spindle_set_speed(spindle_compute_pwm_value(rpm));
    #endif
    #ifndef SPINDLE_ENABLE_OFF_WITH_ZERO_SPEED
      outputs_set(spindle_dir_ccw ? OUTPUT_SPINDLE_REV : OUTPUT_SPINDLE_FWD, true);
    #endif
    #ifdef VARIABLE_SPINDLE
      vfd_spindle(spindle_dir_ccw ? VFD_REV : VFD_FWD, sys.spindle_speed);
    #else
      vfd_spindle(spindle_dir_ccw ? VFD_REV : VFD_FWD, settings.rpm_max);
    #endif
    outputs_spindle_running(true);
    if (!was_running) {        // started, or reversed: spin up from scratch
      // Reversed: the at-speed signal may still show the old direction.
      spindle_at_speed_restart(was_on);
      spindle_spinup_pending = true;
    }

  }

  sys.report_ovr_counter = 0; // Set to report change immediately
}


// ---------------------------------------------------------------------------
// Port: the VFD's relay contacts and the collet (PLAN.md part 1)
// ---------------------------------------------------------------------------
//
// At speed ($77): after M3/M4 and after every S change, the parser waits (like
// G4) until the VFD's "frequency reached" relay closes, at most $52 ms - then
// ALARM:14. While cutting, losing the signal is handled per $53 (0 ignore,
// 1 feed hold, 2 alarm after a controlled stop). A speed change that does not
// go through the parser (the spindle override) re-arms the wait without
// blocking; the same $52 then applies. $52=0 or no input: no waiting, but the
// fixed $54 after M3/M4. Never in laser mode.
//
// Fault ($78): the spindle outputs drop at once (so that resetting the VFD
// cannot restart it), motion stops with a feed hold, and once the machine
// stands, ALARM:19 - with the position kept. $X and $H are refused while the
// input stays active; a reset or $X pulses the VFD reset output ($92).
//
// VFD over RS485 ($33 > 0): at speed comes from the drive's status instead of
// the relay, a fault it reports counts like the fault input. A drive that does
// not answer, refuses or does not follow (vfd_trouble()) is handled like a
// fault, with ALARM:23 and $X / $H refused (error:51) while it lasts.

#define SPINDLE_INPUT_DEBOUNCE_SCANS  100   // 10 ms at the 10 kHz scan
// After a speed change of a spindle that is already running, the at-speed
// relay is ignored this long: the VFD sees the new set point only after its
// analog input filter, and until then still reports the old speed as reached.
#define SPINDLE_AT_SPEED_SETTLE_MS    250

#define AS_OFF   0    // spindle off, or no at-speed monitoring
#define AS_WAIT  1    // waiting for the relay
#define AS_OK    2    // at speed, monitored

static uint8_t sp_in_stable = 0;          // bit 0 at speed, bit 1 fault (debounced)
static uint8_t sp_in_candidate = 0;
static uint8_t sp_in_settle = 0;
static volatile uint8_t as_state = AS_OFF;
static volatile uint32_t as_t0 = 0;       // hal_millis() when the wait began
static volatile uint32_t as_settle_ms = 0;
static volatile uint8_t as_sync_wait = 0; // the parser is waiting (spindle_wait_at_speed)
static volatile uint8_t fault_state = 0;  // 1 fault seen, alarm pending; 2 alarm raised
static volatile uint8_t loss_state = 0;   // 1 at-speed lost, reaction pending; 2 done
static uint8_t comm_state = 0;            // VFD over RS485: 1 trouble, alarm pending; 2 alarm raised


// At-speed waiting and monitoring apply: an input is assigned, $52 > 0, no laser.
static uint8_t spindle_at_speed_used()
{
  if (!(io_in_channel(IN_FN_AT_SPEED) || vfd_enabled()) || (settings.at_speed_timeout_ms == 0)) { return(false); }
  #ifdef VARIABLE_SPINDLE
    if (bit_istrue(settings.flags,BITFLAG_LASER_MODE)) { return(false); }
  #endif
  return(true);
}


// The spindle was started or its speed changed: wait for the relay again.
static void spindle_at_speed_restart(uint8_t was_running)
{
  if (!spindle_at_speed_used() || !(outputs_get(OUTPUT_SPINDLE_FWD) || outputs_get(OUTPUT_SPINDLE_REV))) {
    as_state = AS_OFF;
    return;
  }
  #ifdef VARIABLE_SPINDLE
    if (pwm_current == SPINDLE_PWM_OFF_VALUE) { as_state = AS_OFF; return; }   // S0: nothing to reach
  #endif
  as_settle_ms = was_running ? SPINDLE_AT_SPEED_SETTLE_MS : 0;
  as_t0 = hal_millis();
  as_state = AS_WAIT;
  loss_state = 0;
}


static uint8_t spindle_machine_moving()
{
  if (sys.state & (STATE_CYCLE | STATE_JOG | STATE_HOMING)) { return(true); }
  if ((sys.state == STATE_HOLD) && !(sys.suspend & SUSPEND_HOLD_COMPLETE)) { return(true); }
  if ((sys.state == STATE_SAFETY_DOOR) && !(sys.suspend & SUSPEND_RETRACT_COMPLETE)) { return(true); }
  return(false);
}


uint8_t spindle_fault_active() { return (sp_in_stable & 2) ? true : false; }
uint8_t spindle_at_speed() { return (sp_in_stable & 1) ? true : false; }

uint8_t spindle_status()
{
  if (fault_state || spindle_fault_active() || comm_state || vfd_trouble()) { return(SPINDLE_STATUS_FAULT); }
  if (loss_state == 1) { return(SPINDLE_STATUS_LOST); }
  switch (as_state) {
    case AS_WAIT: return(SPINDLE_STATUS_SPINUP);
    case AS_OK: return(SPINDLE_STATUS_AT_SPEED);
  }
  return(SPINDLE_STATUS_NONE);
}


// From the 10 kHz input scan. fn = the input function word.
void spindle_scan(uint32_t fn)
{
  uint8_t in = ((fn >> IN_FN_AT_SPEED) & 1) | (((fn >> IN_FN_SPIN_FAULT) & 1) << 1);
  if (vfd_enabled()) { in = (in & 2) | vfd_at_speed() | (vfd_fault() << 1); }
  if (in != sp_in_candidate) {
    sp_in_candidate = in;
    sp_in_settle = 0;
  } else if (sp_in_settle < SPINDLE_INPUT_DEBOUNCE_SCANS) {
    if (++sp_in_settle >= SPINDLE_INPUT_DEBOUNCE_SCANS) { sp_in_stable = sp_in_candidate; }
  }

  // VFD fault: spindle off now, motion to a controlled stop; the alarm follows
  // in spindle_service() once the machine stands.
  if ((sp_in_stable & 2) && !fault_state && (sys.state != STATE_ALARM)) {
    fault_state = 1;
    spindle_stop();
    as_state = AS_OFF;
    if (sys.state & (STATE_CYCLE | STATE_JOG)) { system_set_exec_state_flag(EXEC_FEED_HOLD); }
  }

  if (as_state == AS_WAIT) {
    if ((sp_in_stable & 1) && ((uint32_t)(hal_millis() - as_t0) >= as_settle_ms)) { as_state = AS_OK; }
  } else if (as_state == AS_OK) {
    // Back at speed after a loss that only held the machine: watch again.
    if ((sp_in_stable & 1) && (loss_state == 2)) { loss_state = 0; }
    if (!(sp_in_stable & 1) && (settings.at_speed_loss != 0) && !loss_state) {
      loss_state = 1;
      if (sys.state & (STATE_CYCLE | STATE_JOG)) { system_set_exec_state_flag(EXEC_FEED_HOLD); }
    }
  }
}


// Stops where Grbl stands: ALARM:code without the position being lost. Grbl's
// own mc_reset() would call a stop during a hold an aborted cycle (ALARM:3),
// so the machine is first declared still - which it is, the callers wait for it.
static void spindle_alarm_stopped(uint8_t code)
{
  sys.state = STATE_ALARM;
  sys.step_control = STEP_CONTROL_NORMAL_OP;
  sys.suspend = SUSPEND_DISABLE;
  mc_reset();
  system_set_exec_alarm(code);
}


// From the main loop (protocol_exec_rt_system and the critical alarm loop).
void spindle_service()
{
  if (fault_state == 1) {
    if (spindle_machine_moving()) { return; }   // the feed hold is still braking
    fault_state = 2;
    spindle_alarm_stopped(EXEC_ALARM_SPINDLE_FAULT);
    return;
  }
  if ((fault_state == 2) && (sys.state != STATE_ALARM)) { fault_state = 0; }

  // The VFD over RS485 cannot be trusted with the spindle: off (as far as
  // it can be told), controlled stop, ALARM:23.
  if (vfd_trouble() && !comm_state && !fault_state && (sys.state != STATE_ALARM)) {
    comm_state = 1;
    spindle_stop();
    loss_state = 0;     // the silent drive also lost "at speed": one alarm, this one
    if (sys.state & (STATE_CYCLE | STATE_JOG)) { system_set_exec_state_flag(EXEC_FEED_HOLD); }
  }
  if (comm_state == 1) {
    if (spindle_machine_moving()) { return; }
    comm_state = 2;
    spindle_alarm_stopped(EXEC_ALARM_SPINDLE_COMM);
    return;
  }
  if ((comm_state == 2) && (sys.state != STATE_ALARM)) { comm_state = 0; }

  // Speed changed by an override: the relay must come back within $52.
  if ((as_state == AS_WAIT) && !as_sync_wait &&
      ((uint32_t)(hal_millis() - as_t0) >= settings.at_speed_timeout_ms)) {
    as_state = AS_OK;          // treat as lost from here
    if (!(sp_in_stable & 1) && (settings.at_speed_loss != 0) && !loss_state) {
      loss_state = 1;
      if (sys.state & (STATE_CYCLE | STATE_JOG)) { system_set_exec_state_flag(EXEC_FEED_HOLD); }
    }
  }

  if (loss_state == 1) {
    if (settings.at_speed_loss == 2) {
      if (spindle_machine_moving()) { return; }
      loss_state = 2;
      spindle_alarm_stopped(EXEC_ALARM_SPINDLE_AT_SPEED);
    } else {
      loss_state = 2;        // feed hold done; resume with cycle start
    }
  }
}


// Called by spindle_sync() after M3/M4 or an S change: wait until the spindle
// is at speed (or the fixed $54 has passed).
static void spindle_wait_at_speed(uint8_t state)
{
  if ((state == SPINDLE_DISABLE) || sys.abort) { return; }
  #ifdef VARIABLE_SPINDLE
    if (bit_istrue(settings.flags,BITFLAG_LASER_MODE)) { return; }
  #endif
  if (as_state == AS_WAIT) {
    as_sync_wait = true;
    while (as_state == AS_WAIT) {
      protocol_execute_realtime();
      if (sys.abort) { as_sync_wait = false; return; }
      if ((uint32_t)(hal_millis() - as_t0) >= settings.at_speed_timeout_ms) {
        as_sync_wait = false;
        as_state = AS_OFF;
        spindle_alarm_stopped(EXEC_ALARM_SPINDLE_AT_SPEED);
        protocol_execute_realtime();
        return;
      }
    }
    as_sync_wait = false;
  } else if (!spindle_at_speed_used() && settings.spinup_delay_ms && spindle_spinup_pending) {
    delay_sec(settings.spinup_delay_ms / 1000.0f, DELAY_MODE_DWELL);
  }
  spindle_spinup_pending = false;
}


// ---- collet ----

uint8_t spindle_collet_is_open() { return outputs_get(OUTPUT_COLLET); }


// Opens or closes the collet. Closing is always allowed. Opening needs a
// stopped spindle (outputs off for $51 and the at-speed relay open), a
// machine that stands (Idle, or a finished hold) and no STOP.
uint8_t spindle_collet_set(uint8_t open)
{
  if (!open) { outputs_set(OUTPUT_COLLET, false); return(STATUS_OK); }
  if (!io_out_channel(OUTPUT_COLLET)) { return(STATUS_SETTING_DISABLED); }
  if (estop_latched || estop_active()) { return(STATUS_ESTOP_ACTIVE); }
  uint8_t still = (sys.state == STATE_IDLE) ||
                  ((sys.state == STATE_HOLD) && (sys.suspend & SUSPEND_HOLD_COMPLETE));
  if (!still) { return(STATUS_COLLET_INTERLOCK); }
  if (outputs_get(OUTPUT_SPINDLE_FWD) || outputs_get(OUTPUT_SPINDLE_REV)) { return(STATUS_COLLET_INTERLOCK); }
  if (outputs_spindle_off_ms() < settings.spindle_rundown_ms) { return(STATUS_COLLET_INTERLOCK); }
  if (io_in_channel(IN_FN_AT_SPEED) && spindle_at_speed()) { return(STATUS_COLLET_INTERLOCK); }
  if (vfd_turning()) { return(STATUS_COLLET_INTERLOCK); }   // the drive still has output
  outputs_set(OUTPUT_COLLET, true);
  return(STATUS_OK);
}


// G-code parser entry-point for setting spindle state. Forces a planner buffer sync and bails
// if an abort or check-mode is active.
#ifdef VARIABLE_SPINDLE
  void spindle_sync(uint8_t state, float rpm)
  {
    if (sys.state == STATE_CHECK_MODE) { return; }
    protocol_buffer_synchronize(); // Empty planner buffer to ensure spindle is set when programmed.
    spindle_set_state(state,rpm);
    spindle_wait_at_speed(state);  // Port: cut only once the spindle is at speed.
  }
#else
  void _spindle_sync(uint8_t state)
  {
    if (sys.state == STATE_CHECK_MODE) { return; }
    protocol_buffer_synchronize(); // Empty planner buffer to ensure spindle is set when programmed.
    _spindle_set_state(state);
    spindle_wait_at_speed(state);  // Port: cut only once the spindle is at speed.
  }
#endif
