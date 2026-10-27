/*
  vfd.c - spindle VFD over RS485 / Modbus RTU

  Part of Grbl-Giga, a port of Grbl 1.1 to the Arduino GIGA R1 WiFi.

  Grbl is free software: you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation, either version 3 of the License, or (at your option) any later
  version. Distributed WITHOUT ANY WARRANTY; see the GNU GPL for details.

  -------------------------------------------------------------------------------

  One transaction at a time, driven from the main loop: vfd_service() picks
  the next request, the HAL sends it (driver enable around it) and collects
  the answer in its interrupt, and the next vfd_service() pass checks it.
  Nothing here waits for the line.

  What the spindle code asks for (vfd_spindle(), any context) is compared on
  every pass with what the drive has acknowledged, in this order:

    1. stop, if the drive should stop and has not acknowledged a stop
    2. the fault reset, when asked for
    3. a $VFD=R / $VFD=W request
    4. the frequency, if the drive should run at another one
    5. run / direction
    6. the status, every VFD_POLL_MS

  A poll that finds the drive doing something else than it acknowledged
  (power-cycled, switched to the keypad) makes Grbl send it again; if the
  drive still disagrees after VFD_CONFIRM_MS, that is ALARM:23. A drive that
  ran as told and then stopped by itself (its keypad STOP) is not told again:
  ALARM:23 at once.
*/

#include "grbl.h"

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

#define ACT_NONE   0
#define ACT_CMD    1    // run / stop / direction
#define ACT_FREQ   2    // frequency set point
#define ACT_RESET  3    // fault reset (value 1), then its release (0)
#define ACT_POLL   4    // status
#define ACT_RAW    5    // $VFD=R / $VFD=W

#define PH_WAIT    0    // request out, waiting for the answer
#define PH_RESEND  1    // send it again once the line is quiet

#define LINK_OFF   0    // $33=0
#define LINK_WAIT  1    // no answer yet since the start
#define LINK_OK    2
#define LINK_LOST  3    // the last transaction got no valid answer

#define RAW_IDLE     0
#define RAW_PENDING  1
#define RAW_OK       2
#define RAW_LOST     3
#define RAW_REFUSED  4
#define RAW_MAX_REGS 8

#define CMD_UNKNOWN   0xFF
#define FREQ_UNKNOWN  0xFFFFFFFFUL

// Modbus answers: OK, broken or not ours (try again), or an exception code.
#define MB_OK    0
#define MB_BAD  (-1)

static struct {
  uint8_t inited;
  uint8_t type, addr, format;
  uint32_t baud;
  uint32_t char_us;           // one character on the line
  uint32_t gap_us;            // 3.5 characters, 1750 us above 19200 baud

  // Wanted - written from any context.
  volatile uint8_t want_dir;
  volatile float want_rpm;
  volatile uint8_t want_reset;   // 2: send the reset, 1: send its release
  volatile uint8_t at_speed;     // read by the input scan
  volatile uint8_t fault;

  // Acknowledged by the drive.
  uint8_t done_cmd;
  uint32_t done_freq;            // 0.01 Hz
  uint8_t may_run;               // a run command went out, no standing drive seen since
  uint8_t last_cmd;              // the wanted command the confirm timer belongs to

  // The transaction in flight.
  uint8_t act, phase, tries;
  uint16_t act_value;
  uint8_t req[HAL_RS485_FRAME_MAX];
  uint8_t req_len;
  uint32_t t_deadline;
  uint32_t t_next;               // no new transaction before (back-off after a failure)
  uint32_t t_poll;
  uint8_t link;

  // The drive, from the last poll (in the common form the profile fills).
  uint8_t d_valid;
  uint8_t d_run;                 // VFD_* the drive runs (or was told by whoever controls it)
  uint8_t d_reversing;           // still turning the other way after a reversal
  uint8_t d_turning;             // output frequency, or decelerating
  uint8_t d_standing;            // stopped with no output frequency
  uint8_t d_remote;              // bit 0 frequency, bit 1 run command from RS485
  uint16_t d_err, d_status, d_fcmd, d_fout;
  uint32_t t_mismatch;           // since the drive disagrees (0 = it agrees)
  uint8_t not_following;
  uint8_t refused;               // a run or frequency command came back as an exception
  uint8_t followed;              // seen doing the command written last
  uint8_t stopped;               // then stopped by itself (keypad STOP, a stop input)

  // $VFD=R / W
  volatile uint8_t raw_state;
  uint8_t raw_req[8], raw_len, raw_n;
  uint16_t raw_regs[RAW_MAX_REGS];

  uint32_t n_tx, n_ok, n_retry, n_fail, n_exc;
  uint8_t last_exc;
} vfd;

static uint8_t sim_mode = 0;     // $TVFD, below


// ---------------------------------------------------------------------------
// Modbus RTU frames
// ---------------------------------------------------------------------------

static uint16_t mb_crc(const uint8_t *p, uint8_t n)
{
  uint16_t crc = 0xFFFF;
  while (n--) {
    crc ^= *p++;
    uint8_t i;
    for (i = 0; i < 8; i++) { crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : (crc >> 1); }
  }
  return crc;
}

static uint8_t mb_finish(uint8_t *f, uint8_t n)
{
  uint16_t crc = mb_crc(f, n);
  f[n] = crc & 0xFF;          // low byte first
  f[n + 1] = crc >> 8;
  return n + 2;
}

static uint8_t mb_read(uint8_t *f, uint8_t addr, uint16_t reg, uint8_t count)
{
  f[0] = addr; f[1] = 0x03;
  f[2] = reg >> 8; f[3] = reg & 0xFF;
  f[4] = 0; f[5] = count;
  return mb_finish(f, 6);
}

static uint8_t mb_write(uint8_t *f, uint8_t addr, uint16_t reg, uint16_t value)
{
  f[0] = addr; f[1] = 0x06;
  f[2] = reg >> 8; f[3] = reg & 0xFF;
  f[4] = value >> 8; f[5] = value & 0xFF;
  return mb_finish(f, 6);
}

// Checks the answer r (n bytes) to the request q. Registers read go to regs.
static int mb_check(const uint8_t *q, const uint8_t *r, int n, uint16_t *regs)
{
  if ((n < 5) || (mb_crc(r, n - 2) != (uint16_t)(r[n - 2] | (r[n - 1] << 8)))) { return(MB_BAD); }
  if (r[0] != q[0]) { return(MB_BAD); }
  if (r[1] == (q[1] | 0x80)) { return(((n == 5) && r[2]) ? r[2] : MB_BAD); }
  if (r[1] != q[1]) { return(MB_BAD); }
  if (q[1] == 0x03) {
    uint8_t count = q[5], i;
    if ((r[2] != 2 * count) || (n != 5 + 2 * count)) { return(MB_BAD); }
    for (i = 0; i < count; i++) { regs[i] = (r[3 + 2 * i] << 8) | r[4 + 2 * i]; }
    return(MB_OK);
  }
  if (q[1] == 0x06) { return(((n == 8) && (memcmp(r, q, 6) == 0)) ? MB_OK : MB_BAD); }
  return(MB_BAD);
}


// ---------------------------------------------------------------------------
// Profiles: the vendor's registers
// ---------------------------------------------------------------------------
//
// Delta (MS300, MH300, C2000, CP2000, VFD-E, VFD-EL, ...), standard Modbus:
//   2000H  command: bits 1-0 01 stop, 10 run; bits 5-4 01 FWD, 10 REV
//   2001H  frequency command, 0.01 Hz
//   2002H  bit 1: fault reset
//   2100H  error code (low byte; the C2000 has warnings in the high byte)
//   2101H  status: bits 1-0 00 stopped, 01 decelerating, 10 standby, 11 running;
//          bits 4-3 00 FWD, 01 REV->FWD, 10 FWD->REV, 11 REV;
//          bit 8 frequency from RS485, bit 10 run command from RS485
//   2102H  frequency command (F), 2103H output frequency (H), 0.01 Hz
// The drive needs its frequency source (00-20) and run command source (00-21)
// - on the VFD-E 02-00 and 02-01 - on RS485 and its 09-xx matching $34-$36
// (README).

#define DELTA_REG_CMD     0x2000
#define DELTA_REG_FREQ    0x2001
#define DELTA_REG_CTRL    0x2002
#define DELTA_REG_STATUS  0x2100
#define DELTA_POLL_REGS   4

static uint8_t profile_request(uint8_t act, uint16_t value, uint8_t *f)
{
  switch (act) {
    case ACT_CMD:
      return mb_write(f, vfd.addr, DELTA_REG_CMD,
                      (value == VFD_FWD) ? 0x0012 : ((value == VFD_REV) ? 0x0022 : 0x0001));
    case ACT_FREQ:  return mb_write(f, vfd.addr, DELTA_REG_FREQ, value);
    case ACT_RESET: return mb_write(f, vfd.addr, DELTA_REG_CTRL, value ? 0x0002 : 0x0000);
    case ACT_POLL:  return mb_read(f, vfd.addr, DELTA_REG_STATUS, DELTA_POLL_REGS);
  }
  return 0;
}

static void profile_decode(const uint16_t *r)
{
  uint8_t run = r[1] & 3, dir = (r[1] >> 3) & 3;
  vfd.d_err = r[0] & 0xFF;
  vfd.d_status = r[1];
  vfd.d_fcmd = r[2];
  vfd.d_fout = r[3];
  vfd.d_run = (run >= 2) ? ((dir <= 1) ? VFD_FWD : VFD_REV) : VFD_STOP;
  vfd.d_reversing = (dir == 1) || (dir == 2);
  vfd.d_turning = (run & 1) || r[3];
  vfd.d_standing = (run == 0) && !r[3];
  vfd.d_remote = ((r[1] >> 8) & 1) | (((r[1] >> 10) & 1) << 1);
  vfd.d_valid = true;
}


// ---------------------------------------------------------------------------
// The line: the RS485 port, or the simulated drive of the test hook
// ---------------------------------------------------------------------------

static void sim_request(const uint8_t *f, uint8_t n);
static int sim_frame(uint8_t *buf, uint8_t max);

static void bus_send(const uint8_t *f, uint8_t n)
{
  if (sim_mode) { sim_request(f, n); } else { hal_rs485_send(f, n); }
}
static uint8_t bus_sending() { return sim_mode ? false : hal_rs485_sending(); }
static int bus_frame(uint8_t *buf, uint8_t max) { return sim_mode ? sim_frame(buf, max) : hal_rs485_frame(buf, max); }
static uint32_t bus_quiet_us() { return sim_mode ? 1000000UL : hal_rs485_quiet_us(); }


// ---------------------------------------------------------------------------
// Set-up and the spindle's side
// ---------------------------------------------------------------------------

void vfd_init()
{
  uint8_t type = (settings.vfd_type < VFD_N_TYPES) ? settings.vfd_type : VFD_NONE;
  if (vfd.inited && (type == vfd.type) && (settings.vfd_address == vfd.addr) &&
      (settings.vfd_baud == vfd.baud) && (settings.vfd_format == vfd.format)) {
    return;     // a reset changes nothing on the line, nor what the drive was told
  }
  uint8_t want_dir = vfd.want_dir;
  float want_rpm = vfd.want_rpm;
  memset(&vfd, 0, sizeof(vfd));
  vfd.inited = true;
  vfd.want_dir = want_dir;
  vfd.want_rpm = want_rpm;
  vfd.type = type;
  vfd.addr = settings.vfd_address;
  vfd.baud = settings.vfd_baud;
  vfd.format = settings.vfd_format;
  vfd.link = type ? LINK_WAIT : LINK_OFF;
  vfd.done_cmd = CMD_UNKNOWN;
  vfd.done_freq = FREQ_UNKNOWN;
  vfd.last_cmd = CMD_UNKNOWN;
  vfd.t_next = hal_millis();
  vfd.t_poll = vfd.t_next - VFD_POLL_MS;

  if (type == VFD_NONE) { hal_rs485_stop(); return; }
  uint32_t bits = 10 + ((vfd.format >= 2) ? 1 : 0) + (((vfd.format == 1) || (vfd.format >= 4)) ? 1 : 0);
  uint32_t baud = (vfd.baud > 0) ? vfd.baud : 9600;
  vfd.char_us = (bits * 1000000UL + baud - 1) / baud;
  vfd.gap_us = (baud <= 19200) ? (vfd.char_us * 7 + 1) / 2 : 1750;
  hal_rs485_init(vfd.baud, vfd.format, RS485_DE_PIN);
}

uint8_t vfd_enabled() { return(vfd.type != VFD_NONE); }

void vfd_spindle(uint8_t dir, float rpm)
{
  vfd.want_dir = dir;
  vfd.want_rpm = rpm;
  vfd.at_speed = false;
}

void vfd_speed(float rpm)
{
  vfd.want_rpm = rpm;
  vfd.at_speed = false;
}

void vfd_fault_reset() { if (vfd.type) { vfd.want_reset = 2; } }

uint8_t vfd_at_speed() { return(vfd.type ? vfd.at_speed : false); }
uint8_t vfd_fault() { return(vfd.type ? vfd.fault : false); }
uint8_t vfd_may_run() { return(vfd.type ? vfd.may_run : false); }

uint8_t vfd_turning()
{
  if (!vfd.type) { return(false); }
  return(vfd.may_run || (vfd.d_valid && vfd.d_turning));
}


// The command and frequency (0.01 Hz) the drive should have now.
static void vfd_target(uint8_t *cmd, uint32_t *freq)
{
  float rpm = vfd.want_rpm;
  uint32_t f = 0;
  if ((rpm > 0.0f) && (settings.rpm_max > 0.0f)) {
    float centi = rpm / settings.rpm_max * settings.vfd_max_hz * 100.0f;
    f = (centi >= 65535.0f) ? 65535UL : (uint32_t)lroundf(centi);
  }
  *freq = f;
  *cmd = f ? vfd.want_dir : VFD_STOP;
}

uint8_t vfd_trouble()
{
  if (!vfd.type) { return(false); }
  if (vfd.refused || vfd.not_following || vfd.stopped) { return(true); }
  if (vfd.link != LINK_LOST) { return(false); }
  uint8_t cmd;
  uint32_t freq;
  vfd_target(&cmd, &freq);
  return((cmd != VFD_STOP) || vfd.may_run);
}


// ---------------------------------------------------------------------------
// Transactions
// ---------------------------------------------------------------------------

static void act_send(uint32_t now)
{
  bus_send(vfd.req, vfd.req_len);
  vfd.tries++;
  vfd.n_tx++;
  vfd.t_deadline = now + (vfd.req_len * vfd.char_us + 999) / 1000 + VFD_TIMEOUT_MS + 1;
  vfd.phase = PH_WAIT;
}

static void act_start(uint8_t act, uint16_t value, uint32_t now)
{
  vfd.act = act;
  vfd.act_value = value;
  vfd.tries = 0;
  if (act == ACT_RAW) {
    memcpy(vfd.req, vfd.raw_req, vfd.raw_len);
    vfd.req_len = vfd.raw_len;
  } else {
    vfd.req_len = profile_request(act, value, vfd.req);
  }
  // The drive may act on a run command even if its answer is lost.
  if ((act == ACT_CMD) && (value != VFD_STOP)) { vfd.may_run = true; }
  if (act == ACT_POLL) { vfd.t_poll = now; }
  act_send(now);
}

// A status that came back: does the drive do what it acknowledged?
static void vfd_poll_result(const uint16_t *regs, uint32_t now)
{
  profile_decode(regs);
  vfd.fault = (vfd.d_err != 0);
  if (vfd.d_standing) { vfd.may_run = false; }

  uint8_t cmd;
  uint32_t freq;
  vfd_target(&cmd, &freq);
  // Nothing left to write: this poll shows the drive after the last write.
  uint8_t settled = (cmd == vfd.done_cmd) && ((cmd == VFD_STOP) || (freq == vfd.done_freq));
  if (vfd.d_err) {
    vfd.t_mismatch = 0;           // a fault: spindle_control takes over
    vfd.not_following = false;
  } else if (settled) {
    if ((vfd.d_run == cmd) && ((cmd == VFD_STOP) || (vfd.d_fcmd == freq))) {
      vfd.t_mismatch = 0;
      vfd.not_following = false;
      vfd.followed = true;
    } else if (vfd.followed && (cmd != VFD_STOP) && (vfd.d_run == VFD_STOP)) {
      // It ran as told and then stopped: telling it again would override
      // its keypad STOP. Not again - ALARM:23 instead.
      vfd.stopped = true;
    } else {
      if (!vfd.t_mismatch) { vfd.t_mismatch = now | 1; }
      else if ((uint32_t)(now - vfd.t_mismatch) >= VFD_CONFIRM_MS) { vfd.not_following = true; }
      vfd.done_cmd = CMD_UNKNOWN;     // tell it again
      vfd.done_freq = FREQ_UNKNOWN;
    }
  }

  uint8_t at = false;
  // Not while reversing: the output frequency is still the old direction's.
  if (settled && !vfd.d_err && (cmd != VFD_STOP) && (vfd.d_run == cmd) && !vfd.d_reversing) {
    uint32_t tol = freq * VFD_AT_SPEED_PERCENT / 100;
    if (tol < 50) { tol = 50; }
    uint32_t diff = (vfd.d_fout > freq) ? vfd.d_fout - freq : freq - vfd.d_fout;
    at = (diff <= tol);
  }
  vfd.at_speed = at;
}

static void act_done(const uint16_t *regs, uint32_t now)
{
  vfd.n_ok++;
  vfd.link = LINK_OK;
  switch (vfd.act) {
    case ACT_CMD:
      vfd.done_cmd = vfd.act_value;
      vfd.refused = false;
      vfd.at_speed = false;
      vfd.followed = false;
      break;
    case ACT_FREQ:
      vfd.done_freq = vfd.act_value;
      vfd.refused = false;
      vfd.at_speed = false;
      break;
    case ACT_RESET:
      if (vfd.want_reset == vfd.act_value + 1) { vfd.want_reset = vfd.act_value; }
      break;
    case ACT_POLL:
      vfd_poll_result(regs, now);
      break;
    case ACT_RAW:
      if (vfd.raw_n) { memcpy(vfd.raw_regs, regs, vfd.raw_n * sizeof(uint16_t)); }
      else { vfd.done_cmd = CMD_UNKNOWN; vfd.done_freq = FREQ_UNKNOWN; }   // a write may have changed either
      vfd.raw_state = RAW_OK;
      break;
  }
  vfd.act = ACT_NONE;
}

// exc: the drive's exception code, 0 for no (valid) answer at all.
static void act_failed(uint8_t exc, uint32_t now)
{
  vfd.n_fail++;
  vfd.t_next = now + VFD_POLL_MS;
  if (exc) {
    vfd.n_exc++;
    vfd.last_exc = exc;
    vfd.link = LINK_OK;
    if ((vfd.act == ACT_CMD) || (vfd.act == ACT_FREQ)) { vfd.refused = true; }
    if (vfd.act == ACT_RESET) { vfd.want_reset = 0; }
  } else {
    vfd.link = LINK_LOST;
    vfd.at_speed = false;
  }
  if (vfd.act == ACT_RAW) { vfd.raw_state = exc ? RAW_REFUSED : RAW_LOST; }
  vfd.act = ACT_NONE;
}

void vfd_service()
{
  if (!vfd.type) { return; }
  uint32_t now = hal_millis();

  if (vfd.act != ACT_NONE) {
    if (vfd.phase == PH_WAIT) {
      if (bus_sending()) { return; }
      uint8_t rx[HAL_RS485_FRAME_MAX];
      uint16_t regs[RAW_MAX_REGS];
      int n = bus_frame(rx, sizeof(rx));
      if (n != 0) {
        int r = (n < 0) ? MB_BAD : mb_check(vfd.req, rx, n, regs);
        if (r == MB_OK) { act_done(regs, now); return; }
        if (r > 0) { act_failed((uint8_t)r, now); return; }
      } else if ((int32_t)(now - vfd.t_deadline) < 0) {
        return;
      }
      // Broken, not ours, or nothing: once more, or give up.
      if (vfd.tries > VFD_RETRIES) { act_failed(0, now); return; }
      vfd.n_retry++;
      vfd.phase = PH_RESEND;
    }
    if (bus_quiet_us() >= vfd.gap_us) { act_send(now); }
    return;
  }

  if (bus_quiet_us() < vfd.gap_us) { return; }
  if ((int32_t)(now - vfd.t_next) < 0) { return; }

  uint8_t cmd;
  uint32_t freq;
  vfd_target(&cmd, &freq);
  if (cmd != vfd.last_cmd) {       // a new command gets its own VFD_CONFIRM_MS, and a clean slate
    vfd.last_cmd = cmd;
    vfd.t_mismatch = 0;
    vfd.not_following = false;
    vfd.refused = false;
    vfd.followed = false;
    vfd.stopped = false;
  }

  if ((cmd == VFD_STOP) && (vfd.done_cmd != VFD_STOP)) { act_start(ACT_CMD, VFD_STOP, now); }
  else if (vfd.want_reset) { act_start(ACT_RESET, vfd.want_reset - 1, now); }
  else if (vfd.raw_state == RAW_PENDING) { act_start(ACT_RAW, 0, now); }
  else if ((cmd != VFD_STOP) && (freq != vfd.done_freq)) { act_start(ACT_FREQ, (uint16_t)freq, now); }
  else if (cmd != vfd.done_cmd) { act_start(ACT_CMD, cmd, now); }
  else if ((uint32_t)(now - vfd.t_poll) >= VFD_POLL_MS) { act_start(ACT_POLL, 0, now); }
}


// ---------------------------------------------------------------------------
// Reports and $VFD
// ---------------------------------------------------------------------------

static void print_hex4(uint16_t v)
{
  static const char digits[] = "0123456789ABCDEF";
  int8_t sh;
  for (sh = 12; sh >= 0; sh -= 4) { serial_write(digits[(v >> sh) & 0xF]); }
}

// 0.01 Hz as Hz with two decimals.
static void print_centi(uint32_t v)
{
  print_uint32_base10(v / 100);
  serial_write('.');
  serial_write('0' + (v / 10) % 10);
  serial_write('0' + v % 10);
}

static void print_dir(uint8_t d)
{
  printPgmString((d == VFD_FWD) ? PSTR("FWD") : ((d == VFD_REV) ? PSTR("REV") : PSTR("STOP")));
}

static uint8_t link_code()
{
  switch (vfd.link) {
    case LINK_WAIT: return 1;
    case LINK_OK: return 2;
    case LINK_LOST: return 3;
  }
  return 0;
}

void vfd_report_diag()
{
  printPgmString(PSTR(" vfd="));
  print_uint8_base10(link_code());
  serial_write('/');
  print_uint32_base10(vfd.d_valid ? vfd.d_err : 0);
  serial_write('/');
  print_uint32_base10(vfd.d_valid ? vfd.d_fout : 0);
  serial_write('/');
  print_uint32_base10(vfd.n_fail);
}

// [VFD:Delta link=OK want=FWD,200.00 drive=FWD,200.00,199.50 err=0 st=0503
//  remote=3 at=1 trouble=0 tx=120 ok=118 retry=2 fail=0 exc=0/0 sim=0 clk=120]
static void vfd_report()
{
  static const char *const link_names[4] = { "OFF", "WAIT", "OK", "LOST" };
  uint8_t cmd;
  uint32_t freq;
  vfd_target(&cmd, &freq);
  printPgmString(PSTR("[VFD:"));
  printPgmString((vfd.type == VFD_DELTA) ? PSTR("Delta") : PSTR("none"));
  printPgmString(PSTR(" link="));
  printString(link_names[link_code()]);
  printPgmString(PSTR(" want="));
  print_dir(cmd);
  serial_write(',');
  print_centi(freq);
  printPgmString(PSTR(" drive="));
  if (vfd.d_valid) {
    print_dir(vfd.d_run);
    serial_write(',');
    print_centi(vfd.d_fcmd);
    serial_write(',');
    print_centi(vfd.d_fout);
  } else {
    serial_write('-');
  }
  printPgmString(PSTR(" err="));
  print_uint32_base10(vfd.d_valid ? vfd.d_err : 0);
  printPgmString(PSTR(" st="));
  print_hex4(vfd.d_valid ? vfd.d_status : 0);
  printPgmString(PSTR(" remote="));
  print_uint8_base10(vfd.d_valid ? vfd.d_remote : 0);
  printPgmString(PSTR(" at="));
  print_uint8_base10(vfd_at_speed());
  printPgmString(PSTR(" trouble="));
  print_uint8_base10(vfd.refused ? 2 : (vfd.not_following ? 3 : (vfd.stopped ? 4 : (vfd_trouble() ? 1 : 0))));
  printPgmString(PSTR(" tx="));
  print_uint32_base10(vfd.n_tx);
  printPgmString(PSTR(" ok="));
  print_uint32_base10(vfd.n_ok);
  printPgmString(PSTR(" retry="));
  print_uint32_base10(vfd.n_retry);
  printPgmString(PSTR(" fail="));
  print_uint32_base10(vfd.n_fail);
  printPgmString(PSTR(" exc="));
  print_uint32_base10(vfd.n_exc);
  serial_write('/');
  print_uint8_base10(vfd.last_exc);
  printPgmString(PSTR(" sim="));
  print_uint8_base10(sim_mode);
  printPgmString(PSTR(" clk="));
  print_uint32_base10(hal_rs485_clock_hz() / 1000000UL);
  printPgmString(PSTR("]\r\n"));
}

// Up to four hex digits from line[*i] on.
static uint8_t read_hex16(char *line, uint8_t *i, uint16_t *v)
{
  uint8_t n = 0;
  *v = 0;
  for (;;) {
    char c = line[*i];
    uint8_t d;
    if ((c >= '0') && (c <= '9')) { d = c - '0'; }
    else if ((c >= 'A') && (c <= 'F')) { d = c - 'A' + 10; }
    else { break; }
    if (++n > 4) { return(false); }
    *v = (*v << 4) | d;
    (*i)++;
  }
  return(n > 0);
}

uint8_t vfd_command(char *line)
{
  if (line[4] == 0) { vfd_report(); return(STATUS_OK); }
  if (line[4] != '=') { return(STATUS_INVALID_STATEMENT); }
  if (!vfd.type) { return(STATUS_SETTING_DISABLED); }
  if ((sys.state != STATE_IDLE) && (sys.state != STATE_ALARM)) { return(STATUS_IDLE_ERROR); }
  if (vfd.raw_state == RAW_PENDING) { return(STATUS_IDLE_ERROR); }

  char op = line[5];
  uint8_t i = 6;
  uint16_t reg, v = 1;
  if (((op != 'R') && (op != 'W')) || !read_hex16(line, &i, &reg)) { return(STATUS_INVALID_STATEMENT); }
  if (line[i] == ',') {
    i++;
    if (!read_hex16(line, &i, &v)) { return(STATUS_INVALID_STATEMENT); }
  } else if (op == 'W') {
    return(STATUS_INVALID_STATEMENT);
  }
  if (line[i] != 0) { return(STATUS_INVALID_STATEMENT); }
  if (op == 'R') {
    if ((v < 1) || (v > RAW_MAX_REGS)) { return(STATUS_INVALID_STATEMENT); }
    vfd.raw_len = mb_read(vfd.raw_req, vfd.addr, reg, (uint8_t)v);
    vfd.raw_n = (uint8_t)v;
  } else {
    vfd.raw_len = mb_write(vfd.raw_req, vfd.addr, reg, v);
    vfd.raw_n = 0;
  }
  vfd.raw_state = RAW_PENDING;
  while (vfd.raw_state == RAW_PENDING) {
    protocol_execute_realtime();
    if (sys.abort) { vfd.raw_state = RAW_IDLE; return(STATUS_OK); }
  }
  if (vfd.raw_state == RAW_LOST) { vfd.raw_state = RAW_IDLE; return(STATUS_VFD_TROUBLE); }
  if (vfd.raw_state == RAW_REFUSED) {
    vfd.raw_state = RAW_IDLE;
    printPgmString(PSTR("[MSG:VFD exception "));
    print_uint8_base10(vfd.last_exc);
    printPgmString(PSTR("]\r\n"));
    return(STATUS_VFD_REFUSED);
  }
  vfd.raw_state = RAW_IDLE;
  printPgmString(PSTR("[VFD:"));
  serial_write(op);
  print_hex4(reg);
  serial_write('=');
  if (op == 'R') {
    uint8_t k;
    for (k = 0; k < vfd.raw_n; k++) {
      if (k) { serial_write(','); }
      print_hex4(vfd.raw_regs[k]);
    }
  } else {
    print_hex4(v);
  }
  printPgmString(PSTR("]\r\n"));
  return(STATUS_OK);
}


// ---------------------------------------------------------------------------
// Test hook: a simulated Delta drive in place of the RS485 port ($TVFD)
// ---------------------------------------------------------------------------
//
// It gets the very frames the port would send and answers like a Delta drive
// with 01-00 = 600 Hz would (2000H-2002H, 2100H-2104H; anything else is
// exception 2, a frequency above 600 Hz exception 3), SIM_DELAY_MS later.
// Its output frequency ramps at SIM_RAMP_PER_MS. A fault coasts the motor;
// while its cause lasts ($TVFD=2) the fault reset does nothing, after that
// ($TVFD=1) the error stays latched until the drive gets one.

#define SIM_OFF      0
#define SIM_OK       1
#define SIM_FAULT    2
#define SIM_SILENT   3
#define SIM_GARBLE   4
#define SIM_KEYPAD   5
#define SIM_KSTOP    6           // once: its keypad STOP, then healthy (it runs when told again)
#define SIM_N_MODES  7

#define SIM_RAMP_PER_MS  40      // 0.01 Hz per ms = 400 Hz/s
#define SIM_DELAY_MS     3
#define SIM_FMAX         60000   // 600.00 Hz

static struct {
  uint8_t running, dir;
  uint16_t fcmd, fout, err;
  uint32_t t;
  uint8_t resp[HAL_RS485_FRAME_MAX];
  uint8_t resp_len;
  uint32_t resp_at;
  uint8_t flip;
} sim;

static void sim_update(uint32_t now)
{
  uint32_t dt = now - sim.t;
  uint32_t step = ((dt > 60000UL) ? 60000UL : dt) * SIM_RAMP_PER_MS;
  uint16_t target = sim.running ? sim.fcmd : 0;
  sim.t = now;
  if (sim.fout < target) { sim.fout = (target - sim.fout > step) ? sim.fout + step : target; }
  else if (sim.fout > target) { sim.fout = (sim.fout - target > step) ? sim.fout - step : target; }
}

static uint16_t sim_reg(uint16_t reg)
{
  switch (reg) {
    case 0x2100: return sim.err;
    case 0x2101: {
      uint16_t st = sim.running ? (sim.fcmd ? 3 : 2) : (sim.fout ? 1 : 0);
      if (sim.dir == VFD_REV) { st |= 3 << 3; }
      if (sim_mode != SIM_KEYPAD) { st |= (1 << 8) | (1 << 10); }
      return st;
    }
    case 0x2102: return sim.fcmd;
    case 0x2103: return sim.fout;
    case 0x2104: return sim.fout / 400;          // some output current, 0.01 A
  }
  return 0;
}

static void sim_request(const uint8_t *f, uint8_t n)
{
  uint32_t now = hal_millis();
  sim_update(now);
  sim.resp_len = 0;
  if ((n != 8) || (mb_crc(f, 6) != (uint16_t)(f[6] | (f[7] << 8)))) { return; }   // a drive ignores it
  if ((f[0] != settings.vfd_address) || (sim_mode == SIM_SILENT)) { return; }

  uint8_t *r = sim.resp, len = 0, exc = 0;
  uint16_t reg = (f[2] << 8) | f[3], v = (f[4] << 8) | f[5];
  if (f[1] == 0x06) {
    if (reg == DELTA_REG_CMD) {
      if ((sim_mode != SIM_KEYPAD) && !sim.err) {
        uint8_t d = (v >> 4) & 3;
        if (d == 1) { sim.dir = VFD_FWD; }
        else if (d == 2) { sim.dir = VFD_REV; }
        else if (d == 3) { sim.dir = (sim.dir == VFD_REV) ? VFD_FWD : VFD_REV; }
        if ((v & 3) == 1) { sim.running = false; }
        else if ((v & 3) >= 2) { sim.running = true; }
      }
    } else if (reg == DELTA_REG_FREQ) {
      if (v > SIM_FMAX) { exc = 3; } else { sim.fcmd = v; }
    } else if (reg == DELTA_REG_CTRL) {
      if ((v & 2) && (sim_mode != SIM_FAULT)) { sim.err = 0; }
    } else {
      exc = 2;
    }
    if (!exc) { memcpy(r, f, 6); len = 6; }
  } else if (f[1] == 0x03) {
    if ((v == 0) || (v > 5) || (reg < 0x2100) || (reg + v > 0x2105)) {
      exc = 2;
    } else {
      uint8_t k;
      r[0] = f[0]; r[1] = 0x03; r[2] = 2 * v;
      for (k = 0; k < v; k++) {
        uint16_t x = sim_reg(reg + k);
        r[3 + 2 * k] = x >> 8;
        r[4 + 2 * k] = x & 0xFF;
      }
      len = 3 + 2 * v;
    }
  } else {
    exc = 1;
  }
  if (exc) { r[0] = f[0]; r[1] = f[1] | 0x80; r[2] = exc; len = 3; }
  len = mb_finish(r, len);
  if ((sim_mode == SIM_GARBLE) && (sim.flip ^= 1)) { r[len - 1] ^= 0x5A; }
  sim.resp_len = len;
  sim.resp_at = now + SIM_DELAY_MS;
}

static int sim_frame(uint8_t *buf, uint8_t max)
{
  if (!sim.resp_len || ((int32_t)(hal_millis() - sim.resp_at) < 0)) { return 0; }
  int n = (sim.resp_len <= max) ? sim.resp_len : max;
  memcpy(buf, sim.resp, n);
  sim.resp_len = 0;
  return n;
}

uint8_t vfd_test(char *line)
{
  uint8_t char_counter = 6;
  float mode, code = 2;
  if (!read_float(line, &char_counter, &mode)) { return(STATUS_BAD_NUMBER_FORMAT); }
  if (line[char_counter] == ',') {
    char_counter++;
    if (!read_float(line, &char_counter, &code)) { return(STATUS_BAD_NUMBER_FORMAT); }
  }
  if ((line[char_counter] != 0) || (mode < 0) || (mode >= SIM_N_MODES) || (code < 1) || (code > 255)) {
    return(STATUS_INVALID_STATEMENT);
  }
  uint8_t m = (uint8_t)mode;
  if (m == SIM_OFF) {
    sim_mode = SIM_OFF;
    memset(&sim, 0, sizeof(sim));
    return(STATUS_OK);
  }
  if (sim_mode == SIM_OFF) {
    memset(&sim, 0, sizeof(sim));
    sim.dir = VFD_FWD;
    sim.t = hal_millis();
  }
  sim_update(hal_millis());
  if (m == SIM_KSTOP) {           // ramps down like a stop command
    sim.running = false;
    m = SIM_OK;
  }
  sim_mode = m;
  if (m == SIM_FAULT) {           // trips: output off, the motor coasts
    sim.err = (uint16_t)code;
    sim.running = false;
    sim.fout = 0;
  }
  return(STATUS_OK);
}
