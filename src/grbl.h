/*
  grbl.h - main Grbl include file
  Part of Grbl

  Copyright (c) 2015-2016 Sungeun K. Jeon for Gnea Research LLC
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
*/

#ifndef grbl_h
#define grbl_h

// Grbl versioning system. The version string is deliberately left at 1.1 so
// that existing senders (UGS, bCNC, Candle, ioSender, ...) recognise the
// firmware; the port identity is reported separately in the $I build info.
#define GRBL_VERSION "1.1h"
#define GRBL_VERSION_BUILD "20261012"
#define GRBL_PLATFORM "GIGA-R1/STM32H747"

// Define standard libraries used by Grbl.
#include <math.h>
#include <inttypes.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

// Platform shims (PROGMEM/PSTR, interrupt masking, delays) and the STM32H747 HAL.
#include "platform.h"

// Define the Grbl system include files. NOTE: Do not alter organization.
#include "config.h"
#include "nuts_bolts.h"
#include "io.h"
#include "settings.h"
#include "system.h"
#include "defaults.h"
#include "cpu_map.h"
#include "kinematics.h"
#include "planner.h"
#include "coolant_control.h"
#include "eeprom.h"
#include "gcode.h"
#include "limits.h"
#include "motion_control.h"
#include "planner.h"
#include "print.h"
#include "probe.h"
#include "protocol.h"
#include "report.h"
#include "serial.h"
#include "spindle_control.h"
#include "stepper.h"
#include "jog.h"
#include "outputs.h"
#include "estop.h"
#include "drvpwr.h"
#include "mpg.h"
#include "job.h"
#include "display.h"

// ---------------------------------------------------------------------------
// COMPILE-TIME ERROR CHECKING OF DEFINE VALUES:

#ifndef HOMING_CYCLE_0
  #error "Required HOMING_CYCLE_0 not defined."
#endif

#if defined(USE_SPINDLE_DIR_AS_ENABLE_PIN) && !defined(VARIABLE_SPINDLE)
  #error "USE_SPINDLE_DIR_AS_ENABLE_PIN may only be used with VARIABLE_SPINDLE enabled"
#endif

#if !defined(USE_SPINDLE_DIR_AS_ENABLE_PIN) && defined(SPINDLE_ENABLE_OFF_WITH_ZERO_SPEED)
  #error "SPINDLE_ENABLE_OFF_WITH_ZERO_SPEED may only be used with USE_SPINDLE_DIR_AS_ENABLE_PIN enabled"
#endif

#if defined(PARKING_ENABLE)
  #if defined(HOMING_FORCE_SET_ORIGIN)
    #error "HOMING_FORCE_SET_ORIGIN is not supported with PARKING_ENABLE at this time."
  #endif
#endif

#if defined(ENABLE_PARKING_OVERRIDE_CONTROL)
  #if !defined(PARKING_ENABLE)
    #error "ENABLE_PARKING_OVERRIDE_CONTROL must be enabled with PARKING_ENABLE."
  #endif
#endif

#if defined(SPINDLE_PWM_MIN_VALUE)
  #if !(SPINDLE_PWM_MIN_VALUE > 0)
    #error "SPINDLE_PWM_MIN_VALUE must be greater than zero."
  #endif
#endif

#if (REPORT_WCO_REFRESH_BUSY_COUNT < REPORT_WCO_REFRESH_IDLE_COUNT)
  #error "WCO busy refresh is less than idle refresh."
#endif
#if (REPORT_OVR_REFRESH_BUSY_COUNT < REPORT_OVR_REFRESH_IDLE_COUNT)
  #error "Override busy refresh is less than idle refresh."
#endif
#if (REPORT_WCO_REFRESH_IDLE_COUNT < 2)
  #error "WCO refresh must be greater than one."
#endif
#if (REPORT_OVR_REFRESH_IDLE_COUNT < 1)
  #error "Override refresh must be greater than zero."
#endif

// The compile-time dual-axis feature of AVR Grbl 1.1h is replaced on this port
// by the run-time $42 setting: the fourth motor can be an axis of its own or a
// squared second motor of X or Y. See kinematics.h and limits.c.
#if defined(ENABLE_DUAL_AXIS)
  #error "ENABLE_DUAL_AXIS is replaced by the $42 setting on the GIGA port."
#endif
#if !defined(CORE_CM7)
  // Built for the M4 core: the Grbl sources are compiled but nothing calls
  // them, and the linker drops them. See Grbl_Giga_R1.ino.
#endif

// ---------------------------------------------------------------------------

#endif
