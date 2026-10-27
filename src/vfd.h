/*
  vfd.h - spindle VFD over RS485 / Modbus RTU

  Part of Grbl-Giga, a port of Grbl 1.1 to the Arduino GIGA R1 WiFi.

  Grbl is free software: you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation, either version 3 of the License, or (at your option) any later
  version. Distributed WITHOUT ANY WARRANTY; see the GNU GPL for details.

  -------------------------------------------------------------------------------

  With $33 > 0 Grbl also runs the spindle VFD over RS485 (Modbus RTU): the
  isolated transceiver of shield v4 on USART2 (D18 TX, D19 RX), its driver
  enable on D17 (cpu_map.h). The FWD/REV outputs, PWM and the 0-10 V output
  keep working as before, so either way of wiring the drive works.

    M3 / M4   run forward / reverse
    M5, S0    stop
    S         frequency = S / $30 x $37, S limited to $31-$30 and scaled by
              the spindle override exactly as for the PWM output

  Every VFD_POLL_MS the drive is asked for its error code, status, set point
  and output frequency (which also feeds the drive's own communication
  watchdog), and that replaces the relay contacts:

    - at speed: output frequency within VFD_AT_SPEED_PERCENT of the set point
      and the drive running the way it was told. $52 waits for it after
      M3/M4/S and $53 watches it while cutting, as with the at-speed relay,
      which is not used while $33 > 0.
    - fault: the drive reports an error code. Spindle off, controlled stop,
      ALARM:19, $X refused until it clears; a reset or $X sends the drive's
      fault reset (and pulses $92 as before). The fault relay ($78), if
      assigned, still counts as well.
    - the collet stays closed while the drive reports any output frequency,
      or has not been seen standing since it was last told to run.

  ALARM:23 (after a controlled stop, position kept) when the drive cannot be
  trusted with the spindle: no valid answer after 1 + VFD_RETRIES tries
  while the spindle should run or may still run, a command the drive
  refused (Modbus exception), or a drive that does not run, stop or turn as
  told within VFD_CONFIRM_MS (its run command set to the keypad, say). $X and
  $H are refused with error:51 while that lasts. A silent drive with the
  spindle off and standing is no alarm: it only shows in $VFD and $I.

     $33  drive: 0 none (0-10 V and relays only), 1 Delta (MS300, C2000, VFD-E...)
     $34  Modbus address of the drive, 1-247
     $35  baud rate, 1200-115200
     $36  frame: 0 8N1, 1 8N2, 2 8E1, 3 8O1, 4 8E2, 5 8O2
     $37  drive frequency at S = $30, Hz

  The protocol (CRC, framing, retries, timeouts, the alarms) is common; what
  differs between vendors - the registers and the meaning of the status -
  is a profile in vfd.c. Delta is the first.
*/

#ifndef vfd_h
#define vfd_h

// $33
#define VFD_NONE     0
#define VFD_DELTA    1
#define VFD_N_TYPES  2

// Directions for vfd_spindle().
#define VFD_STOP     0
#define VFD_FWD      1
#define VFD_REV      2

// Highest $37: 655.35 Hz in the drive's 0.01 Hz steps.
#define VFD_MAX_HZ   655.35f

// (Re)reads $33-$37. Power-up, every reset and every change of those
// settings; the port is only touched when they changed.
void vfd_init();
uint8_t vfd_enabled();

// What the spindle should do: dir VFD_*, rpm as sys.spindle_speed (limits and
// override applied). Only stores it - safe from any interrupt.
void vfd_spindle(uint8_t dir, float rpm);
// A new speed for the running spindle (the override, from the stepper interrupt).
void vfd_speed(float rpm);
// Sends the drive's fault reset (system_reset_pulses()).
void vfd_fault_reset();

// From every wait loop (protocol_service_machine): one Modbus transaction at
// a time, never waiting for the line.
void vfd_service();

// For the spindle's at-speed and fault handling (also read by the input scan).
uint8_t vfd_at_speed();
uint8_t vfd_fault();
uint8_t vfd_turning();     // output frequency, or not seen standing since a run command
uint8_t vfd_trouble();     // ALARM:23 reason: no answer, refused, not following, or stopped by itself
uint8_t vfd_may_run();     // a run command went out and no stop has been seen since

// $I: " vfd=<link 0 off, 1 waiting, 2 ok, 3 lost>/<error code>/<output 0.01 Hz>/<failed transactions>"
void vfd_report_diag();

// $VFD prints the state; $VFD=R<reg>[,<count>] reads registers and
// $VFD=W<reg>,<value> writes one (hexadecimal, Idle or Alarm only).
uint8_t vfd_command(char *line);

// Test hook $TVFD=mode[,code]: a simulated Delta drive takes the place of the
// RS485 port. 0 off (the port again), 1 healthy (an error it reported stays
// latched until its fault reset), 2 in fault with error code (default 2),
// the reset ignored while it lasts, 3 silent, 4 every second answer broken,
// 5 not under RS485 control (answers, but ignores run and stop), 6 its keypad
// STOP pressed once (it stops, then runs when told again; mode 1 afterwards).
uint8_t vfd_test(char *line);

#endif
