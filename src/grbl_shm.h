/*
  grbl_shm.h - the block of shared SRAM between Grbl (M7) and the display (M4)

  Part of Grbl-Giga, a port of Grbl 1.1 to the Arduino GIGA R1 WiFi.

  Grbl is free software: you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation, either version 3 of the License, or (at your option) any later
  version. Distributed WITHOUT ANY WARRANTY; see the GNU GPL for details.

  -------------------------------------------------------------------------------

  Both cores are built from the same sketch, so both include this header and
  agree on the layout by construction. The block lives in SRAM4 (the D3 domain,
  0x38000000), which both cores reach on their own buses. The core's linker
  scripts put nothing there except OpenAMP (from 0x38000400, only with the RPC
  library, which this sketch does not use) and the PDM buffer at 0x3800FC00,
  so the 4 kB at 0x3800C000 are free.

  The M7 maps SRAM4 non-cacheable through the MPU; the M4 has no data cache.
  So no cache maintenance is needed, only ordering, which a sequence counter
  provides: the M7 makes `seq` odd, writes the status, makes it even again.
  The M4 copies the status and keeps the copy only if `seq` was even and the
  same before and after. A torn copy is simply retried on the next frame.

  The M4 owns the second half: a heartbeat the M7 reports in $I, a small ring
  of real-time commands from the touch screen, which the M7 feeds into the
  same path as characters from the PC, and a one-slot mailbox for the touch
  actions that are G-code or $-commands underneath (jog, home, unlock, zero).
  The M7 runs those itself, between two lines from the PC, and answers in
  req_done/req_status; see display.c.
*/

#ifndef grbl_shm_h
#define grbl_shm_h

#include <stdint.h>

#define GRBL_SHM_ADDR       0x3800C000UL
#define GRBL_SHM_MAGIC      0x47524742UL   // "GRGB"
#define GRBL_SHM_VERSION    6
#define GRBL_SHM_M4_MAGIC   0x4D344433UL   // "M4D3" - changes with the layout of the M4 half

// Machine state as the display shows it (a superset of Grbl's report states).
#define GSHM_STATE_IDLE     0
#define GSHM_STATE_RUN      1
#define GSHM_STATE_HOLD0    2   // hold complete
#define GSHM_STATE_HOLD1    3   // decelerating into hold
#define GSHM_STATE_JOG      4
#define GSHM_STATE_HOME     5
#define GSHM_STATE_ALARM    6
#define GSHM_STATE_CHECK    7
#define GSHM_STATE_DOOR     8   // + door sub-state in door_sub
#define GSHM_STATE_SLEEP    9

// Inputs: `inputs` has bit n-1 = channel INn active (after $79),
// GSHM_IN_ESTOP and GSHM_IN_DRVPWR; `in_fn` bit f = input function f active
// (io.h order, $60+f).
#define GSHM_N_IN           20
#define GSHM_IN_ESTOP       (1UL << 20)  // STOP input signals STOP
#define GSHM_IN_DRVPWR      (1UL << 21)  // driver supply present (D53, after $79)

// Driver power (drv_power), as drvpwr_state().
#define GSHM_DRV_NOT_WATCHED 0   // $94=0
#define GSHM_DRV_OFF         1   // no supply
#define GSHM_DRV_RETURNING   2   // back, $95 not over yet
#define GSHM_DRV_ON          3

// Handwheel (mpg_state), as mpg.h.
#define GSHM_MPG_DISABLED    0   // $96=0
#define GSHM_MPG_NO_AXIS     1   // selector at OFF
#define GSHM_MPG_READY       2
#define GSHM_MPG_BLOCKED     3   // an axis is selected, the machine cannot jog now

// Outputs: `outputs` has bit n-1 = channel OUTn on (logical, before $93), plus:
#define GSHM_N_OUT          12
#define GSHM_OUT_ENABLE     (1u << 12)  // drivers enabled
#define GSHM_OUT_BRAKE      (1u << 13)  // Z brake released (BRAKE output on)
// `out_fn` bit f = output function f on (io.h order, $80+f).

// Input functions as the display names them, in io.h / $60 order.
#define GSHM_IN_FN_NAMES  { "X-", "X+", "Y-", "Y+", "Z-", "Z+", "A-", "A+", \
                            "ALM X", "ALM Y", "ALM Z", "ALM A", "PROBE",   \
                            "RESET", "HOLD", "START", "DOOR", "AT SPEED", "VFD FLT" }
#define GSHM_N_IN_FN        19
#define GSHM_OUT_FN_NAMES { "FWD", "REV", "FLOOD", "MIST", "AUX0", "AUX1", "AUX2", \
                            "AUX3", "AUX4", "AUX5", "COLLET", "SRV RST", "VFD RST" }
#define GSHM_N_OUT_FN       13

// Spindle status (spindle_status), as spindle_control.h.
#define GSHM_SPINDLE_NONE     0
#define GSHM_SPINDLE_SPINUP   1
#define GSHM_SPINDLE_AT_SPEED 2
#define GSHM_SPINDLE_LOST     3
#define GSHM_SPINDLE_FAULT    4

// flags
#define GSHM_F_SPINDLE_STOP (1u << 0)   // spindle stop override active (in a hold)
#define GSHM_F_SOFT_LIMITS  (1u << 1)   // $20=1
#define GSHM_F_HOMING       (1u << 2)   // $22=1
#define GSHM_F_TOUCH_JOG    (1u << 3)   // a continuous jog from the touch screen runs
#define GSHM_F_ESTOP        (1u << 4)   // central STOP latched (alarm until a reset)
#define GSHM_F_ESTOP_IN     (1u << 5)   // STOP input active now ($57=1)
#define GSHM_F_TOOL_CHANGE  (1u << 6)   // M6 pause in progress
#define GSHM_F_COLLET       (1u << 7)   // collet open

// Real-time bytes the touch screen sends that are not Grbl's own: they switch
// an aux output (M64/M65 P0-P5) and never reach the serial path.
#define GSHM_CMD_AUX_TOGGLE 0xB0        // + n, n = 0-5
#define GSHM_N_AUX          6

// Touch actions that are commands underneath (req_code).
#define GSHM_REQ_UNLOCK     1   // $X
#define GSHM_REQ_HOME       2   // $H
#define GSHM_REQ_JOG        3   // $J= along req_axis by req_dir * req_value (0 = until released)
#define GSHM_REQ_ZERO       4   // G10 L20 P0 <axis>0
#define GSHM_REQ_ZERO_ALL   5   // G10 L20 P0 X0 Y0 Z0 (A0)
#define GSHM_REQ_SETTING    6   // $<req_axis>=<req_value>
#define GSHM_REQ_COLLET     7   // collet open (req_value 1) or closed (0)
#define GSHM_REQ_PULSE      8   // reset pulse on output function req_axis (11 servo, 12 VFD)
#define GSHM_REQ_JOB_START  9   // run file req_axis of the USB listing (req_value = its list_seq)
#define GSHM_REQ_JOB_STOP   10  // STOP the program from the USB stick
#define GSHM_REQ_USB_FORMAT 11  // format the USB stick

// The settings on the $$ page, in the order of set_val[]: the global ones in
// $$ order (SETTINGS_GLOBAL_NUMBERS in settings.h), then the axis settings.
#define GSHM_N_SETTINGS     92
#define GSHM_SETTING_NUMBERS { 0, 1, 2, 3, 4, 10, 11, 12, 13, 20, 21, 22, 23, 24, 25, 26, 27, \
                               30, 31, 32, 40, 41, 42, 43, 46,                            \
                               48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59,            \
                               60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72,        \
                               73, 74, 75, 76, 77, 78, 79,                                \
                               80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93,    \
                               94, 95, 96, 97, 98,                                        \
                               100, 101, 102, 103, 110, 111, 112, 113,                    \
                               120, 121, 122, 123, 130, 131, 132, 133 }

typedef struct {
  uint32_t magic;           // GRBL_SHM_MAGIC once the M7 has initialised the block
  uint32_t version;         // GRBL_SHM_VERSION
  volatile uint32_t seq;    // odd while the M7 is writing the status below
  uint32_t m7_ms;           // M7 uptime when the status was taken

  // ---- status, written by the M7 ----
  uint8_t  state;           // GSHM_STATE_*
  uint8_t  door_sub;        // Door:n sub-state
  uint8_t  alarm;           // alarm code being shown (0 = none)
  uint8_t  last_error;      // last error:n code (0 = none)
  uint32_t error_ms;        // M7 uptime of that error, so the M4 can age it out

  float    mpos[4];         // machine position, mm (deg for a rotary A)
  float    wco[4];          // work coordinate offset; wpos = mpos - wco

  float    feed;            // current feed rate, mm/min
  float    feed_prog;       // programmed F, mm/min
  float    spindle_rpm;     // spindle speed being output
  float    spindle_prog;    // programmed S

  uint8_t  ovr_feed;        // percent
  uint8_t  ovr_rapid;
  uint8_t  ovr_spindle;
  uint8_t  motion;          // G0=0 G1=1 G2=2 G3=3 G38.x=140.., G80=80

  uint8_t  coord_select;    // 0-5 = G54-G59
  uint8_t  units_inch;      // 1 = G20
  uint8_t  distance_inc;    // 1 = G91
  uint8_t  plane;           // 0 = G17, 1 = G18, 2 = G19

  uint8_t  spindle_mode;    // 0 = M5, 1 = M3, 2 = M4
  uint8_t  coolant;         // bit 0 = M8 flood, bit 1 = M7 mist
  uint8_t  tool;
  uint8_t  report_inches;   // $13

  uint8_t  a_mode;          // $42
  uint8_t  kinematics;      // $40
  uint8_t  usb_connected;   // a terminal has the COM port open
  uint8_t  planner_free;    // free planner blocks

  uint8_t  planner_size;
  uint8_t  flags;           // GSHM_F_*
  uint16_t rx_free;         // free bytes in the serial receive buffer

  uint32_t inputs;          // bit n-1 = INn, GSHM_IN_ESTOP, GSHM_IN_DRVPWR
  uint32_t in_fn;           // input functions active
  uint16_t outputs;         // bit n-1 = OUTn, GSHM_OUT_*
  uint16_t out_fn;          // output functions on
  uint16_t pwm_permille;    // spindle PWM duty, 0-1000
  uint8_t  spindle_status;  // GSHM_SPINDLE_*
  uint8_t  limit_trigger;   // switches behind the last ALARM:1 (bit 2*axis -, 2*axis+1 +)
  uint32_t estop_trips;     // central STOP trips since power-up

  int32_t  line_number;     // line being executed (N word), 0 if none
  uint32_t lines_ok;        // count of lines acknowledged with ok
  uint32_t step_rate;       // spare, for diagnostics

  float    jog_rate[4];     // $110-$113, for the jog speed shown on the screen
  float    set_val[GSHM_N_SETTINGS];   // value of every setting, as $$ prints it

  uint8_t  drv_power;       // GSHM_DRV_*
  uint8_t  mpg_state;       // GSHM_MPG_*
  uint8_t  mpg_axis;        // 0 none, 1-4 = X Y Z A
  uint8_t  mpg_mult;        // 1, 10, 100
  uint8_t  settings_version; // SETTINGS_VERSION, for the splash screen
  uint8_t  pad[23];         // room for additions without moving the M4 half

  // ---- written by the M4 ----
  volatile uint32_t m4_magic;   // GRBL_SHM_M4_MAGIC once the display runs
  volatile uint32_t m4_frames;  // incremented for every redraw pass
  volatile uint32_t m4_ms;      // M4 uptime
  volatile uint32_t m4_status;  // init progress / error code of the display
  volatile uint32_t m4_fb;      // framebuffer address (480x800 RGB565, portrait), for $SCREEN
  volatile uint8_t  cmd_head;   // M4 writes, M7 reads
  volatile uint8_t  cmd_tail;   // M7 writes, M4 reads
  volatile uint8_t  cmd[30];    // real-time command bytes from the touch screen

  // Touch action mailbox: the M4 fills req_* and then increments req_seq; the
  // M7 runs it when it can, sets req_status and then req_done = req_seq.
  volatile uint8_t  req_seq;
  volatile uint8_t  req_done;   // M7 writes
  volatile uint8_t  req_status; // M7 writes: Grbl status code, 0 = ok
  volatile uint8_t  req_code;   // GSHM_REQ_*
  volatile uint8_t  req_axis;   // 0-3
  volatile int8_t   req_dir;    // +1 / -1
  volatile uint8_t  req_speed;  // jog feed, percent of the axis's $11x
  volatile uint8_t  jog_hold;   // 1 while a finger holds a continuous-jog button
  volatile float    req_value;  // jog distance, mm (deg for a rotary A); 0 = continuous
  volatile uint32_t req_ms;     // m4_ms when the request was made; the M7 drops stale ones
  volatile uint32_t jog_alive;  // counted up by every touch poll while jog_hold is set

  // Touch controller and RGB LED of the shield, for $I.
  volatile uint32_t rgb_status;    // 0 not started, 1 IS31FL3197 answered, 0xE1 no answer
  volatile uint32_t touch_status;  // 0 not started, 1 GT911 found, 0xE0.. error
  volatile uint32_t touch_count;   // presses seen
  volatile uint16_t touch_x, touch_y;  // where the last press began (landscape px)
  volatile uint16_t touch_res_x, touch_res_y;  // resolution the GT911 is configured for

  // Simulated touch, for automated tests ($TOUCH=x,y,ms). M7 writes.
  volatile uint32_t sim_seq;    // incremented for each new simulated press
  volatile uint16_t sim_x, sim_y;
  volatile uint32_t sim_ms;     // how long the finger stays down
  volatile uint32_t sim_delay;  // ms from now (and after earlier presses) until it goes down
  volatile uint32_t sim_ack;    // M4 writes: the last sim_seq it has taken

  // $SPLASH=ms: the M4 shows the splash screen again for at least splash_ms
  // (for screenshots and tests). M7 writes.
  volatile uint32_t splash_seq;
  volatile uint32_t splash_ms;
} grbl_shm_t;

#define GRBL_SHM  ((grbl_shm_t *)GRBL_SHM_ADDR)

#endif
