/*
  drvpwr.c - driver power input (supply voltage of the step and servo drives)

  Part of Grbl-Giga, a port of Grbl 1.1 to the Arduino GIGA R1 WiFi.

  Grbl is free software: you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation, either version 3 of the License, or (at your option) any later
  version. Distributed WITHOUT ANY WARRANTY; see the GNU GPL for details.

  -------------------------------------------------------------------------------

  See drvpwr.h. Everything runs in the input scan, as the servo alarms do:
  a loss is confirmed after DRVPWR_LOSS_SCANS, the return after $95. At the
  loss the brake engages and the enable drops at once - the drives cannot
  hold the axis any more, and a Z brake left released by the BRAKE output
  would let it fall. Then Grbl's own way out: mc_reset() and the alarm code.
*/

#include "grbl.h"

static volatile uint8_t enabled = 0;            // $94, as last read
static volatile uint32_t return_scans = 0;      // $95 in scans
static volatile uint8_t lost = 0;               // watched, and not back for $95 yet
static volatile uint8_t present = 0;            // the input, as the last scan read it
static volatile uint16_t off_scans = 0;
static volatile uint32_t on_scans = 0;
static volatile uint32_t losses = 0;
static volatile uint8_t note_on = 0;            // [MSG:Driver power on] to print


void drvpwr_init()
{
  uint32_t sreg = GRBL_IRQ_SAVE();
  uint8_t was = enabled;
  enabled = settings.drv_power_enable ? 1 : 0;
  return_scans = (uint32_t)settings.drv_power_delay_ms * (INPUT_SCAN_FREQUENCY / 1000);
  if (!enabled || !was) {
    // Just switched on (or at power-up): the supply counts as present until
    // the scan has seen it missing for DRVPWR_LOSS_SCANS - which then is a
    // loss like any other, ALARM:22 included.
    lost = 0;
    off_scans = 0;
    on_scans = 0;
    present = 1;
  }
  GRBL_IRQ_RESTORE(sreg);
}


// mc_reset() flushes the planner, stops spindle, coolant and a program from
// the stick; in an alarm there is nothing of that left to stop.
static void drvpwr_alarm()
{
  if (sys_rt_exec_alarm) { return; }    // an alarm on its way already says enough
  if (sys.state != STATE_ALARM) { mc_reset(); }
  system_set_exec_alarm(EXEC_ALARM_DRIVER_POWER);
}


void drvpwr_scan(uint32_t channels)
{
  if (!enabled) { return; }
  if (channels & (1UL << IO_DRVPWR_BIT)) {
    present = 1;
    off_scans = 0;
    if (lost && (++on_scans >= return_scans)) {
      lost = 0;
      note_on = 1;
    }
  } else {
    present = 0;
    on_scans = 0;
    if (!lost && (++off_scans >= DRVPWR_LOSS_SCANS)) {
      lost = 1;
      losses++;
      note_on = 0;
      uint32_t sreg = GRBL_IRQ_SAVE();
      st_estop_halt();                  // brake on, drives off, no further step
      GRBL_IRQ_RESTORE(sreg);
      drvpwr_alarm();                   // reported even in an alarm: a position kept over it is gone
      return;
    }
  }
  // As with a servo alarm: whatever left the alarm while the supply is still
  // off (or not back for $95) lands straight back in it.
  if (lost && (sys.state != STATE_ALARM)) { drvpwr_alarm(); }
}


uint8_t drvpwr_ok() { return (!enabled || !lost) ? true : false; }

uint8_t drvpwr_state()
{
  if (!enabled) { return(DRVPWR_NOT_WATCHED); }
  if (!lost) { return(DRVPWR_ON); }
  return(present ? DRVPWR_RETURNING : DRVPWR_OFF);
}

uint32_t drvpwr_loss_count() { return losses; }


void drvpwr_service()
{
  if (!note_on) { return; }
  note_on = 0;
  report_feedback_message(MESSAGE_DRIVER_POWER_ON);
}
