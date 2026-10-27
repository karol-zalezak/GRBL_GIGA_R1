/*
  grbl_main.c - An embedded CNC Controller with rs274/ngc (g-code) support
  Part of Grbl

  Copyright (c) 2011-2016 Sungeun K. Jeon for Gnea Research LLC
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

  This is upstream Grbl's main.c split in two. The Arduino core owns main(), so
  what was the one-time set-up becomes grbl_init() and what was the body of the
  outer for(;;) becomes grbl_loop(). The sketch calls them from setup() and
  loop(); the semantics are unchanged, including the way a system abort unwinds
  out of protocol_main_loop() and re-initialises everything on the next pass.

  The system globals stay here, in a C translation unit, so they keep C linkage
  and match the extern declarations the rest of the core makes.
*/

#include "grbl.h"


// Declare system global variable structure
system_t sys;
int32_t sys_position[N_AXIS];      // Real-time machine (aka home) position vector in steps.
int32_t sys_probe_position[N_AXIS]; // Last probe position in machine coordinates and steps.
volatile uint8_t sys_probe_state;   // Probing state value.  Used to coordinate the probing cycle with stepper ISR.
volatile uint8_t sys_rt_exec_state;   // Global realtime executor bitflag variable for state management. See EXEC bitmasks.
volatile uint8_t sys_rt_exec_alarm;   // Global realtime executor bitflag variable for setting various alarms.
volatile uint8_t sys_rt_exec_motion_override; // Global realtime executor bitflag variable for motion-based overrides.
volatile uint8_t sys_rt_exec_accessory_override; // Global realtime executor bitflag variable for spindle/coolant overrides.
#ifdef DEBUG
  volatile uint8_t sys_rt_exec_debug;
#endif


// Build with -DDEBUG_BOOT to have each start-up step reported on the USB port
// (Grbl waits up to 15 s for a terminal first). For bring-up only.
#ifdef DEBUG_BOOT
  // The last stage reached is also kept in a word of SRAM4, which a reset
  // does not clear, so a crash can be located on the next start.
  #define BOOT_MARK (*(volatile uint32_t *)0x3800BFF0UL)
  static uint32_t boot_n = 0;
  static void boot_stage(const char *s)
  {
    boot_n++;
    BOOT_MARK = 0xB0070000UL | boot_n;
    printString("[boot] "); print_uint32_base10(boot_n); serial_write(' ');
    printString(s); printString("\r\n");
    hal_serial_flush();
  }
  #define BOOT_STAGE(s) boot_stage(s)
#else
  #define BOOT_STAGE(s) do { } while (0)
#endif


void grbl_init(void)
{
  #ifdef DEBUG_BOOT
    uint32_t previous = BOOT_MARK;
    uint32_t t0 = hal_millis();
    while (!hal_serial_connected() && (uint32_t)(hal_millis() - t0) < 15000) { }
  #endif
  hal_init();      // Clocks, cycle counter, interrupt priorities, LEDs
  #ifdef DEBUG_BOOT
    grbl_serial_init();
    hal_delay_ms(300);
    printString("[boot] previous run reached ");
    print_uint32_base10(((previous >> 16) == 0xB007UL) ? (previous & 0xFFFF) : 0);
    printString("\r\n");
  #endif
  grbl_serial_init(); // Set up the PC link (renamed: mbed has a serial_init of its own)
  BOOT_STAGE("hal");
  display_init();  // Shared memory for the M4; the M4 itself starts later
  BOOT_STAGE("display");

  // Bring up the flash EEPROM emulation before anything reads a setting.
  // A false return just means nothing has been stored yet; the shadow reads as
  // 0xFF and settings_init() writes the defaults, exactly as on a blank AVR.
  hal_nvs_init();
  BOOT_STAGE("nvs");

  settings_init(); // Load Grbl settings from the emulated EEPROM
  BOOT_STAGE("settings");
  io_init();       // Input and output channels IN1-IN20, OUT1-OUT14, STOP pin
  BOOT_STAGE("io");
  stepper_init();  // Configure stepper pins and interrupt timers
  BOOT_STAGE("stepper");
  system_init();   // Configure control input pins
  BOOT_STAGE("system");

  memset(sys_position,0,sizeof(sys_position)); // Clear machine position.

  // Initialize system state.
  #ifdef FORCE_INITIALIZATION_ALARM
    // Force Grbl into an ALARM state upon a power-cycle or hard reset.
    sys.state = STATE_ALARM;
  #else
    sys.state = STATE_IDLE;
  #endif

  // Check for power-up and set system alarm if homing is enabled to force homing cycle
  // by setting Grbl's alarm state. Alarm locks out all g-code commands, including the
  // startup scripts, but allows access to settings and internal commands. Only a homing
  // cycle '$H' or kill alarm locks '$X' will disable the alarm.
  // NOTE: The startup script will run after successful completion of the homing cycle, but
  // not after disabling the alarm locks. Prevents motion startup blocks from crashing into
  // things uncontrollably. Very bad.
  #ifdef HOMING_INIT_LOCK
    if (bit_istrue(settings.flags,BITFLAG_HOMING_ENABLE)) { sys.state = STATE_ALARM; }
  #endif
}


// One pass of Grbl's initialization loop. Called repeatedly from the sketch's
// loop(). protocol_main_loop() only returns on a system abort, at which point
// everything below is re-run to bring the machine back to a known state.
void grbl_loop(void)
{
  // Reset system variables.
  uint8_t prior_state = sys.state;
  memset(&sys, 0, sizeof(system_t)); // Clear system struct variable.
  sys.state = prior_state;
  sys.f_override = DEFAULT_FEED_OVERRIDE;  // Set to 100%
  sys.r_override = DEFAULT_RAPID_OVERRIDE; // Set to 100%
  sys.spindle_speed_ovr = DEFAULT_SPINDLE_SPEED_OVERRIDE; // Set to 100%
  memset(sys_probe_position,0,sizeof(sys_probe_position)); // Clear probe position.
  sys_probe_state = 0;
  sys_rt_exec_state = 0;
  sys_rt_exec_alarm = 0;
  sys_rt_exec_motion_override = 0;
  sys_rt_exec_accessory_override = 0;

  // Reset Grbl primary systems.
  serial_reset_read_buffer(); // Clear serial read buffer
  gc_init(); // Set g-code parser to default state
  BOOT_STAGE("gcode");
  io_rebuild();    // Channel tables from the settings (they may have been restored)
  outputs_init();  // All output functions and channels off
  BOOT_STAGE("outputs");
  spindle_init();
  BOOT_STAGE("spindle");
  coolant_init();
  limits_init();
  estop_init();    // After limits_init(): the scan it rides on runs from there
  drvpwr_init();   // A reset keeps its state; only a change of $94 restarts it
  mpg_init();      // Handwheel pins and TIM8, once (before the M4 starts)
  vfd_init();      // RS485 to the spindle VFD; the port only when $33-$36 changed
  panel_init();    // operator panel: buttons A4-A6, potentiometers A0/A1
  BOOT_STAGE("limits");
  probe_init();
  plan_reset(); // Clear block buffer and planner variables
  st_reset(); // Clear stepper subsystem variables.
  BOOT_STAGE("stepper reset");

  // Sync cleared gcode and planner positions to current system position.
  plan_sync_position();
  gc_sync_position();

  // Every pin Grbl uses is configured now, so the M4 can come up without
  // racing Grbl on the shared GPIO mode registers. Only the first call acts.
  display_start();
  BOOT_STAGE("m4 started");

  // Print welcome message. Indicates an initialization has occured at power-up or with a reset.
  report_init_message();

  // Start Grbl main loop. Processes program inputs and executes them.
  protocol_main_loop();
}
