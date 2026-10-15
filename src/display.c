/*
  display.c - what the status display shows (M7 side)

  Part of Grbl-Giga, a port of Grbl 1.1 to the Arduino GIGA R1 WiFi.

  Grbl is free software: you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation, either version 3 of the License, or (at your option) any later
  version. Distributed WITHOUT ANY WARRANTY; see the GNU GPL for details.

  -------------------------------------------------------------------------------

  Grbl runs on the Cortex-M7; the GIGA Display Shield belongs to the Cortex-M4.
  This module is the whole interface between them on the Grbl side: every
  DISPLAY_REFRESH_MS it gathers the facts a `?` status report carries, plus
  modal state, inputs and outputs, and copies them into the shared block
  described in grbl_shm.h. Drawing is the M4's business entirely, so a full
  repaint of the 800x480 panel costs Grbl nothing but this copy - a few
  microseconds, 25 times a second.

  display_service() is called from protocol_exec_rt_system(), i.e. from every
  one of Grbl's wait loops, so the display follows a feed hold or a dwell just
  as the status report does.
*/

#include "grbl.h"
#include "grbl_shm.h"
#include "grbl_job.h"

// A continuous jog from the touch screen stops when the finger lifts - and
// also, as a dead man's switch, when the M4 stops confirming the finger is
// still there for this long.
#define TOUCH_JOG_TIMEOUT_MS  300
// A continuous jog is planned as this many seconds at the jog feed (and cut at
// the soft limits). It is normally cancelled long before.
#define TOUCH_JOG_SECONDS     30
// A touch action not taken up within this time is dropped, not run late.
#define TOUCH_REQ_MAX_AGE_MS  500

static uint8_t alarm_code = 0;
static uint8_t error_code = 0;
static uint32_t error_ms = 0;
static bool started = false;

static bool touch_jog = false;           // a continuous jog from the touch screen runs
static uint32_t touch_jog_alive = 0;     // last jog_alive seen
static uint32_t touch_jog_alive_ms = 0;  // when it last changed


void display_init()
{
  #ifdef DISPLAY_M4
    hal_display_init();
    job_boot();       // the USB stick's block, before the M4 starts
  #endif
}


// Starts the M4 and the publishing. Grbl's own start-up calls display_service()
// long before every module is up (settings_init() already synchronises the
// planner), so nothing is published until this has run.
void display_start()
{
  #ifdef DISPLAY_M4
    if (started) { return; }
    started = true;
    hal_display_start();
  #endif
}


void display_note_alarm(uint8_t code) { alarm_code = code; }

void display_note_error(uint8_t code)
{
  error_code = code;
  error_ms = hal_millis();
}


static uint8_t display_state(uint8_t *door_sub)
{
  *door_sub = 0;
  switch (sys.state) {
    case STATE_IDLE: return GSHM_STATE_IDLE;
    case STATE_CYCLE: return GSHM_STATE_RUN;
    case STATE_HOLD:
      if (!(sys.suspend & SUSPEND_JOG_CANCEL)) {
        return (sys.suspend & SUSPEND_HOLD_COMPLETE) ? GSHM_STATE_HOLD0 : GSHM_STATE_HOLD1;
      }
      return GSHM_STATE_JOG; // A jog cancel shows as Jog, as in the status report.
    case STATE_JOG: return GSHM_STATE_JOG;
    case STATE_HOMING: return GSHM_STATE_HOME;
    case STATE_ALARM: return GSHM_STATE_ALARM;
    case STATE_CHECK_MODE: return GSHM_STATE_CHECK;
    case STATE_SAFETY_DOOR:
      if (sys.suspend & SUSPEND_INITIATE_RESTORE) { *door_sub = 3; }
      else if (sys.suspend & SUSPEND_RETRACT_COMPLETE) { *door_sub = (sys.suspend & SUSPEND_SAFETY_DOOR_AJAR) ? 1 : 0; }
      else { *door_sub = 2; }
      return GSHM_STATE_DOOR;
    case STATE_SLEEP: return GSHM_STATE_SLEEP;
  }
  return GSHM_STATE_IDLE;
}


// Touch-screen buttons send the same real-time bytes a sender would. Only
// real-time commands are taken: they never enter the line buffer, so the
// touch screen cannot interleave characters with a program the PC is streaming.
// The aux output buttons have bytes of their own, handled here: like M64/M65
// they switch at once, in any state.
static void display_take_commands()
{
  int c;
  while ((c = hal_display_cmd_read()) >= 0) {
    if ((c >= GSHM_CMD_AUX_TOGGLE) && (c < GSHM_CMD_AUX_TOGGLE + GSHM_N_AUX)) {
      uint8_t aux = c - GSHM_CMD_AUX_TOGGLE;
      // Off if the output is on now (whatever switched it on), on otherwise.
      // An output that follows the spindle ($46) stays on while it runs.
      outputs_aux_set(aux, !outputs_get(OUTPUT_AUX0 + aux));
    } else if ((c == CMD_RESET) || (c == CMD_STATUS_REPORT) || (c == CMD_CYCLE_START) ||
        (c == CMD_FEED_HOLD) || (c >= 0x80)) {
      grbl_serial_rx_char((uint8_t)c);
    }
  }
}


// Dead man's switch for a continuous jog started from the touch screen.
static void display_jog_watchdog()
{
  if (!touch_jog) { return; }
  if (sys.state != STATE_JOG) { touch_jog = false; return; }   // finished, cancelled or alarm
  grbl_shm_t *m = GRBL_SHM;
  uint32_t now = hal_millis();
  uint32_t alive = m->jog_alive;
  if (alive != touch_jog_alive) { touch_jog_alive = alive; touch_jog_alive_ms = now; }
  if (!m->jog_hold || ((uint32_t)(now - touch_jog_alive_ms) > TOUCH_JOG_TIMEOUT_MS)) {
    touch_jog = false;
    grbl_serial_rx_char(CMD_JOG_CANCEL);
  }
}


// Appends v with 4 decimals (no printf float support needed).
static void append_float(char *s, float v)
{
  s += strlen(s);
  if (v < 0) { *s++ = '-'; v = -v; }
  uint64_t f = (uint64_t)(v * 10000.0f + 0.5f);
  uint64_t ip = f / 10000;
  uint32_t fp = (uint32_t)(f % 10000);
  char tmp[24];
  int n = 0;
  do { tmp[n++] = (char)('0' + ip % 10); ip /= 10; } while (ip);
  while (n) { *s++ = tmp[--n]; }
  *s++ = '.';
  *s++ = (char)('0' + fp / 1000);
  *s++ = (char)('0' + (fp / 100) % 10);
  *s++ = (char)('0' + (fp / 10) % 10);
  *s++ = (char)('0' + fp % 10);
  *s = 0;
}


static const char touch_axis_letter[N_AXIS] = { 'X', 'Y', 'Z', 'A' };
static const uint8_t setting_numbers[GSHM_N_SETTINGS] = GSHM_SETTING_NUMBERS;

// Builds and runs the command behind a touch action. Returns its status.
static uint8_t display_run_request(uint8_t code, uint8_t axis, int8_t dir, float value, uint8_t speed)
{
  static char cmd[LINE_BUFFER_SIZE];   // $H runs the startup lines through this buffer
  bool idle = (sys.state == STATE_IDLE) && (plan_get_current_block() == NULL);
  bool alarm = (sys.state == STATE_ALARM);
  uint8_t idx;
  cmd[0] = 0;

  switch (code) {
    case GSHM_REQ_UNLOCK:
    case GSHM_REQ_HOME:
      if (!idle && !alarm) { return(STATUS_IDLE_ERROR); }
      strcpy(cmd, (code == GSHM_REQ_UNLOCK) ? "$X" : "$H");
      return(system_execute_line(cmd));

    case GSHM_REQ_ZERO:
    case GSHM_REQ_ZERO_ALL:
      if (alarm) { return(STATUS_SYSTEM_GC_LOCK); }
      if (!idle) { return(STATUS_IDLE_ERROR); }
      strcpy(cmd, "G10L20P0");
      for (idx = 0; idx < N_AXIS; idx++) {
        if ((code == GSHM_REQ_ZERO) && (idx != axis)) { continue; }
        if ((idx == A_AXIS) && axis_a_is_slave()) { continue; }
        char w[3] = { touch_axis_letter[idx], '0', 0 };
        strcat(cmd, w);
      }
      if (strlen(cmd) == 8) { return(STATUS_GCODE_NO_AXIS_WORDS); }
      return(gc_execute_line(cmd));

    case GSHM_REQ_SETTING: {
      if (!idle && !alarm) { return(STATUS_IDLE_ERROR); }
      char num[4];
      uint8_t n = axis, i = 0;
      if (n >= 100) { num[i++] = (char)('0' + n / 100); }
      if (n >= 10) { num[i++] = (char)('0' + (n / 10) % 10); }
      num[i++] = (char)('0' + n % 10);
      num[i] = 0;
      strcpy(cmd, "$");
      strcat(cmd, num);
      strcat(cmd, "=");
      append_float(cmd, value);
      return(system_execute_line(cmd));
    }

    case GSHM_REQ_COLLET:
      return(spindle_collet_set(value > 0.5f));

    case GSHM_REQ_JOB_START: {
      grbl_job_t *jb = GRBL_JOB;
      // The listing must still be the one the finger chose from.
      if ((uint16_t)value != jb->list_seq) { return(STATUS_USB_FILE); }
      if (axis >= jb->n_files) { return(STATUS_USB_FILE); }
      char name[GJOB_NAME_LEN];
      strcpy(name, (const char *)jb->file[axis].name);
      return(job_start(name, true));
    }

    case GSHM_REQ_JOB_STOP:
      job_stop(0);
      return(STATUS_OK);

    case GSHM_REQ_USB_FORMAT:
      return(job_format(false));

    case GSHM_REQ_PULSE:
      if ((axis != OUTPUT_SERVO_RESET) && (axis != OUTPUT_VFD_RESET)) { return(STATUS_INVALID_STATEMENT); }
      if (!io_out_channel(axis)) { return(STATUS_SETTING_DISABLED); }
      outputs_pulse(axis, settings.reset_pulse_ms);
      return(STATUS_OK);

    case GSHM_REQ_JOG: {
      if (axis >= N_AXIS) { return(STATUS_INVALID_JOG_COMMAND); }
      if ((axis == A_AXIS) && axis_a_is_slave()) { return(STATUS_GCODE_UNSUPPORTED_COMMAND); }
      if (!idle && (sys.state != STATE_JOG)) { return(STATUS_IDLE_ERROR); }
      if (speed < 1) { speed = 1; }
      if (speed > 100) { speed = 100; }
      float feed = settings.max_rate[axis] * (float)speed / 100.0f;
      if (feed < 1.0f) { feed = 1.0f; }
      bool continuous = (value <= 0.0f);
      float dist = continuous ? (feed * TOUCH_JOG_SECONDS / 60.0f) : value;
      if (continuous && !GRBL_SHM->jog_hold) { return(STATUS_OK); }   // already let go
      if (continuous) {
        // Stop at the soft limit instead of having the whole jog refused.
        float room = jog_soft_limit_room(axis, dir);
        if (room <= 0.0f) { return(STATUS_TRAVEL_EXCEEDED); }
        if (dist > room) { dist = room; }
      }
      jog_line(cmd, axis, (dir < 0) ? -dist : dist, feed);
      uint8_t status = system_execute_line(cmd);
      if ((status == STATUS_OK) && continuous && (sys.state == STATE_JOG)) {
        touch_jog = true;
        touch_jog_alive = GRBL_SHM->jog_alive;
        touch_jog_alive_ms = hal_millis();
      }
      return(status);
    }
  }
  return(STATUS_INVALID_STATEMENT);
}


// The collet and the reset pulse buttons work in any state - the collet is
// opened and closed during an M6 pause, while Grbl parses no lines - so
// display_service() takes those requests on the spot. Everything else waits
// for display_touch_service() between two lines.
static void display_touch_immediate()
{
  grbl_shm_t *m = GRBL_SHM;
  if (m->m4_magic != GRBL_SHM_M4_MAGIC) { return; }
  uint8_t seq = m->req_seq;
  if (seq == m->req_done) { return; }
  hal_memory_barrier();
  uint8_t code = m->req_code;
  if ((code != GSHM_REQ_COLLET) && (code != GSHM_REQ_PULSE) && (code != GSHM_REQ_JOB_STOP)) { return; }
  uint8_t status;
  if (!hal_display_present() || ((int32_t)(m->m4_ms - m->req_ms) > TOUCH_REQ_MAX_AGE_MS)) {
    status = STATUS_IDLE_ERROR;
  } else {
    status = display_run_request(code, m->req_axis, m->req_dir, m->req_value, m->req_speed);
  }
  if (status != STATUS_OK) { display_note_error(status); }
  m->req_status = status;
  hal_memory_barrier();
  m->req_done = seq;
}


void display_touch_service()
{
  #ifdef DISPLAY_M4
    if (!started) { return; }
    grbl_shm_t *m = GRBL_SHM;
    if (m->m4_magic != GRBL_SHM_M4_MAGIC) { return; }
    uint8_t seq = m->req_seq;
    if (seq == m->req_done) { return; }
    hal_memory_barrier();
    // A touch that waited here while Grbl was busy (a dwell, a stream from the
    // PC) is dropped rather than run late, at some unrelated point of a job.
    uint8_t status;
    // Signed: the M4 stamps req_ms after it wrote m4_ms for that loop, so a
    // fresh request can look a millisecond younger than "now".
    if (!hal_display_present() || ((int32_t)(m->m4_ms - m->req_ms) > TOUCH_REQ_MAX_AGE_MS)) {
      status = STATUS_IDLE_ERROR;
    } else {
      status = display_run_request(m->req_code, m->req_axis, m->req_dir, m->req_value, m->req_speed);
    }
    if (status != STATUS_OK) { display_note_error(status); }
    m->req_status = status;
    hal_memory_barrier();
    m->req_done = seq;
  #endif
}


uint8_t display_splash(char *line)
{
  #ifdef DISPLAY_M4
    // line is "$SPLASH" or "$SPLASH=ms"
    uint32_t ms = 3000;
    if (line[7] == '=') {
      uint8_t char_counter = 8;
      float v;
      if (!read_float(line, &char_counter, &v)) { return(STATUS_BAD_NUMBER_FORMAT); }
      if ((line[char_counter] != 0) || (v < 0) || (v > 60000)) { return(STATUS_INVALID_STATEMENT); }
      ms = (uint32_t)v;
    } else if (line[7] != 0) {
      return(STATUS_INVALID_STATEMENT);
    }
    if (!hal_display_present()) { return(STATUS_SETTING_DISABLED); }
    grbl_shm_t *m = GRBL_SHM;
    m->splash_ms = ms;
    hal_memory_barrier();
    m->splash_seq = m->splash_seq + 1;
    return(STATUS_OK);
  #else
    (void)line;
    return(STATUS_SETTING_DISABLED);
  #endif
}


uint8_t display_simulate_touch(char *line)
{
  #ifdef DISPLAY_M4
    // line is "$TOUCH=x,y[,ms[,delay]]"
    if (strncmp(line, "$TOUCH=", 7) != 0) { return(STATUS_INVALID_STATEMENT); }
    uint8_t char_counter = 7;
    float x, y, ms = 100.0f, delay = 0.0f;
    if (!read_float(line, &char_counter, &x)) { return(STATUS_BAD_NUMBER_FORMAT); }
    if (line[char_counter++] != ',') { return(STATUS_INVALID_STATEMENT); }
    if (!read_float(line, &char_counter, &y)) { return(STATUS_BAD_NUMBER_FORMAT); }
    if (line[char_counter] == ',') {
      char_counter++;
      if (!read_float(line, &char_counter, &ms)) { return(STATUS_BAD_NUMBER_FORMAT); }
      if (line[char_counter] == ',') {
        char_counter++;
        if (!read_float(line, &char_counter, &delay)) { return(STATUS_BAD_NUMBER_FORMAT); }
      }
    }
    if (line[char_counter] != 0) { return(STATUS_INVALID_STATEMENT); }
    if ((x < 0) || (x > 799) || (y < 0) || (y > 479) || (ms < 1) || (ms > 60000) ||
        (delay < 0) || (delay > 60000)) { return(STATUS_INVALID_STATEMENT); }
    if (!hal_display_present()) { return(STATUS_SETTING_DISABLED); }
    grbl_shm_t *m = GRBL_SHM;
    // The M4 copies a press within one poll (10 ms); wait for it to take the
    // previous one before overwriting the fields.
    uint32_t t0 = hal_millis();
    while ((m->sim_ack != m->sim_seq) && ((uint32_t)(hal_millis() - t0) < 100)) { }
    m->sim_x = (uint16_t)x;
    m->sim_y = (uint16_t)y;
    m->sim_ms = (uint32_t)ms;
    m->sim_delay = (uint32_t)delay;
    hal_memory_barrier();
    m->sim_seq = m->sim_seq + 1;
    return(STATUS_OK);
  #else
    (void)line;
    return(STATUS_SETTING_DISABLED);
  #endif
}


void display_service()
{
  #ifdef DISPLAY_M4
    if (!started) { return; }
    display_take_commands();
    display_jog_watchdog();
    display_touch_immediate();

    static uint32_t last_ms = 0;
    uint32_t now = hal_millis();
    if ((uint32_t)(now - last_ms) < DISPLAY_REFRESH_MS) { return; }
    last_ms = now;

    grbl_shm_t *m = GRBL_SHM;
    uint8_t idx;

    // Gather everything first, so the window in which the block is marked as
    // being written stays as short as possible.
    int32_t current_position[N_AXIS];
    memcpy(current_position, sys_position, sizeof(sys_position));
    float mpos[N_AXIS], wco[N_AXIS];
    system_convert_array_steps_to_mpos(mpos, current_position);
    for (idx = 0; idx < N_AXIS; idx++) {
      wco[idx] = gc_state.coord_system[idx] + gc_state.coord_offset[idx];
      if (idx == TOOL_LENGTH_OFFSET_AXIS) { wco[idx] += gc_state.tool_length_offset; }
    }
    uint8_t door_sub;
    uint8_t state = display_state(&door_sub);

    uint32_t inputs = io_read_channels();       // bit n-1 = INn, bit 20 = STOP, 21 = DRV PWR
    uint32_t in_fn = io_functions(inputs);
    uint16_t outputs = io_out_state();          // bit n-1 = OUTn
    if (st_drivers_enabled()) { outputs |= GSHM_OUT_ENABLE; }
    if (st_brake_released()) { outputs |= GSHM_OUT_BRAKE; }

    int32_t line_number = 0;
    plan_block_t *cur_block = plan_get_current_block();
    if (cur_block != NULL) { line_number = cur_block->line_number; }

    m->seq++;               // odd: being written
    hal_memory_barrier();

    m->m7_ms = now;
    m->state = state;
    m->door_sub = door_sub;
    m->alarm = (sys.state == STATE_ALARM) ? alarm_code : 0;
    m->last_error = error_code;
    m->error_ms = error_ms;
    for (idx = 0; idx < N_AXIS; idx++) { m->mpos[idx] = mpos[idx]; m->wco[idx] = wco[idx]; }
    m->feed = st_get_realtime_rate();
    m->feed_prog = gc_state.feed_rate;
    m->spindle_rpm = sys.spindle_speed;
    m->spindle_prog = gc_state.spindle_speed;
    m->ovr_feed = sys.f_override;
    m->ovr_rapid = sys.r_override;
    m->ovr_spindle = sys.spindle_speed_ovr;
    m->motion = gc_state.modal.motion;
    m->coord_select = gc_state.modal.coord_select;
    m->units_inch = (gc_state.modal.units == UNITS_MODE_INCHES);
    m->distance_inc = (gc_state.modal.distance == DISTANCE_MODE_INCREMENTAL);
    m->plane = gc_state.modal.plane_select;
    m->spindle_mode = (gc_state.modal.spindle == SPINDLE_ENABLE_CW) ? 1 :
                      (gc_state.modal.spindle == SPINDLE_ENABLE_CCW) ? 2 : 0;
    m->coolant = ((gc_state.modal.coolant & COOLANT_FLOOD_ENABLE) ? 1 : 0) |
                 ((gc_state.modal.coolant & COOLANT_MIST_ENABLE) ? 2 : 0);
    m->tool = gc_state.tool;
    m->report_inches = bit_istrue(settings.flags,BITFLAG_REPORT_INCHES);
    m->a_mode = settings.a_mode;
    m->kinematics = settings.kinematics;
    m->usb_connected = hal_serial_connected();
    m->planner_free = plan_get_block_buffer_available();
    m->planner_size = BLOCK_BUFFER_SIZE - 1;
    m->flags = (sys.spindle_stop_ovr ? GSHM_F_SPINDLE_STOP : 0) |
               (bit_istrue(settings.flags,BITFLAG_SOFT_LIMIT_ENABLE) ? GSHM_F_SOFT_LIMITS : 0) |
               (bit_istrue(settings.flags,BITFLAG_HOMING_ENABLE) ? GSHM_F_HOMING : 0) |
               (touch_jog ? GSHM_F_TOUCH_JOG : 0) |
               (estop_latched ? GSHM_F_ESTOP : 0) |
               (estop_active() ? GSHM_F_ESTOP_IN : 0) |
               (gc_tool_change_pending ? GSHM_F_TOOL_CHANGE : 0) |
               (spindle_collet_is_open() ? GSHM_F_COLLET : 0);
    for (idx = 0; idx < N_AXIS; idx++) { m->jog_rate[idx] = settings.max_rate[idx]; }
    for (idx = 0; idx < GSHM_N_SETTINGS; idx++) { m->set_val[idx] = settings_value(setting_numbers[idx]); }
    m->rx_free = serial_get_rx_buffer_available();
    m->inputs = inputs;
    m->in_fn = in_fn;
    m->outputs = outputs;
    m->out_fn = outputs_get_mask();
    m->pwm_permille = spindle_get_pwm_permille();
    m->spindle_status = spindle_status();
    m->limit_trigger = limits_last_trigger();
    m->estop_trips = estop_trip_count();
    m->drv_power = drvpwr_state();
    m->mpg_state = mpg_state();
    m->mpg_axis = (mpg_axis() < N_AXIS) ? mpg_axis() + 1 : 0;
    m->mpg_mult = mpg_mult();
    m->settings_version = SETTINGS_VERSION;
    m->line_number = line_number;

    hal_memory_barrier();
    m->seq++;               // even: consistent
  #endif
}


void display_report_diag()
{
  #ifdef DISPLAY_M4
    grbl_shm_t *m = GRBL_SHM;
    if (m->m4_magic != GRBL_SHM_M4_MAGIC) { return; }
    printPgmString(PSTR(" touch="));
    print_uint32_base10(m->touch_status);
    serial_write('/');
    print_uint32_base10(m->touch_count);
    serial_write('@');
    print_uint32_base10(m->touch_x);
    serial_write(',');
    print_uint32_base10(m->touch_y);
    serial_write('/');
    print_uint32_base10(m->touch_res_x);
    serial_write('x');
    print_uint32_base10(m->touch_res_y);
    printPgmString(PSTR(" rgb="));
    print_uint32_base10(m->rgb_status);
    printPgmString(PSTR(" m4ms="));
    print_uint32_base10(m->m4_ms);
  #endif
}


uint8_t display_dump_screen()
{
  const uint8_t *fb = hal_display_framebuffer();
  if (!fb) { return(STATUS_SETTING_DISABLED); }
  const uint32_t n = 480UL * 800UL * 2UL;
  printPgmString(PSTR("[SCREEN:480x800:RGB565:"));
  print_uint32_base10(n);
  serial_write(']'); serial_write(13); serial_write(10);
  uint32_t i;
  for (i = 0; i < n; i++) {
    serial_write(fb[i]);
    if (sys_rt_exec_state & EXEC_RESET) { break; }
  }
  serial_write(13); serial_write(10);
  return(STATUS_OK);
}
