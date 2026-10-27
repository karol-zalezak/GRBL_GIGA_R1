/*
  protocol.c - controls Grbl execution protocol and procedures
  Part of Grbl

  Copyright (c) 2011-2016 Sungeun K. Jeon for Gnea Research LLC
  Copyright (c) 2009-2011 Simen Svale Skogsrud

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
*/

#include "grbl.h"

static char line[LINE_BUFFER_SIZE]; // Line to be executed. Zero-terminated.
// Port: the line being received from the PC, at file scope so that lines can
// also be taken from protocol_exec_rt_system() while a program from the USB
// stick runs (see protocol_serial_during_job()).
static uint8_t line_flags = 0;
static uint8_t char_counter = 0;
static uint8_t serial_in_main = false;   // the main loop is between receiving and running a line

uint32_t protocol_last_line_ms = 0;

static void protocol_exec_rt_suspend();


/*
  GRBL PRIMARY LOOP:
*/
void protocol_main_loop()
{
  // Perform some machine checks to make sure everything is good to go.
  #ifdef CHECK_LIMITS_AT_INIT
    if (bit_istrue(settings.flags, BITFLAG_HARD_LIMIT_ENABLE)) {
      if (limits_get_state()) {
        sys.state = STATE_ALARM; // Ensure alarm state is active.
        report_feedback_message(MESSAGE_CHECK_LIMITS);
      }
    }
  #endif
  // Port: a central STOP still latched after the reset keeps the machine in alarm.
  if (estop_latched) {
    sys.state = STATE_ALARM;
    report_alarm_message(EXEC_ALARM_ESTOP);   // prints [MSG:E-stop] too
  }
  // Port: reset pulses for the servos (after ALARM:17) and the VFD (in fault).
  system_reset_pulses();

  // Check for and report alarm state after a reset, error, or an initial power up.
  // NOTE: Sleep mode disables the stepper drivers and position can't be guaranteed.
  // Re-initialize the sleep state as an ALARM mode to ensure user homes or acknowledges.
  if (sys.state & (STATE_ALARM | STATE_SLEEP)) {
    report_feedback_message(MESSAGE_ALARM_LOCK);
    sys.state = STATE_ALARM; // Ensure alarm state is set.
  } else {
    // Check if the safety door is open.
    sys.state = STATE_IDLE;
    if (system_check_safety_door_ajar()) {
      bit_true(sys_rt_exec_state, EXEC_SAFETY_DOOR);
      protocol_execute_realtime(); // Enter safety door mode. Should return as IDLE state.
    }
    // All systems go!
    system_execute_startup(line); // Execute startup script.
  }

  // ---------------------------------------------------------------------------------
  // Primary loop! Upon a system abort, this exits back to main() to reset the system.
  // This is also where Grbl idles while waiting for something to do.
  // ---------------------------------------------------------------------------------

  uint8_t c;
  line_flags = 0;
  char_counter = 0;
  // Port: a program from the USB stick learns about the reset that brought
  // Grbl here (its own, or one that ends it).
  job_reset();
  for (;;) {

    // Settings live in a RAM shadow of the Data Flash; this writes any pending
    // change back once the machine is idle. Deferring it here keeps a
    // multi-millisecond flash erase away from active motion and coalesces a
    // burst of $-commands into a single erase cycle. See eeprom.c.
    eeprom_flush_pending();


    // Process one line of incoming serial data, as the data becomes available. Performs an
    // initial filtering by removing spaces and comments and capitalizing all letters.
    while((c = serial_read()) != SERIAL_NO_DATA) {
      if (protocol_line_char(line, &char_counter, &line_flags, c)) { // End of line reached

        serial_in_main = true;
        protocol_execute_realtime(); // Runtime command check point.
        serial_in_main = false;
        if (sys.abort) { return; } // Bail to calling function upon system abort

        #ifdef REPORT_ECHO_LINE_RECEIVED
          report_echo_line_received(line);
        #endif

        // Direct and execute one line of formatted input, and report status of execution.
        protocol_last_line_ms = hal_millis();
        report_status_message(protocol_execute_line(line, line_flags, false));

        // Reset tracking data for next line.
        line_flags = 0;
        char_counter = 0;
      }
    }

    // Touch-screen actions that are commands (jog, home, unlock, zero) run here,
    // where the serial buffer is empty and no line from the PC is half received.
    if ((char_counter == 0) && (line_flags == 0)) {
      display_touch_service();
      if (sys.abort) { return; }
      // The handwheel's jogs, at the same point.
      mpg_service();
      if (sys.abort) { return; }
      // A program from the USB stick feeds its lines here, as fast as the
      // planner takes them - between lines from the PC, which it refuses.
      job_service();
      if (sys.abort) { return; }
    }

    // If there are no more characters in the serial read buffer to be processed and executed,
    // this indicates that g-code streaming has either filled the planner buffer or has
    // completed. In either case, auto-cycle start, if enabled, any queued moves.
    protocol_auto_cycle_start();

    protocol_execute_realtime();  // Runtime command check point.
    if (sys.abort) { return; } // Bail to main() program loop to reset system.
  }

  return; /* Never reached */
}


// Port: while a program from the USB stick runs, Grbl spends long stretches
// outside the main loop - in a full planner, an M0 pause, a dwell - and lines
// from the PC would wait for all of it. They are taken here instead, from
// every wait loop: the read-only $ commands and STOP ($FS) are answered, any
// other line is refused at once (STATUS_JOB_RUNNING).
static void protocol_serial_during_job()
{
  if (serial_in_main) { return; }      // the main loop holds a complete line
  int c;
  while ((c = serial_read()) != SERIAL_NO_DATA) {
    if (protocol_line_char(line, &char_counter, &line_flags, (uint8_t)c)) {
      protocol_last_line_ms = hal_millis();
      report_status_message(protocol_execute_line(line, line_flags, false));
      line_flags = 0;
      char_counter = 0;
    }
  }
}


// Grbl's input filter, upstream's per-character loop body: drops spaces,
// control characters and comments, upper-cases letters, flags an overflow.
// Port: what follows the '=' or '+' of a $F command (a file name, upload
// data) is kept as it comes - case, spaces, '(' and ';' - so that a file on
// the stick can be named "Front panel (v2).nc".
uint8_t protocol_line_char(char *buf, uint8_t *count, uint8_t *flags, uint8_t c)
{
  if ((c == '\n') || (c == '\r')) { // End of line reached
    buf[*count] = 0; // Set string termination character.
    return(true);
  }
  if (*flags == LINE_FLAG_VERBATIM) {
    if (c < ' ') {
      // Control characters still go
    } else if (*count >= (LINE_BUFFER_SIZE-1)) {
      *flags |= LINE_FLAG_OVERFLOW;
    } else {
      buf[(*count)++] = c;
    }
    return(false);
  }
  if (*flags) {
    // Throw away all (except EOL) comment characters and overflow characters.
    if (c == ')') {
      // End of '()' comment. Resume line allowed.
      if (*flags & LINE_FLAG_COMMENT_PARENTHESES) { *flags &= ~(LINE_FLAG_COMMENT_PARENTHESES); }
    }
  } else {
    if (c <= ' ') {
      // Throw away whitepace and control characters
    } else if (c == '/') {
      // Block delete NOT SUPPORTED. Ignore character.
      // NOTE: If supported, would simply need to check the system if block delete is enabled.
    } else if (c == '(') {
      // Enable comments flag and ignore all characters until ')' or EOL.
      // NOTE: This doesn't follow the NIST definition exactly, but is good enough for now.
      // In the future, we could simply remove the items within the comments, but retain the
      // comment control characters, so that the g-code parser can error-check it.
      *flags |= LINE_FLAG_COMMENT_PARENTHESES;
    } else if (c == ';') {
      // NOTE: ';' comment to EOL is a LinuxCNC definition. Not NIST.
      *flags |= LINE_FLAG_COMMENT_SEMICOLON;
    // TODO: Install '%' feature
    // } else if (c == '%') {
      // Program start-end percent sign NOT SUPPORTED.
      // NOTE: This maybe installed to tell Grbl when a program is running vs manual input,
      // where, during a program, the system auto-cycle start will continue to execute
      // everything until the next '%' sign. This will help fix resuming issues with certain
      // functions that empty the planner buffer to execute its task on-time.
    } else if (*count >= (LINE_BUFFER_SIZE-1)) {
      // Detect line buffer overflow and set flag.
      *flags |= LINE_FLAG_OVERFLOW;
    } else if (c >= 'a' && c <= 'z') { // Upcase lowercase
      buf[(*count)++] = c-'a'+'A';
    } else {
      buf[(*count)++] = c;
      if ((*count > 2) && ((c == '=') || (c == '+')) && (buf[0] == '$') && (buf[1] == 'F')) {
        *flags |= LINE_FLAG_VERBATIM;
      }
    }
  }
  return(false);
}


// Runs one filtered line and returns its status (upstream's main loop body).
// While a program from the USB stick runs, the PC gets only the read-only $
// commands and STOP ($FS); everything else is refused with STATUS_JOB_RUNNING.
uint8_t protocol_execute_line(char *line, uint8_t flags, uint8_t from_job)
{
  if (flags & LINE_FLAG_OVERFLOW) {
    return(STATUS_OVERFLOW);
  } else if (line[0] == 0) {
    // Empty or comment line. For syncing purposes.
    return(STATUS_OK);
  } else if (line[0] == '$') {
    // Grbl '$' system command. Not from a file: $-commands there are refused.
    if (from_job) { return(STATUS_INVALID_STATEMENT); }
    if (job_active() && !job_allows(line)) { return(STATUS_JOB_RUNNING); }
    return(system_execute_line(line));
  } else if (!from_job && job_active()) {
    return(STATUS_JOB_RUNNING);
  } else if (sys.state & (STATE_ALARM | STATE_JOG)) {
    // Everything else is gcode. Block if in alarm or jog mode.
    return(STATUS_SYSTEM_GC_LOCK);
  }
  // Parse and execute g-code block.
  return(gc_execute_line(line));
}


// Block until all buffered steps are executed or in a cycle state. Works with feed hold
// during a synchronize call, if it should happen. Also, waits for clean cycle end.
void protocol_buffer_synchronize()
{
  // If system is queued, ensure cycle resumes if the auto start flag is present.
  protocol_auto_cycle_start();
  do {
    protocol_execute_realtime();   // Check and execute run-time commands
    if (sys.abort) { return; } // Check for system abort
  } while (plan_get_current_block() || (sys.state == STATE_CYCLE));
}


// Auto-cycle start triggers when there is a motion ready to execute and if the main program is not
// actively parsing commands.
// NOTE: This function is called from the main loop, buffer sync, and mc_line() only and executes
// when one of these conditions exist respectively: There are no more blocks sent (i.e. streaming
// is finished, single commands), a command that needs to wait for the motions in the buffer to
// execute calls a buffer sync, or the planner buffer is full and ready to go.
void protocol_auto_cycle_start()
{
  if (plan_get_current_block() != NULL) { // Check if there are any blocks in the buffer.
    system_set_exec_state_flag(EXEC_CYCLE_START); // If so, execute them!
  }
}


// This function is the general interface to Grbl's real-time command execution system. It is called
// from various check points in the main program, primarily where there may be a while loop waiting
// for a buffer to clear space or any point where the execution time from the last check point may
// be more than a fraction of a second. This is a way to execute realtime commands asynchronously
// (aka multitasking) with grbl's g-code parsing and planning functions. This function also serves
// as an interface for the interrupts to set the system realtime flags, where only the main program
// handles them, removing the need to define more computationally-expensive volatile variables. This
// also provides a controlled way to execute certain tasks without having two or more instances of
// the same task, such as the planner recalculating the buffer upon a feedhold or overrides.
// NOTE: The sys_rt_exec_state variable flags are set by any process, step or serial interrupts, pinouts,
// limit switches, or the main program.
void protocol_execute_realtime()
{
  protocol_exec_rt_system();
  if (sys.suspend) { protocol_exec_rt_suspend(); }
}


// Executes run-time commands, when required. This function primarily operates as Grbl's state
// machine and controls the various real-time features Grbl has to offer.
// NOTE: Do not alter this unless you know exactly what you are doing!
// Port: the machine-side timers that must run whatever Grbl is doing - driver
// enable and brake ($1, $48-$50), the reset pulses, the spindle's at-speed and
// fault handling.
static void protocol_service_machine()
{
  st_power_service();
  outputs_service();
  vfd_service();
  panel_service();
  spindle_service();
  io_test_service();
  job_rt_service();
  drvpwr_service();
  mpg_rt_service();
}


void protocol_service_link()
{
  serial_pump();
  display_service();
  protocol_service_machine();
  if (sys_rt_exec_state & EXEC_STATUS_REPORT) {
    report_realtime_status();
    system_clear_exec_state_flag(EXEC_STATUS_REPORT);
  }
}


void protocol_exec_rt_system()
{
  uint8_t rt_exec; // Temp variable to avoid calling volatile multiple times.

  // Services the PC link. This has to live here rather than in
  // protocol_execute_realtime(), because two of Grbl's wait loops - the suspend
  // loop that holds the machine during a feed hold, and the sleep loop - call
  // this function directly and never go through protocol_execute_realtime().
  //
  // On AVR that made no difference: receive was interrupt driven, so `~`, `?`
  // and reset always got through no matter where the main program was. Native
  // USB CDC has no per-character receive interrupt, so characters are only
  // collected here. With the pump one level up, a feed hold was unrecoverable -
  // nothing was ever read, so the resume command could not arrive, status
  // requests went unanswered, and the CDC receive FIFO filled until the host's
  // own writes began to time out.
  serial_pump();
  display_service();
  protocol_service_machine();
  if (job_active()) { protocol_serial_during_job(); }
  rt_exec = sys_rt_exec_alarm; // Copy volatile sys_rt_exec_alarm.
  if (rt_exec) { // Enter only if any bit flag is true
    // System alarm. Everything has shutdown by something that has gone severely wrong. Report
    // the source of the error to the user. If critical, Grbl disables by entering an infinite
    // loop until system reset/abort.
    sys.state = STATE_ALARM; // Set system alarm state
    report_alarm_message(rt_exec);
    // Halt everything upon a critical event flag. Hard and soft limits flag this upstream; a
    // driver alarm is just as final, since that motor's position is gone.
    if ((rt_exec == EXEC_ALARM_HARD_LIMIT) || (rt_exec == EXEC_ALARM_SOFT_LIMIT) ||
        (rt_exec == EXEC_ALARM_MOTOR_FAULT) || (rt_exec == EXEC_ALARM_ESTOP)) {
      report_feedback_message(MESSAGE_CRITICAL_EVENT);
      system_clear_exec_state_flag(EXEC_RESET); // Disable any existing reset
      do {
        // Block everything, except reset and status reports, until user issues reset or power
        // cycles. Hard limits typically occur while unattended or not paying attention. Gives
        // the user and a GUI time to do what is needed before resetting, like killing the
        // incoming stream. The same could be said about soft limits. While the position is not
        // lost, continued streaming could cause a serious crash if by chance it gets executed.
        // The USB link needs pumping to get the alarm out, and the display keeps showing it.
        // Status reports are answered too, as the comment above promises (upstream's loop is
        // empty), so a sender sees the alarm state and the input that caused it.
        protocol_service_link();
      } while (bit_isfalse(sys_rt_exec_state,EXEC_RESET));
    }
    system_clear_exec_alarm(); // Clear alarm
  }

  rt_exec = sys_rt_exec_state; // Copy volatile sys_rt_exec_state.
  if (rt_exec) {

    // Execute system abort.
    if (rt_exec & EXEC_RESET) {
      sys.abort = true;  // Only place this is set true.
      return; // Nothing else to do but exit.
    }

    // The status report is printed further down, after this pass's state changes.

    // NOTE: Once hold is initiated, the system immediately enters a suspend state to block all
    // main program processes until either reset or resumed. This ensures a hold completes safely.
    if (rt_exec & (EXEC_MOTION_CANCEL | EXEC_FEED_HOLD | EXEC_SAFETY_DOOR | EXEC_SLEEP)) {

      // State check for allowable states for hold methods.
      if (!(sys.state & (STATE_ALARM | STATE_CHECK_MODE))) {
      
        // If in CYCLE or JOG states, immediately initiate a motion HOLD.
        if (sys.state & (STATE_CYCLE | STATE_JOG)) {
          if (!(sys.suspend & (SUSPEND_MOTION_CANCEL | SUSPEND_JOG_CANCEL))) { // Block, if already holding.
            st_update_plan_block_parameters(); // Notify stepper module to recompute for hold deceleration.
            sys.step_control = STEP_CONTROL_EXECUTE_HOLD; // Initiate suspend state with active flag.
            if (sys.state == STATE_JOG) { // Jog cancelled upon any hold event, except for sleeping.
              if (!(rt_exec & EXEC_SLEEP)) { sys.suspend |= SUSPEND_JOG_CANCEL; } 
            }
          }
        }
        // If IDLE, Grbl is not in motion. Simply indicate suspend state and hold is complete.
        if (sys.state == STATE_IDLE) { sys.suspend = SUSPEND_HOLD_COMPLETE; }

        // Execute and flag a motion cancel with deceleration and return to idle. Used primarily by probing cycle
        // to halt and cancel the remainder of the motion.
        if (rt_exec & EXEC_MOTION_CANCEL) {
          // MOTION_CANCEL only occurs during a CYCLE, but a HOLD and SAFETY_DOOR may been initiated beforehand
          // to hold the CYCLE. Motion cancel is valid for a single planner block motion only, while jog cancel
          // will handle and clear multiple planner block motions.
          if (!(sys.state & STATE_JOG)) { sys.suspend |= SUSPEND_MOTION_CANCEL; } // NOTE: State is STATE_CYCLE.
        }

        // Execute a feed hold with deceleration, if required. Then, suspend system.
        if (rt_exec & EXEC_FEED_HOLD) {
          // Block SAFETY_DOOR, JOG, and SLEEP states from changing to HOLD state.
          if (!(sys.state & (STATE_SAFETY_DOOR | STATE_JOG | STATE_SLEEP))) { sys.state = STATE_HOLD; }
        }

        // Execute a safety door stop with a feed hold and disable spindle/coolant.
        // NOTE: Safety door differs from feed holds by stopping everything no matter state, disables powered
        // devices (spindle/coolant), and blocks resuming until switch is re-engaged.
        if (rt_exec & EXEC_SAFETY_DOOR) {
          report_feedback_message(MESSAGE_SAFETY_DOOR_AJAR);
          // If jogging, block safety door methods until jog cancel is complete. Just flag that it happened.
          if (!(sys.suspend & SUSPEND_JOG_CANCEL)) {
            // Check if the safety re-opened during a restore parking motion only. Ignore if
            // already retracting, parked or in sleep state.
            if (sys.state == STATE_SAFETY_DOOR) {
              if (sys.suspend & SUSPEND_INITIATE_RESTORE) { // Actively restoring
                #ifdef PARKING_ENABLE
                  // Set hold and reset appropriate control flags to restart parking sequence.
                  if (sys.step_control & STEP_CONTROL_EXECUTE_SYS_MOTION) {
                    st_update_plan_block_parameters(); // Notify stepper module to recompute for hold deceleration.
                    sys.step_control = (STEP_CONTROL_EXECUTE_HOLD | STEP_CONTROL_EXECUTE_SYS_MOTION);
                    sys.suspend &= ~(SUSPEND_HOLD_COMPLETE);
                  } // else NO_MOTION is active.
                #endif
                sys.suspend &= ~(SUSPEND_RETRACT_COMPLETE | SUSPEND_INITIATE_RESTORE | SUSPEND_RESTORE_COMPLETE);
                sys.suspend |= SUSPEND_RESTART_RETRACT;
              }
            }
            if (sys.state != STATE_SLEEP) { sys.state = STATE_SAFETY_DOOR; }
          }
          // NOTE: This flag doesn't change when the door closes, unlike sys.state. Ensures any parking motions
          // are executed if the door switch closes and the state returns to HOLD.
          sys.suspend |= SUSPEND_SAFETY_DOOR_AJAR;
        }
        
      }

      if (rt_exec & EXEC_SLEEP) {
        if (sys.state == STATE_ALARM) { sys.suspend |= (SUSPEND_RETRACT_COMPLETE|SUSPEND_HOLD_COMPLETE); }
        sys.state = STATE_SLEEP; 
      }

      system_clear_exec_state_flag((EXEC_MOTION_CANCEL | EXEC_FEED_HOLD | EXEC_SAFETY_DOOR | EXEC_SLEEP));
    }

    // Execute a cycle start by starting the stepper interrupt to begin executing the blocks in queue.
    if (rt_exec & EXEC_CYCLE_START) {
      uint8_t start_pending = false;   // Port: kept for the next pass while the spindle gets to speed
      // Block if called at same time as the hold commands: feed hold, motion cancel, and safety door.
      // Ensures auto-cycle-start doesn't resume a hold without an explicit user-input.
      if (!(rt_exec & (EXEC_FEED_HOLD | EXEC_MOTION_CANCEL | EXEC_SAFETY_DOOR))) {
        // Resume door state when parking motion has retracted and door has been closed.
        if ((sys.state == STATE_SAFETY_DOOR) && !(sys.suspend & SUSPEND_SAFETY_DOOR_AJAR)) {
          if (sys.suspend & SUSPEND_RESTORE_COMPLETE) {
            sys.state = STATE_IDLE; // Set to IDLE to immediately resume the cycle.
          } else if (sys.suspend & SUSPEND_RETRACT_COMPLETE) {
            // Flag to re-energize powered components and restore original position, if disabled by SAFETY_DOOR.
            // NOTE: For a safety door to resume, the switch must be closed, as indicated by HOLD state, and
            // the retraction execution is complete, which implies the initial feed hold is not active. To
            // restore normal operation, the restore procedures must be initiated by the following flag. Once,
            // they are complete, it will call CYCLE_START automatically to resume and exit the suspend.
            sys.suspend |= SUSPEND_INITIATE_RESTORE;
          }
        }
        // Cycle start only when IDLE or when a hold is complete and ready to resume.
        // Port: a hold does not resume while the collet is open (manual tool change).
        if ((sys.state == STATE_IDLE) ||
            ((sys.state & STATE_HOLD) && (sys.suspend & SUSPEND_HOLD_COMPLETE) && !spindle_collet_is_open())) {
          if (sys.state == STATE_HOLD && sys.spindle_stop_ovr) {
            sys.spindle_stop_ovr |= SPINDLE_STOP_OVR_RESTORE_CYCLE; // Set to restore in suspend routine and cycle start after.
          } else if ((sys.state & STATE_HOLD) && spindle_resume_wait()) {
            // Port: the spindle was restored or its speed changed in the hold and is not
            // at speed yet: resume once it is (or ALARM:14 after $52, which ends the hold).
            start_pending = (sys.state & STATE_HOLD) ? true : false;
          } else {
            // Start cycle only if queued motions exist in planner buffer and the motion is not canceled.
            sys.step_control = STEP_CONTROL_NORMAL_OP; // Restore step control to normal operation
            if (plan_get_current_block() && bit_isfalse(sys.suspend,SUSPEND_MOTION_CANCEL)) {
              sys.suspend = SUSPEND_DISABLE; // Break suspend state.
              sys.state = STATE_CYCLE;
              st_prep_buffer(); // Initialize step segment buffer before beginning cycle.
              st_wake_up();
            } else { // Otherwise, do nothing. Set and resume IDLE state.
              sys.suspend = SUSPEND_DISABLE; // Break suspend state.
              sys.state = STATE_IDLE;
            }
          }
        }
      }
      if (!start_pending) { system_clear_exec_state_flag(EXEC_CYCLE_START); }
    }

    if (rt_exec & EXEC_CYCLE_STOP) {
      // Reinitializes the cycle plan and stepper system after a feed hold for a resume. Called by
      // realtime command execution in the main program, ensuring that the planner re-plans safely.
      // NOTE: Bresenham algorithm variables are still maintained through both the planner and stepper
      // cycle reinitializations. The stepper path should continue exactly as if nothing has happened.
      // NOTE: EXEC_CYCLE_STOP is set by the stepper subsystem when a cycle or feed hold completes.
      if ((sys.state & (STATE_HOLD|STATE_SAFETY_DOOR|STATE_SLEEP)) && !(sys.soft_limit) && !(sys.suspend & SUSPEND_JOG_CANCEL)) {
        // Hold complete. Set to indicate ready to resume.  Remain in HOLD or DOOR states until user
        // has issued a resume command or reset.
        plan_cycle_reinitialize();
        if (sys.step_control & STEP_CONTROL_EXECUTE_HOLD) { sys.suspend |= SUSPEND_HOLD_COMPLETE; }
        bit_false(sys.step_control,(STEP_CONTROL_EXECUTE_HOLD | STEP_CONTROL_EXECUTE_SYS_MOTION));
      } else {
        // Motion complete. Includes CYCLE/JOG/HOMING states and jog cancel/motion cancel/soft limit events.
        // NOTE: Motion and jog cancel both immediately return to idle after the hold completes.
        if (sys.suspend & SUSPEND_JOG_CANCEL) {   // For jog cancel, flush buffers and sync positions.
          sys.step_control = STEP_CONTROL_NORMAL_OP;
          plan_reset();
          st_reset();
          gc_sync_position();
          plan_sync_position();
        }
        if (sys.suspend & SUSPEND_SAFETY_DOOR_AJAR) { // Only occurs when safety door opens during jog.
          sys.suspend &= ~(SUSPEND_JOG_CANCEL);
          sys.suspend |= SUSPEND_HOLD_COMPLETE;
          sys.state = STATE_SAFETY_DOOR;
        } else {
          sys.suspend = SUSPEND_DISABLE;
          sys.state = STATE_IDLE;
        }
      }
      system_clear_exec_state_flag(EXEC_CYCLE_STOP);
    }

    // Execute and serial print status. Upstream prints it before the hold, start
    // and stop handling above; here it comes after, so that a real-time command
    // and a `?` arriving together - as they do in one USB packet, where a UART
    // would have let the main loop run between the two characters - report the
    // state the command produced.
    if (rt_exec & EXEC_STATUS_REPORT) {
      report_realtime_status();
      system_clear_exec_state_flag(EXEC_STATUS_REPORT);
    }
  }

  // Execute overrides.
  rt_exec = sys_rt_exec_motion_override; // Copy volatile sys_rt_exec_motion_override
  if (rt_exec) {
    system_clear_exec_motion_overrides(); // Clear all motion override flags.

    uint8_t new_f_override =  sys.f_override;
    if (rt_exec & EXEC_FEED_OVR_RESET) { new_f_override = DEFAULT_FEED_OVERRIDE; }
    if (rt_exec & EXEC_FEED_OVR_COARSE_PLUS) { new_f_override += FEED_OVERRIDE_COARSE_INCREMENT; }
    if (rt_exec & EXEC_FEED_OVR_COARSE_MINUS) { new_f_override -= FEED_OVERRIDE_COARSE_INCREMENT; }
    if (rt_exec & EXEC_FEED_OVR_FINE_PLUS) { new_f_override += FEED_OVERRIDE_FINE_INCREMENT; }
    if (rt_exec & EXEC_FEED_OVR_FINE_MINUS) { new_f_override -= FEED_OVERRIDE_FINE_INCREMENT; }
    new_f_override = min(new_f_override,MAX_FEED_RATE_OVERRIDE);
    new_f_override = max(new_f_override,MIN_FEED_RATE_OVERRIDE);

    uint8_t new_r_override = sys.r_override;
    if (rt_exec & EXEC_RAPID_OVR_RESET) { new_r_override = DEFAULT_RAPID_OVERRIDE; }
    if (rt_exec & EXEC_RAPID_OVR_MEDIUM) { new_r_override = RAPID_OVERRIDE_MEDIUM; }
    if (rt_exec & EXEC_RAPID_OVR_LOW) { new_r_override = RAPID_OVERRIDE_LOW; }

    if ((new_f_override != sys.f_override) || (new_r_override != sys.r_override)) {
      sys.f_override = new_f_override;
      sys.r_override = new_r_override;
      sys.report_ovr_counter = 0; // Set to report change immediately
      plan_update_velocity_profile_parameters();
      plan_cycle_reinitialize();
    }
  }

  rt_exec = sys_rt_exec_accessory_override;
  if (rt_exec) {
    system_clear_exec_accessory_overrides(); // Clear all accessory override flags.

    // NOTE: Unlike motion overrides, spindle overrides do not require a planner reinitialization.
    uint8_t last_s_override =  sys.spindle_speed_ovr;
    if (rt_exec & EXEC_SPINDLE_OVR_RESET) { last_s_override = DEFAULT_SPINDLE_SPEED_OVERRIDE; }
    if (rt_exec & EXEC_SPINDLE_OVR_COARSE_PLUS) { last_s_override += SPINDLE_OVERRIDE_COARSE_INCREMENT; }
    if (rt_exec & EXEC_SPINDLE_OVR_COARSE_MINUS) { last_s_override -= SPINDLE_OVERRIDE_COARSE_INCREMENT; }
    if (rt_exec & EXEC_SPINDLE_OVR_FINE_PLUS) { last_s_override += SPINDLE_OVERRIDE_FINE_INCREMENT; }
    if (rt_exec & EXEC_SPINDLE_OVR_FINE_MINUS) { last_s_override -= SPINDLE_OVERRIDE_FINE_INCREMENT; }
    last_s_override = min(last_s_override,MAX_SPINDLE_SPEED_OVERRIDE);
    last_s_override = max(last_s_override,MIN_SPINDLE_SPEED_OVERRIDE);

    if (last_s_override != sys.spindle_speed_ovr) {
      sys.spindle_speed_ovr = last_s_override;
      // NOTE: Spindle speed overrides during HOLD state are taken care of by suspend function.
      if (sys.state == STATE_IDLE) { spindle_set_state(gc_state.modal.spindle, gc_state.spindle_speed); }
			else { bit_true(sys.step_control, STEP_CONTROL_UPDATE_SPINDLE_PWM); }
      sys.report_ovr_counter = 0; // Set to report change immediately
    }

    if (rt_exec & EXEC_SPINDLE_OVR_STOP) {
      // Spindle stop override allowed only while in HOLD state.
      // NOTE: Report counters are set in spindle_set_state() when spindle stop is executed.
      if (sys.state == STATE_HOLD) {
        if (!(sys.spindle_stop_ovr)) { sys.spindle_stop_ovr = SPINDLE_STOP_OVR_INITIATE; }
        else if (sys.spindle_stop_ovr & SPINDLE_STOP_OVR_ENABLED) { sys.spindle_stop_ovr |= SPINDLE_STOP_OVR_RESTORE; }
      }
    }

    // NOTE: Since coolant state always performs a planner sync whenever it changes, the current
    // run state can be determined by checking the parser state.
    // NOTE: Coolant overrides only operate during IDLE, CYCLE, HOLD, and JOG states. Ignored otherwise.
    if (rt_exec & (EXEC_COOLANT_FLOOD_OVR_TOGGLE | EXEC_COOLANT_MIST_OVR_TOGGLE)) {
      if ((sys.state == STATE_IDLE) || (sys.state & (STATE_CYCLE | STATE_HOLD | STATE_JOG))) {
        uint8_t coolant_state = gc_state.modal.coolant;
        #ifdef ENABLE_M7
          if (rt_exec & EXEC_COOLANT_MIST_OVR_TOGGLE) {
            if (coolant_state & COOLANT_MIST_ENABLE) { bit_false(coolant_state,COOLANT_MIST_ENABLE); }
            else { coolant_state |= COOLANT_MIST_ENABLE; }
          }
          if (rt_exec & EXEC_COOLANT_FLOOD_OVR_TOGGLE) {
            if (coolant_state & COOLANT_FLOOD_ENABLE) { bit_false(coolant_state,COOLANT_FLOOD_ENABLE); }
            else { coolant_state |= COOLANT_FLOOD_ENABLE; }
          }
        #else
          if (coolant_state & COOLANT_FLOOD_ENABLE) { bit_false(coolant_state,COOLANT_FLOOD_ENABLE); }
          else { coolant_state |= COOLANT_FLOOD_ENABLE; }
        #endif
        coolant_set_state(coolant_state); // Report counter set in coolant_set_state().
        gc_state.modal.coolant = coolant_state;
      }
    }
  }

  #ifdef DEBUG
    if (sys_rt_exec_debug) {
      report_realtime_debug();
      sys_rt_exec_debug = 0;
    }
  #endif

  // Reload step segment buffer
  if (sys.state & (STATE_CYCLE | STATE_HOLD | STATE_SAFETY_DOOR | STATE_HOMING | STATE_SLEEP| STATE_JOG)) {
    st_prep_buffer();
  }

}


// Handles Grbl system suspend procedures, such as feed hold, safety door, and parking motion.
// The system will enter this loop, create local variables for suspend tasks, and return to
// whatever function that invoked the suspend, such that Grbl resumes normal operation.
// This function is written in a way to promote custom parking motions. Simply use this as a
// template
static void protocol_exec_rt_suspend()
{
  #ifdef PARKING_ENABLE
    // Declare and initialize parking local variables
    float restore_target[N_AXIS];
    float parking_target[N_AXIS];
    float retract_waypoint = PARKING_PULLOUT_INCREMENT;
    plan_line_data_t plan_data;
    plan_line_data_t *pl_data = &plan_data;
    memset(pl_data,0,sizeof(plan_line_data_t));
    pl_data->condition = (PL_COND_FLAG_SYSTEM_MOTION|PL_COND_FLAG_NO_FEED_OVERRIDE);
    #ifdef USE_LINE_NUMBERS
      pl_data->line_number = PARKING_MOTION_LINE_NUMBER;
    #endif
  #endif

  plan_block_t *block = plan_get_current_block();
  uint8_t restore_condition;
  #ifdef VARIABLE_SPINDLE
    float restore_spindle_speed;
    if (block == NULL) {
      restore_condition = (gc_state.modal.spindle | gc_state.modal.coolant);
      restore_spindle_speed = gc_state.spindle_speed;
    } else {
      restore_condition = (block->condition & PL_COND_SPINDLE_MASK) | coolant_get_state();
      restore_spindle_speed = block->spindle_speed;
    }
    #ifdef DISABLE_LASER_DURING_HOLD
      if (bit_istrue(settings.flags,BITFLAG_LASER_MODE)) { 
        system_set_exec_accessory_override_flag(EXEC_SPINDLE_OVR_STOP);
      }
    #endif
  #else
    if (block == NULL) { restore_condition = (gc_state.modal.spindle | gc_state.modal.coolant); }
    else { restore_condition = (block->condition & PL_COND_SPINDLE_MASK) | coolant_get_state(); }
  #endif

  while (sys.suspend) {

    if (sys.abort) { return; }

    // Block until initial hold is complete and the machine has stopped motion.
    if (sys.suspend & SUSPEND_HOLD_COMPLETE) {

      // Parking manager. Handles de/re-energizing, switch state checks, and parking motions for 
      // the safety door and sleep states.
      if (sys.state & (STATE_SAFETY_DOOR | STATE_SLEEP)) {
      
        // Handles retraction motions and de-energizing.
        if (bit_isfalse(sys.suspend,SUSPEND_RETRACT_COMPLETE)) {

          // Ensure any prior spindle stop override is disabled at start of safety door routine.
          sys.spindle_stop_ovr = SPINDLE_STOP_OVR_DISABLED;

          #ifndef PARKING_ENABLE

            spindle_set_state(SPINDLE_DISABLE,0.0); // De-energize
            coolant_set_state(COOLANT_DISABLE);     // De-energize

          #else
					
            // Get current position and store restore location and spindle retract waypoint.
            system_convert_array_steps_to_mpos(parking_target,sys_position);
            if (bit_isfalse(sys.suspend,SUSPEND_RESTART_RETRACT)) {
              memcpy(restore_target,parking_target,sizeof(parking_target));
              retract_waypoint += restore_target[PARKING_AXIS];
              retract_waypoint = min(retract_waypoint,PARKING_TARGET);
            }

            // Execute slow pull-out parking retract motion. Parking requires homing enabled, the
            // current location not exceeding the parking target location, and laser mode disabled.
            // NOTE: State is will remain DOOR, until the de-energizing and retract is complete.
            #ifdef ENABLE_PARKING_OVERRIDE_CONTROL
            if ((bit_istrue(settings.flags,BITFLAG_HOMING_ENABLE)) &&
                            (parking_target[PARKING_AXIS] < PARKING_TARGET) &&
                            bit_isfalse(settings.flags,BITFLAG_LASER_MODE) &&
                            (sys.override_ctrl == OVERRIDE_PARKING_MOTION)) {
            #else
            if ((bit_istrue(settings.flags,BITFLAG_HOMING_ENABLE)) &&
                            (parking_target[PARKING_AXIS] < PARKING_TARGET) &&
                            bit_isfalse(settings.flags,BITFLAG_LASER_MODE)) {
            #endif
              // Retract spindle by pullout distance. Ensure retraction motion moves away from
              // the workpiece and waypoint motion doesn't exceed the parking target location.
              if (parking_target[PARKING_AXIS] < retract_waypoint) {
                parking_target[PARKING_AXIS] = retract_waypoint;
                pl_data->feed_rate = PARKING_PULLOUT_RATE;
                pl_data->condition |= (restore_condition & PL_COND_ACCESSORY_MASK); // Retain accessory state
                pl_data->spindle_speed = restore_spindle_speed;
                mc_parking_motion(parking_target, pl_data);
              }

              // NOTE: Clear accessory state after retract and after an aborted restore motion.
              pl_data->condition = (PL_COND_FLAG_SYSTEM_MOTION|PL_COND_FLAG_NO_FEED_OVERRIDE);
              pl_data->spindle_speed = 0.0;
              spindle_set_state(SPINDLE_DISABLE,0.0); // De-energize
              coolant_set_state(COOLANT_DISABLE); // De-energize

              // Execute fast parking retract motion to parking target location.
              if (parking_target[PARKING_AXIS] < PARKING_TARGET) {
                parking_target[PARKING_AXIS] = PARKING_TARGET;
                pl_data->feed_rate = PARKING_RATE;
                mc_parking_motion(parking_target, pl_data);
              }

            } else {

              // Parking motion not possible. Just disable the spindle and coolant.
              // NOTE: Laser mode does not start a parking motion to ensure the laser stops immediately.
              spindle_set_state(SPINDLE_DISABLE,0.0); // De-energize
              coolant_set_state(COOLANT_DISABLE);     // De-energize

            }

          #endif

          sys.suspend &= ~(SUSPEND_RESTART_RETRACT);
          sys.suspend |= SUSPEND_RETRACT_COMPLETE;

        } else {

          
          if (sys.state == STATE_SLEEP) {
            report_feedback_message(MESSAGE_SLEEP_MODE);
            // Spindle and coolant should already be stopped, but do it again just to be sure.
            spindle_set_state(SPINDLE_DISABLE,0.0); // De-energize
            coolant_set_state(COOLANT_DISABLE); // De-energize
            st_go_idle(); // Disable steppers
            while (!(sys.abort)) { protocol_exec_rt_system(); } // Do nothing until reset.
            return; // Abort received. Return to re-initialize.
          }    
          
          // Allows resuming from parking/safety door. Actively checks if safety door is closed and ready to resume.
          if (sys.state == STATE_SAFETY_DOOR) {
            if (!(system_check_safety_door_ajar())) {
              sys.suspend &= ~(SUSPEND_SAFETY_DOOR_AJAR); // Reset door ajar flag to denote ready to resume.
            }
          }

          // Handles parking restore and safety door resume.
          if (sys.suspend & SUSPEND_INITIATE_RESTORE) {

            #ifdef PARKING_ENABLE
              // Execute fast restore motion to the pull-out position. Parking requires homing enabled.
              // NOTE: State is will remain DOOR, until the de-energizing and retract is complete.
              #ifdef ENABLE_PARKING_OVERRIDE_CONTROL
              if (((settings.flags & (BITFLAG_HOMING_ENABLE|BITFLAG_LASER_MODE)) == BITFLAG_HOMING_ENABLE) &&
                   (sys.override_ctrl == OVERRIDE_PARKING_MOTION)) {
              #else
              if ((settings.flags & (BITFLAG_HOMING_ENABLE|BITFLAG_LASER_MODE)) == BITFLAG_HOMING_ENABLE) {
              #endif
                // Check to ensure the motion doesn't move below pull-out position.
                if (parking_target[PARKING_AXIS] <= PARKING_TARGET) {
                  parking_target[PARKING_AXIS] = retract_waypoint;
                  pl_data->feed_rate = PARKING_RATE;
                  mc_parking_motion(parking_target, pl_data);
                }
              }
            #endif

            // Delayed Tasks: Restart spindle and coolant, delay to power-up, then resume cycle.
            if (gc_state.modal.spindle != SPINDLE_DISABLE) {
              // Block if safety door re-opened during prior restore actions.
              if (bit_isfalse(sys.suspend,SUSPEND_RESTART_RETRACT)) {
                if (bit_istrue(settings.flags,BITFLAG_LASER_MODE)) {
                  // When in laser mode, ignore spindle spin-up delay. Set to turn on laser when cycle starts.
                  bit_true(sys.step_control, STEP_CONTROL_UPDATE_SPINDLE_PWM);
                } else {
                  spindle_set_state((restore_condition & (PL_COND_FLAG_SPINDLE_CW | PL_COND_FLAG_SPINDLE_CCW)), restore_spindle_speed);
                  // Port: until at speed ($52) or $54, instead of SAFETY_DOOR_SPINDLE_DELAY.
                  spindle_door_restore_wait();
                  if (sys.abort || (sys.state == STATE_ALARM)) { return; }   // ALARM:14: no coolant, no resume
                }
              }
            }
            if (gc_state.modal.coolant != COOLANT_DISABLE) {
              // Block if safety door re-opened during prior restore actions.
              if (bit_isfalse(sys.suspend,SUSPEND_RESTART_RETRACT)) {
                // NOTE: Laser mode will honor this delay. An exhaust system is often controlled by this pin.
                coolant_set_state((restore_condition & (PL_COND_FLAG_COOLANT_FLOOD | PL_COND_FLAG_COOLANT_MIST)));
                delay_sec(SAFETY_DOOR_COOLANT_DELAY, DELAY_MODE_SYS_SUSPEND);
              }
            }

            #ifdef PARKING_ENABLE
              // Execute slow plunge motion from pull-out position to resume position.
              #ifdef ENABLE_PARKING_OVERRIDE_CONTROL
              if (((settings.flags & (BITFLAG_HOMING_ENABLE|BITFLAG_LASER_MODE)) == BITFLAG_HOMING_ENABLE) &&
                   (sys.override_ctrl == OVERRIDE_PARKING_MOTION)) {
              #else
              if ((settings.flags & (BITFLAG_HOMING_ENABLE|BITFLAG_LASER_MODE)) == BITFLAG_HOMING_ENABLE) {
              #endif
                // Block if safety door re-opened during prior restore actions.
                if (bit_isfalse(sys.suspend,SUSPEND_RESTART_RETRACT)) {
                  // Regardless if the retract parking motion was a valid/safe motion or not, the
                  // restore parking motion should logically be valid, either by returning to the
                  // original position through valid machine space or by not moving at all.
                  pl_data->feed_rate = PARKING_PULLOUT_RATE;
									pl_data->condition |= (restore_condition & PL_COND_ACCESSORY_MASK); // Restore accessory state
									pl_data->spindle_speed = restore_spindle_speed;
                  mc_parking_motion(restore_target, pl_data);
                }
              }
            #endif

            if (bit_isfalse(sys.suspend,SUSPEND_RESTART_RETRACT)) {
              sys.suspend |= SUSPEND_RESTORE_COMPLETE;
              system_set_exec_state_flag(EXEC_CYCLE_START); // Set to resume program.
            }
          }

        }


      } else {

        // Feed hold manager. Controls spindle stop override states.
        // NOTE: Hold ensured as completed by condition check at the beginning of suspend routine.
        if (sys.spindle_stop_ovr) {
          // Handles beginning of spindle stop
          if (sys.spindle_stop_ovr & SPINDLE_STOP_OVR_INITIATE) {
            if (gc_state.modal.spindle != SPINDLE_DISABLE) {
              spindle_set_state(SPINDLE_DISABLE,0.0); // De-energize
              sys.spindle_stop_ovr = SPINDLE_STOP_OVR_ENABLED; // Set stop override state to enabled, if de-energized.
            } else {
              sys.spindle_stop_ovr = SPINDLE_STOP_OVR_DISABLED; // Clear stop override state
            }
          // Handles restoring of spindle state
          } else if (sys.spindle_stop_ovr & (SPINDLE_STOP_OVR_RESTORE | SPINDLE_STOP_OVR_RESTORE_CYCLE)) {
            if (gc_state.modal.spindle != SPINDLE_DISABLE) {
              report_feedback_message(MESSAGE_SPINDLE_RESTORE);
              if (bit_istrue(settings.flags,BITFLAG_LASER_MODE)) {
                // When in laser mode, ignore spindle spin-up delay. Set to turn on laser when cycle starts.
                bit_true(sys.step_control, STEP_CONTROL_UPDATE_SPINDLE_PWM);
              } else {
                spindle_set_state((restore_condition & (PL_COND_FLAG_SPINDLE_CW | PL_COND_FLAG_SPINDLE_CCW)), restore_spindle_speed);
              }
            }
            if (sys.spindle_stop_ovr & SPINDLE_STOP_OVR_RESTORE_CYCLE) {
              system_set_exec_state_flag(EXEC_CYCLE_START);  // Set to resume program.
            }
            sys.spindle_stop_ovr = SPINDLE_STOP_OVR_DISABLED; // Clear stop override state
          }
        } else {
          // Handles spindle state during hold. NOTE: Spindle speed overrides may be altered during hold state.
          // NOTE: STEP_CONTROL_UPDATE_SPINDLE_PWM is automatically reset upon resume in step generator.
          if (bit_istrue(sys.step_control, STEP_CONTROL_UPDATE_SPINDLE_PWM)) {
            spindle_set_state((restore_condition & (PL_COND_FLAG_SPINDLE_CW | PL_COND_FLAG_SPINDLE_CCW)), restore_spindle_speed);
            bit_false(sys.step_control, STEP_CONTROL_UPDATE_SPINDLE_PWM);
          }
        }

      }
    }

    protocol_exec_rt_system();

  }
}
