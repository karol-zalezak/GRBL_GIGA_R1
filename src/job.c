/*
  job.c - running G-code from the USB stick (M7 side)

  Part of Grbl-Giga, a port of Grbl 1.1 to the Arduino GIGA R1 WiFi.

  Grbl is free software: you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation, either version 3 of the License, or (at your option) any later
  version. Distributed WITHOUT ANY WARRANTY; see the GNU GPL for details.

  -------------------------------------------------------------------------------

  The M4 owns the stick (src/m4/usbstick.cpp) and streams a file into the
  shared ring of grbl_job.h; this side decides everything about the job and
  feeds its lines into Grbl through the same function the PC's lines go
  through (protocol_execute_line()), from the main loop, as fast as the
  planner takes them. A program runs so:

    1. Start ($F=name, or START on the display's FILE page): only in Idle,
       with an empty planner, no STOP, and - from the display - the PC quiet.
    2. Check pass: a file not run before (name, size and date) goes through
       Grbl's check mode first ($C: parsed, no motion), which also counts its
       lines and finds the X/Y/Z range it moves in. An error ends the job
       there, with its line; nothing has moved. The parser state from before
       the check is put back afterwards, G92 and all.
    3. Run: the file again, from the top. M0 and M1 pause (START resumes),
       M6 is the tool change pause ($58), M2/M30 end it.
    4. End: the last line parsed and the machine idle.

  While a program runs, the PC gets status reports, real-time commands and
  the read-only $ commands; a G-code line from it is refused with
  STATUS_JOB_RUNNING. The status report carries |SD:<percent>,<name>, as in
  grblHAL.

  STOP ($FS, or STOP on the display) holds the machine, waits until it stands
  and then resets Grbl with the machine declared stopped - the position is
  kept, no ALARM:3. A G-code error while running stops it the same way and
  ends in ALARM:21; a reset, an alarm or the central STOP end it at once; a
  stick pulled out during the run stops it like STOP.

  Files also go onto the stick from the PC, through the serial link
  (tools/usbfiles.py): $FW=name opens the upload, $FW+<hex> lines carry the
  data, $FWE=<YYYYMMDDhhmmss> ends it with the PC file's date (which the
  stick then keeps), $FWA gives it up; $FD=name deletes a file; $FF=ERASE
  formats the stick (FAT32 up to 32 GB, exFAT above; $FF=EXFAT: exFAT). Only
  in Idle or Alarm, and not while a program runs.
*/

#include "grbl.h"
#include "grbl_job.h"
#include <strings.h>

#define JOB_REQ_TIMEOUT_MS  3000
#define JOB_WRITE_TIMEOUT_MS 10000  // the stick writing, closing, renaming
#define JOB_FORMAT_TIMEOUT_MS 900000UL  // formatting a big slow stick FAT32
#define JOB_LINES_PER_PASS  64      // lines fed per main-loop pass
#define JOB_PC_QUIET_MS     1000    // the display starts a job only after the PC was quiet this long
#define JOB_N_KNOWN         4       // files remembered as checked

static grbl_job_t *const J = GRBL_JOB;
_Static_assert(GRBL_JOB_ADDR + sizeof(grbl_job_t) <= 0x3800FC00UL, "job block runs into the PDM buffer");

static uint8_t state = GJOB_IDLE;
static char lbuf[LINE_BUFFER_SIZE];
static uint8_t lcount = 0, lflags = 0;
static uint32_t lines = 0;           // lines taken from the file
static uint8_t at_eof = false;       // every line has been taken
static uint8_t prog_end = false;     // M2/M30 parsed
static uint8_t stop_alarm = 0;       // STOPPING: alarm to raise once stopped (0 = none)
static uint8_t our_reset = false;    // the coming reset is the job's own STOP
static parser_state_t gc_saved;      // parser state from before the check pass
static uint8_t bounds_valid = false;
static float bmin[3], bmax[3];

typedef struct {
  char name[GJOB_NAME_LEN];
  uint32_t size, mtime, lines;
  float min[3], max[3];
} job_known_t;
static job_known_t known[JOB_N_KNOWN];
static uint8_t known_next = 0;

// $FW upload to the stick
static uint8_t uploading = false;
static uint32_t up_wr = 0;


void job_boot()
{
  // SRAM4 holds whatever it held before a reset; the M4 sets up its half
  // when it starts, after this.
  memset((void *)J, 0, offsetof(grbl_job_t, ring));
}


static uint8_t job_m4_ready() { return hal_display_present() && (J->magic == GRBL_JOB_MAGIC); }

uint8_t job_active() { return (state == GJOB_CHECK) || (state == GJOB_RUN) || (state == GJOB_STOPPING); }
uint8_t job_running() { return state == GJOB_RUN; }


// The serial $ commands a running program leaves to the PC: the read-only
// ones and STOP.
uint8_t job_allows(char *line)
{
  if (line[1] == 0) { return(true); }                                   // $ help
  if (line[2] == 0) {
    switch (line[1]) { case '$': case 'G': case '#': case 'I': case 'F': return(true); }
  }
  if ((line[1] == 'F') && (line[2] == 'S') && (line[3] == 0)) { return(true); }   // $FS: STOP
  if ((line[1] == 'N') && (line[2] == 0)) { return(true); }
  // The screenshot only while nothing moves: sending the framebuffer holds
  // the main loop for a second or two, and the step segments would run out.
  if ((strcmp(line, "$SCREEN") == 0) && !(sys.state & (STATE_CYCLE | STATE_JOG | STATE_HOMING))) { return(true); }
  if (strncmp(line, "$TOUCH=", 7) == 0) { return(true); }               // test hooks
  if (strncmp(line, "$TIN=", 5) == 0) { return(true); }
  if (strncmp(line, "$TSW=", 5) == 0) { return(true); }
  return(false);
}


// Sends a request to the M4 and waits for its answer, keeping the link and
// the display going. Returns the M4's status, or -100 on a timeout.
static int8_t job_request_t(uint8_t code, const char *name, uint32_t timeout_ms)
{
  if (name) {
    strncpy((char *)J->req_name, name, GJOB_NAME_LEN - 1);
    J->req_name[GJOB_NAME_LEN - 1] = 0;
  }
  J->req_code = code;
  hal_memory_barrier();
  uint8_t seq = (uint8_t)(J->req_seq + 1);
  J->req_seq = seq;
  uint32_t t0 = hal_millis();
  while (J->ack_seq != seq) {
    protocol_service_link();
    if ((uint32_t)(hal_millis() - t0) > timeout_ms) { return -100; }
  }
  hal_memory_barrier();
  return J->ack_status;
}

static int8_t job_request(uint8_t code, const char *name) { return job_request_t(code, name, JOB_REQ_TIMEOUT_MS); }

// The same without waiting (from places that must not loop).
static void job_post(uint8_t code)
{
  J->req_code = code;
  hal_memory_barrier();
  J->req_seq = (uint8_t)(J->req_seq + 1);
}


static void job_set_state(uint8_t s)
{
  state = s;
  J->job_state = s;
}


static void job_message(const char *what)
{
  printPgmString(PSTR("[MSG:USB job "));
  printString((char *)J->job_name);
  serial_write(' ');
  printString((char *)what);
}

static void job_message_end() { printPgmString(PSTR("]\r\n")); }


// Where the parser stands, in work coordinates (for the range of the program).
static void job_track_bounds()
{
  uint8_t i;
  for (i = 0; i < 3; i++) {
    float w = gc_state.position[i] - gc_state.coord_system[i] - gc_state.coord_offset[i];
    if (i == TOOL_LENGTH_OFFSET_AXIS) { w -= gc_state.tool_length_offset; }
    if (!bounds_valid || (w < bmin[i])) { bmin[i] = w; }
    if (!bounds_valid || (w > bmax[i])) { bmax[i] = w; }
  }
  bounds_valid = true;
}


static job_known_t *job_find_known(const char *name, uint32_t size, uint32_t mtime)
{
  uint8_t i;
  for (i = 0; i < JOB_N_KNOWN; i++) {
    if (known[i].name[0] && (strcasecmp(known[i].name, name) == 0) &&
        (known[i].size == size) && (known[i].mtime == mtime)) { return &known[i]; }
  }
  return 0;
}


// The next line from the ring into lbuf: 1 = a line, 0 = not yet (the M4 is
// still reading), -1 = end of file, -2 = the stick failed.
static int8_t job_next_line()
{
  for (;;) {
    uint32_t wr = J->wr, rd = J->rd;
    hal_memory_barrier();
    if (rd == wr) {
      if (J->rd_err) { return -2; }
      if (J->eof && (J->wr == rd)) {
        if (lcount || lflags) {             // a last line without a line end
          lbuf[lcount] = 0;
          return 1;
        }
        return -1;
      }
      return 0;
    }
    uint8_t c = J->ring[rd & (GJOB_RING_SIZE - 1)];
    J->rd = rd + 1;
    if (protocol_line_char(lbuf, &lcount, &lflags, c)) { return 1; }
  }
}


static void job_publish_progress()
{
  J->job_line = lines;
  J->job_pos = J->rd;
}


// The check pass is over (or failed): parser back to where it was, no motion
// has happened.
static void job_leave_check()
{
  memcpy(&gc_state, &gc_saved, sizeof(parser_state_t));
  gc_sync_position();
  plan_sync_position();
  sys.state = STATE_IDLE;
}


static void job_fail_check(uint8_t status)
{
  job_leave_check();
  J->job_err = status;
  J->job_err_line = lines;
  J->job_end_ms = hal_millis();
  job_set_state(GJOB_FAILED);
  job_post(GJOB_REQ_CLOSE);
  job_message("check: error:");
  print_uint8_base10(status);
  printPgmString(PSTR(" in line "));
  print_uint32_base10(lines);
  job_message_end();
}


// Opens the file from the top, for the check pass or the run.
static uint8_t job_open()
{
  J->rd = 0;
  lcount = lflags = 0;
  lines = 0;
  at_eof = prog_end = false;
  if (job_request(GJOB_REQ_OPEN, (const char *)J->job_name) != 0) { return(STATUS_USB_FILE); }
  return(STATUS_OK);
}


static void job_begin_run()
{
  if (job_open() != STATUS_OK) {
    J->job_err = STATUS_USB_FILE;
    job_set_state(GJOB_FAILED);
    return;
  }
  J->job_start_ms = hal_millis();
  J->job_end_ms = 0;
  job_set_state(GJOB_RUN);
  job_message("running");
  job_message_end();
}


static void job_check_done()
{
  job_leave_check();
  job_known_t *k = &known[known_next];
  known_next = (known_next + 1) % JOB_N_KNOWN;
  strcpy(k->name, (const char *)J->job_name);
  k->size = J->job_size;
  k->mtime = J->f_mtime;
  k->lines = lines;
  uint8_t i;
  for (i = 0; i < 3; i++) {
    k->min[i] = bounds_valid ? bmin[i] : 0;
    k->max[i] = bounds_valid ? bmax[i] : 0;
    J->job_min[i] = k->min[i];
    J->job_max[i] = k->max[i];
  }
  J->job_lines = lines;
  job_message("check: ok, ");
  print_uint32_base10(lines);
  printPgmString(PSTR(" lines"));
  job_message_end();
  job_begin_run();
}


uint8_t job_start(const char *name, uint8_t from_display)
{
  uint8_t i;
  if (job_active()) { return(STATUS_JOB_RUNNING); }
  if (!job_m4_ready()) { return(STATUS_USB_NO_STICK); }
  // The file must be in the listing (size and date come from there).
  int16_t found = -1;
  for (i = 0; i < J->n_files; i++) {
    if (strcasecmp((const char *)J->file[i].name, name) == 0) { found = i; break; }
  }
  if (found < 0) { return(STATUS_USB_FILE); }
  if ((sys.state != STATE_IDLE) || (plan_get_current_block() != NULL)) { return(STATUS_IDLE_ERROR); }
  if (estop_latched || estop_active()) { return(STATUS_ESTOP_ACTIVE); }
  if (from_display && (((uint32_t)(hal_millis() - protocol_last_line_ms) < JOB_PC_QUIET_MS) ||
                       serial_get_rx_buffer_count())) {
    return(STATUS_JOB_PC_BUSY);
  }

  // The listing's name, as the stick spells it.
  strcpy((char *)J->job_name, (const char *)J->file[found].name);
  J->job_size = J->file[found].size;
  J->job_err = 0;
  J->job_alarm = 0;
  J->job_err_line = 0;
  J->job_pos = 0;
  J->job_line = 0;
  J->job_start_ms = hal_millis();
  J->job_end_ms = 0;
  stop_alarm = 0;

  if (job_open() != STATUS_OK) { return(STATUS_USB_FILE); }
  J->job_size = J->f_size;

  job_known_t *k = job_find_known((const char *)J->job_name, J->f_size, J->f_mtime);
  if (k) {
    // Run before, unchanged: straight to the run.
    J->job_checked = 1;
    J->job_lines = k->lines;
    for (i = 0; i < 3; i++) { J->job_min[i] = k->min[i]; J->job_max[i] = k->max[i]; }
    J->job_start_ms = hal_millis();
    job_set_state(GJOB_RUN);
    job_message("running (checked before)");
    job_message_end();
  } else {
    // First the check pass, from the parser state as it is now.
    J->job_checked = 0;
    J->job_lines = 0;
    bounds_valid = false;
    memcpy(&gc_saved, &gc_state, sizeof(parser_state_t));
    sys.state = STATE_CHECK_MODE;
    job_set_state(GJOB_CHECK);
    job_message("check pass");
    job_message_end();
  }
  return(STATUS_OK);
}


// STOP: from the PC ($FS), the display, an error or a lost stick. Safe from
// any context: it only asks for a feed hold; job_rt_service() finishes it.
void job_stop(uint8_t alarm)
{
  if (state == GJOB_CHECK) {
    // Nothing moves in the check pass: end it on the spot (main loop).
    stop_alarm = alarm;
    job_set_state(GJOB_STOPPING);
    return;
  }
  if (state != GJOB_RUN) { return; }
  stop_alarm = alarm;
  job_set_state(GJOB_STOPPING);
  system_set_exec_state_flag(EXEC_FEED_HOLD);
}


// A G-code error in the file.
static void job_error(uint8_t status)
{
  J->job_err = status;
  J->job_err_line = lines;
  if (state == GJOB_CHECK) { job_fail_check(status); return; }
  job_message("stopped: error:");
  print_uint8_base10(status);
  printPgmString(PSTR(" in line "));
  print_uint32_base10(lines);
  job_message_end();
  job_stop(EXEC_ALARM_JOB_ERROR);
}


// The stick went away (or failed) before the whole file was read: the
// program cannot be finished, so it stops at once - not after the lines
// still in the ring, which could be seconds of motion.
static void job_stick_lost()
{
  J->job_err = STATUS_USB_FILE;
  J->job_err_line = lines;
  job_message("stopped: the USB stick was removed or cannot be read");
  job_message_end();
  job_stop(0);
}


// Feeds the program's lines. Main loop only, with no line from the PC half
// received.
void job_service()
{
  if ((state != GJOB_CHECK) && (state != GJOB_RUN)) { return; }
  if (sys.state == STATE_ALARM) { return; }               // job_rt_service() ends it
  uint8_t n;
  for (n = 0; (n < JOB_LINES_PER_PASS) && !prog_end && !at_eof; n++) {
    int8_t r = job_next_line();
    if (r == 0) { break; }
    if (r == -2) { job_stick_lost(); return; }     // the check pass (a run stops in job_rt_service())
    if (r < 0) { at_eof = true; break; }
    lines++;
    uint8_t status = protocol_execute_line(lbuf, lflags, true);
    lcount = lflags = 0;
    if (sys.abort) { return; }
    job_publish_progress();
    if ((state != GJOB_CHECK) && (state != GJOB_RUN)) { return; }    // stopped meanwhile
    if (status != STATUS_OK) { job_error(status); return; }
    if (state == GJOB_CHECK) { job_track_bounds(); }
  }
  job_publish_progress();

  if (state == GJOB_CHECK) {
    if (at_eof || prog_end) { job_check_done(); }
  } else if ((at_eof || prog_end) && (plan_get_current_block() == NULL) && (sys.state == STATE_IDLE)) {
    J->job_end_ms = hal_millis();
    job_set_state(GJOB_DONE);
    job_post(GJOB_REQ_CLOSE);
    job_message("done, ");
    print_uint32_base10(lines);
    printPgmString(PSTR(" lines in "));
    print_uint32_base10((J->job_end_ms - J->job_start_ms) / 1000);
    printPgmString(PSTR(" s"));
    job_message_end();
  }
}


// From every wait loop (protocol_exec_rt_system and the critical alarm loop):
// ends a STOP once the machine stands, and a job that an alarm has ended.
void job_rt_service()
{
  if (state == GJOB_STOPPING) {
    if (sys.state == STATE_CHECK_MODE) {
      // A STOP in the check pass: nothing moved.
      job_leave_check();
      J->job_end_ms = hal_millis();
      job_set_state(GJOB_ABORTED);
      job_post(GJOB_REQ_CLOSE);
      return;
    }
    uint8_t moving = (sys.state & (STATE_CYCLE | STATE_JOG | STATE_HOMING)) ||
                     ((sys.state == STATE_HOLD) && !(sys.suspend & SUSPEND_HOLD_COMPLETE)) ||
                     ((sys.state == STATE_SAFETY_DOOR) && !(sys.suspend & SUSPEND_RETRACT_COMPLETE));
    if (moving && (sys.state != STATE_ALARM)) { return; }
    // It stands: reset Grbl with the machine declared stopped, so the reset
    // does not take it for an aborted cycle - the position is kept.
    J->job_end_ms = hal_millis();
    J->job_alarm = stop_alarm;
    job_set_state(stop_alarm ? GJOB_FAILED : GJOB_ABORTED);
    job_post(GJOB_REQ_CLOSE);
    our_reset = true;
    if (sys.state != STATE_ALARM) {
      sys.step_control = STEP_CONTROL_NORMAL_OP;
      sys.suspend = SUSPEND_DISABLE;
      sys.state = stop_alarm ? STATE_ALARM : STATE_IDLE;
      mc_reset();
      if (stop_alarm) { system_set_exec_alarm(stop_alarm); }
    }
    return;
  }
  if ((state == GJOB_RUN) && J->rd_err) { job_stick_lost(); return; }
  if (((state == GJOB_RUN) || (state == GJOB_CHECK)) && (sys.state == STATE_ALARM)) {
    // An alarm (limit, probe, STOP, servo, spindle ...) has ended it.
    J->job_end_ms = hal_millis();
    J->job_alarm = sys_rt_exec_alarm ? sys_rt_exec_alarm : system_last_alarm;
    job_set_state(GJOB_ABORTED);
    job_post(GJOB_REQ_CLOSE);
  }
}


// Every reset (start of protocol_main_loop): one the job did not ask for
// ends it.
void job_reset()
{
  if (job_active()) {
    if (state == GJOB_CHECK) { sys.state = STATE_IDLE; }    // the reset re-initialised the parser
    J->job_end_ms = hal_millis();
    J->job_alarm = system_last_alarm;
    job_set_state(GJOB_ABORTED);
    job_post(GJOB_REQ_CLOSE);
    job_message("aborted by a reset, line ");
    print_uint32_base10(lines);
    job_message_end();
  } else if (our_reset && ((state == GJOB_ABORTED) || (state == GJOB_FAILED))) {
    job_message(state == GJOB_FAILED ? "stopped at line " : "stopped, line ");
    print_uint32_base10(lines);
    job_message_end();
  }
  our_reset = false;
  if (uploading) { job_post(GJOB_REQ_UP_ABORT); }   // the PC gave up, or will start again
  uploading = false;
}


// M2/M30 from the file: the program ends there.
void job_program_end() { if (job_active()) { prog_end = true; } }


// |SD:<percent>,<name> for the status report (grblHAL's field).
void job_report_status()
{
  if (!job_active()) { return; }
  printPgmString(PSTR("|SD:"));
  uint32_t size = J->job_size;
  printFloat(size ? (float)J->rd * 100.0f / (float)size : 0.0f, 1);
  serial_write(',');
  printString((char *)J->job_name);
}


// ---------------------------------------------------------------------------
// $F commands
// ---------------------------------------------------------------------------

static const char *job_fat_name(uint8_t t)
{
  switch (t) {
    case 1: return PSTR("FAT12");
    case 2: return PSTR("FAT16");
    case 3: return PSTR("FAT32");
    case 4: return PSTR("exFAT");
  }
  return PSTR("?");
}

static void print_uint8_base10_2(uint8_t n)
{
  serial_write('0' + (n / 10) % 10);
  serial_write('0' + n % 10);
}

static void job_print_state()
{
  printPgmString(PSTR("[MSG:USB stick: "));
  if (!job_m4_ready()) { printPgmString(PSTR("display core not running")); }
  else {
    switch (J->usb_state) {
      case GJOB_USB_OFF: printPgmString(PSTR("starting")); break;
      case GJOB_USB_NONE: printPgmString(PSTR("none")); break;
      case GJOB_USB_MOUNTING: printPgmString(PSTR("mounting")); break;
      case GJOB_USB_READY:
        printPgmString(PSTR("ready, "));
        printPgmString(job_fat_name(J->usb_fat));
        printPgmString(PSTR(", "));
        print_uint32_base10(J->usb_size_mb);
        printPgmString(PSTR(" MB, "));
        print_uint32_base10(J->usb_free_mb);
        printPgmString(PSTR(" MB free"));
        break;
      case GJOB_USB_FORMATTING: printPgmString(PSTR("formatting")); break;
      default:
        printPgmString(PSTR("cannot mount, error "));
        printInteger(J->usb_err);
        switch (J->usb_fs) {
          case GJOB_FS_UNREADABLE: printPgmString(PSTR(", cannot be read")); break;
          case GJOB_FS_FAT: printPgmString(PSTR(", FAT but damaged?")); break;
          case GJOB_FS_EXFAT: printPgmString(PSTR(", exFAT but damaged?")); break;
          case GJOB_FS_NTFS: printPgmString(PSTR(", the stick is NTFS")); break;
          case GJOB_FS_GPT: printPgmString(PSTR(", GPT, file system not recognised")); break;
          case GJOB_FS_NONE: printPgmString(PSTR(", no file system")); break;
          default:
            printPgmString(PSTR(", unknown, partition type "));
            print_uint8_base10(J->usb_ptype);
        }
        printPgmString(PSTR(" (FAT16, FAT32 or exFAT only)"));
    }
    printPgmString(PSTR(", "));
    print_uint8_base10(J->n_files);
    printPgmString(PSTR(" files"));
    if (J->list_more) { printPgmString(PSTR(" (more not shown)")); }
  }
  printPgmString(PSTR("]\r\n"));
  static const char *const names[] = { "idle", "opening", "check", "run", "stopping", "done", "failed", "aborted" };
  printPgmString(PSTR("[JOB:"));
  printString((char *)names[state < 8 ? state : 0]);
  if (state != GJOB_IDLE) {
    serial_write('|');
    printString((char *)J->job_name);
    printPgmString(PSTR("|LINE:"));
    print_uint32_base10(J->job_line);
    serial_write('/');
    print_uint32_base10(J->job_lines);
    printPgmString(PSTR("|POS:"));
    print_uint32_base10(J->job_pos);
    serial_write('/');
    print_uint32_base10(J->job_size);
    printPgmString(PSTR("|ERR:"));
    print_uint8_base10(J->job_err);
    serial_write(',');
    print_uint32_base10(J->job_err_line);
    printPgmString(PSTR("|ALARM:"));
    print_uint8_base10(J->job_alarm);
    printPgmString(PSTR("|CHECKED:"));
    print_uint8_base10(J->job_checked);
    printPgmString(PSTR("|MS:"));
    print_uint32_base10((J->job_end_ms ? J->job_end_ms : hal_millis()) - J->job_start_ms);
    printPgmString(PSTR("|RANGE:"));
    uint8_t i;
    for (i = 0; i < 3; i++) {
      if (i) { serial_write(','); }
      printFloat(J->job_min[i], 3);
      serial_write(':');
      printFloat(J->job_max[i], 3);
    }
  }
  printPgmString(PSTR("]\r\n"));
}


// CRC-32 (zlib's), four bits at a time.
static uint32_t job_crc32(uint32_t crc, uint8_t c)
{
  static const uint32_t t[16] = {
    0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
    0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C, 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C };
  crc ^= c;
  crc = (crc >> 4) ^ t[crc & 15];
  return (crc >> 4) ^ t[crc & 15];
}

static void print_hex32(uint32_t v)
{
  int8_t i;
  for (i = 28; i >= 0; i -= 4) { serial_write("0123456789ABCDEF"[(v >> i) & 15]); }
}

// $FT=name: reads the whole file through the ring as fast as the M4 delivers
// it, counting lines; its CRC-32 lets the PC check an upload. The read test
// of PLAN.md part 4.
static uint8_t job_read_test(const char *name)
{
  if (job_active()) { return(STATUS_JOB_RUNNING); }
  if (!job_m4_ready()) { return(STATUS_USB_NO_STICK); }
  J->rd = 0;
  if (job_request(GJOB_REQ_OPEN, name) != 0) { return(STATUS_USB_FILE); }
  uint32_t t0 = hal_millis(), n_lines = 0, bytes = 0, crc = 0xFFFFFFFFUL;
  for (;;) {
    uint32_t wr = J->wr, rd = J->rd;
    hal_memory_barrier();
    while (rd != wr) {
      uint8_t c = J->ring[rd & (GJOB_RING_SIZE - 1)];
      if (c == '\n') { n_lines++; }
      crc = job_crc32(crc, c);
      rd++;
    }
    bytes = rd;
    hal_memory_barrier();
    J->rd = rd;
    if (J->rd_err) { return(STATUS_USB_FILE); }
    if (J->eof && (J->wr == rd)) { break; }
    protocol_service_link();
    if (sys_rt_exec_state & EXEC_RESET) { job_post(GJOB_REQ_CLOSE); return(STATUS_OK); }
  }
  uint32_t ms = hal_millis() - t0;
  printPgmString(PSTR("[MSG:read "));
  printString((char *)name);
  printPgmString(PSTR(": "));
  print_uint32_base10(bytes);
  printPgmString(PSTR(" bytes, "));
  print_uint32_base10(n_lines);
  printPgmString(PSTR(" lines, CRC32 "));
  print_hex32(~crc);
  printPgmString(PSTR(" in "));
  print_uint32_base10(ms);
  printPgmString(PSTR(" ms = "));
  print_uint32_base10(ms ? (uint32_t)((uint64_t)bytes * 1000 / ms) : 0);
  printPgmString(PSTR(" B/s, M4 "));
  print_uint32_base10(J->rate);
  printPgmString(PSTR(" B/s]\r\n"));
  return(STATUS_OK);
}


static int8_t hex_val(char c)
{
  if ((c >= '0') && (c <= '9')) { return c - '0'; }
  if ((c >= 'A') && (c <= 'F')) { return c - 'A' + 10; }
  if ((c >= 'a') && (c <= 'f')) { return c - 'a' + 10; }
  return -1;
}

// A file of the stick has changed or gone: whatever its check pass found
// does not hold any more.
static void job_forget(const char *name)
{
  uint8_t i;
  for (i = 0; i < JOB_N_KNOWN; i++) {
    if (strcasecmp(known[i].name, name) == 0) { known[i].name[0] = 0; }
  }
}

// The M4's answer to a file request as a Grbl status, with the reason.
static uint8_t job_file_status(int8_t st)
{
  if (st == 0) { return(STATUS_OK); }
  printPgmString(PSTR("[MSG:USB stick: "));
  switch (st) {
    case GJOB_ERR_NO_STICK: printPgmString(PSTR("no stick")); break;
    case GJOB_ERR_OPEN: printPgmString(PSTR("no such file, or it cannot be created")); break;
    case GJOB_ERR_WRITE: printPgmString(PSTR("writing failed (stick full or pulled out?)")); break;
    case GJOB_ERR_NAME: printPgmString(PSTR("not a G-code file name (.nc .ngc .gcode .gc .tap .txt .cnc)")); break;
    case GJOB_ERR_RENAME: printPgmString(PSTR("written, but it cannot take its name")); break;
    case GJOB_ERR_FORMAT: printPgmString(PSTR("formatting failed")); break;
    case -100: printPgmString(PSTR("no answer from the display core")); break;
    default: printPgmString(PSTR("error ")); printInteger(st);
  }
  printPgmString(PSTR("]\r\n"));
  return(st == GJOB_ERR_NO_STICK ? STATUS_USB_NO_STICK : STATUS_USB_FILE);
}

// "YYYYMMDDhhmmss" as a FAT date and time (date << 16 | time); 0 if it is not one.
static uint32_t job_fat_time(const char *s)
{
  uint8_t i;
  uint16_t v[6] = { 0 };
  static const uint8_t len[6] = { 4, 2, 2, 2, 2, 2 };
  for (i = 0; i < 6; i++) {
    uint8_t k;
    for (k = 0; k < len[i]; k++) {
      if ((*s < '0') || (*s > '9')) { return 0; }
      v[i] = v[i] * 10 + (*s++ - '0');
    }
  }
  if (*s || (v[0] < 1980) || (v[0] > 2107) || !v[1] || (v[1] > 12) || !v[2] || (v[2] > 31) ||
      (v[3] > 23) || (v[4] > 59) || (v[5] > 59)) { return 0; }
  return ((uint32_t)(v[0] - 1980) << 25) | ((uint32_t)v[1] << 21) | ((uint32_t)v[2] << 16) |
         ((uint32_t)v[3] << 11) | ((uint32_t)v[4] << 5) | (v[5] / 2);
}

// $FW=name / $FW+<hex> / $FWE[=YYYYMMDDhhmmss] / $FWA: a file from the PC
// onto the stick. (Hex and not the
// text itself: the USB receiver takes '!', '~' and '?' anywhere in a line
// for real-time commands, and the line filter would eat the rest.)
static uint8_t job_upload(char *line)
{
  if (job_active()) { return(STATUS_JOB_RUNNING); }
  if (!job_m4_ready()) { return(STATUS_USB_NO_STICK); }
  if (line[3] == '=') {
    char *name = &line[4];
    if (!name[0] || (strlen(name) >= GJOB_NAME_LEN)) { return(STATUS_INVALID_STATEMENT); }
    if ((sys.state != STATE_IDLE) && (sys.state != STATE_ALARM)) { return(STATUS_IDLE_ERROR); }
    uploading = false;
    up_wr = 0;
    job_forget(name);
    uint8_t status = job_file_status(job_request(GJOB_REQ_WRITE, name));
    if (status == STATUS_OK) { uploading = true; }
    return(status);
  }
  if ((line[3] == 'A') && (line[4] == 0)) {           // $FWA: give the upload up
    if (uploading) { job_request(GJOB_REQ_UP_ABORT, 0); }
    uploading = false;
    return(STATUS_OK);
  }
  if (!uploading) { return(STATUS_INVALID_STATEMENT); }
  if (line[3] == '+') {
    char *p = &line[4];
    while (p[0] && p[1]) {
      int8_t h = hex_val(p[0]), l = hex_val(p[1]);
      if ((h < 0) || (l < 0)) { return(STATUS_BAD_NUMBER_FORMAT); }
      uint32_t t0 = hal_millis();
      while ((uint32_t)(up_wr - J->rd) >= GJOB_RING_SIZE) {   // the M4 is writing the stick
        protocol_service_link();
        if (sys_rt_exec_state & EXEC_RESET) { return(STATUS_OK); }
        if ((uint32_t)(hal_millis() - t0) > JOB_WRITE_TIMEOUT_MS) { return(job_file_status(-100)); }
      }
      J->ring[up_wr & (GJOB_RING_SIZE - 1)] = (uint8_t)((h << 4) | l);
      up_wr++;
      p += 2;
    }
    hal_memory_barrier();
    J->wr = up_wr;
    return(STATUS_OK);
  }
  if ((line[3] == 'E') && ((line[4] == 0) || (line[4] == '='))) {
    J->req_time = 0;
    if (line[4] == '=') {
      J->req_time = job_fat_time(&line[5]);
      if (!J->req_time) { return(STATUS_BAD_NUMBER_FORMAT); }
    }
    uint32_t t0 = hal_millis();
    while ((J->rd != up_wr) && ((uint32_t)(hal_millis() - t0) < JOB_WRITE_TIMEOUT_MS)) { protocol_service_link(); }
    uploading = false;
    return(job_file_status(job_request_t(GJOB_REQ_UPLOAD_END, 0, JOB_WRITE_TIMEOUT_MS)));
  }
  return(STATUS_INVALID_STATEMENT);
}

// Formats the stick - everything on it is lost. From $FF=ERASE and the
// display's FORMAT (held 3 s).
uint8_t job_format(uint8_t exfat)
{
  if (job_active()) { return(STATUS_JOB_RUNNING); }
  if (!job_m4_ready()) { return(STATUS_USB_NO_STICK); }
  if ((sys.state != STATE_IDLE) && (sys.state != STATE_ALARM)) { return(STATUS_IDLE_ERROR); }
  if ((J->usb_state != GJOB_USB_READY) && (J->usb_state != GJOB_USB_ERROR)) { return(STATUS_USB_NO_STICK); }
  if (uploading) { job_request(GJOB_REQ_UP_ABORT, 0); uploading = false; }
  uint8_t i;
  for (i = 0; i < JOB_N_KNOWN; i++) { known[i].name[0] = 0; }
  printPgmString(PSTR("[MSG:USB stick: formatting...]\r\n"));
  uint32_t t0 = hal_millis();
  J->req_time = exfat ? 1 : 0;
  uint8_t status = job_file_status(job_request_t(GJOB_REQ_FORMAT, 0, JOB_FORMAT_TIMEOUT_MS));
  if (status == STATUS_OK) {
    printPgmString(PSTR("[MSG:USB stick: formatted "));
    printPgmString(job_fat_name(J->usb_fat));
    printPgmString(PSTR(" in "));
    print_uint32_base10((hal_millis() - t0 + 500) / 1000);
    printPgmString(PSTR(" s]\r\n"));
  }
  return(status);
}

// $FD=name: deletes a file from the stick.
static uint8_t job_delete(char *line)
{
  if (line[3] != '=' || !line[4]) { return(STATUS_INVALID_STATEMENT); }
  if (job_active()) { return(STATUS_JOB_RUNNING); }
  if (!job_m4_ready()) { return(STATUS_USB_NO_STICK); }
  if ((sys.state != STATE_IDLE) && (sys.state != STATE_ALARM)) { return(STATUS_IDLE_ERROR); }
  job_forget(&line[4]);
  return(job_file_status(job_request_t(GJOB_REQ_DELETE, &line[4], JOB_WRITE_TIMEOUT_MS)));
}


uint8_t job_command(char *line)
{
  // line = "$F", "$F=name", "$FS", "$FR", "$FT=name", "$FW...", "$FD=name"
  switch (line[2]) {
    case 0:
      job_print_state();
      if (job_m4_ready()) {
        uint8_t i, n = J->n_files;
        for (i = 0; i < n; i++) {
          printPgmString(PSTR("[FILE:"));
          printString((char *)J->file[i].name);
          printPgmString(PSTR("|SIZE:"));
          print_uint32_base10(J->file[i].size);
          uint32_t t = J->file[i].mtime;
          printPgmString(PSTR("|DATE:"));
          print_uint32_base10(1980 + (t >> 25));
          serial_write('-');
          print_uint8_base10_2(((t >> 21) & 0x0F));
          serial_write('-');
          print_uint8_base10_2(((t >> 16) & 0x1F));
          serial_write('T');
          print_uint8_base10_2(((t >> 11) & 0x1F));
          serial_write(':');
          print_uint8_base10_2(((t >> 5) & 0x3F));
          serial_write(':');
          print_uint8_base10_2((t & 0x1F) * 2);
          printPgmString(PSTR("]\r\n"));
        }
      }
      return(STATUS_OK);
    case '=':
      if (!line[3]) { return(STATUS_INVALID_STATEMENT); }
      return(job_start(&line[3], false));
    case 'S':
      if (line[3]) { return(STATUS_INVALID_STATEMENT); }
      if (!job_active()) { return(STATUS_OK); }
      job_message("STOP");
      job_message_end();
      job_stop(0);
      return(STATUS_OK);
    case 'R':
      if (line[3]) { return(STATUS_INVALID_STATEMENT); }
      if (!job_m4_ready()) { return(STATUS_USB_NO_STICK); }
      return(job_request(GJOB_REQ_RESCAN, 0) == 0 ? STATUS_OK : STATUS_USB_NO_STICK);
    case 'T':
      if (line[3] != '=') { return(STATUS_INVALID_STATEMENT); }
      return(job_read_test(&line[4]));
    case 'W':
      return(job_upload(line));
    case 'D':
      return(job_delete(line));
    case 'X': {                                   // $FX=lba: a sector of the stick in hex (diagnostics)
      if ((line[3] != '=') || !line[4]) { return(STATUS_INVALID_STATEMENT); }
      if (job_active()) { return(STATUS_JOB_RUNNING); }
      if (!job_m4_ready()) { return(STATUS_USB_NO_STICK); }
      uint32_t lba = 0;
      char *p = &line[4];
      while (*p) {
        if ((*p < '0') || (*p > '9')) { return(STATUS_BAD_NUMBER_FORMAT); }
        lba = lba * 10 + (uint32_t)(*p++ - '0');
      }
      if (uploading) { job_request(GJOB_REQ_UP_ABORT, 0); uploading = false; }
      J->req_time = lba;
      uint8_t st = job_file_status(job_request(GJOB_REQ_SECTOR, 0));
      if (st != STATUS_OK) { return(st); }
      uint16_t i;
      for (i = 0; i < 512; i++) {
        if ((i & 31) == 0) {
          printPgmString(PSTR("[SEC:"));
          serial_write("0123456789ABCDEF"[(i >> 8) & 15]);
          serial_write("0123456789ABCDEF"[(i >> 4) & 15]);
          serial_write("0123456789ABCDEF"[i & 15]);
          serial_write(' ');
        }
        uint8_t b = J->ring[i];
        serial_write("0123456789ABCDEF"[b >> 4]);
        serial_write("0123456789ABCDEF"[b & 15]);
        if ((i & 31) == 31) { printPgmString(PSTR("]\r\n")); }
      }
      return(STATUS_OK);
    }
    case 'F':                                     // $FF=ERASE: format; $FF alone says so
      if (line[3] == 0) {
        printPgmString(PSTR("[MSG:$FF=ERASE formats the USB stick (FAT32 up to 32 GB, exFAT above; "
                            "$FF=EXFAT: exFAT) - everything on it is lost]\r\n"));
        return(STATUS_OK);
      }
      if (line[3] != '=') { return(STATUS_INVALID_STATEMENT); }
      if (strcasecmp(&line[4], "ERASE") == 0) { return(job_format(false)); }
      if (strcasecmp(&line[4], "EXFAT") == 0) { return(job_format(true)); }
      return(STATUS_INVALID_STATEMENT);
  }
  return(STATUS_INVALID_STATEMENT);
}
