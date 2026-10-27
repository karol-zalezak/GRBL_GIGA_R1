/*
  gui.cpp - the status display and touch panel, running on the M4 co-processor

  Part of Grbl-Giga, a port of Grbl 1.1 to the Arduino GIGA R1 WiFi.

  Grbl is free software: you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation, either version 3 of the License, or (at your option) any later
  version. Distributed WITHOUT ANY WARRANTY; see the GNU GPL for details.

  -------------------------------------------------------------------------------

  Reads the machine status the M7 publishes in shared memory (grbl_shm.h)
  25 times a second, redraws whatever changed, and turns touches into
  commands for Grbl:

    +--------+------------------+--------------+  +--------+
    | STATE  | G1 G54 G90 G21   | USB  Line 12 |  |  HOLD  |
    +--------+------------------+--------------+  +--------+
    |                                          |  | START  |
    |   page: DRO | JOG | OVR | I/O | $$       |  +--------+
    |                                          |  | RESET  |
    +------------------------------------------+  +--------+
    | message line: alarms, errors, hints      |  | UNLOCK |
    | [DRO] [JOG] [OVR] [I/O] [$$]             |  |        |
    +------------------------------------------+  +--------+

  HOLD, START, RESET and the overrides send Grbl's real-time bytes through a
  ring in shared memory, exactly as a sender would. Jog, home, unlock and
  zero are commands underneath; they go through a one-slot mailbox and the
  M7 runs them between two lines from the PC (display.c). Actions that move
  the machine or change offsets need the button held (a bar fills), so a
  brushing finger does nothing. A continuous jog runs only while the finger
  stays on the button: lifting it, sliding off it, or the M4 going quiet all
  stop it - the last one by the M7's own dead man's timer. The $$ page lists
  every setting with an explanation and edits it with a keypad, switches or
  choices; the M7 stores it as `$n=value`.

  The shield's RGB LED shows the machine state in the colour of the badge.

  Each widget remembers a signature of what it drew last and is skipped when
  nothing in it changed, so a still machine costs almost nothing.
*/

#if defined(CORE_CM4)

#include <Arduino.h>
#include <string.h>
#include <stddef.h>
#include <math.h>

#include "canvas.h"
#include "touch.h"
#include "usbstick.h"
#include "../grbl_job.h"
#include <strings.h>
#include "../grbl_shm.h"
#include "logo.h"

extern uint32_t cv_dma_errors;

#define REFRESH_MS      40
#define TOUCH_POLL_MS   10
#define STALE_MS        3000      // no fresh status for this long: show "waiting" (a flash erase on the M7 takes 1-2 s)
#define ERROR_SHOW_MS   10000
#define HINT_MS         2500
#define RELEASE_MS      250       // no report for this long while pressed = finger lifted
#define REPEAT_DELAY_MS 500       // override +/- buttons repeat when held
#define REPEAT_MS       150

// Grbl's real-time bytes (config.h on the M7 side).
#define RT_RESET        0x18
#define RT_CYCLE_START  '~'
#define RT_FEED_HOLD    '!'
#define RT_JOG_CANCEL   0x85
#define RT_FEED_100     0x90
#define RT_FEED_P10     0x91
#define RT_FEED_M10     0x92
#define RT_FEED_P1      0x93
#define RT_FEED_M1      0x94
#define RT_RAPID_100    0x95
#define RT_RAPID_50     0x96
#define RT_RAPID_25     0x97
#define RT_SPIN_100     0x99
#define RT_SPIN_P10     0x9A
#define RT_SPIN_M10     0x9B
#define RT_SPIN_STOP    0x9E
#define RT_FLOOD        0xA0
#define RT_MIST         0xA1

// Layout (landscape pixels).
#define LX        4         // left area: top bar, page, message, tabs
#define LW        686
#define RX        696       // right column: HOLD START RESET UNLOCK
#define RW        100
#define TOP_Y     4
#define TOP_H     52
#define PAGE_Y    62
#define PAGE_H    312
#define MSG_Y     378
#define MSG_H     28
#define TAB_Y     412
#define TAB_H     64
#define IO_BTN_Y  282       // I/O page button row
#define IO_BTN_H  90

// Colours
#define C_BG        RGB(10, 12, 16)
#define C_PANEL     RGB(24, 28, 35)
#define C_BTN       RGB(46, 53, 64)
#define C_TEXT      RGB(236, 239, 243)
#define C_DIM       RGB(132, 140, 152)
#define C_FAINT     RGB(70, 76, 86)
#define C_LED_OFF   RGB(48, 54, 63)
#define C_AMBER     RGB(255, 176, 0)
#define C_RED       RGB(235, 50, 50)
#define C_GREEN     RGB(60, 210, 100)
#define C_BAR       RGB(70, 150, 255)
#define C_SEL       RGB(70, 150, 255)
#define C_DARK      RGB(12, 18, 30)
#define C_HOLD_BG   RGB(205, 140, 10)
#define C_START_BG  RGB(30, 140, 70)
#define C_RESET_BG  RGB(190, 40, 40)
#define C_UNLOCK_BG RGB(55, 105, 200)
#define C_ON_BG     RGB(35, 160, 85)

static const uint16_t axis_color[4] = {
  RGB(255, 96, 96), RGB(96, 220, 120), RGB(90, 160, 255), RGB(255, 190, 60)
};
static const char axis_letter[4] = { 'X', 'Y', 'Z', 'A' };

static grbl_shm_t st;                 // last consistent copy of the M7's status
static uint32_t st_fresh_at = 0;      // our time when st last changed
static uint32_t st_last_m7ms = 0;
static bool have_status = false;
static bool waiting_shown = false;
static bool live = false;             // fresh status: touches are acted on

enum { PAGE_DRO, PAGE_JOG, PAGE_OVR, PAGE_IO, PAGE_SET, PAGE_FILE, PAGE_COUNT };
#define PAGE_ALL  0xFF
static uint8_t page = PAGE_DRO;
static bool page_dirty = true;

// Widgets that are not buttons.
enum { W_STATE, W_MODAL, W_LINK, W_MSG, W_WAIT, W_SPLASH_VER, W_SPLASH_BAR, W_SPLASH_MSG,
       W_DRO0, W_DRO1, W_DRO2, W_DRO3, W_SUM_FEED, W_SUM_SPINDLE,
       W_JDRO0, W_JDRO1, W_JDRO2, W_JDRO3, W_JLBL_Z, W_JLBL_A, W_MPG,
       W_OVR_L0, W_OVR_L1, W_OVR_L2, W_OVR_L3,
       W_IN1, W_IN2, W_OUT, W_PLAN, W_SET_HEAD, W_SET_DESC1, W_SET_DESC2, W_ESTOP,
       W_FILE_DETAIL, W_FILE_STATUS, W_COUNT };

// Buttons.
enum {
  B_NONE,
  B_HOLD, B_START, B_RESET, B_UNLOCK,
  B_TAB0, B_TAB1, B_TAB2, B_TAB3, B_TAB4, B_TAB5,
  B_ZERO0, B_ZERO1, B_ZERO2, B_ZERO3,
  B_STEP0, B_STEP1, B_STEP2, B_STEP3, B_STEP4, B_SPEED,
  B_JOG_XP, B_JOG_XN, B_JOG_YP, B_JOG_YN, B_JOG_ZP, B_JOG_ZN, B_JOG_AP, B_JOG_AN,
  B_HOME, B_ZERO_ALL,
  B_FEED_M10, B_FEED_M1, B_FEED_100, B_FEED_P1, B_FEED_P10,
  B_RAPID_25, B_RAPID_50, B_RAPID_100,
  B_SPIN_M10, B_SPIN_100, B_SPIN_P10, B_SPIN_STOP, B_SPIN_START,
  B_FLOOD, B_MIST,
  B_AUX0, B_AUX1, B_AUX2, B_AUX3, B_AUX4, B_AUX5, B_COLLET, B_SRV_RST, B_VFD_RST,
  B_SROW0, B_SROW1, B_SROW2, B_SROW3, B_SROW4, B_SROW5, B_SUP, B_SDOWN,
  B_K0, B_K1, B_K2, B_K3, B_K4, B_K5, B_K6, B_K7, B_K8, B_K9, B_KDOT, B_KDEL, B_KCLR,
  B_KCANCEL, B_KSAVE,
  B_OPT0, B_OPT_LAST = B_OPT0 + 21,     // up to 22 switches ($79)
  B_FROW0, B_FROW1, B_FROW2, B_FROW3, B_FROW4, B_FPREV, B_FNEXT, B_FSTART, B_FFORMAT, B_FSTOP,
  B_COUNT
};

typedef struct { int16_t x, y, w, h; uint8_t page; } rect_t;
static rect_t brect[B_COUNT];

static uint32_t sig_w[W_COUNT];
static uint32_t sig_b[B_COUNT];
static bool force_redraw = true;

// Jog settings chosen on the JOG page.
#define N_STEPS   5          // four step sizes and continuous
#define STEP_CONT 4
static const float step_mm[4] = { 0.01f, 0.1f, 1.0f, 10.0f };
static const float step_in[4] = { 0.001f, 0.01f, 0.1f, 1.0f };
static const char *step_mm_txt[4] = { "0.01", "0.1", "1", "10" };
static const char *step_in_txt[4] = { "0.001", "0.01", "0.1", "1" };
static uint8_t step_sel = 2;
static const uint8_t speeds[] = { 5, 10, 25, 50, 100 };
static uint8_t speed_sel = 2;

// Touch state.
static uint8_t  t_active = B_NONE;    // button under the finger, B_NONE if none
static bool     t_down = false;
static bool     t_fired = false;      // a hold button's action has been done
static uint32_t t_press_ms = 0, t_last_ms = 0, t_repeat_ms = 0;
static bool     jog_cont = false;     // a continuous jog is held
static uint32_t touch_state = 0;      // touch_init() result

#define SIM_QUEUE 4
typedef struct { uint16_t x, y; uint32_t ms, start; } sim_t;
static sim_t    sim_q[SIM_QUEUE];     // simulated touches ($TOUCH) not yet begun
static uint8_t  sim_n = 0;
static uint32_t sim_seq_seen = 0;
static bool     sim_on = false;
static uint16_t sim_x = 0, sim_y = 0;
static uint32_t sim_until = 0;

static char     hint[64] = "";
static uint32_t hint_until = 0;


// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static uint32_t fnv(uint32_t h, const void *p, uint32_t n)
{
  const uint8_t *b = (const uint8_t *)p;
  h ^= 2166136261UL;
  while (n--) { h = (h ^ *b++) * 16777619UL; }
  return h;
}

static uint32_t fnv_s(uint32_t h, const char *s) { return fnv(h, s, strlen(s)); }

static bool changed(int id, uint32_t sig)
{
  if (!force_redraw && sig_w[id] == sig) { return false; }
  sig_w[id] = sig;
  return true;
}

static bool changed_btn(int id, uint32_t sig)
{
  if (!force_redraw && sig_b[id] == sig) { return false; }
  sig_b[id] = sig;
  return true;
}

// Fixed-point formatting without printf's float support.
static void fmt_fixed(char *out, float v, int dec)
{
  static const int32_t pow10[] = { 1, 10, 100, 1000, 10000 };
  if (isnan(v) || isinf(v) || fabsf(v) > 9.9e8f) { strcpy(out, "---"); return; }
  int64_t s = llroundf(v * (float)pow10[dec]);
  bool neg = s < 0;
  if (neg) { s = -s; }
  char tmp[24];
  int n = 0;
  int64_t ip = s / pow10[dec], fp = s % pow10[dec];
  for (int i = 0; i < dec; i++) { tmp[n++] = (char)('0' + fp % 10); fp /= 10; }
  if (dec) { tmp[n++] = '.'; }
  do { tmp[n++] = (char)('0' + ip % 10); ip /= 10; } while (ip && n < 20);
  if (neg && s != 0) { tmp[n++] = '-'; }
  for (int i = 0; i < n; i++) { out[i] = tmp[n - 1 - i]; }
  out[n] = 0;
}

static void fmt_uint(char *out, uint32_t v)
{
  char tmp[12];
  int n = 0;
  do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
  for (int i = 0; i < n; i++) { out[i] = tmp[n - 1 - i]; }
  out[n] = 0;
}

// Right-aligns s in a field of width chars.
static void rpad(char *out, const char *s, int width)
{
  int n = strlen(s);
  int pad = width - n;
  if (pad < 0) { pad = 0; }
  for (int i = 0; i < pad; i++) { out[i] = ' '; }
  strcpy(out + pad, s);
}

static void cat(char *dst, const char *src) { strcat(dst, src); }
static void cat_uint(char *dst, uint32_t v) { char n[12]; fmt_uint(n, v); strcat(dst, n); }

static bool linear_axis(int i) { return !(i == 3 && st.a_mode == 0); }
static bool a_slave(void) { return st.a_mode >= 2; }

// a + (b - a) * t / 256, per RGB565 channel.
static uint16_t mix(uint16_t a, uint16_t b, int t)
{
  int ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31;
  int br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
  int r = ar + (((br - ar) * t) >> 8);
  int g = ag + (((bg - ag) * t) >> 8);
  int bl = ab + (((bb - ab) * t) >> 8);
  return (uint16_t)((r << 11) | (g << 5) | bl);
}

static void text_center(int x, int y, int w, const font_t *f, const char *s, uint16_t fg, uint16_t bg)
{
  cv_text(x + (w - cv_text_width(f, s)) / 2, y, f, s, fg, bg);
}

static void set_hint(const char *s)
{
  strncpy(hint, s, sizeof(hint) - 1);
  hint[sizeof(hint) - 1] = 0;
  hint_until = millis() + HINT_MS;
}

static bool st_is(uint8_t s) { return st.state == s; }
static bool estop_shown(void) { return (st.flags & GSHM_F_ESTOP) != 0; }
// Driver supply watched and missing, or back but $95 not over: $H/$X refused.
static bool drv_waiting(void) { return st.drv_power == GSHM_DRV_OFF || st.drv_power == GSHM_DRV_RETURNING; }

// The value of setting $n from the status copy (0 if not on the list).
static float set_value(uint8_t n)
{
  static const uint8_t nums[GSHM_N_SETTINGS] = GSHM_SETTING_NUMBERS;
  for (int i = 0; i < GSHM_N_SETTINGS; i++) { if (nums[i] == n) { return st.set_val[i]; } }
  return 0;
}
static uint8_t set_byte(uint8_t n) { return (uint8_t)(set_value(n) + 0.5f); }
static bool st_hold(void) { return st_is(GSHM_STATE_HOLD0) || st_is(GSHM_STATE_HOLD1); }


// ---------------------------------------------------------------------------
// Texts
// ---------------------------------------------------------------------------

static const char *alarm_text(uint8_t code)
{
  switch (code) {
    case 1:  return "Hard limit triggered - position lost";
    case 2:  return "Soft limit - target outside travel";
    case 3:  return "Reset during motion - position lost";
    case 4:  return "Probe already triggered before probing";
    case 5:  return "Probe did not make contact";
    case 6:  return "Homing aborted by reset";
    case 7:  return "Homing aborted: safety door opened";
    case 8:  return "Homing: pull-off did not clear the switch";
    case 9:  return "Homing: limit switch not found";
    case 10: return "Homing: second gantry switch not found";
    case 14: return "Spindle not at speed (at-speed relay, $52)";
    case 17: return "Servo alarm input active - drive fault";
    case 18: return "E-STOP - safety relay tripped";
    case 19: return "VFD fault - spindle stopped";
    case 20: return "Homing hit the switch at the far end";
    case 21: return "USB program stopped by a G-code error";
    case 22: return "Drive power was lost - HOME or UNLOCK";
    case 23: return "VFD on RS485 silent or not following";
  }
  return "";
}

static const char *error_text(uint8_t code)
{
  switch (code) {
    case 1:  return "Expected command letter";
    case 2:  return "Bad number format";
    case 3:  return "Invalid $ statement";
    case 4:  return "Negative value";
    case 5:  return "Setting disabled";
    case 6:  return "Step pulse must be >= 3 us";
    case 7:  return "Settings read failed, defaults restored";
    case 8:  return "Command needs idle state";
    case 9:  return "G-code locked during alarm or jog";
    case 10: return "Soft limits need homing enabled";
    case 11: return "Line too long";
    case 12: return "Step rate too high";
    case 13: return "Safety door open";
    case 14: return "Startup line too long";
    case 15: return "Jog target exceeds machine travel";
    case 16: return "Invalid jog command";
    case 17: return "Laser mode needs PWM";
    case 20: return "Unsupported command";
    case 21: return "Modal group violation";
    case 22: return "Feed rate not set";
    case 23: return "Value must be an integer";
    case 24: return "Two commands need axis words";
    case 25: return "Word repeated in block";
    case 26: return "No axis words";
    case 27: return "Invalid line number";
    case 28: return "Value word missing";
    case 29: return "Unsupported coordinate system";
    case 30: return "G53 needs G0 or G1";
    case 31: return "Unused axis words";
    case 32: return "Arc needs an axis word in its plane";
    case 33: return "Invalid motion target";
    case 34: return "Invalid arc radius";
    case 35: return "Arc needs I/J/K in its plane";
    case 36: return "Unused value words";
    case 37: return "G43.1 needs exactly the tool length axis";
    case 38: return "Value too large (tool number / output)";
    case 39: return "Collet open - spindle start refused";
    case 40: return "E-STOP active - release it and RESET";
    case 41: return "Limit switch held in that direction";
    case 42: return "Channel already used by another function";
    case 43: return "VFD fault still active";
    case 44: return "Collet: spindle must stand, machine idle";
    case 45: return "No limit switch at the homing end ($23)";
    case 46: return "No USB stick";
    case 47: return "File cannot be read";
    case 48: return "A program from the USB stick runs";
    case 49: return "PC is sending - start from the PC or wait";
    case 50: return "Drives have no power (or $95 not over)";
    case 51: return "VFD on RS485 silent or not following";
    case 52: return "VFD refused the request (exception)";
  }
  return "";
}



// ---------------------------------------------------------------------------
// Settings ($$ page): what each one is, how to show and edit it
// ---------------------------------------------------------------------------

enum { T_INT, T_FLOAT, T_BOOL, T_AXES, T_AUX, T_OUT, T_STATUS, T_ENUM, T_INCH, T_OUTCH, T_INMASK };

typedef struct {
  uint8_t num, type;
  const char *title;    // fits the list row (20 chars)
  const char *brief;    // second line of the row (30 chars)
  const char *desc;     // the editor's explanation
  const char *opts;     // T_ENUM: "name|name|..." for 0, 1, ...
} set_meta_t;

static const set_meta_t set_meta[] = {
  { 0, T_INT, "Step pulse", "us, at least 3",
    "Length of every step pulse in microseconds. Most drivers need 2 to 10 us. The pulse must also be shorter than the step period at the highest step rate.", 0 },
  { 1, T_INT, "Step idle delay", "ms, 255 = always enabled",
    "How long the drives stay enabled after a motion has ended before they are switched off (Z brake first, see $48-$50). 255 keeps them enabled all the time.", 0 },
  { 2, T_AXES, "Step pulse invert", "ticked axes inverted",
    "Inverts the step signal of the ticked axes, so the pulse becomes a low level. Only for drivers that step on a falling edge.", 0 },
  { 3, T_AXES, "Direction invert", "ticked axes reversed",
    "Reverses the direction of the ticked axes. Use it when an axis moves the wrong way. With a slave A axis, A mirrors the second motor of the gantry.", 0 },
  { 4, T_BOOL, "Invert enable", "on = drives enabled high",
    "Polarity of the drive enable output D41. Off: the drives are enabled by a low level, as most drives expect. On: they are enabled by a high level.", 0 },
  { 10, T_STATUS, "Status report", "what ? reports",
    "What a status report contains. MPos: machine position instead of work position. Buffer: planner and receive buffer use. Most senders work with MPos (1) or MPos + Buffer (3).", 0 },
  { 11, T_FLOAT, "Junction deviation", "mm, cornering speed",
    "How fast the machine may take a corner between two moves. Larger is faster, but rounds corners more and shakes the machine. Typical 0.005 to 0.02 mm.", 0 },
  { 12, T_FLOAT, "Arc tolerance", "mm",
    "Grbl cuts arcs into short straight lines; this is how far the lines may stray from the true arc. Smaller means more, shorter lines. Default 0.002 mm.", 0 },
  { 13, T_BOOL, "Report inches", "on = inches",
    "Status reports, the DRO and the jog step sizes in inches instead of millimetres. G-code units (G20/G21) and the settings stay as they are.", 0 },
  { 20, T_BOOL, "Soft limits", "needs homing ($22)",
    "Refuses moves whose target lies outside the travel set by $130-$133 (ALARM:2 during a job). Needs homing, so that the machine position is known.", 0 },
  { 21, T_BOOL, "Hard limits", "a limit switch = alarm",
    "A limit switch that closes stops everything at once with ALARM:1. With two inputs per axis Grbl knows which end was hit and lets the axis jog off a held switch.", 0 },
  { 22, T_BOOL, "Homing", "enables $H",
    "Enables the homing cycle $H (HOME on the JOG page). With homing on, Grbl starts locked in alarm until the machine is homed or unlocked.", 0 },
  { 23, T_AXES, "Homing direction", "ticked = towards minus",
    "Ticked axes home to their minus switch, the others to their plus switch. The switch at the other end closing during homing is ALARM:20.", 0 },
  { 24, T_FLOAT, "Homing feed", "mm/min, slow locate",
    "Speed of the slow second approach to the switches, the one that sets the machine origin precisely.", 0 },
  { 25, T_FLOAT, "Homing seek", "mm/min, fast search",
    "Speed of the first, fast search for the limit switches.", 0 },
  { 26, T_INT, "Homing debounce", "ms",
    "Pause after a switch has triggered, to let it settle before the next phase of the homing cycle.", 0 },
  { 27, T_FLOAT, "Homing pull-off", "mm",
    "How far the axes move back off their switches at the end of homing. It must release them, or homing fails with ALARM:8.", 0 },
  { 30, T_FLOAT, "Max spindle speed", "rpm at full PWM / 3.3 V",
    "The S value that gives full PWM duty on D6 and 3.3 V on the DAC output A12 (10 V after the shield). Higher S values are limited to it.", 0 },
  { 31, T_FLOAT, "Min spindle speed", "rpm at the lowest PWM",
    "The S value that gives the lowest PWM duty. Smaller S values run at that minimum; S0 switches the output off.", 0 },
  { 32, T_BOOL, "Laser mode", "on = laser, off = spindle",
    "For lasers: moves no longer stop when S changes, and with M4 the power follows the actual speed, so corners do not burn. Leave off for a spindle.", 0 },
  { 33, T_ENUM, "Spindle VFD (RS485)", "drive on D17-D19, Modbus",
    "Also runs the spindle VFD over RS485 (Modbus RTU, shield v4): M3/M4/M5 and S, its status for at speed ($52) and faults. FWD/REV and 0-10 V keep working. Not with laser mode.", "none|Delta" },
  { 34, T_INT, "VFD address", "Modbus address, 1-247",
    "The drive's own Modbus address (Delta: 09-00).", 0 },
  { 35, T_INT, "VFD baud rate", "bit/s, 1200-115200",
    "Speed of the RS485 line; the drive must be set the same (Delta: 09-01, in kbps).", 0 },
  { 36, T_ENUM, "VFD frame", "data, parity, stop bits",
    "Character frame of the RS485 line, RTU. Delta: 09-04 = 12 + this value (13 = 8N2).", "8N1|8N2|8E1|8O1|8E2|8O2" },
  { 37, T_FLOAT, "VFD frequency at $30", "Hz the drive runs at S=$30",
    "The frequency sent to the drive for S = $30 (max spindle speed); lower S scale down linearly. A two-pole 24000 rpm spindle: 400 Hz.", 0 },
  { 38, T_ENUM, "Spindle PWM on OUT13", "D6: output or PWM",
    "Off: OUT13 (D6) is an output like the others. On: D6 carries the spindle / laser PWM (1 kHz). Inverted: for a TTL PWM input behind OUT13, which sinks to 0 V. Laser mode needs it.", "off|on|inverted" },
  { 39, T_BOOL, "Panel buttons", "START HOLD STOP on A4-A6",
    "The operator panel's buttons: START and HOLD as the inputs, STOP ends the program with a controlled stop - position kept, spindle off. Not the safety STOP on D49.", 0 },
  { 44, T_ENUM, "Panel pot A0", "override it sets",
    "What the potentiometer on A0 sets when it is turned: 10-100 % left, 100 % in the middle, up to 200 % right (rapid: 25/50/100 %). The PC and this screen can still change it.", "none|feed|spindle|rapid" },
  { 45, T_ENUM, "Panel pot A1", "override it sets",
    "What the potentiometer on A1 sets when it is turned, as for A0 ($44). Usually A0 feed, A1 spindle.", "none|feed|spindle|rapid" },
  { 40, T_ENUM, "Kinematics", "Cartesian or CoreXY",
    "CoreXY: the X driver runs belt motor A, the Y driver motor B; X and Y then home one after the other. Not possible together with a slave A axis.", "Cartesian|CoreXY" },
  { 41, T_ENUM, "CoreXY limits", "what $110/$120 limit",
    "On CoreXY: whether $110/$120 limit each belt motor (0) or the X and Y axes of the head (1). With 0, a diagonal runs at 71 percent of $110.", "per motor|head axes" },
  { 42, T_ENUM, "A axis mode", "rotary, linear or slave",
    "Rotary: degrees, never scaled by G20. Linear: millimetres. Slave of X or Y: the A driver repeats every X (or Y) step, and homing squares the gantry on the A- / A+ switches.", "rotary|linear|slave of X|slave of Y" },
  { 43, T_BOOL, "Home A axis", "A in the $H cycle",
    "Homes an independent A axis as the last cycle of $H. Without it A has no known machine position, and soft limits leave it out.", 0 },
  { 46, T_AUX, "Aux with spindle", "AUX on with M3/M4",
    "AUX outputs that switch on whenever the spindle runs, for example the dust extraction on AUX0, without any M-code in the program.", 0 },
  { 48, T_INT, "Brake: enable delay", "ms, drives on -> brake open",
    "Z brake: time from switching the drives on until the brake is released, so that the servo holds the axis first. Only when the drives were off ($1).", 0 },
  { 49, T_INT, "Brake: open time", "ms, brake open -> 1st step",
    "Z brake: time the brake needs to open. The first step waits for it. With $59=0 (brake in the drive) $48 + $49 still pass before the first step.", 0 },
  { 50, T_INT, "Brake: close time", "ms, brake on -> drives off",
    "Z brake: time the brake needs to close. The drives are switched off only after it, so the axis never hangs unheld. Only with $59=1.", 0 },
  { 51, T_INT, "Spindle run-down", "ms before the collet opens",
    "The collet opens only after the spindle has been off this long, and while the at-speed relay (if assigned) reports no speed.", 0 },
  { 52, T_INT, "At-speed timeout", "ms, 0 = do not wait",
    "After M3/M4 and every S change Grbl waits until the VFD's at-speed relay ($77) or the drive on RS485 reports the speed, at most this long; then ALARM:14. Also before a hold or a closed door resumes with the spindle started again. 0: no waiting, $54 instead.", 0 },
  { 53, T_ENUM, "At-speed lost", "reaction while cutting",
    "What happens when the at-speed relay opens while the spindle should be at speed (overload, belt slip): ignore it, feed hold, or alarm after a controlled stop.", "ignore|feed hold|alarm" },
  { 54, T_INT, "Spin-up delay", "ms after M3/M4",
    "Fixed wait after M3/M4 when the at-speed relay is not used ($77=0 or $52=0), and before a hold or a closed door resumes with the spindle started again.", 0 },
  { 55, T_INT, "Reset pulse", "ms, servo and VFD reset",
    "Length of the pulses on the servo reset ($91) and VFD reset ($92) outputs: at RESET or UNLOCK after a servo alarm or a VFD fault, or from the I/O page.", 0 },
  { 56, T_INT, "Direction setup", "us before the next step",
    "Delay between a direction change and the next step pulse. The Nanotec C5 needs 35 us; 0 for drives that need none.", 0 },
  { 57, T_BOOL, "Central STOP", "STOP input D49 in use",
    "Watches the signal contact of the safety relay on D49. STOP ends all steps at once, brakes Z, switches every output off: ALARM:18 until RESET with STOP released.", 0 },
  { 58, T_ENUM, "M6 tool change", "error or pause",
    "What M6 does: refused as in Grbl, or it stops the spindle and pauses for a manual tool change - COLLET on the I/O page, then START.", "error|pause" },
  { 59, T_ENUM, "Z brake control", "drive or BRAKE output",
    "Who works the Z brake: the drive itself (Delta BRKR, Nanotec output 1; BRAKE unused) or the BRAKE output D16 with the times $48-$50.", "drive|BRAKE output" },
  { 60, T_INCH, "Limit X-", "input channel, 0 = none",
    "Input (1-20) of the X switch at the minus end. X+ on the same input: two switches in series, Grbl cannot tell which end. 0: no switch there.", 0 },
  { 61, T_INCH, "Limit X+", "input channel, 0 = none",
    "Input (1-20) of the X switch at the plus end. The same input as X-: two switches in series. 0: the axis has a switch at the minus end only.", 0 },
  { 62, T_INCH, "Limit Y-", "input channel, 0 = none",
    "Input (1-20) of the Y switch at the minus end. Y+ on the same input: two switches in series. 0: no switch there.", 0 },
  { 63, T_INCH, "Limit Y+", "input channel, 0 = none",
    "Input (1-20) of the Y switch at the plus end. The same input as Y-: two switches in series. 0: no switch there.", 0 },
  { 64, T_INCH, "Limit Z-", "input channel, 0 = none",
    "Input (1-20) of the Z switch at the minus end. Z+ on the same input: two switches in series. 0: no switch there.", 0 },
  { 65, T_INCH, "Limit Z+", "input channel, 0 = none",
    "Input (1-20) of the Z switch at the plus end. The same input as Z-: two switches in series. 0: no switch there.", 0 },
  { 66, T_INCH, "Limit A-", "input channel, 0 = none",
    "Input (1-20) of the A switch at the minus end - with a slave A, the second gantry motor's switch. 0: no switch there.", 0 },
  { 67, T_INCH, "Limit A+", "input channel, 0 = none",
    "Input (1-20) of the A switch at the plus end - with a slave A, the second gantry motor's switch. 0: no switch there.", 0 },
  { 68, T_INCH, "Servo alarm X", "input channel, 0 = none",
    "Input of the X drive's alarm output. Active: ALARM:17 at once, like a hard limit; the next RESET pulses the servo reset ($91).", 0 },
  { 69, T_INCH, "Servo alarm Y", "input channel, 0 = none",
    "Input of the Y drive's alarm output. Active: ALARM:17 at once, like a hard limit; the next RESET pulses the servo reset ($91).", 0 },
  { 70, T_INCH, "Servo alarm Z", "input channel, 0 = none",
    "Input of the Z drive's alarm output. Active: ALARM:17 at once, like a hard limit; the next RESET pulses the servo reset ($91).", 0 },
  { 71, T_INCH, "Servo alarm A", "input channel, 0 = none",
    "Input of the A drive's alarm output. Active: ALARM:17 at once, like a hard limit; the next RESET pulses the servo reset ($91).", 0 },
  { 72, T_INCH, "Probe", "input channel, 0 = none",
    "Input of the probe (G38.x). Grbl watches it on every step while probing.", 0 },
  { 73, T_INCH, "Reset button", "input channel, 0 = none",
    "Input of an external RESET button (soft reset, like Ctrl-X). Not the emergency stop - that is the STOP input D49 ($57).", 0 },
  { 74, T_INCH, "Hold button", "input channel, 0 = none",
    "Input of an external feed hold button.", 0 },
  { 75, T_INCH, "Start button", "input channel, 0 = none",
    "Input of an external cycle start button.", 0 },
  { 76, T_INCH, "Safety door", "input channel, 0 = none",
    "Input of the safety door switch. Active: the machine holds as with HOLD.", 0 },
  { 77, T_INCH, "Spindle at speed", "input channel, 0 = none",
    "Input of the VFD's 'frequency reached' relay. Grbl waits for it after M3/M4 and S changes ($52) and watches it while cutting ($53).", 0 },
  { 78, T_INCH, "Spindle fault", "input channel, 0 = none",
    "Input of the VFD's fault relay. Active: spindle off, controlled stop, ALARM:19; UNLOCK refused until it clears. RESET or UNLOCK pulses $92.", 0 },
  { 79, T_INMASK, "NC inputs", "ticked = normally closed",
    "Ticked inputs are active while their optocoupler is off: for normally closed contacts, where a broken wire trips too. STOP and PWR (driver power, D53) are the last two.", 0 },
  { 80, T_OUTCH, "Spindle FWD", "output channel, 0 = none",
    "Output for the VFD's forward run input: on with M3. FWD and REV are never on together.", 0 },
  { 81, T_OUTCH, "Spindle REV", "output channel, 0 = none",
    "Output for the VFD's reverse run input: on with M4.", 0 },
  { 82, T_OUTCH, "Flood coolant", "output channel, 0 = none",
    "Output switched by M8 (and the FLOOD button), off with M9.", 0 },
  { 83, T_OUTCH, "Mist coolant", "output channel, 0 = none",
    "Output switched by M7 (and the MIST button), off with M9.", 0 },
  { 84, T_OUTCH, "AUX0", "output channel, 0 = none",
    "Output of AUX0: M62/M64 P0 on, M63/M65 P0 off; with the spindle if ticked in $46 (dust extraction).", 0 },
  { 85, T_OUTCH, "AUX1", "output channel, 0 = none",
    "Output of AUX1: M62/M64 P1 on, M63/M65 P1 off; with the spindle if ticked in $46.", 0 },
  { 86, T_OUTCH, "AUX2", "output channel, 0 = none",
    "Output of AUX2: M62/M64 P2 on, M63/M65 P2 off; with the spindle if ticked in $46.", 0 },
  { 87, T_OUTCH, "AUX3", "output channel, 0 = none",
    "Output of AUX3: M62/M64 P3 on, M63/M65 P3 off; with the spindle if ticked in $46.", 0 },
  { 88, T_OUTCH, "AUX4", "output channel, 0 = none",
    "Output of AUX4: M62/M64 P4 on, M63/M65 P4 off; with the spindle if ticked in $46.", 0 },
  { 89, T_OUTCH, "AUX5", "output channel, 0 = none",
    "Output of AUX5: M62/M64 P5 on, M63/M65 P5 off; with the spindle if ticked in $46.", 0 },
  { 90, T_OUTCH, "Collet open", "output channel, 0 = none",
    "Output of the collet valve: on = collet open. Opens only with the spindle stopped ($51) and the machine standing; M3/M4 refused while open.", 0 },
  { 91, T_OUTCH, "Servo reset", "output channel, 0 = none",
    "Output to the alarm reset inputs of the drives: a $55 pulse at RESET or UNLOCK after ALARM:17, or from the I/O page.", 0 },
  { 92, T_OUTCH, "VFD reset", "output channel, 0 = none",
    "Output to the VFD's fault reset input: a $55 pulse at RESET or UNLOCK while the fault input is active, or from the I/O page.", 0 },
  { 93, T_OUT, "Output invert", "ticked = inverted",
    "Outputs OUT1-OUT14 inverted at the pin. The display and the M-codes still show the logical state.", 0 },
  { 94, T_BOOL, "Driver power watch", "drive supply on D53",
    "Watches the supply of the drives on D53. Gone for 2 ms: steps end, Z brake on, ALARM:22 - the position is lost. HOME or UNLOCK once it is back for $95.", 0 },
  { 95, T_INT, "Driver power delay", "ms back before HOME/UNLOCK",
    "How long the drive supply must be back before HOME and UNLOCK are accepted: the time the drives take to boot. 0: at once.", 0 },
  { 96, T_ENUM, "Handwheel (MPG)", "pendant on D54-D63",
    "Handwheel pendant on the camera pins D54-D63 (display socket J3): wheel A/B, axis X/Y/Z/4, step x1/x10/x100, LED. Turning jogs the selected axis; it stops when the wheel stops.", "off|on|on, reversed" },
  { 97, T_FLOAT, "MPG step x1", "mm per detent at x1",
    "Distance one detent of the wheel moves the axis at x1 (degrees for a rotary A). At x10 and x100 a detent moves 10 and 100 times as far.", 0 },
  { 98, T_FLOAT, "MPG max feed", "mm/min",
    "Highest feed the handwheel moves an axis at, never above the axis's $110-$113. Turning faster than that is dropped, not stored up, so the axis stops with the wheel.", 0 },
};
#define N_SET_META (sizeof(set_meta) / sizeof(set_meta[0]))

static const uint8_t set_numbers[GSHM_N_SETTINGS] = GSHM_SETTING_NUMBERS;

// Settings $100-$133, the same four for every axis.
static const char *const axis_set_title[4] = { " steps/", " max rate", " acceleration", " max travel" };
static const char *const axis_set_desc[4] = {
  "Motor steps for 1 mm of travel (1 degree for a rotary A): steps per motor revolution x microstepping / travel per revolution.",
  "The fastest the axis may move, per minute. G0 rapids run at this speed, and the jog speed buttons are a percentage of it.",
  "Acceleration of the axis, per second squared. Too high and the motor loses steps; too low and corners and short moves are slow.",
  "Length of the axis. Soft limits keep moves within it, and homing searches at most 1.5 times this far for the switch.",
};

typedef struct {
  set_meta_t m;
  char title[24];
  char brief[32];
} set_info_t;

// Everything about the setting at index idx of set_numbers[].
static void set_info(int idx, set_info_t *out)
{
  uint8_t n = set_numbers[idx];
  for (unsigned i = 0; i < N_SET_META; i++) {
    if (set_meta[i].num == n) {
      out->m = set_meta[i];
      strcpy(out->title, set_meta[i].title);
      strcpy(out->brief, set_meta[i].brief);
      return;
    }
  }
  uint8_t axis = n % 10, group = (uint8_t)(n / 10 - 10);
  if (axis > 3) { axis = 3; }
  if (group > 3) { group = 3; }
  bool deg = (axis == 3) && (st.a_mode == 0);
  const char *u = deg ? "deg" : "mm";
  out->title[0] = axis_letter[axis];
  out->title[1] = 0;
  cat(out->title, axis_set_title[group]);
  if (group == 0) { cat(out->title, u); }
  out->brief[0] = 0;
  switch (group) {
    case 0: cat(out->brief, deg ? "steps per degree" : "steps per mm"); break;
    case 1: cat(out->brief, u); cat(out->brief, "/min, also G0 speed"); break;
    case 2: cat(out->brief, u); cat(out->brief, "/s2"); break;
    case 3: cat(out->brief, u); cat(out->brief, ", soft limits, homing"); break;
  }
  out->m.num = n;
  out->m.type = T_FLOAT;
  out->m.title = out->title;
  out->m.brief = out->brief;
  out->m.desc = axis_set_desc[group];
  out->m.opts = 0;
}

static bool set_numeric(uint8_t type) { return type == T_INT || type == T_FLOAT || type == T_INCH || type == T_OUTCH; }
static bool set_toggles(uint8_t type) { return type == T_AXES || type == T_AUX || type == T_OUT || type == T_STATUS || type == T_INMASK; }

static int set_n_opts(const set_meta_t *m)
{
  switch (m->type) {
    case T_AXES: return 4;
    case T_AUX: return GSHM_N_AUX;
    case T_OUT: return GSHM_N_OUT;
    case T_INMASK: return GSHM_N_IN + 2;
    case T_STATUS: case T_BOOL: return 2;
    case T_ENUM: {
      int n = 1;
      for (const char *p = m->opts; *p; p++) { if (*p == '|') { n++; } }
      return n;
    }
  }
  return 0;
}

// Name of option k of an enum, without the number.
static void enum_name(const set_meta_t *m, int k, char *out)
{
  const char *p = m->opts;
  while (k > 0 && *p) { if (*p++ == '|') { k--; } }
  int n = 0;
  while (p[n] && p[n] != '|' && n < 20) { out[n] = p[n]; n++; }
  out[n] = 0;
}

static void opt_label(const set_meta_t *m, int k, char *out)
{
  out[0] = 0;
  switch (m->type) {
    case T_AXES: out[0] = axis_letter[k]; out[1] = 0; break;
    case T_AUX: strcpy(out, "AUX"); cat_uint(out, k); break;
    case T_OUT: cat_uint(out, k + 1); break;   // 14 small buttons: "OUT14" would not fit
    case T_INMASK:
      if (k < GSHM_N_IN) { strcpy(out, "IN"); cat_uint(out, k + 1); } else { strcpy(out, (k == GSHM_N_IN) ? "STOP" : "PWR"); }
      break;
    case T_STATUS: strcpy(out, k ? "Buffer" : "MPos"); break;
    case T_BOOL: strcpy(out, k ? "1 on" : "0 off"); break;
    case T_ENUM: { char nm[24]; cat_uint(out, k); cat(out, " "); enum_name(m, k, nm); cat(out, nm); break; }
  }
}

// A setting's value as the list shows it.
static void set_format(const set_meta_t *m, float v, char *out)
{
  uint32_t iv = (uint32_t)(v + 0.5f);
  out[0] = 0;
  switch (m->type) {
    case T_INT: fmt_uint(out, iv); break;
    case T_FLOAT: fmt_fixed(out, v, 3); break;
    case T_BOOL: strcpy(out, iv ? "on" : "off"); break;
    case T_AXES:
      for (int i = 0; i < 4; i++) {
        if (iv & (1u << i)) { char l[3] = { axis_letter[i], ' ', 0 }; cat(out, l); }
      }
      if (!out[0]) { strcpy(out, "none"); } else { out[strlen(out) - 1] = 0; }
      break;
    case T_AUX: case T_OUT: case T_INMASK: {
      int first = (m->type == T_AUX) ? 0 : 1;
      int n = set_n_opts(m);
      strcpy(out, (m->type == T_AUX) ? "AUX" : (m->type == T_OUT) ? "OUT" : "IN");
      bool any = false;
      for (int i = 0; i < n; i++) {
        if (!(iv & (1u << i))) { continue; }
        if (strlen(out) > 24) { cat(out, "..."); break; }
        if (any) { cat(out, ","); }
        if ((m->type == T_INMASK) && (i >= GSHM_N_IN)) { cat(out, (i == GSHM_N_IN) ? "STOP" : "PWR"); }
        else { cat_uint(out, i + first); }
        any = true;
      }
      if (!any) { strcpy(out, "none"); }
      break;
    }
    case T_INCH: case T_OUTCH:
      if (!iv) { strcpy(out, "unused"); break; }
      strcpy(out, (m->type == T_INCH) ? "IN" : "OUT"); cat_uint(out, iv);
      break;
    case T_STATUS: strcpy(out, (iv & 1) ? "MPos" : "WPos"); if (iv & 2) { cat(out, "+Buf"); } break;
    case T_ENUM: enum_name(m, (int)iv, out); break;
  }
}

// Word-wraps s into lines of at most width characters.
static int wrap(const char *s, int width, char lines[][34], int max_lines)
{
  int n = 0;
  while (*s && n < max_lines) {
    while (*s == ' ') { s++; }
    int len = strlen(s);
    if (len <= width) { strcpy(lines[n++], s); break; }
    int cut = width;
    while (cut > 0 && s[cut] != ' ') { cut--; }
    if (cut == 0) { cut = width; }
    memcpy(lines[n], s, cut);
    lines[n][cut] = 0;
    n++;
    s += cut;
  }
  return n;
}

#define SET_ROWS  6
static int8_t   edit_idx = -1;        // setting being edited, -1 = the list
static char     edit_buf[12] = "";    // number typed so far
static uint32_t edit_bits = 0;        // new value of a switch / choice setting
static uint8_t  list_top = 0;         // first setting in the list
static bool     save_pending = false;
static uint8_t  save_seq = 0;
static uint32_t save_ms = 0;


// FILE page state (programs from the USB stick); drawn further down.
#define FILE_ROWS  5

static int16_t  file_sel = -1;        // selected entry of the listing
static uint8_t  file_top = 0;         // first entry shown
static uint16_t file_seq = 0;         // list_seq the selection belongs to
static bool     file_view_job = false;   // the page shows a running job

static bool job_is_active(uint8_t s) { return s >= GJOB_OPENING && s <= GJOB_STOPPING; }

static void fmt_time(char *out, uint32_t ms)
{
  uint32_t s = ms / 1000;
  out[0] = 0;
  if (s >= 3600) { cat_uint(out, s / 3600); cat(out, ":"); s %= 3600; if (s / 60 < 10) { cat(out, "0"); } }
  cat_uint(out, s / 60);
  cat(out, ":");
  if (s % 60 < 10) { cat(out, "0"); }
  cat_uint(out, s % 60);
}


// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

static void place(uint8_t id, int x, int y, int w, int h, uint8_t pg)
{
  brect[id].x = x; brect[id].y = y; brect[id].w = w; brect[id].h = h; brect[id].page = pg;
}

static void layout_init(void)
{
  int i;
  // Right column.
  for (i = 0; i < 4; i++) { place(B_HOLD + i, RX, 4 + i * 118, RW, 112, PAGE_ALL); }
  // Tabs.
  for (i = 0; i < 6; i++) { place(B_TAB0 + i, LX + i * 114, TAB_Y, 110, TAB_H, PAGE_ALL); }
  // FILE page: the listing with its scroll buttons, the detail and START, and
  // the STOP of a running program.
  for (i = 0; i < 5; i++) { place(B_FROW0 + i, LX, PAGE_Y + i * 52, 376, 50, PAGE_FILE); }
  place(B_FPREV, 384, PAGE_Y, 90, 126, PAGE_FILE);
  place(B_FNEXT, 384, PAGE_Y + 134, 90, 126, PAGE_FILE);
  place(B_FSTART, 480, PAGE_Y + 164, 210, 96, PAGE_FILE);
  place(B_FFORMAT, LX + LW - 150, 326, 150, 46, PAGE_FILE);
  place(B_FSTOP, LX + 356, PAGE_Y + 200, 330, 110, PAGE_FILE);   // 330 x 110 fits the scratch buffer
  // DRO page: a zero button at the end of each axis row.
  for (i = 0; i < 4; i++) { place(B_ZERO0 + i, 560, PAGE_Y + i * 66, 130, 62, PAGE_DRO); }
  // JOG page: step sizes and speed on top, then the pad.
  for (i = 0; i < N_STEPS; i++) { place(B_STEP0 + i, LX + i * 94, PAGE_Y, 88, 56, PAGE_JOG); }
  place(B_SPEED, 474, PAGE_Y, 216, 56, PAGE_JOG);
  #define PAD_X(c) (LX + (c) * 94)
  #define PAD_Y(r) (124 + (r) * 84)
  place(B_JOG_YP, PAD_X(1), PAD_Y(0), 88, 78, PAGE_JOG);
  place(B_JOG_XN, PAD_X(0), PAD_Y(1), 88, 78, PAGE_JOG);
  place(B_HOME,   PAD_X(1), PAD_Y(1), 88, 78, PAGE_JOG);
  place(B_JOG_XP, PAD_X(2), PAD_Y(1), 88, 78, PAGE_JOG);
  place(B_JOG_YN, PAD_X(1), PAD_Y(2), 88, 78, PAGE_JOG);
  place(B_ZERO_ALL, PAD_X(0), PAD_Y(2), 88, 78, PAGE_JOG);
  place(B_JOG_ZP, PAD_X(3), PAD_Y(0), 88, 78, PAGE_JOG);
  place(B_JOG_ZN, PAD_X(3), PAD_Y(2), 88, 78, PAGE_JOG);
  place(B_JOG_AP, PAD_X(4), PAD_Y(0), 88, 78, PAGE_JOG);
  place(B_JOG_AN, PAD_X(4), PAD_Y(2), 88, 78, PAGE_JOG);
  // OVERRIDE page: four rows, a label panel and buttons.
  for (i = 0; i < 5; i++) { place(B_FEED_M10 + i, 160 + i * 107, PAGE_Y, 101, 72, PAGE_OVR); }
  for (i = 0; i < 3; i++) { place(B_RAPID_25 + i, 160 + i * 179, PAGE_Y + 78, 172, 72, PAGE_OVR); }
  for (i = 0; i < 3; i++) { place(B_SPIN_M10 + i, 160 + i * 96, PAGE_Y + 156, 90, 72, PAGE_OVR); }
  for (i = 0; i < 2; i++) { place(B_SPIN_STOP + i, 448 + i * 124, PAGE_Y + 156, 118, 72, PAGE_OVR); }
  for (i = 0; i < 2; i++) { place(B_FLOOD + i, 160 + i * 268, PAGE_Y + 234, 262, 72, PAGE_OVR); }
  // I/O page: the aux outputs, the collet and the reset pulses.
  for (i = 0; i < GSHM_N_AUX; i++) { place(B_AUX0 + i, LX + i * 70, IO_BTN_Y, 66, IO_BTN_H, PAGE_IO); }
  place(B_COLLET, LX + 420, IO_BTN_Y, 90, IO_BTN_H, PAGE_IO);
  place(B_SRV_RST, LX + 514, IO_BTN_Y, 80, IO_BTN_H, PAGE_IO);
  place(B_VFD_RST, LX + 598, IO_BTN_Y, 84, IO_BTN_H, PAGE_IO);
  // $$ page: the list with its scroll buttons ...
  for (i = 0; i < SET_ROWS; i++) { place(B_SROW0 + i, LX, PAGE_Y + i * 52, 586, 50, PAGE_SET); }
  place(B_SUP, 596, PAGE_Y, 94, 152, PAGE_SET);
  place(B_SDOWN, 596, PAGE_Y + 160, 94, 152, PAGE_SET);
  // ... and the editor's keypad, right of its description:
  //   7 8 9 DEL / 4 5 6 CLR / 1 2 3 . / 0 CANCEL SAVE
  #define KEY_X(c) (340 + (c) * 88)
  #define KEY_Y(r) (PAGE_Y + (r) * 80)
  static const uint8_t keys[3][3] = { { 7, 8, 9 }, { 4, 5, 6 }, { 1, 2, 3 } };
  for (int r = 0; r < 3; r++) {
    for (int c = 0; c < 3; c++) { place(B_K0 + keys[r][c], KEY_X(c), KEY_Y(r), 82, 72, PAGE_SET); }
  }
  place(B_KDEL, KEY_X(3), KEY_Y(0), 82, 72, PAGE_SET);
  place(B_KCLR, KEY_X(3), KEY_Y(1), 82, 72, PAGE_SET);
  place(B_KDOT, KEY_X(3), KEY_Y(2), 82, 72, PAGE_SET);
  place(B_K0, KEY_X(0), KEY_Y(3), 82, 72, PAGE_SET);
  place(B_KCANCEL, KEY_X(1), KEY_Y(3), 82, 72, PAGE_SET);
  place(B_KSAVE, KEY_X(2), KEY_Y(3), 170, 72, PAGE_SET);
  // The option buttons are placed when the editor opens (place_options()).
}

static set_info_t edit_info;          // the setting being edited

static bool btn_visible(uint8_t id)
{
  if (id == B_ZERO3) { return !a_slave(); }
  if (id >= B_SROW0 && id <= B_SROW5) { return edit_idx < 0 && list_top + (id - B_SROW0) < GSHM_N_SETTINGS; }
  if (id == B_SUP || id == B_SDOWN) { return edit_idx < 0; }
  if ((id >= B_FROW0 && id <= B_FFORMAT)) { return !file_view_job; }
  if (id == B_FSTOP) { return file_view_job; }
  if (id >= B_K0 && id <= B_KCLR) { return edit_idx >= 0 && set_numeric(edit_info.m.type); }
  if (id == B_KCANCEL || id == B_KSAVE) { return edit_idx >= 0; }
  if (id >= B_OPT0 && id <= B_OPT_LAST) {
    return edit_idx >= 0 && !set_numeric(edit_info.m.type) && (id - B_OPT0) < set_n_opts(&edit_info.m);
  }
  return true;
}

// Option buttons: four narrow ones per row for bits (seven small ones for the
// 22 inputs of $79, the last one left of CANCEL, and for the 14 outputs of
// $93), two wide ones for choices.
static void place_options(void)
{
  int n = set_n_opts(&edit_info.m);
  bool narrow = (edit_info.m.type == T_AXES || edit_info.m.type == T_AUX);
  bool tiny = (edit_info.m.type == T_INMASK || edit_info.m.type == T_OUT);
  for (int k = 0; k <= B_OPT_LAST - B_OPT0; k++) {
    if (k >= n) { place(B_OPT0 + k, 0, 0, 0, 0, PAGE_SET); continue; }
    if (tiny) { place(B_OPT0 + k, 340 + (k % 7) * 50, KEY_Y(k / 7), 46, 72, PAGE_SET); }
    else if (narrow) { place(B_OPT0 + k, KEY_X(k % 4), KEY_Y(k / 4), 82, 72, PAGE_SET); }
    else { place(B_OPT0 + k, KEY_X((k % 2) * 2), KEY_Y(k / 2), 170, 72, PAGE_SET); }
  }
}

static void editor_open(int idx)
{
  edit_idx = (int8_t)idx;
  set_info(idx, &edit_info);
  edit_buf[0] = 0;
  edit_bits = (uint32_t)(st.set_val[idx] + 0.5f);
  place_options();
  page_dirty = true;
}

static void editor_close(void)
{
  if (edit_idx < 0) { return; }
  edit_idx = -1;
  page_dirty = true;
}

static float edit_value(void)
{
  if (!set_numeric(edit_info.m.type)) { return (float)edit_bits; }
  float v = 0, scale = 0;
  for (const char *p = edit_buf; *p; p++) {
    if (*p == '.') { scale = 1; continue; }
    if (scale > 0) { scale /= 10; v += (*p - '0') * scale; }
    else { v = v * 10 + (*p - '0'); }
  }
  return v;
}

static bool edit_valid(void)
{
  if (!set_numeric(edit_info.m.type)) { return true; }
  for (const char *p = edit_buf; *p; p++) { if (*p >= '0' && *p <= '9') { return true; } }
  return false;
}


// ---------------------------------------------------------------------------
// What each button does
// ---------------------------------------------------------------------------

static uint16_t hold_time(uint8_t id)
{
  if (id == B_UNLOCK || id == B_SPIN_START) { return 500; }
  if (id == B_HOME || id == B_ZERO_ALL || id == B_FSTART) { return 1000; }
  if (id == B_FSTOP) { return 500; }
  if (id == B_FFORMAT) { return 3000; }
  if (id >= B_ZERO0 && id <= B_ZERO3) { return 800; }
  if (id >= B_AUX0 && id <= B_AUX5) { return 500; }
  if (id == B_COLLET) { return (st.flags & GSHM_F_COLLET) ? 0 : 600; }   // hold to open, tap to close
  if (id == B_SRV_RST || id == B_VFD_RST) { return 500; }
  return 0;
}

static bool autorepeat(uint8_t id)
{
  return id == B_FEED_M10 || id == B_FEED_M1 || id == B_FEED_P1 || id == B_FEED_P10 ||
         id == B_SPIN_M10 || id == B_SPIN_P10;
}

static bool is_jog(uint8_t id) { return id >= B_JOG_XP && id <= B_JOG_AN; }

// Whether the button can act now; if not, *why says so.
static bool btn_enabled(uint8_t id, const char **why)
{
  bool idle = st_is(GSHM_STATE_IDLE), alarm = st_is(GSHM_STATE_ALARM);
  const char *w = 0;
  if (id == B_UNLOCK) {
    if (!alarm) { w = "Unlock is for the alarm state"; }
    else if (estop_shown()) { w = "E-STOP: release it, then RESET"; }
    else if (drv_waiting()) { w = "Drives have no power yet"; }
  } else if (id == B_HOME) {
    if (!(st.flags & GSHM_F_HOMING)) { w = "Homing is disabled ($22=0)"; }
    else if (!idle && !alarm) { w = "Home only when idle or in alarm"; }
    else if (drv_waiting()) { w = "Drives have no power yet"; }
  } else if ((id >= B_ZERO0 && id <= B_ZERO3) || id == B_ZERO_ALL) {
    if (!idle) { w = "Zeroing only when idle"; }
  } else if (is_jog(id)) {
    if ((id == B_JOG_AP || id == B_JOG_AN) && a_slave()) { w = "A is a slave motor ($42)"; }
    else if (!idle && !st_is(GSHM_STATE_JOG)) { w = "Jog only when idle"; }
  } else if (id == B_SPIN_STOP) {
    // In a hold both send Grbl's spindle stop override, which toggles: each
    // only when its own way round applies. Idle: M5 / M3 (M4) with the S.
    if (st_hold()) {
      if (st.flags & GSHM_F_SPINDLE_STOP) { w = "Spindle stopped: START turns it on"; }
    } else if (idle) {
      // An M3 waiting for the speed is not modal yet, but already outputs.
      if (!st.spindle_mode && st.spindle_rpm <= 0.0f) { w = "The spindle is off"; }
    } else { w = "Spindle STOP when idle or in feed hold"; }
  } else if (id == B_SPIN_START) {
    if (st_hold()) {
      if (!(st.flags & GSHM_F_SPINDLE_STOP)) { w = "Spindle start after its STOP in hold"; }
    } else if (idle) {
      if (st.spindle_mode || st.spindle_rpm > 0.0f) { w = "The spindle is on"; }
      else if (st.flags & GSHM_F_COLLET) { w = "Collet open: close it first"; }
      else if (set_byte(32)) { w = "Not in laser mode ($32=1)"; }
    } else { w = "Spindle START when idle or in feed hold"; }
  } else if (id == B_FLOOD || id == B_MIST) {
    if (!idle && !st_is(GSHM_STATE_RUN) && !st_hold()) { w = "Coolant only when idle, running or held"; }
  } else if (id >= B_AUX0 && id <= B_AUX5) {
    if (!set_byte(84 + (id - B_AUX0))) { w = "No output assigned ($84-$89)"; }
  } else if (id == B_COLLET) {
    if (!set_byte(90)) { w = "No collet output ($90=0)"; }
    else if (!(st.flags & GSHM_F_COLLET) && !idle && !st_is(GSHM_STATE_HOLD0)) { w = "Collet only when idle or held"; }
  } else if (id == B_SRV_RST) {
    if (!set_byte(91)) { w = "No servo reset output ($91=0)"; }
  } else if (id == B_VFD_RST) {
    if (!set_byte(92)) { w = "No VFD reset output ($92=0)"; }
  } else if (id == B_FSTART) {
    grbl_job_t *jb = GRBL_JOB;
    if (file_sel < 0 || file_sel >= jb->n_files) { w = "Select a file first"; }
    else if (job_is_active(jb->job_state)) { w = "A program is running"; }
    else if (!idle) { w = "Start only when idle"; }
  } else if (id == B_FSTOP) {
    if (!job_is_active(GRBL_JOB->job_state)) { w = "No program running"; }
  } else if (id == B_FFORMAT) {
    grbl_job_t *jb = GRBL_JOB;
    if (jb->usb_state != GJOB_USB_READY && jb->usb_state != GJOB_USB_ERROR) { w = "No USB stick"; }
    else if (job_is_active(jb->job_state)) { w = "A program is running"; }
    else if (!idle && !alarm) { w = "Format only when idle"; }
  } else if (id == B_FPREV) {
    if (file_top == 0) { w = "This is the first page"; }
  } else if (id == B_FNEXT) {
    if (file_top + FILE_ROWS >= GRBL_JOB->n_files) { w = "This is the last page"; }
  } else if (id == B_SUP) {
    if (list_top == 0) { w = "This is the first page"; }
  } else if (id == B_SDOWN) {
    if (list_top + SET_ROWS >= GSHM_N_SETTINGS) { w = "This is the last page"; }
  } else if (id == B_KDOT) {
    if (edit_info.m.type != T_FLOAT || strchr(edit_buf, '.')) { w = "No decimal point here"; }
  } else if (id == B_KSAVE) {
    if (!idle && !alarm) { w = "Settings only when idle or in alarm"; }
    else if (save_pending) { w = "Saving..."; }
    else if (!edit_valid()) { w = "Type the new value first"; }
  }
  if (why) { *why = w; }
  return w == 0;
}

static void send_rt(uint8_t c)
{
  volatile grbl_shm_t *m = GRBL_SHM;
  uint8_t head = m->cmd_head;
  uint8_t next = (uint8_t)((head + 1) % sizeof(m->cmd));
  if (next == m->cmd_tail) { set_hint("Command queue full"); return; }
  m->cmd[head] = c;
  __DMB();
  m->cmd_head = next;
}

static bool send_req(uint8_t code, uint8_t axis, int8_t dir, float value)
{
  volatile grbl_shm_t *m = GRBL_SHM;
  if (m->req_seq != m->req_done) { set_hint("Grbl is busy - try again"); return false; }
  m->req_code = code;
  m->req_axis = axis;
  m->req_dir = dir;
  m->req_value = value;
  m->req_speed = speeds[speed_sel];
  m->req_ms = millis();
  __DMB();
  m->req_seq = (uint8_t)(m->req_seq + 1);
  return true;
}

static void jog_axis(uint8_t id, uint8_t *axis, int8_t *dir)
{
  static const uint8_t ax[8] = { 0, 0, 1, 1, 2, 2, 3, 3 };
  *axis = ax[id - B_JOG_XP];
  *dir = ((id - B_JOG_XP) & 1) ? -1 : 1;
}

static void jog_start(uint8_t id)
{
  uint8_t axis; int8_t dir;
  jog_axis(id, &axis, &dir);
  volatile grbl_shm_t *m = GRBL_SHM;
  if (step_sel == STEP_CONT) {
    m->jog_hold = 1;
    m->jog_alive = m->jog_alive + 1;
    jog_cont = true;
    send_req(GSHM_REQ_JOG, axis, dir, 0.0f);
  } else {
    float d;
    if (axis == 3 && !linear_axis(3)) {
      d = st.report_inches ? step_in[step_sel] : step_mm[step_sel];   // degrees, as labelled
    } else {
      d = st.report_inches ? step_in[step_sel] * 25.4f : step_mm[step_sel];
    }
    send_req(GSHM_REQ_JOG, axis, dir, d);
  }
}

static void jog_stop(void)
{
  if (!jog_cont) { return; }
  jog_cont = false;
  GRBL_SHM->jog_hold = 0;
  send_rt(RT_JOG_CANCEL);
}

// Finger down on an enabled button.
static void btn_down(uint8_t id)
{
  switch (id) {
    case B_HOLD:  send_rt(RT_FEED_HOLD); break;
    case B_START: send_rt(RT_CYCLE_START); break;
    case B_RESET: send_rt(RT_RESET); break;
    case B_TAB0: case B_TAB1: case B_TAB2: case B_TAB3: case B_TAB4: case B_TAB5:
      if (page != id - B_TAB0) { page = id - B_TAB0; page_dirty = true; }
      else if (id == B_TAB4) { editor_close(); }      // the $$ tab again: back to the list
      break;
    case B_SROW0: case B_SROW1: case B_SROW2: case B_SROW3: case B_SROW4: case B_SROW5:
      editor_open(list_top + (id - B_SROW0));
      break;
    case B_SUP:   list_top = (uint8_t)(list_top - SET_ROWS); break;
    case B_FROW0: case B_FROW1: case B_FROW2: case B_FROW3: case B_FROW4:
      file_sel = file_top + (id - B_FROW0);
      file_seq = GRBL_JOB->list_seq;
      break;
    case B_FPREV: file_top = (uint8_t)(file_top - FILE_ROWS); break;
    case B_FNEXT: file_top = (uint8_t)(file_top + FILE_ROWS); break;
    case B_SDOWN: list_top = (uint8_t)(list_top + SET_ROWS); break;
    case B_K0: case B_K1: case B_K2: case B_K3: case B_K4:
    case B_K5: case B_K6: case B_K7: case B_K8: case B_K9: case B_KDOT: {
      size_t n = strlen(edit_buf);
      if (n < 10) { edit_buf[n] = (id == B_KDOT) ? '.' : (char)('0' + (id - B_K0)); edit_buf[n + 1] = 0; }
      break;
    }
    case B_KDEL: { size_t n = strlen(edit_buf); if (n) { edit_buf[n - 1] = 0; } break; }
    case B_KCLR: edit_buf[0] = 0; break;
    case B_KCANCEL: editor_close(); break;
    case B_KSAVE:
      if (send_req(GSHM_REQ_SETTING, edit_info.m.num, 0, edit_value())) {
        save_pending = true;
        save_seq = GRBL_SHM->req_seq;
        save_ms = millis();
      }
      break;
    case B_COLLET: send_req(GSHM_REQ_COLLET, 0, 0, 0.0f); break;   // open: a tap closes it
    case B_STEP0: case B_STEP1: case B_STEP2: case B_STEP3: case B_STEP4:
      step_sel = id - B_STEP0;
      break;
    case B_SPEED: speed_sel = (uint8_t)((speed_sel + 1) % sizeof(speeds)); break;
    case B_FEED_M10: send_rt(RT_FEED_M10); break;
    case B_FEED_M1:  send_rt(RT_FEED_M1); break;
    case B_FEED_100: send_rt(RT_FEED_100); break;
    case B_FEED_P1:  send_rt(RT_FEED_P1); break;
    case B_FEED_P10: send_rt(RT_FEED_P10); break;
    case B_RAPID_25: send_rt(RT_RAPID_25); break;
    case B_RAPID_50: send_rt(RT_RAPID_50); break;
    case B_RAPID_100: send_rt(RT_RAPID_100); break;
    case B_SPIN_M10: send_rt(RT_SPIN_M10); break;
    case B_SPIN_100: send_rt(RT_SPIN_100); break;
    case B_SPIN_P10: send_rt(RT_SPIN_P10); break;
    case B_SPIN_STOP:
      if (st_hold()) { send_rt(RT_SPIN_STOP); }
      else { send_rt(GSHM_CMD_SPINDLE_OFF); }   // idle: M5, also while a START still waits
      break;
    case B_FLOOD: send_rt(RT_FLOOD); break;
    case B_MIST:  send_rt(RT_MIST); break;
    default:
      if (is_jog(id)) { jog_start(id); }
      else if (id >= B_OPT0 && id <= B_OPT_LAST) {
        int k = id - B_OPT0;
        if (set_toggles(edit_info.m.type)) { edit_bits ^= (1u << k); }
        else { edit_bits = (uint32_t)k; }
      }
      break;
  }
}

// A hold button held long enough.
static void btn_long(uint8_t id)
{
  if (id == B_UNLOCK) { send_req(GSHM_REQ_UNLOCK, 0, 0, 0.0f); }
  else if (id == B_HOME) { send_req(GSHM_REQ_HOME, 0, 0, 0.0f); }
  else if (id == B_ZERO_ALL) { send_req(GSHM_REQ_ZERO_ALL, 0, 0, 0.0f); }
  else if (id >= B_ZERO0 && id <= B_ZERO3) { send_req(GSHM_REQ_ZERO, id - B_ZERO0, 0, 0.0f); }
  else if (id >= B_AUX0 && id <= B_AUX5) { send_rt(GSHM_CMD_AUX_TOGGLE + (id - B_AUX0)); }
  else if (id == B_SPIN_START) {
    if (st_hold()) { send_rt(RT_SPIN_STOP); }                   // the stop override again: restores
    else { send_req(GSHM_REQ_SPINDLE, 0, 0, 1.0f); }            // idle: M3 / M4 with the S
  }
  else if (id == B_COLLET) { send_req(GSHM_REQ_COLLET, 0, 0, 1.0f); }
  else if (id == B_FSTART) { send_req(GSHM_REQ_JOB_START, (uint8_t)file_sel, 0, (float)file_seq); }
  else if (id == B_FSTOP) { send_req(GSHM_REQ_JOB_STOP, 0, 0, 0.0f); }
  else if (id == B_FFORMAT) { send_req(GSHM_REQ_USB_FORMAT, 0, 0, 0.0f); }
  else if (id == B_SRV_RST) { send_req(GSHM_REQ_PULSE, 11, 0, 0.0f); }
  else if (id == B_VFD_RST) { send_req(GSHM_REQ_PULSE, 12, 0, 0.0f); }
}

// Finger lifted (or slid off) a button.
static void btn_up(uint8_t id, bool slid_off)
{
  if (is_jog(id)) { jog_stop(); return; }
  if (!slid_off && hold_time(id) && !t_fired) {
    if (id == B_UNLOCK) { set_hint("Hold UNLOCK to unlock"); }
    else if (id == B_HOME) { set_hint("Hold HOME for 1 s to home"); }
    else if (id == B_FFORMAT) { set_hint("Hold FORMAT 3 s: erases the whole stick"); }
    else { set_hint("Hold the button until the bar is full"); }
  }
}


// ---------------------------------------------------------------------------
// Touch input
// ---------------------------------------------------------------------------

static bool inside(uint8_t id, int x, int y, int slack)
{
  const rect_t &r = brect[id];
  return x + slack >= r.x && x < r.x + r.w + slack && y + slack >= r.y && y < r.y + r.h + slack;
}

static uint8_t hit(int x, int y)
{
  for (uint8_t id = 1; id < B_COUNT; id++) {
    if (brect[id].w == 0) { continue; }
    if (brect[id].page != PAGE_ALL && brect[id].page != page) { continue; }
    if (brect[id].page != PAGE_ALL && estop_shown()) { continue; }   // under the E-STOP banner
    if (!btn_visible(id)) { continue; }
    if (inside(id, x, y, 4)) { return id; }
  }
  return B_NONE;
}

static void touch_press(uint16_t x, uint16_t y, uint32_t now)
{
  volatile grbl_shm_t *m = GRBL_SHM;
  t_down = true;
  t_fired = false;
  t_press_ms = now;
  t_repeat_ms = now;
  m->touch_x = x;
  m->touch_y = y;
  m->touch_count = m->touch_count + 1;
  t_active = live ? hit(x, y) : B_NONE;
  if (t_active == B_NONE) { return; }
  const char *why;
  if (!btn_enabled(t_active, &why)) {
    set_hint(why);
    t_active = B_NONE;
    return;
  }
  if (!hold_time(t_active)) { btn_down(t_active); }
}

static void touch_held(uint32_t now)
{
  if (t_active == B_NONE) { return; }
  if (jog_cont) { GRBL_SHM->jog_alive = GRBL_SHM->jog_alive + 1; }
  uint16_t ht = hold_time(t_active);
  if (ht && !t_fired && (uint32_t)(now - t_press_ms) >= ht) {
    t_fired = true;
    if (btn_enabled(t_active, 0)) { btn_long(t_active); }
  }
  if (autorepeat(t_active) && (uint32_t)(now - t_press_ms) >= REPEAT_DELAY_MS &&
      (uint32_t)(now - t_repeat_ms) >= REPEAT_MS) {
    t_repeat_ms = now;
    btn_down(t_active);
  }
}

static void touch_move(uint16_t x, uint16_t y, uint32_t now)
{
  if (t_active != B_NONE && !inside(t_active, x, y, 20)) {
    btn_up(t_active, true);
    t_active = B_NONE;
  }
  touch_held(now);
}

static void touch_release(void)
{
  t_down = false;
  if (t_active != B_NONE) { btn_up(t_active, false); }
  t_active = B_NONE;
}

static void touch_poll(uint32_t now)
{
  volatile grbl_shm_t *m = GRBL_SHM;
  uint16_t x = 0, y = 0;
  int n;

  // Simulated presses ($TOUCH) queue up, each starting after its delay and
  // after the one before it, and replace the panel while they last.
  uint32_t sseq = m->sim_seq;
  if (sseq != sim_seq_seen) {
    sim_seq_seen = sseq;
    if (sim_n < SIM_QUEUE) {
      sim_q[sim_n].x = m->sim_x;
      sim_q[sim_n].y = m->sim_y;
      sim_q[sim_n].ms = m->sim_ms;
      sim_q[sim_n].start = now + m->sim_delay;
      sim_n++;
    }
    m->sim_ack = sseq;
  }
  if (!sim_on && sim_n && (int32_t)(now - sim_q[0].start) >= 0) {
    if (t_down) { touch_release(); }
    sim_on = true;
    sim_x = sim_q[0].x;
    sim_y = sim_q[0].y;
    sim_until = now + sim_q[0].ms;
    for (uint8_t i = 1; i < sim_n; i++) { sim_q[i - 1] = sim_q[i]; }
    sim_n--;
  }
  if (sim_on) {
    // The panel is still read (and its reports thrown away), so that a
    // simulated press takes the same time as a real one - the I2C transfer
    // is what once made a real press look older than a simulated one.
    uint16_t dx, dy;
    touch_read(&dx, &dy);
    if ((int32_t)(now - sim_until) < 0) { n = 1; x = sim_x; y = sim_y; }
    else { sim_on = false; n = 0; }
  } else {
    n = touch_read(&x, &y);
  }

  if (n < 0) {
    // No new report. While pressed the GT911 reports every scan, so a long
    // silence means the finger has gone.
    if (t_down && (uint32_t)(now - t_last_ms) > RELEASE_MS) { touch_release(); }
    else if (t_down) { touch_held(now); }
    return;
  }
  t_last_ms = now;
  if (n > 0) {
    if (!t_down) { touch_press(x, y, now); }
    else { touch_move(x, y, now); }
  } else if (t_down) {
    touch_release();
  }
}


// ---------------------------------------------------------------------------
// Drawing: buttons
// ---------------------------------------------------------------------------

enum { IC_NONE, IC_PAUSE, IC_PLAY, IC_STOP, IC_LOCK, IC_UP, IC_DOWN };

static void draw_icon(int icon, int cx, int cy, uint16_t c, uint16_t bg)
{
  switch (icon) {
    case IC_PAUSE:
      cv_fill(cx - 11, cy - 13, 8, 26, c);
      cv_fill(cx + 3, cy - 13, 8, 26, c);
      break;
    case IC_PLAY:
      for (int i = 0; i < 24; i++) { int hh = 28 * (24 - i) / 24; cv_fill(cx - 10 + i, cy - hh / 2, 1, hh, c); }
      break;
    case IC_STOP:
      cv_fill(cx - 12, cy - 12, 24, 24, c);
      break;
    case IC_LOCK:
      cv_disc(cx, cy - 5, 10, c, bg);
      cv_disc(cx, cy - 5, 6, bg, c);
      cv_fill(cx - 13, cy - 2, 26, 16, c);
      break;
    case IC_UP:
      for (int r = 0; r < 14; r++) { cv_fill(cx - r - 1, cy - 7 + r, 2 * r + 2, 1, c); }
      break;
    case IC_DOWN:
      for (int r = 0; r < 14; r++) { cv_fill(cx - r - 1, cy + 6 - r, 2 * r + 2, 1, c); }
      break;
  }
}

// Knocks the corners out, for a slightly rounded look.
static void round_corners(int w, int h)
{
  static const uint8_t cut[3] = { 3, 1, 1 };
  for (int i = 0; i < 3; i++) {
    cv_fill(0, i, cut[i], 1, C_BG);
    cv_fill(w - cut[i], i, cut[i], 1, C_BG);
    cv_fill(0, h - 1 - i, cut[i], 1, C_BG);
    cv_fill(w - cut[i], h - 1 - i, cut[i], 1, C_BG);
  }
}

static void draw_btn(uint8_t id, const char *label, const font_t *f, const char *sub,
                     uint16_t bg, uint16_t fg, int icon)
{
  const rect_t &r = brect[id];
  bool pressed = (t_active == id);
  int prog = -1;
  uint16_t ht = hold_time(id);
  if (pressed && ht) {
    uint32_t held = millis() - t_press_ms;
    prog = t_fired ? 100 : (int)(held * 100 / ht);
    if (prog > 100) { prog = 100; }
  }
  uint16_t b = pressed ? mix(bg, RGB(255, 255, 255), 64) : bg;
  uint16_t v[4] = { b, fg, (uint16_t)prog, (uint16_t)icon };
  uint32_t sig = fnv(fnv_s(fnv_s(id, label), sub ? sub : ""), v, sizeof(v));
  if (!changed_btn(id, sig)) { return; }

  cv_begin(r.x, r.y, r.w, r.h, b);
  round_corners(r.w, r.h);
  int lh = f->h + (sub ? 2 + font_small.h : 0) + (icon ? 36 : 0);
  int y = (r.h - lh) / 2;
  if (icon) { draw_icon(icon, r.w / 2, y + 15, fg, b); y += 36; }
  text_center(0, y, r.w, f, label, fg, b);
  if (sub) { text_center(0, y + f->h + 2, r.w, &font_small, sub, mix(fg, b, 96), b); }
  if (prog >= 0) {
    cv_fill(4, r.h - 9, r.w - 8, 5, mix(b, RGB(0, 0, 0), 96));
    cv_fill(4, r.h - 9, (r.w - 8) * prog / 100, 5, fg);
  }
  cv_end();
}

static void draw_btn_hidden(uint8_t id)
{
  if (!changed_btn(id, 0xFFFFFFFFUL)) { return; }
  const rect_t &r = brect[id];
  cv_screen_fill(r.x, r.y, r.w, r.h, C_BG);
}

// A plain button: normal, lit (selected / on) or disabled.
static void draw_plain(uint8_t id, const char *label, const font_t *f, const char *sub,
                       bool lit, uint16_t lit_bg, uint16_t lit_fg, uint16_t fg)
{
  if (!btn_enabled(id, 0)) { draw_btn(id, label, f, sub, C_PANEL, C_FAINT, IC_NONE); return; }
  if (lit) { draw_btn(id, label, f, sub, lit_bg, lit_fg, IC_NONE); return; }
  draw_btn(id, label, f, sub, C_BTN, fg, IC_NONE);
}

static void draw_right(void)
{
  bool hold = st_hold();
  draw_btn(B_HOLD, "HOLD", &font_med, 0, hold ? RGB(255, 196, 40) : C_HOLD_BG, RGB(20, 20, 20), IC_PAUSE);
  bool resumable = st_is(GSHM_STATE_HOLD0) || (st_is(GSHM_STATE_DOOR) && st.door_sub == 0);
  draw_btn(B_START, "START", &font_med, 0, resumable ? RGB(50, 200, 100) : C_START_BG, C_TEXT, IC_PLAY);
  draw_btn(B_RESET, "RESET", &font_med, 0, C_RESET_BG, C_TEXT, IC_STOP);
  if (btn_enabled(B_UNLOCK, 0)) { draw_btn(B_UNLOCK, "UNLOCK", &font_med, "hold", C_UNLOCK_BG, C_TEXT, IC_LOCK); }
  else { draw_btn(B_UNLOCK, "UNLOCK", &font_med, 0, C_PANEL, C_FAINT, IC_LOCK); }
}

static void draw_tabs(void)
{
  static const char *names[6] = { "DRO", "JOG", "OVR", "I/O", "$$", "FILE" };
  for (int i = 0; i < 6; i++) {
    if (page == i) { draw_btn(B_TAB0 + i, names[i], &font_big, 0, C_SEL, C_DARK, IC_NONE); }
    else { draw_btn(B_TAB0 + i, names[i], &font_big, 0, C_BTN, C_DIM, IC_NONE); }
  }
}


// ---------------------------------------------------------------------------
// Drawing: top bar and message line (every page)
// ---------------------------------------------------------------------------

static void draw_state(void)
{
  const char *txt = "IDLE";
  uint16_t bg = RGB(40, 160, 90), fg = C_TEXT;
  switch (st.state) {
    case GSHM_STATE_RUN:   txt = "RUN";     bg = RGB(30, 120, 220); break;
    case GSHM_STATE_HOLD0: txt = "HOLD";    bg = RGB(235, 165, 20); fg = RGB(20, 20, 20); break;
    case GSHM_STATE_HOLD1: txt = "HOLD..."; bg = RGB(235, 165, 20); fg = RGB(20, 20, 20); break;
    case GSHM_STATE_JOG:   txt = "JOG";     bg = RGB(0, 160, 180); break;
    case GSHM_STATE_HOME:  txt = "HOMING";  bg = RGB(140, 90, 220); break;
    case GSHM_STATE_ALARM: txt = "ALARM";   bg = C_RED; break;
    case GSHM_STATE_CHECK: txt = "CHECK";   bg = RGB(100, 104, 115); break;
    case GSHM_STATE_DOOR:  txt = "DOOR";    bg = RGB(230, 110, 20); break;
    case GSHM_STATE_SLEEP: txt = "SLEEP";   bg = RGB(70, 70, 80); break;
  }
  if (!changed(W_STATE, fnv_s(0, txt))) { return; }
  cv_begin(LX, TOP_Y, 150, TOP_H, bg);
  text_center(0, (TOP_H - font_big.h) / 2, 150, &font_big, txt, fg, bg);
  cv_end();
}

static void draw_modal(void)
{
  static const char *wcs[] = { "G54", "G55", "G56", "G57", "G58", "G59" };
  char l1[48] = "", l2[48] = "";
  switch (st.motion) {
    case 0: cat(l1, "G0 "); break;
    case 1: cat(l1, "G1 "); break;
    case 2: cat(l1, "G2 "); break;
    case 3: cat(l1, "G3 "); break;
    case 80: cat(l1, "G80 "); break;
    default: cat(l1, "G38 "); break;
  }
  cat(l1, st.coord_select < 6 ? wcs[st.coord_select] : "G5?");
  cat(l1, st.distance_inc ? " G91" : " G90");
  cat(l1, st.units_inch ? " G20" : " G21");
  cat(l1, st.plane == 0 ? " G17" : (st.plane == 1 ? " G18" : " G19"));
  cat(l2, "T"); cat_uint(l2, st.tool);
  cat(l2, st.spindle_mode == 1 ? "  M3" : (st.spindle_mode == 2 ? "  M4" : "  M5"));
  if (st.coolant & 2) { cat(l2, "  M7"); }
  if (st.coolant & 1) { cat(l2, "  M8"); }
  if (!st.coolant) { cat(l2, "  M9"); }
  if (!changed(W_MODAL, fnv_s(fnv_s(0, l1), l2))) { return; }
  cv_begin(160, TOP_Y, 296, TOP_H, C_PANEL);
  cv_text(10, 3, &font_med, l1, C_TEXT, C_PANEL);
  cv_text(10, 27, &font_med, l2, C_DIM, C_PANEL);
  cv_end();
}

// Two lines: the USB cable to the PC (with the line number of a program the
// PC streams, if it has N words), and the USB stick - none, mounted and how,
// one that cannot be mounted, or the program running from it.
static void draw_link(void)
{
  char l1[32] = "", l2[32] = "";
  if (!st.usb_connected) { strcpy(l1, "USB idle"); }
  else if (st.line_number > 0) { strcpy(l1, "USB line "); cat_uint(l1, (uint32_t)st.line_number); }
  else { strcpy(l1, "USB connected"); }

  grbl_job_t *jb = GRBL_JOB;
  uint16_t c2 = C_LED_OFF;
  if (jb->magic != GRBL_JOB_MAGIC || jb->usb_state == GJOB_USB_OFF) {
    strcpy(l2, "Stick ...");
  } else if (jb->job_state == GJOB_CHECK || jb->job_state == GJOB_RUN || jb->job_state == GJOB_STOPPING) {
    // A program from the stick: its progress.
    uint32_t pm = jb->job_size ? (uint32_t)((uint64_t)(jb->job_pos > jb->job_size ? jb->job_size : jb->job_pos) * 1000ULL / jb->job_size) : 0;
    c2 = (jb->job_state == GJOB_RUN) ? C_GREEN : C_AMBER;
    if (jb->job_state == GJOB_CHECK) { strcpy(l2, "check "); }
    cat_uint(l2, pm / 10); cat(l2, "%  L"); cat_uint(l2, jb->job_line);
  } else switch (jb->usb_state) {
    case GJOB_USB_NONE: strcpy(l2, "No USB stick"); break;
    case GJOB_USB_MOUNTING: c2 = C_AMBER; strcpy(l2, "Stick mounting"); break;
    case GJOB_USB_FORMATTING: c2 = C_AMBER; strcpy(l2, "Formatting..."); break;
    case GJOB_USB_READY:
      c2 = C_GREEN;
      strcpy(l2, jb->usb_fat == 4 ? "Stick exFAT" : jb->usb_fat == 3 ? "Stick FAT32" : "Stick FAT16");
      break;
    default:
      c2 = C_RED;
      strcpy(l2, jb->usb_fs == GJOB_FS_NTFS ? "Stick NTFS" : "Stick error");
  }
  if (!changed(W_LINK, fnv_s(fnv_s(st.usb_connected | (c2 << 1), l1), l2))) { return; }
  cv_begin(462, TOP_Y, 228, TOP_H, C_PANEL);
  cv_disc(16, 14, 7, st.usb_connected ? C_GREEN : C_LED_OFF, C_PANEL);
  cv_text(30, 3, &font_med, l1, st.usb_connected ? C_TEXT : C_DIM, C_PANEL);
  cv_disc(16, 38, 7, c2, C_PANEL);
  cv_text(30, 27, &font_med, l2, (c2 == C_LED_OFF) ? C_DIM : C_TEXT, C_PANEL);
  cv_end();
}

static void draw_message(void)
{
  char msg[96] = "";
  uint16_t fg = C_DIM;
  bool err_fresh = st.last_error && (uint32_t)(st.m7_ms - st.error_ms) < ERROR_SHOW_MS;
  if (hint[0] && (int32_t)(millis() - hint_until) < 0) {
    fg = C_SEL;
    cat(msg, hint);
  } else if (estop_shown()) {
    fg = C_RED;
    cat(msg, (st.flags & GSHM_F_ESTOP_IN) ? "E-STOP active - release it, re-arm the safety relay"
                                          : "E-STOP released - press RESET, then HOME");
  } else if ((GRBL_JOB->job_state >= GJOB_DONE) && GRBL_JOB->job_end_ms &&
             (uint32_t)(st.m7_ms - GRBL_JOB->job_end_ms) < ERROR_SHOW_MS && !(st.state == GSHM_STATE_ALARM && st.alarm != 21)) {
    grbl_job_t *jb = GRBL_JOB;
    fg = (jb->job_state == GJOB_DONE) ? C_GREEN : C_AMBER;
    cat(msg, "USB program ");
    cat(msg, jb->job_state == GJOB_DONE ? "done" : jb->job_state == GJOB_FAILED ? "failed" : "stopped");
    if (jb->job_err) {
      cat(msg, ": error:"); cat_uint(msg, jb->job_err); cat(msg, " line "); cat_uint(msg, jb->job_err_line);
      cat(msg, " "); cat(msg, error_text(jb->job_err));
    } else if (jb->job_alarm) {
      cat(msg, ": ALARM:"); cat_uint(msg, jb->job_alarm); cat(msg, " at line "); cat_uint(msg, jb->job_line);
    } else if (jb->job_state == GJOB_DONE) {
      char a[16]; fmt_time(a, jb->job_end_ms - jb->job_start_ms);
      cat(msg, " in "); cat(msg, a);
    } else {
      cat(msg, " at line "); cat_uint(msg, jb->job_line);
    }
  } else if (st.flags & GSHM_F_TOOL_CHANGE) {
    fg = C_AMBER;
    cat(msg, "Tool change T"); cat_uint(msg, st.tool); cat(msg, ": I/O page COLLET, then START");
  } else if (st.state == GSHM_STATE_ALARM) {
    fg = C_RED;
    if (st.drv_power == GSHM_DRV_OFF) {
      cat(msg, "Drives have no power - switch them on");
    } else if (st.drv_power == GSHM_DRV_RETURNING) {
      fg = C_AMBER;
      cat(msg, "Drive power back - wait for the drives ($95)");
    } else if (st.alarm) {
      cat(msg, "ALARM:"); cat_uint(msg, st.alarm); cat(msg, "  "); cat(msg, alarm_text(st.alarm));
    } else {
      cat(msg, "Locked - HOME (JOG page) or UNLOCK");
    }
  } else if (err_fresh) {
    fg = C_AMBER;
    cat(msg, "error:"); cat_uint(msg, st.last_error); cat(msg, "  "); cat(msg, error_text(st.last_error));
  } else if (st.state == GSHM_STATE_DOOR) {
    fg = C_AMBER;
    cat(msg, "Safety door open - close it and press START");
  } else {
    cat(msg, "Grbl 1.1h  GIGA R1  A: ");
    cat(msg, st.a_mode == 0 ? "rotary" : st.a_mode == 1 ? "linear" : st.a_mode == 2 ? "slave of X" : "slave of Y");
    if (st.kinematics) { cat(msg, "  CoreXY"); }
    if (touch_state != TOUCH_OK) { cat(msg, "  no touch"); }
  }
  if (!changed(W_MSG, fnv_s(fg, msg))) { return; }
  cv_begin(LX, MSG_Y, LW, MSG_H, C_BG);
  cv_text(4, 3, &font_med, msg, fg, C_BG);
  cv_end();
}


// ---------------------------------------------------------------------------
// Drawing: DRO page
// ---------------------------------------------------------------------------

static void draw_dro(int i)
{
  const int x = LX, w = 552, h = 62, y = PAGE_Y + i * 66;
  bool slave = (i == 3) && a_slave();
  bool inch = st.report_inches && linear_axis(i);
  int dec = inch ? 4 : 3;
  float scale = inch ? (1.0f / 25.4f) : 1.0f;
  char wpos[24], mpos[24], field[24], mfield[24];
  fmt_fixed(wpos, (st.mpos[i] - st.wco[i]) * scale, dec);
  fmt_fixed(mpos, st.mpos[i] * scale, dec);
  rpad(field, wpos, 9);
  rpad(mfield, mpos, 10);
  const char *unit = !linear_axis(i) ? "deg" : (inch ? "in" : "mm");

  uint32_t brk = (i == 2) ? ((set_byte(59) ? 2u : 0u) | ((st.outputs & GSHM_OUT_BRAKE) ? 1u : 0u)) : 0u;
  uint32_t sig = fnv_s(fnv_s(fnv_s((slave ? 1u : 0u) | (brk << 4), field), mfield), unit);
  if (!changed(W_DRO0 + i, sig)) { return; }

  uint16_t col = slave ? C_FAINT : axis_color[i];
  cv_begin(x, y, w, h, C_PANEL);
  cv_fill(0, 0, 5, h, col);
  char letter[2] = { axis_letter[i], 0 };
  cv_text(16, (h - font_dro.h) / 2, &font_dro, letter, col, C_PANEL);
  if (slave) {
    const char *t = (st.a_mode == 2) ? "second X motor (slave)" : "second Y motor (slave)";
    cv_text(66, (h - font_med.h) / 2, &font_med, t, C_DIM, C_PANEL);
  } else {
    cv_text(384 - cv_text_width(&font_dro, field), (h - font_dro.h) / 2, &font_dro, field, C_TEXT, C_PANEL);
    cv_text(396, 8, &font_med, unit, C_DIM, C_PANEL);
    cv_text(396, 38, &font_small, "M", C_FAINT, C_PANEL);
    cv_text(406, 38, &font_small, mfield, C_DIM, C_PANEL);
    if (i == 2 && set_byte(59)) {
      bool held = !(st.outputs & GSHM_OUT_BRAKE);
      cv_text(470, 10, &font_small, held ? "BRAKED" : "FREE", held ? C_AMBER : C_FAINT, C_PANEL);
    }
  }
  cv_end();
}

static void draw_zero(int i)
{
  if (!btn_visible(B_ZERO0 + i)) { draw_btn_hidden(B_ZERO0 + i); return; }
  char label[4] = { axis_letter[i], '=', '0', 0 };
  draw_plain(B_ZERO0 + i, label, &font_big, "hold", false, 0, 0, axis_color[i]);
}

static void ovr_suffix(char *s, uint8_t v)
{
  cat(s, "OVR "); cat_uint(s, v); cat(s, "%");
}

static void draw_summary(void)
{
  bool inch = st.report_inches;
  float sc = inch ? (1.0f / 25.4f) : 1.0f;
  char v[16], line[48] = "", ovr[16] = "";
  fmt_fixed(v, st.feed * sc, inch ? 1 : 0);
  cat(line, v); cat(line, "   F");
  fmt_fixed(v, st.feed_prog * sc, inch ? 1 : 0);
  cat(line, v);
  ovr_suffix(ovr, st.ovr_feed);
  if (changed(W_SUM_FEED, fnv_s(fnv_s(inch, line), ovr))) {
    cv_begin(LX, 326, 340, 46, C_PANEL);
    cv_text(10, 3, &font_small, inch ? "FEED  in/min" : "FEED  mm/min", C_DIM, C_PANEL);
    cv_text(330 - cv_text_width(&font_small, ovr), 3, &font_small, ovr, st.ovr_feed == 100 ? C_DIM : C_AMBER, C_PANEL);
    cv_text(10, 21, &font_med, line, C_TEXT, C_PANEL);
    cv_end();
  }

  // With the VFD over RS485 the drive's output frequency gives the actual
  // speed: "actual / set"; the PWM duty means nothing then.
  char sl[48] = "";
  bool actual = st.spindle_actual >= 0.0f;
  if (actual) { fmt_fixed(v, st.spindle_actual, 0); cat(sl, v); cat(sl, " / "); }
  fmt_fixed(v, st.spindle_rpm, 0);
  cat(sl, v);
  cat(sl, st.spindle_mode == 1 ? " CW" : (st.spindle_mode == 2 ? " CCW" : " OFF"));
  cat(sl, "  S"); fmt_fixed(v, st.spindle_prog, 0); cat(sl, v);
  if (!actual) { cat(sl, "  PWM "); cat_uint(sl, st.pwm_permille / 10); cat(sl, "%"); }
  char so[16] = "";
  ovr_suffix(so, st.ovr_spindle);
  static const char *const sp_status[5] = { "", "SPIN-UP", "AT SPEED", "SPEED LOST", "VFD FAULT" };
  const char *sps = (st.spindle_status < 5) ? sp_status[st.spindle_status] : "";
  if (st.flags & GSHM_F_COLLET) { sps = "COLLET OPEN"; }
  uint16_t spc = (st.spindle_status >= 3 || (st.flags & GSHM_F_COLLET)) ? C_RED :
                 (st.spindle_status == 1) ? C_AMBER : C_GREEN;
  if (changed(W_SUM_SPINDLE, fnv_s(fnv_s(fnv_s(st.spindle_mode, sl), so), sps))) {
    cv_begin(350, 326, 340, 46, C_PANEL);
    cv_text(10, 3, &font_small, "SPINDLE  rpm", C_DIM, C_PANEL);
    cv_text(140, 3, &font_small, sps, spc, C_PANEL);
    cv_text(330 - cv_text_width(&font_small, so), 3, &font_small, so, st.ovr_spindle == 100 ? C_DIM : C_AMBER, C_PANEL);
    cv_text(10, 21, &font_med, sl, st.spindle_mode ? C_TEXT : C_DIM, C_PANEL);
    cv_end();
  }
}

static void draw_page_dro(void)
{
  for (int i = 0; i < 4; i++) { draw_dro(i); draw_zero(i); }
  draw_summary();
}


// ---------------------------------------------------------------------------
// Drawing: JOG page
// ---------------------------------------------------------------------------

static void draw_jog_dro(int i)
{
  const int x = 474, y = 124 + i * 62, w = 216, h = 58;
  bool slave = (i == 3) && a_slave();
  bool inch = st.report_inches && linear_axis(i);
  int dec = inch ? 4 : 3;
  float scale = inch ? (1.0f / 25.4f) : 1.0f;
  char wpos[24], mpos[24], mline[32] = "M ";
  fmt_fixed(wpos, (st.mpos[i] - st.wco[i]) * scale, dec);
  fmt_fixed(mpos, st.mpos[i] * scale, dec);
  cat(mline, mpos);
  if (!changed(W_JDRO0 + i, fnv_s(fnv_s(slave, wpos), mline))) { return; }
  uint16_t col = slave ? C_FAINT : axis_color[i];
  cv_begin(x, y, w, h, C_PANEL);
  cv_fill(0, 0, 4, h, col);
  char letter[2] = { axis_letter[i], 0 };
  cv_text(10, 2, &font_big, letter, col, C_PANEL);
  if (slave) {
    cv_text(40, 12, &font_med, "slave", C_DIM, C_PANEL);
  } else {
    const font_t *f = (cv_text_width(&font_big, wpos) <= w - 40) ? &font_big : &font_med;
    cv_text(w - 6 - cv_text_width(f, wpos), 2, f, wpos, C_TEXT, C_PANEL);
    cv_text(w - 6 - cv_text_width(&font_small, mline), 39, &font_small, mline, C_DIM, C_PANEL);
  }
  cv_end();
}

static void draw_jog_label(int widget, int col, int i, const char *sub)
{
  if (!changed(widget, fnv_s(i, sub))) { return; }
  int x = PAD_X(col), y = PAD_Y(1);
  cv_begin(x, y, 88, 78, C_BG);
  char letter[2] = { axis_letter[i], 0 };
  text_center(0, 8, 88, &font_big, letter, (i == 3 && a_slave()) ? C_FAINT : axis_color[i], C_BG);
  text_center(0, 50, 88, &font_small, sub, C_FAINT, C_BG);
  cv_end();
}

// The handwheel's state, in the free cell left of Y+: the axis it moves (in
// that axis's colour) and its step, dim while the machine cannot jog.
static void draw_mpg(void)
{
  uint8_t s = st.mpg_state, ax = st.mpg_axis, mult = st.mpg_mult;
  if (!changed(W_MPG, fnv_s((uint32_t)s | ((uint32_t)ax << 8) | ((uint32_t)mult << 16), ""))) { return; }
  cv_begin(PAD_X(0), PAD_Y(0), 88, 78, C_BG);
  if (s != GSHM_MPG_DISABLED) {
    text_center(0, 4, 88, &font_small, "MPG", C_DIM, C_BG);
    if (s == GSHM_MPG_NO_AXIS || ax < 1 || ax > 4) {
      text_center(0, 26, 88, &font_med, "OFF", C_FAINT, C_BG);
    } else {
      char l[2] = { axis_letter[ax - 1], 0 };
      bool ready = (s == GSHM_MPG_READY);
      text_center(0, 18, 88, &font_big, l, ready ? axis_color[ax - 1] : C_FAINT, C_BG);
      char m[8] = "x";
      cat_uint(m, mult);
      text_center(0, 56, 88, &font_small, ready ? m : "blocked", ready ? C_TEXT : C_AMBER, C_BG);
    }
  }
  cv_end();
}

static void draw_page_jog(void)
{
  bool inch = st.report_inches;
  for (int i = 0; i < 4; i++) {
    const char *t = inch ? step_in_txt[i] : step_mm_txt[i];
    draw_plain(B_STEP0 + i, t, &font_med, inch ? "in" : "mm", step_sel == i, C_SEL, C_DARK, C_TEXT);
  }
  draw_plain(B_STEP4, "CONT", &font_med, "hold", step_sel == STEP_CONT, C_SEL, C_DARK, C_TEXT);

  char sp[8] = "", sub[32] = "X ";
  cat_uint(sp, speeds[speed_sel]); cat(sp, "%");
  char v[16];
  float rate = st.jog_rate[0] * speeds[speed_sel] / 100.0f;
  fmt_fixed(v, inch ? rate / 25.4f : rate, inch ? 1 : 0);
  cat(sub, v); cat(sub, inch ? " in/min" : " mm/min");
  draw_btn(B_SPEED, sp, &font_big, sub, C_BTN, C_TEXT, IC_NONE);

  static const char *jl[8] = { "X+", "X-", "Y+", "Y-", "Z+", "Z-", "A+", "A-" };
  for (int k = 0; k < 8; k++) { draw_plain(B_JOG_XP + k, jl[k], &font_big, 0, false, 0, 0, axis_color[k / 2]); }

  draw_plain(B_HOME, "HOME", &font_med, (st.flags & GSHM_F_HOMING) ? "hold 1 s" : "$22=0", false, 0, 0, C_TEXT);
  draw_plain(B_ZERO_ALL, "ALL=0", &font_med, "hold", false, 0, 0, C_TEXT);

  const char *mode = (step_sel == STEP_CONT) ? "continuous" : "step";
  draw_jog_label(W_JLBL_Z, 3, 2, mode);
  draw_jog_label(W_JLBL_A, 4, 3, a_slave() ? "slave" : mode);
  for (int i = 0; i < 4; i++) { draw_jog_dro(i); }
  draw_mpg();
}


// ---------------------------------------------------------------------------
// Drawing: OVERRIDE page
// ---------------------------------------------------------------------------

static void draw_ovr_label(int row, const char *name, const char *value, bool normal)
{
  if (!changed(W_OVR_L0 + row, fnv_s(fnv_s(normal, name), value))) { return; }
  cv_begin(LX, PAGE_Y + row * 78, 150, 72, C_PANEL);
  cv_text(10, 6, &font_small, name, C_DIM, C_PANEL);
  cv_text(10, 28, &font_big, value, normal ? C_TEXT : C_AMBER, C_PANEL);
  cv_end();
}

static void draw_page_ovr(void)
{
  char v[16];
  v[0] = 0; cat_uint(v, st.ovr_feed); cat(v, "%");
  draw_ovr_label(0, "FEED", v, st.ovr_feed == 100);
  draw_plain(B_FEED_M10, "-10", &font_big, 0, false, 0, 0, C_TEXT);
  draw_plain(B_FEED_M1, "-1", &font_big, 0, false, 0, 0, C_TEXT);
  draw_plain(B_FEED_100, "100%", &font_big, 0, st.ovr_feed == 100, C_SEL, C_DARK, C_TEXT);
  draw_plain(B_FEED_P1, "+1", &font_big, 0, false, 0, 0, C_TEXT);
  draw_plain(B_FEED_P10, "+10", &font_big, 0, false, 0, 0, C_TEXT);

  v[0] = 0; cat_uint(v, st.ovr_rapid); cat(v, "%");
  draw_ovr_label(1, "RAPID", v, st.ovr_rapid == 100);
  draw_plain(B_RAPID_25, "25%", &font_big, 0, st.ovr_rapid == 25, C_SEL, C_DARK, C_TEXT);
  draw_plain(B_RAPID_50, "50%", &font_big, 0, st.ovr_rapid == 50, C_SEL, C_DARK, C_TEXT);
  draw_plain(B_RAPID_100, "100%", &font_big, 0, st.ovr_rapid == 100, C_SEL, C_DARK, C_TEXT);

  bool sp_stopped = (st.flags & GSHM_F_SPINDLE_STOP) != 0;
  v[0] = 0; cat_uint(v, st.ovr_spindle); cat(v, "%");
  draw_ovr_label(2, "SPINDLE", sp_stopped ? "OFF" : v, !sp_stopped && st.ovr_spindle == 100);
  draw_plain(B_SPIN_M10, "-10", &font_big, 0, false, 0, 0, C_TEXT);
  draw_plain(B_SPIN_100, "100%", &font_big, 0, st.ovr_spindle == 100, C_SEL, C_DARK, C_TEXT);
  draw_plain(B_SPIN_P10, "+10", &font_big, 0, false, 0, 0, C_TEXT);
  draw_plain(B_SPIN_STOP, "STOP", &font_big, st_hold() ? "in hold" : "M5", false, 0, 0, C_TEXT);
  draw_plain(B_SPIN_START, "START", &font_big, "hold", true, RGB(50, 200, 100), C_TEXT, C_TEXT);

  const char *cl = (st.coolant == 3) ? "M7 M8" : (st.coolant & 1) ? "M8" : (st.coolant & 2) ? "M7" : "off";
  draw_ovr_label(3, "COOLANT", cl, true);
  draw_plain(B_FLOOD, "FLOOD", &font_big, "M8", (st.coolant & 1) != 0, C_ON_BG, C_TEXT, C_TEXT);
  draw_plain(B_MIST, "MIST", &font_big, "M7", (st.coolant & 2) != 0, C_ON_BG, C_TEXT, C_TEXT);
}


// ---------------------------------------------------------------------------
// Drawing: I/O page
// ---------------------------------------------------------------------------

// One indicator: disc and label. Returns the x after it.
static int led(int x, const char *label, bool on, uint16_t on_color)
{
  cv_disc(x + 10, 23, 9, on ? on_color : C_LED_OFF, C_PANEL);
  cv_text(x + 24, 12, &font_med, label, on ? C_TEXT : C_DIM, C_PANEL);
  return x + 24 + cv_text_width(&font_med, label) + 14;
}

static int group(int x, const char *label)
{
  cv_text(x, 15, &font_small, label, C_FAINT, C_PANEL);
  return x + cv_text_width(&font_small, label) + 8;
}

// What input channel ch (1-20) carries, as a short label ("" = nothing).
// The two limits of one axis on one channel show as "X-/+".
static void in_label(int ch, char *out, uint16_t *color)
{
  static const char *const names[GSHM_N_IN_FN] = {
    "X-", "X+", "Y-", "Y+", "Z-", "Z+", "A-", "A+", "ALM X", "ALM Y", "ALM Z", "ALM A",
    "PROBE", "RESET", "HOLD", "START", "DOOR", "AT SPD", "VFD FL" };
  out[0] = 0;
  *color = C_GREEN;
  for (int f = 0; f < GSHM_N_IN_FN; f++) {
    if (set_byte(60 + f) != ch) { continue; }
    if (out[0]) {   // a second function: only the partner limit can share
      out[1] = 0; cat(out, "-/+");
      break;
    }
    strcpy(out, names[f]);
    if (f < 8) { *color = C_AMBER; }
    else if (f < 12 || f == 18) { *color = C_RED; }
  }
}

// What output channel ch (1-14) carries. OUT13 is the spindle PWM with $38 > 0.
static void out_label(int ch, char *out)
{
  static const char *const names[GSHM_N_OUT_FN] = {
    "FWD", "REV", "FLD", "MIST", "AUX0", "AUX1", "AUX2", "AUX3", "AUX4", "AUX5", "COLL", "SRST", "VRST" };
  out[0] = 0;
  if ((ch == 13) && set_byte(38)) { strcpy(out, "PWM"); return; }
  for (int f = 0; f < GSHM_N_OUT_FN; f++) {
    if (set_byte(80 + f) == ch) { strcpy(out, names[f]); return; }
  }
}

// Signature of the assignment, so a row is redrawn when a setting changes.
static uint32_t io_assign_sig(int first, int n)
{
  uint8_t v[GSHM_N_IN_FN];
  for (int i = 0; i < n; i++) { v[i] = set_byte(first + i); }
  return fnv(0, v, n);
}

// One row of input channels: IN[first+1] .. IN[first+10].
static void draw_in_row(int widget, int y, int first)
{
  uint32_t in = st.inputs & 0xFFFFFu;
  uint32_t sig = fnv(io_assign_sig(60, GSHM_N_IN_FN) ^ (uint32_t)widget, &in, sizeof(in));
  if (!changed(widget, sig)) { return; }
  cv_begin(LX, y, LW, 54, C_PANEL);
  cv_text(6, 18, &font_small, "IN", C_FAINT, C_PANEL);
  for (int k = 0; k < 10; k++) {
    int ch = first + k + 1, cx = 30 + k * 65;
    bool on = (in >> (ch - 1)) & 1;
    char lbl[12], num[4] = "";
    uint16_t col;
    in_label(ch, lbl, &col);
    cat_uint(num, ch);
    cv_disc(cx + 8, 14, 7, on ? (lbl[0] ? col : C_TEXT) : C_LED_OFF, C_PANEL);
    cv_text(cx + 20, 3, &font_med, num, on ? C_TEXT : C_DIM, C_PANEL);
    text_center(cx, 32, 62, &font_small, lbl[0] ? lbl : "-", lbl[0] ? (on ? C_TEXT : C_DIM) : C_FAINT, C_PANEL);
  }
  cv_end();
}

static void draw_out_row(void)
{
  uint16_t out = st.outputs & GSHM_OUT_MASK;
  uint32_t sig = fnv(io_assign_sig(80, GSHM_N_OUT_FN) ^ set_byte(38), &out, sizeof(out));
  if (!changed(W_OUT, sig)) { return; }
  cv_begin(LX, PAGE_Y + 116, LW, 54, C_PANEL);
  cv_text(6, 18, &font_small, "OUT", C_FAINT, C_PANEL);
  for (int k = 0; k < GSHM_N_OUT; k++) {
    int cx = 34 + k * 46;
    bool on = (out >> k) & 1;
    char lbl[8], num[4] = "";
    out_label(k + 1, lbl);
    cat_uint(num, k + 1);
    cv_disc(cx + 7, 14, 6, on ? C_GREEN : C_LED_OFF, C_PANEL);
    cv_text(cx + 16, 3, &font_med, num, on ? C_TEXT : C_DIM, C_PANEL);
    text_center(cx, 32, 44, &font_small, lbl[0] ? lbl : "-", lbl[0] ? (on ? C_TEXT : C_DIM) : C_FAINT, C_PANEL);
  }
  cv_end();
}

static void draw_io_status(void)
{
  uint32_t v[4] = { st.flags & (GSHM_F_ESTOP | GSHM_F_ESTOP_IN | GSHM_F_COLLET),
                    st.outputs & (GSHM_OUT_ENABLE | GSHM_OUT_BRAKE), set_byte(57) | (set_byte(59) << 1),
                    st.drv_power };
  if (!changed(W_PLAN, fnv(5, v, sizeof(v)))) { return; }
  cv_begin(LX, PAGE_Y + 174, LW, 42, C_PANEL);
  int x = group(10, "STOP");
  if (!set_byte(57)) { x = led(x, "off", false, C_GREEN); }
  else if (st.flags & (GSHM_F_ESTOP | GSHM_F_ESTOP_IN)) { x = led(x, "E-STOP", true, C_RED); }
  else { x = led(x, "OK", true, C_GREEN); }
  x = group(x + 4, "DRIVES");
  x = led(x, (st.outputs & GSHM_OUT_ENABLE) ? "ON" : "OFF", st.outputs & GSHM_OUT_ENABLE, C_GREEN);
  // Their supply (D53, $94): red missing, amber back but $95 not over, dark
  // when not watched.
  switch (st.drv_power) {
    case GSHM_DRV_OFF: x = led(x, "PWR", true, C_RED); break;
    case GSHM_DRV_RETURNING: x = led(x, "PWR", true, C_AMBER); break;
    case GSHM_DRV_ON: x = led(x, "PWR", true, C_GREEN); break;
    default: x = led(x, "PWR", false, C_GREEN); break;
  }
  x = group(x + 4, "Z BRAKE");
  if (!set_byte(59)) { x = led(x, "drive", false, C_GREEN); }
  else { x = led(x, (st.outputs & GSHM_OUT_BRAKE) ? "off" : "on", true, (st.outputs & GSHM_OUT_BRAKE) ? C_GREEN : C_AMBER); }
  x = group(x + 4, "COLLET");
  led(x, (st.flags & GSHM_F_COLLET) ? "OPEN" : "closed", st.flags & GSHM_F_COLLET, C_RED);
  cv_end();
}

static void draw_page_io(void)
{
  draw_in_row(W_IN1, PAGE_Y, 0);
  draw_in_row(W_IN2, PAGE_Y + 58, 10);
  draw_out_row();
  draw_io_status();
  for (int i = 0; i < GSHM_N_AUX; i++) {
    char name[6] = "AUX0", sub[16] = "";
    name[3] = (char)('0' + i);
    uint8_t ch = set_byte(84 + i);
    if (ch) { cat(sub, "OUT"); cat_uint(sub, ch); } else { cat(sub, "none"); }
    bool on = (st.out_fn >> (4 + i)) & 1;
    draw_plain(B_AUX0 + i, name, &font_med, sub, on, C_ON_BG, C_TEXT, C_TEXT);
  }
  bool open = (st.flags & GSHM_F_COLLET) != 0;
  draw_plain(B_COLLET, "COLLET", &font_med, open ? "OPEN-tap" : "hold", open, C_RED, C_TEXT, C_TEXT);
  draw_plain(B_SRV_RST, "SERVO", &font_med, "reset", (st.out_fn >> 11) & 1, C_ON_BG, C_TEXT, C_TEXT);
  draw_plain(B_VFD_RST, "VFD", &font_med, "reset", (st.out_fn >> 12) & 1, C_ON_BG, C_TEXT, C_TEXT);
}


// ---------------------------------------------------------------------------
// Drawing: E-STOP banner (instead of the page while the STOP is latched)
// ---------------------------------------------------------------------------

static void draw_estop(void)
{
  bool active = (st.flags & GSHM_F_ESTOP_IN) != 0;
  if (!changed(W_ESTOP, active ? 2 : 1)) { return; }
  const uint16_t bg = RGB(170, 20, 20), fg = C_TEXT;
  static const char *const lines[4][2] = {
    { "", "" },
    { "1. Release the emergency stop button", "" },
    { "2. Re-arm the safety relay", "" },
    { "3. RESET (right), then HOME:", "   the motors were without power" },
  };
  for (int b = 0; b < 5; b++) {
    int h = (b < 4) ? 68 : 40;
    cv_begin(LX, PAGE_Y + b * 68, LW, h, bg);
    if (b == 0) {
      text_center(0, 4, LW, &font_big, "E - S T O P", fg, bg);
      text_center(0, 44, LW, &font_med, active ? "STOP signal active" : "STOP released - RESET to continue",
                  active ? RGB(255, 220, 220) : RGB(255, 235, 120), bg);
    } else if (b < 4) {
      cv_text(40, 8, &font_med, lines[b][0], fg, bg);
      cv_text(40, 36, &font_med, lines[b][1], RGB(255, 200, 200), bg);
    }
    cv_end();
  }
}


// ---------------------------------------------------------------------------
// Drawing: FILE page (programs from the USB stick, PLAN.md part 4)
// ---------------------------------------------------------------------------

static void fmt_size(char *out, uint32_t b)
{
  out[0] = 0;
  if (b < 10240) { cat_uint(out, b); cat(out, " B"); }
  else if (b < 10UL * 1024 * 1024) { cat_uint(out, (b + 512) / 1024); cat(out, " kB"); }
  else { cat_uint(out, (b + 524288) / 1048576); cat(out, " MB"); }
}

static const char *job_state_name(uint8_t s)
{
  switch (s) {
    case GJOB_OPENING:  return "OPENING";
    case GJOB_CHECK:    return "CHECKING";
    case GJOB_RUN:      return "RUNNING";
    case GJOB_STOPPING: return "STOPPING";
    case GJOB_DONE:     return "DONE";
    case GJOB_FAILED:   return "FAILED";
    case GJOB_ABORTED:  return "STOPPED";
  }
  return "";
}

// Progress of the running program as text and per mille, from the parser's
// place in the file (it runs a planner's length ahead of the machine).
static uint32_t job_permille(void)
{
  grbl_job_t *jb = GRBL_JOB;
  uint32_t size = jb->job_size;
  if (!size) { return 0; }
  uint32_t pos = jb->job_pos;
  if (pos > size) { pos = size; }
  return (uint32_t)((uint64_t)pos * 1000ULL / size);
}

// File names come from the stick as UTF-8 and the fonts are ASCII: Latin
// letters lose their accents ("fr�zka" shows as "frezka"), anything else
// becomes '?'.
static void copy_name(char *dst, const volatile char *src)
{
  static const char latin[] =                      // U+00C0 .. U+017F
    "AAAAAAACEEEEIIII" "DNOOOOOxOUUUUYTs" "aaaaaaaceeeeiiii" "dnooooo/ouuuuyty"
    "AaAaAaCcCcCcCcDd" "DdEeEeEeEeEeGgGg" "GgGgHhHhIiIiIiIi" "IiJjJjKkkLlLlLlL"
    "lLlNnNnNnnNnOoOo" "OoOoRrRrRrSsSsSs" "SsTtTtTtUuUuUuUu" "UuUuWwYyYZzZzZzs";
  int n = 0, i = 0;
  while (src[i] && (i < GJOB_NAME_LEN - 1)) {
    uint8_t c = (uint8_t)src[i++];
    if (c < 0x80) { dst[n++] = (char)c; continue; }
    uint32_t u = 0xFFFF;
    if (((c & 0xE0) == 0xC0) && (((uint8_t)src[i] & 0xC0) == 0x80)) { u = ((c & 0x1Fu) << 6) | ((uint8_t)src[i] & 0x3Fu); }
    while ((i < GJOB_NAME_LEN - 1) && (((uint8_t)src[i] & 0xC0) == 0x80)) { i++; }   // the rest of the sequence
    dst[n++] = (u >= 0xC0 && u < 0x180) ? latin[u - 0xC0] : '?';
  }
  dst[n] = 0;
}

static void draw_file_row(int i)
{
  grbl_job_t *jb = GRBL_JOB;
  uint8_t id = B_FROW0 + i;
  int idx = file_top + i;
  if (idx >= jb->n_files) { draw_btn_hidden(id); return; }
  char name[GJOB_NAME_LEN], size[16];
  copy_name(name, jb->file[idx].name);
  fmt_size(size, jb->file[idx].size);
  bool sel = (idx == file_sel);
  bool pressed = (t_active == id);
  uint32_t sig = fnv_s(fnv_s((sel ? 1u : 0u) | (pressed ? 2u : 0u), name), size);
  if (!changed_btn(id, sig)) { return; }
  const rect_t &r = brect[id];
  uint16_t bg = sel ? C_SEL : (pressed ? mix(C_PANEL, RGB(255, 255, 255), 40) : C_PANEL);
  uint16_t fg = sel ? C_DARK : C_TEXT;
  cv_begin(r.x, r.y, r.w, r.h, bg);
  // Long names are cut to the row.
  char shown[27];
  strncpy(shown, name, 26);
  shown[26] = 0;
  if (strlen(name) > 26) { shown[24] = '.'; shown[25] = '.'; }
  cv_text(10, 3, &font_med, shown, fg, bg);
  cv_text(r.w - 10 - cv_text_width(&font_small, size), 29, &font_small, size, sel ? C_DARK : C_DIM, bg);
  cv_end();
}

static void draw_file_detail(void)
{
  grbl_job_t *jb = GRBL_JOB;
  char l1[GJOB_NAME_LEN] = "", l2[40] = "", l3[40] = "", l4[40] = "", l5[40] = "";
  uint16_t c5 = C_DIM;
  if (file_sel >= 0 && file_sel < jb->n_files) {
    copy_name(l1, jb->file[file_sel].name);
    l1[16] = 0;                          // the panel is 16 characters wide
    fmt_size(l2, jb->file[file_sel].size);
    // What the last job found out about it, if it was this file.
    if (strcasecmp((const char *)jb->job_name, (const char *)jb->file[file_sel].name) == 0 && jb->job_state != GJOB_IDLE) {
      if (jb->job_lines) { cat(l2, ", "); cat_uint(l2, jb->job_lines); cat(l2, " ln"); }
      char a[16], b[16];
      for (int i = 0; i < 2; i++) {
        char *l = i ? l4 : l3;
        cat(l, i ? "Y " : "X ");
        fmt_fixed(a, jb->job_min[i], 1); fmt_fixed(b, jb->job_max[i], 1);
        cat(l, a); cat(l, ".."); cat(l, b);
      }
      cat(l5, "last: ");
      cat(l5, job_state_name(jb->job_state));
      c5 = (jb->job_state == GJOB_DONE) ? C_GREEN : (jb->job_state == GJOB_FAILED) ? C_RED : C_AMBER;
    } else {
      strcpy(l3, "not run yet:");
      strcpy(l4, "checked first");
    }
  } else {
    strcpy(l1, "no file");
    strcpy(l2, "selected");
  }
  uint32_t sig = fnv_s(fnv_s(fnv_s(fnv_s(fnv_s(c5, l1), l2), l3), l4), l5);
  if (!changed(W_FILE_DETAIL, sig)) { return; }
  cv_begin(480, PAGE_Y, 210, 160, C_PANEL);
  cv_text(8, 4, &font_med, l1, C_TEXT, C_PANEL);
  cv_text(8, 32, &font_small, l2, C_DIM, C_PANEL);
  cv_text(8, 56, &font_small, l3, C_DIM, C_PANEL);
  cv_text(8, 78, &font_small, l4, C_DIM, C_PANEL);
  cv_text(8, 110, &font_med, l5, c5, C_PANEL);
  cv_end();
}

static void draw_file_status(void)
{
  grbl_job_t *jb = GRBL_JOB;
  char t[80] = "USB stick: ";
  uint16_t c = C_DIM;
  if (jb->magic != GRBL_JOB_MAGIC) { strcpy(t, "USB stick: starting"); }
  else switch (jb->usb_state) {
    case GJOB_USB_NONE: cat(t, "none (FAT32 or exFAT)"); break;
    case GJOB_USB_MOUNTING: cat(t, "mounting..."); break;
    case GJOB_USB_READY:
      c = C_GREEN;
      cat(t, jb->usb_fat == 4 ? "exFAT, " : jb->usb_fat == 3 ? "FAT32, " : "FAT16, ");
      cat_uint(t, jb->n_files); cat(t, jb->n_files == 1 ? " file, " : " files, ");
      if (jb->usb_free_mb >= 10240) { cat_uint(t, jb->usb_free_mb / 1024); cat(t, " GB free"); }
      else if (jb->usb_free_mb >= 1024) {
        cat_uint(t, jb->usb_free_mb / 1024); cat(t, "."); cat_uint(t, (jb->usb_free_mb % 1024) * 10 / 1024); cat(t, " GB free");
      } else { cat_uint(t, jb->usb_free_mb); cat(t, " MB free"); }
      if (jb->list_more) { cat(t, " (more not shown)"); }
      break;
    case GJOB_USB_FORMATTING: c = C_AMBER; cat(t, "formatting - leave it in"); break;
    case GJOB_USB_ERROR:
      c = C_RED;
      cat(t, jb->usb_fs == GJOB_FS_NTFS ? "NTFS" : (jb->usb_fs == GJOB_FS_FAT || jb->usb_fs == GJOB_FS_EXFAT) ?
             "damaged?" : "cannot be read");
      cat(t, " - FAT32 or exFAT only");
      break;
    default: cat(t, "starting"); break;
  }
  if (!changed(W_FILE_STATUS, fnv_s(c, t))) { return; }
  cv_begin(LX, 326, LW - 154, 46, C_PANEL);
  cv_disc(16, 23, 7, c == C_DIM ? C_LED_OFF : c, C_PANEL);
  cv_text(32, 12, &font_med, t, c == C_DIM ? C_DIM : C_TEXT, C_PANEL);
  cv_end();
}

static void draw_job_view(void)
{
  grbl_job_t *jb = GRBL_JOB;
  char name[GJOB_NAME_LEN];
  copy_name(name, jb->job_name);
  uint8_t s = jb->job_state;
  uint32_t pm = job_permille();
  uint32_t now = st.m7_ms;
  uint32_t el = (s == GJOB_RUN || s == GJOB_STOPPING) ? (uint32_t)(now - jb->job_start_ms) : 0;
  char l1[64] = "", l2[64] = "", l3[64] = "";
  strcpy(l1, job_state_name(s));
  if (s == GJOB_CHECK) { cat(l1, "  (check pass, nothing moves)"); }
  if (s == GJOB_RUN && st_hold()) { strcpy(l1, "PAUSED - START continues"); }
  cat_uint(l2, pm / 10); cat(l2, "."); cat_uint(l2, pm % 10); cat(l2, " %   line ");
  cat_uint(l2, jb->job_line);
  if (jb->job_lines) { cat(l2, " / "); cat_uint(l2, jb->job_lines); }
  if (s == GJOB_RUN || s == GJOB_STOPPING) {
    char a[16];
    cat(l3, "elapsed "); fmt_time(a, el); cat(l3, a);
    if (pm > 20) {
      uint32_t left = (uint32_t)((uint64_t)el * (1000 - pm) / pm);
      cat(l3, "   left ~"); fmt_time(a, left); cat(l3, a);
    }
  }
  uint32_t sig = fnv_s(fnv_s(fnv_s(fnv_s(pm, name), l1), l2), l3);
  if (!changed(W_FILE_DETAIL, sig)) { return; }
  cv_begin(LX, PAGE_Y, LW, 66, C_PANEL);
  cv_text(10, 2, &font_big, name, C_TEXT, C_PANEL);
  cv_text(10, 42, &font_small, l1, (s == GJOB_STOPPING) ? C_AMBER : C_SEL, C_PANEL);
  cv_end();
  cv_begin(LX, PAGE_Y + 70, LW, 60, C_PANEL);
  cv_fill(10, 8, LW - 20, 18, C_LED_OFF);
  cv_fill(10, 8, (LW - 20) * pm / 1000, 18, s == GJOB_CHECK ? C_AMBER : C_BAR);
  cv_text(10, 32, &font_med, l2, C_TEXT, C_PANEL);
  cv_end();
  cv_begin(LX, PAGE_Y + 134, LW, 60, C_PANEL);
  cv_text(10, 18, &font_med, l3, C_DIM, C_PANEL);
  cv_end();
}

static void draw_page_file(void)
{
  grbl_job_t *jb = GRBL_JOB;
  bool active = job_is_active(jb->job_state);
  if (active != file_view_job) { file_view_job = active; page_dirty = true; return; }
  if (page_dirty) { return; }
  if (active) {
    draw_job_view();
    bool stop = btn_enabled(B_FSTOP, 0);
    draw_btn(B_FSTOP, "STOP", &font_big, "hold: stop, keep position",
             stop ? C_RESET_BG : C_PANEL, stop ? C_TEXT : C_FAINT, IC_STOP);
    return;
  }
  // A new listing (stick changed): drop a selection that no longer fits.
  if (jb->list_seq != file_seq) {
    file_seq = jb->list_seq;
    if (file_sel >= jb->n_files) { file_sel = -1; }
    if (file_top >= jb->n_files) { file_top = 0; }
  }
  for (int i = 0; i < FILE_ROWS; i++) { draw_file_row(i); }
  bool up = btn_enabled(B_FPREV, 0), down = btn_enabled(B_FNEXT, 0);
  draw_btn(B_FPREV, "prev", &font_med, 0, up ? C_BTN : C_PANEL, up ? C_TEXT : C_FAINT, IC_UP);
  draw_btn(B_FNEXT, "next", &font_med, 0, down ? C_BTN : C_PANEL, down ? C_TEXT : C_FAINT, IC_DOWN);
  draw_file_detail();
  if (btn_enabled(B_FSTART, 0)) { draw_btn(B_FSTART, "START", &font_big, "hold 1 s", C_START_BG, C_TEXT, IC_PLAY); }
  else { draw_btn(B_FSTART, "START", &font_big, 0, C_PANEL, C_FAINT, IC_PLAY); }
  draw_file_status();
  bool fmt = btn_enabled(B_FFORMAT, 0);
  draw_btn(B_FFORMAT, "FORMAT", &font_med, 0, fmt ? C_BTN : C_PANEL, fmt ? C_TEXT : C_FAINT, IC_NONE);
}


// ---------------------------------------------------------------------------
// Drawing: $$ page
// ---------------------------------------------------------------------------

static void draw_set_row(int i)
{
  uint8_t id = B_SROW0 + i;
  int idx = list_top + i;
  if (idx >= GSHM_N_SETTINGS) { draw_btn_hidden(id); return; }
  set_info_t in;
  set_info(idx, &in);
  char num[8] = "$", v[40];
  cat_uint(num, in.m.num);
  set_format(&in.m, st.set_val[idx], v);
  bool pressed = (t_active == id);
  uint32_t sig = fnv_s(fnv_s(fnv_s(fnv_s(pressed ? 1u : 0u, num), in.title), in.brief), v);
  if (!changed_btn(id, sig)) { return; }
  const rect_t &r = brect[id];
  uint16_t bg = pressed ? mix(C_PANEL, RGB(255, 255, 255), 40) : C_PANEL;
  cv_begin(r.x, r.y, r.w, r.h, bg);
  cv_text(10, 14, &font_med, num, C_SEL, bg);
  cv_text(76, 3, &font_med, in.title, C_TEXT, bg);
  cv_text(76, 29, &font_small, in.brief, C_DIM, bg);
  const font_t *f = (cv_text_width(&font_big, v) <= 190) ? &font_big : &font_med;
  cv_text(r.w - 10 - cv_text_width(f, v), (r.h - f->h) / 2, f, v, C_TEXT, bg);
  cv_end();
}

static void draw_set_list(void)
{
  for (int i = 0; i < SET_ROWS; i++) { draw_set_row(i); }
  char pg[12] = "";
  cat_uint(pg, list_top / SET_ROWS + 1); cat(pg, "/"); cat_uint(pg, (GSHM_N_SETTINGS + SET_ROWS - 1) / SET_ROWS);
  bool up = btn_enabled(B_SUP, 0), down = btn_enabled(B_SDOWN, 0);
  draw_btn(B_SUP, "prev", &font_med, pg, up ? C_BTN : C_PANEL, up ? C_TEXT : C_FAINT, IC_UP);
  draw_btn(B_SDOWN, "next", &font_med, pg, down ? C_BTN : C_PANEL, down ? C_TEXT : C_FAINT, IC_DOWN);
}

static void draw_set_editor(void)
{
  const set_meta_t *m = &edit_info.m;
  bool numeric = set_numeric(m->type);
  char num[8] = "$", now[48] = "now: ", v[40], field[40];
  cat_uint(num, m->num);
  set_format(m, st.set_val[edit_idx], v);
  cat(now, v);
  bool placeholder = false;
  if (numeric) {
    if (edit_buf[0]) { strcpy(field, edit_buf); }
    else { strcpy(field, v); placeholder = true; }
  } else {
    set_format(m, (float)edit_bits, field);
  }
  uint32_t sig = fnv_s(fnv_s(fnv_s(fnv_s(placeholder ? 1u : 0u, num), m->title), now), field);
  if (changed(W_SET_HEAD, sig)) {
    cv_begin(LX, PAGE_Y, 330, 136, C_PANEL);
    cv_text(10, 2, &font_big, num, C_SEL, C_PANEL);
    cv_text(10, 42, &font_med, m->title, C_TEXT, C_PANEL);
    cv_text(10, 68, &font_small, now, C_DIM, C_PANEL);
    cv_frame(10, 88, 310, 44, 2, placeholder ? C_FAINT : C_SEL);
    const font_t *f = (cv_text_width(&font_big, field) <= 290) ? &font_big : &font_med;
    cv_text(20, 88 + (44 - f->h) / 2, f, field, placeholder ? C_FAINT : C_TEXT, C_PANEL);
    cv_end();
  }
  char lines[10][34];
  int n = wrap(m->desc, 31, lines, 10);
  for (int b = 0; b < 2; b++) {
    uint32_t s2 = fnv_s(b, m->desc);
    if (!changed(W_SET_DESC1 + b, s2)) { continue; }
    cv_begin(LX, PAGE_Y + 136 + b * 88, 330, 88, C_PANEL);
    for (int j = 0; j < 5; j++) {
      int li = b * 5 + j;
      if (li < n) { cv_text(10, 2 + j * 17, &font_small, lines[li], C_DIM, C_PANEL); }
    }
    cv_end();
  }

  if (numeric) {
    for (int k = 0; k <= 9; k++) {
      char l[2] = { (char)('0' + k), 0 };
      draw_plain(B_K0 + k, l, &font_big, 0, false, 0, 0, C_TEXT);
    }
    draw_plain(B_KDOT, ".", &font_big, 0, false, 0, 0, C_TEXT);
    draw_plain(B_KDEL, "DEL", &font_med, 0, false, 0, 0, C_TEXT);
    draw_plain(B_KCLR, "CLR", &font_med, 0, false, 0, 0, C_TEXT);
  } else {
    int nopt = set_n_opts(m);
    bool toggles = set_toggles(m->type);
    for (int k = 0; k < nopt; k++) {
      char l[24];
      opt_label(m, k, l);
      bool lit = toggles ? ((edit_bits >> k) & 1) : (edit_bits == (uint32_t)k);
      const font_t *f = (m->type == T_AXES) ? &font_big : (m->type == T_INMASK) ? &font_small : &font_med;
      draw_plain(B_OPT0 + k, l, f, 0, lit, C_SEL, C_DARK, C_TEXT);
    }
  }
  draw_plain(B_KCANCEL, "CANCEL", &font_med, 0, false, 0, 0, C_TEXT);
  if (btn_enabled(B_KSAVE, 0)) { draw_btn(B_KSAVE, "SAVE", &font_big, 0, C_ON_BG, C_TEXT, IC_NONE); }
  else { draw_btn(B_KSAVE, "SAVE", &font_big, 0, C_PANEL, C_FAINT, IC_NONE); }
}

static void draw_page_set(void)
{
  // The answer to a SAVE: close the editor on success; on an error the M7
  // has put the error on the message line, and the editor stays open.
  if (save_pending) {
    volatile grbl_shm_t *m = GRBL_SHM;
    if (m->req_done == save_seq) {
      save_pending = false;
      if (m->req_status == 0) {
        char h[40] = "Saved $";
        cat_uint(h, edit_info.m.num);
        set_hint(h);
        editor_close();
      } else {
        hint_until = 0;
      }
    } else if ((uint32_t)(millis() - save_ms) > 1500) {
      save_pending = false;
      set_hint("Grbl did not answer - not saved");
    }
  }
  if (page_dirty) { return; }    // the mode changed: draw it after the clear
  if (edit_idx < 0) { draw_set_list(); } else { draw_set_editor(); }
}


// ---------------------------------------------------------------------------
// RGB LED of the shield
// ---------------------------------------------------------------------------

static uint32_t rgb_key = 0xFFFFFFFFUL;

static void rgb_update(uint32_t now, bool stale)
{
  uint8_t r = 0, g = 0, b = 0;
  uint32_t key = stale ? 0x100 : st.state;
  if (!stale && estop_shown()) {
    // E-STOP: fast red blink, 0.1 s on, 0.1 s off.
    key = 0x400;
    if (((now / 100) & 1) == 0) { r = 255; } else { key |= 0x200; }
  } else if (!stale) {
    switch (st.state) {
      case GSHM_STATE_IDLE:  r = 0;   g = 90;  b = 25;  break;
      case GSHM_STATE_RUN:   r = 0;   g = 40;  b = 140; break;
      case GSHM_STATE_HOLD0:
      case GSHM_STATE_HOLD1: r = 150; g = 80;  b = 0;   break;
      case GSHM_STATE_JOG:   r = 0;   g = 100; b = 110; break;
      case GSHM_STATE_HOME:  r = 80;  g = 0;   b = 150; break;
      case GSHM_STATE_ALARM:
        // Blinks, 0.4 s on, 0.4 s off.
        if (((now / 400) & 1) == 0) { r = 180; } else { key |= 0x200; }
        break;
      case GSHM_STATE_CHECK: r = 50;  g = 50;  b = 50;  break;
      case GSHM_STATE_DOOR:  r = 170; g = 45;  b = 0;   break;
      case GSHM_STATE_SLEEP: r = 6;   g = 6;   b = 6;   break;
    }
  }
  if (key == rgb_key) { return; }
  rgb_key = key;
  rgb_set(r, g, b);
}


// ---------------------------------------------------------------------------
// Splash screen (design variant A): the Grbl logo, "GIGA R1", a progress bar
// and one line of status. From power-up until Grbl answers and the USB stick
// has settled - at least SPLASH_MIN_MS - and again, without the bar, while
// the M7 is silent. Touches are not acted on while it shows.
// ---------------------------------------------------------------------------

#define SPLASH_MIN_MS    2000   // shown at least this long after power-up
#define SPLASH_USB_MS    2500   // longest wait for the USB stick once Grbl answers
#define SPLASH_DONE_MS   300    // the full bar stays up this long
#define SPLASH_STEP      5      // bar animation, percent a frame

static bool splash_on = true;
static uint32_t splash_t0 = 0;          // when it was shown
static uint32_t splash_ready_at = 0;    // when Grbl answered (0: not yet)
static uint32_t splash_done_at = 0;     // when the bar reached 100 % (0: not yet)
static uint32_t splash_hold_until = 0;  // $SPLASH=ms
static uint32_t splash_seq_seen = 0;
static int splash_pct = 0;              // bar as drawn

// "fw 2026-10-14" from the compiler's "Oct 14 2026".
static void build_date(char *out)
{
  static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  const char *d = __DATE__;
  int mo = 1;
  for (int i = 0; i < 12; i++) { if (strncmp(d, months + 3 * i, 3) == 0) { mo = i + 1; break; } }
  int day = (d[4] == ' ' ? 0 : d[4] - '0') * 10 + (d[5] - '0');
  out[0] = 'f'; out[1] = 'w'; out[2] = ' ';
  memcpy(out + 3, d + 7, 4);
  out[7] = '-'; out[8] = (char)('0' + mo / 10); out[9] = (char)('0' + mo % 10);
  out[10] = '-'; out[11] = (char)('0' + day / 10); out[12] = (char)('0' + day % 10);
  out[13] = 0;
}

// pct < 0: no progress (the M7 went silent), just the track.
static void draw_splash(int pct, const char *msg, uint8_t settings_version)
{
  if (changed(W_WAIT, 1)) {               // the static parts, once per showing
    cv_screen_fill(0, 0, SCREEN_W, SCREEN_H, C_BG);
    cv_begin((SCREEN_W - LOGO_W) / 2, 60, LOGO_W, LOGO_H, C_BG);
    cv_bitmap4(0, 0, LOGO_W, LOGO_H, logo_bitmap, C_TEXT, C_BG);
    cv_end();
    const char *t1 = "GIGA R1", *t2 = "CNC controller  |  Grbl 1.1h";
    cv_begin(100, 239, 600, 80, C_BG);   // 600 x 80 is exactly the scratch buffer
    cv_text((600 - cv_text_width(&font_big, t1)) / 2, 0, &font_big, t1, C_TEXT, C_BG);
    cv_text((600 - cv_text_width(&font_med, t2)) / 2, 49, &font_med, t2, C_DIM, C_BG);
    cv_end();
    char fw[16];
    build_date(fw);
    cv_begin(16, 451, 200, 17, C_BG);
    cv_text(0, 0, &font_small, fw, C_DIM, C_BG);
    cv_end();
  }
  if (changed(W_SPLASH_VER, settings_version)) {
    cv_begin(SCREEN_W - 216, 451, 200, 17, C_BG);
    if (settings_version) {
      char v[20] = "settings v";
      cat_uint(v, settings_version);
      cv_text(200 - cv_text_width(&font_small, v), 0, &font_small, v, C_DIM, C_BG);
    }
    cv_end();
  }
  if (changed(W_SPLASH_BAR, (uint32_t)(pct + 2))) {
    cv_begin(160, 404, 480, 6, C_PANEL);
    if (pct > 0) { cv_fill(0, 0, 480 * (pct > 100 ? 100 : pct) / 100, 6, C_SEL); }
    cv_end();
  }
  if (changed(W_SPLASH_MSG, fnv_s(0, msg))) {
    cv_begin(100, 420, 600, 18, C_BG);
    cv_text((600 - cv_text_width(&font_small, msg)) / 2, 0, &font_small, msg, C_DIM, C_BG);
    cv_end();
  }
}

// Shows the splash again from the start ($SPLASH, or at power-up).
static void splash_restart(uint32_t now, uint32_t hold_ms)
{
  splash_on = true;
  splash_t0 = now;
  splash_ready_at = 0;
  splash_done_at = 0;
  splash_hold_until = now + hold_ms;
  splash_pct = 0;
  sig_w[W_WAIT] = 0xFFFFFFFFUL;
  sig_w[W_SPLASH_VER] = 0xFFFFFFFFUL;
  sig_w[W_SPLASH_BAR] = 0xFFFFFFFFUL;
  sig_w[W_SPLASH_MSG] = 0xFFFFFFFFUL;
}

// One frame of the start-up splash. Returns false once it is over.
static bool splash_step(uint32_t now, bool fresh)
{
  grbl_job_t *jb = GRBL_JOB;
  bool usb_settled = (jb->magic == GRBL_JOB_MAGIC) &&
                     (jb->usb_state != GJOB_USB_OFF) && (jb->usb_state != GJOB_USB_MOUNTING);
  int target;
  const char *msg;
  if (!fresh) {
    target = 15;
    msg = "Starting Grbl on the M7 core...";
  } else {
    if (!splash_ready_at) { splash_ready_at = now; }
    if (!usb_settled && (uint32_t)(now - splash_ready_at) < SPLASH_USB_MS) {
      target = 70;
      msg = "Waiting for the USB stick...";
    } else {
      target = 100;
      msg = "Ready";
    }
  }
  if (splash_pct < target) {
    splash_pct += SPLASH_STEP;
    if (splash_pct > target) { splash_pct = target; }
  }
  if ((splash_pct >= 100) && !splash_done_at) { splash_done_at = now; }
  draw_splash(splash_pct, msg, fresh ? st.settings_version : 0);
  return !(splash_done_at && ((uint32_t)(now - splash_done_at) >= SPLASH_DONE_MS) &&
           ((uint32_t)(now - splash_t0) >= SPLASH_MIN_MS) && ((int32_t)(now - splash_hold_until) >= 0));
}


// ---------------------------------------------------------------------------
// Status from the M7
// ---------------------------------------------------------------------------

static bool read_status(void)
{
  volatile grbl_shm_t *m = GRBL_SHM;
  if (m->magic != GRBL_SHM_MAGIC || m->version != GRBL_SHM_VERSION) { return false; }
  for (int tries = 0; tries < 4; tries++) {
    uint32_t s1 = m->seq;
    if (s1 & 1) { delayMicroseconds(20); continue; }
    __DMB();
    memcpy(&st, (const void *)m, offsetof(grbl_shm_t, pad));
    __DMB();
    if (m->seq == s1) { return st.m7_ms != 0; }   // m7_ms is 0 until the M7's first status
  }
  return false;
}


// ---------------------------------------------------------------------------
// Entry points (called from the sketch's setup()/loop() on the M4)
// ---------------------------------------------------------------------------

static uint32_t last_frame = 0;
static uint32_t last_poll = 0;

void display_m4_setup(void)
{
  volatile grbl_shm_t *m = GRBL_SHM;
  m->m4_status = 1;
  bool ok = cv_init(C_BG);
  m->m4_status = ok ? 2 : 0xE1;
  m->m4_fb = ok ? cv_framebuffer() : 0;
  // Start with nothing queued, whatever the M7 has already consumed.
  m->cmd_head = m->cmd_tail;
  m->req_seq = m->req_done;
  m->jog_hold = 0;
  sim_seq_seen = m->sim_seq;
  m->sim_ack = sim_seq_seen;
  splash_seq_seen = m->splash_seq;
  splash_restart(millis(), 0);
  m->touch_count = 0;
  m->m4_frames = 0;
  m->m4_ms = millis();
  layout_init();
  touch_state = touch_init();
  uint16_t rx, ry;
  touch_resolution(&rx, &ry);
  m->touch_res_x = rx;
  m->touch_res_y = ry;
  m->touch_status = touch_state;
  m->rgb_status = rgb_init();
  __DMB();
  m->m4_magic = GRBL_SHM_M4_MAGIC;
  usbstick_start();          // the USB-A port, in a thread of its own
}

void display_m4_loop(void)
{
  volatile grbl_shm_t *m = GRBL_SHM;
  uint32_t now = millis();
  m->m4_ms = now;

  if ((uint32_t)(now - last_poll) >= TOUCH_POLL_MS) {
    last_poll = now;
    touch_poll(now);
  }
  if ((uint32_t)(now - last_frame) < REFRESH_MS) { delay(1); return; }
  last_frame = now;

  if (read_status()) {
    if (!have_status || st.m7_ms != st_last_m7ms) { st_fresh_at = now; st_last_m7ms = st.m7_ms; }
    have_status = true;
  }
  bool stale = !have_status || (uint32_t)(now - st_fresh_at) > STALE_MS;
  if (m->splash_seq != splash_seq_seen) {         // $SPLASH: show it again
    splash_seq_seen = m->splash_seq;
    splash_restart(now, m->splash_ms);
    waiting_shown = true;
  }
  live = !stale && !splash_on;
  rgb_update(now, stale);

  if (splash_on) {
    if (jog_cont) { jog_stop(); }
    if (!splash_step(now, !stale)) {
      splash_on = false;                           // to the main screen
    }
    waiting_shown = true;
    force_redraw = false;
  } else if (stale) {
    if (jog_cont) { jog_stop(); }
    if (!waiting_shown) { force_redraw = true; waiting_shown = true; }
    draw_splash(-1, "Waiting for Grbl on the M7 core...", 0);
    force_redraw = false;
  } else {
    if (waiting_shown) {
      waiting_shown = false;
      force_redraw = true;
      sig_w[W_WAIT] = 0;
      cv_screen_fill(0, 0, SCREEN_W, SCREEN_H, C_BG);
    }
    static bool banner = false;
    if (estop_shown() != banner) {
      banner = estop_shown();
      if (banner && jog_cont) { jog_stop(); }
      page_dirty = true;
      sig_w[W_ESTOP] = 0;
    }
    if (page_dirty) {
      page_dirty = false;
      cv_screen_fill(LX, PAGE_Y, LW, PAGE_H, C_BG);
      force_redraw = true;
    }
    draw_state();
    draw_modal();
    draw_link();
    draw_right();
    if (banner) {
      draw_estop();
    } else {
      switch (page) {
        case PAGE_DRO: draw_page_dro(); break;
        case PAGE_JOG: draw_page_jog(); break;
        case PAGE_OVR: draw_page_ovr(); break;
        case PAGE_IO:  draw_page_io(); break;
        case PAGE_SET: draw_page_set(); break;
        case PAGE_FILE: draw_page_file(); break;
      }
    }
    draw_message();
    draw_tabs();
    force_redraw = false;
  }
  m->m4_frames = m->m4_frames + 1;
  m->m4_status = cv_dma_errors ? cv_dma_errors : 2;
}

#endif // CORE_CM4
