/*
  io.c - universal input and output channels and their functions

  Part of Grbl-Giga, a port of Grbl 1.1 to the Arduino GIGA R1 WiFi.

  Grbl is free software: you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation, either version 3 of the License, or (at your option) any later
  version. Distributed WITHOUT ANY WARRANTY; see the GNU GPL for details.

  -------------------------------------------------------------------------------

  See io.h. The input side is one table: the GPIO ports the twenty channels
  (and STOP) sit on, which of their pins are channels and which channel each
  pin is, the $79 mask and the functions every channel carries. The scan reads
  each port once (seven reads; a GPIO read crosses to the D3 domain and costs
  some 40 core cycles) and then only walks the bits that are set, so a quiet
  machine costs little more than the reads. The table is rebuilt in a local
  copy and published with interrupts masked, because the scan and the stepper
  interrupt read it.
*/

#include "grbl.h"

#define IO_MAX_PORTS  8

typedef struct {
  uint8_t n_ports;
  volatile const uint32_t *idr[IO_MAX_PORTS];
  uint16_t port_pins[IO_MAX_PORTS];       // pins of that port that are channels
  uint8_t pin_channel[IO_MAX_PORTS][16];  // port pin -> channel index (STOP = 20, DRV PWR = 21)
  uint32_t invert;                        // $79
  uint8_t fn_channel[N_IN_FUNCS];         // channel number, 0 = unused
  uint32_t ch_functions[IO_N_IN_PINS];    // functions each channel carries
  volatile const uint32_t *probe_idr;     // probe fast path, 0 = no probe
  uint16_t probe_mask;
  uint8_t probe_invert;
} io_in_table_t;

static const uint8_t in_pins[N_IN_CHANNELS] = IN_CHANNEL_PINS;
static const uint8_t out_pins[N_OUT_CHANNELS] = OUT_CHANNEL_PINS;

static io_in_table_t in_tab;

// Test hook $TSW: virtual switches that close when an axis passes a position.
#define IO_N_TEST_SWITCHES 4
typedef struct {
  uint8_t channel;      // 1-21, 0 = slot free
  uint8_t axis;
  int8_t dir;           // +1: active at or above the position, -1: at or below
  int32_t steps;        // the position, in motor steps (sys_position)
} io_test_switch_t;
static io_test_switch_t test_sw[IO_N_TEST_SWITCHES];
static volatile uint8_t test_sw_n = 0;

// Test hook $TIN with a delay: changes waiting for their time.
#define IO_N_TEST_PENDING 8
typedef struct {
  uint8_t channel;      // 0 = slot free
  uint8_t on;
  uint32_t at;          // hal_millis()
} io_test_pending_t;
static io_test_pending_t test_pending[IO_N_TEST_PENDING];
static uint8_t test_pending_n = 0;
static volatile uint16_t out_state = 0;    // logical, bit n-1 = OUTn
static volatile uint16_t out_reserved = 0; // channels another module drives (io_out_reserve)
static volatile uint32_t test_pulled = 0;  // channels the test hook holds down
static bool pins_ready = false;


static uint8_t in_pin(uint8_t c)
{
  if (c < N_IN_CHANNELS) { return in_pins[c]; }
  return (c == N_IN_CHANNELS) ? ESTOP_PIN : DRVPWR_PIN;
}


static void io_drive(uint8_t c)   // c = 0-based channel
{
  uint16_t m = (uint16_t)(1u << c);
  if (out_reserved & m) { return; }
  uint8_t level = ((out_state & m) != 0) != ((settings.out_invert & m) != 0);
  hal_pin_write(out_pins[c], level);
}


void io_rebuild()
{
  io_in_table_t t;
  uint8_t c, f, p;
  memset(&t, 0, sizeof(t));
  for (c = 0; c < IO_N_IN_PINS; c++) {
    volatile const uint32_t *idr = hal_pin_idr(in_pin(c));
    for (p = 0; p < t.n_ports; p++) { if (t.idr[p] == idr) { break; } }
    if (p == t.n_ports) {
      if (t.n_ports >= IO_MAX_PORTS) { continue; }   // cannot happen with the GIGA map
      t.idr[t.n_ports++] = idr;
    }
    uint16_t m = hal_pin_mask(in_pin(c));
    t.port_pins[p] |= m;
    t.pin_channel[p][__builtin_ctz(m)] = c;
  }
  t.invert = settings.in_invert & ((1UL << IO_N_IN_PINS) - 1);
  for (f = 0; f < N_IN_FUNCS; f++) {
    c = settings.in_func[f];
    t.fn_channel[f] = (c <= N_IN_CHANNELS) ? c : 0;
    if (t.fn_channel[f]) { t.ch_functions[c - 1] |= (1UL << f); }
  }
  c = t.fn_channel[IN_FN_PROBE];
  if (c) {
    t.probe_idr = hal_pin_idr(in_pin(c - 1));
    t.probe_mask = hal_pin_mask(in_pin(c - 1));
    t.probe_invert = (t.invert >> (c - 1)) & 1;
  }

  uint32_t sreg = GRBL_IRQ_SAVE();
  memcpy(&in_tab, &t, sizeof(t));
  GRBL_IRQ_RESTORE(sreg);

  // $93 may have changed: put every output at its level again.
  for (c = 0; c < N_OUT_CHANNELS; c++) { io_drive(c); }
}


void io_init()
{
  uint8_t c;
  if (!pins_ready) {
    // Once per power-up: a soft reset must not undo the test hook, and the
    // pins never change their job, only their meaning.
    pins_ready = true;
    for (c = 0; c < IO_N_IN_PINS; c++) { hal_pin_input(in_pin(c), HAL_PIN_INPUT_PULLUP); }
    out_state = 0;
    for (c = 0; c < N_OUT_CHANNELS; c++) {
      io_drive(c);                 // level first, so the pin never glitches on
      hal_pin_output(out_pins[c]);
      io_drive(c);
    }
  }
  io_rebuild();
}


uint32_t io_read_channels()
{
  uint32_t ch = 0;
  uint8_t p;
  for (p = 0; p < in_tab.n_ports; p++) {
    uint32_t low = ~(*in_tab.idr[p]) & in_tab.port_pins[p];   // low = optocoupler on
    while (low) {
      ch |= (1UL << in_tab.pin_channel[p][__builtin_ctz(low)]);
      low &= low - 1;
    }
  }
  if (test_sw_n) {   // test hook: what a switch at that position would do to the optocoupler
    uint8_t i;
    for (i = 0; i < IO_N_TEST_SWITCHES; i++) {
      const io_test_switch_t *t = &test_sw[i];
      if (!t->channel) { continue; }
      int32_t pos = sys_position[t->axis];
      if ((t->dir > 0) ? (pos >= t->steps) : (pos <= t->steps)) { ch |= (1UL << (t->channel - 1)); }
    }
  }
  return ch ^ in_tab.invert;
}


uint32_t io_functions(uint32_t channels)
{
  uint32_t fn = 0;
  while (channels) {
    fn |= in_tab.ch_functions[__builtin_ctz(channels)];
    channels &= channels - 1;
  }
  return fn;
}


// The operator panel's START and HOLD buttons count as those functions too.
uint32_t io_read_functions() { return io_functions(io_read_channels()) | panel_functions(); }

uint8_t io_in_channel(uint8_t fn) { return (fn < N_IN_FUNCS) ? in_tab.fn_channel[fn] : 0; }


uint8_t io_probe_raw()
{
  if (!in_tab.probe_idr) { return 0; }
  return ((*in_tab.probe_idr & in_tab.probe_mask) ? 0 : 1) ^ in_tab.probe_invert;
}


void io_out_write(uint8_t channel, uint8_t on)
{
  if ((channel == 0) || (channel > N_OUT_CHANNELS)) { return; }
  uint8_t c = channel - 1;
  uint32_t sreg = GRBL_IRQ_SAVE();
  if (on) { out_state |= (uint16_t)(1u << c); } else { out_state &= (uint16_t)~(1u << c); }
  io_drive(c);
  GRBL_IRQ_RESTORE(sreg);
}


uint16_t io_out_state() { return out_state; }


void io_out_reserve(uint8_t channel, uint8_t reserved)
{
  if ((channel == 0) || (channel > N_OUT_CHANNELS)) { return; }
  uint8_t c = channel - 1;
  uint32_t sreg = GRBL_IRQ_SAVE();
  if (reserved) { out_reserved |= (uint16_t)(1u << c); }
  else { out_reserved &= (uint16_t)~(1u << c); io_drive(c); }
  GRBL_IRQ_RESTORE(sreg);
}

uint8_t io_out_reserved(uint8_t channel)
{
  if ((channel == 0) || (channel > N_OUT_CHANNELS)) { return(false); }
  return((out_reserved >> (channel - 1)) & 1);
}

uint8_t io_out_channel(uint8_t fn)
{
  if (fn >= N_OUT_FUNCS) { return 0; }
  uint8_t c = settings.out_func[fn];
  return (c <= N_OUT_CHANNELS) ? c : 0;
}


uint8_t io_check_in_assignment(uint8_t fn, uint8_t channel)
{
  uint8_t f;
  if (channel > N_IN_CHANNELS) { return(STATUS_INVALID_STATEMENT); }
  if (channel == 0) { return(STATUS_OK); }
  for (f = 0; f < N_IN_FUNCS; f++) {
    if ((f == fn) || (settings.in_func[f] != channel)) { continue; }
    // The two limits of one axis may share an input (switches in series).
    if ((fn <= IN_FN_LIM_AP) && (f <= IN_FN_LIM_AP) && ((fn >> 1) == (f >> 1))) { continue; }
    return(STATUS_CHANNEL_IN_USE);
  }
  return(STATUS_OK);
}


uint8_t io_check_out_assignment(uint8_t fn, uint8_t channel)
{
  uint8_t f;
  if (channel > N_OUT_CHANNELS) { return(STATUS_INVALID_STATEMENT); }
  if (channel == 0) { return(STATUS_OK); }
  if (io_out_reserved(channel)) { return(STATUS_CHANNEL_IN_USE); }   // OUT13 as PWM ($38)
  for (f = 0; f < N_OUT_FUNCS; f++) {
    if ((f != fn) && (settings.out_func[f] == channel)) { return(STATUS_CHANNEL_IN_USE); }
  }
  return(STATUS_OK);
}


uint8_t io_test_input(uint8_t channel, uint8_t on)
{
  uint8_t c;
  if (channel > IO_N_IN_PINS) { return(STATUS_INVALID_STATEMENT); }
  if (channel == 0) {
    for (c = 0; c < IO_N_IN_PINS; c++) { hal_pin_input(in_pin(c), HAL_PIN_INPUT_PULLUP); }
    test_pulled = 0;
    memset(test_pending, 0, sizeof(test_pending));
    test_pending_n = 0;
    return(STATUS_OK);
  }
  c = channel - 1;
  if (on) {
    hal_pin_pull_down(in_pin(c));
    test_pulled |= (1UL << c);
  } else {
    hal_pin_input(in_pin(c), HAL_PIN_INPUT_PULLUP);
    test_pulled &= ~(1UL << c);
  }
  return(STATUS_OK);
}


uint32_t io_test_mask() { return test_pulled; }


uint8_t io_test_input_later(uint8_t channel, uint8_t on, uint32_t delay_ms)
{
  uint8_t i;
  if ((channel == 0) || (channel > IO_N_IN_PINS)) { return(STATUS_INVALID_STATEMENT); }
  for (i = 0; i < IO_N_TEST_PENDING; i++) {
    if (!test_pending[i].channel) {
      test_pending[i].on = on;
      test_pending[i].at = hal_millis() + delay_ms;
      test_pending[i].channel = channel;
      test_pending_n++;
      return(STATUS_OK);
    }
  }
  return(STATUS_OVERFLOW);
}


// Main loop (every wait loop included, so a change lands during a hold too).
void io_test_service()
{
  if (!test_pending_n) { return; }
  uint8_t i;
  uint32_t now = hal_millis();
  for (i = 0; i < IO_N_TEST_PENDING; i++) {
    io_test_pending_t *t = &test_pending[i];
    if (t->channel && ((int32_t)(now - t->at) >= 0)) {
      io_test_input(t->channel, t->on);
      t->channel = 0;
      test_pending_n--;
    }
  }
}


uint8_t io_test_switch(uint8_t channel, uint8_t axis, int8_t dir, float mm)
{
  uint8_t i;
  uint32_t sreg;
  if (channel == 0) {
    sreg = GRBL_IRQ_SAVE();
    memset(test_sw, 0, sizeof(test_sw));
    test_sw_n = 0;
    GRBL_IRQ_RESTORE(sreg);
    return(STATUS_OK);
  }
  if ((channel > N_IN_CHANNELS + 1) || (axis >= N_AXIS)) { return(STATUS_INVALID_STATEMENT); }
  for (i = 0; i < IO_N_TEST_SWITCHES; i++) {
    if (!test_sw[i].channel || (test_sw[i].channel == channel)) { break; }
  }
  if (i == IO_N_TEST_SWITCHES) { return(STATUS_OVERFLOW); }
  sreg = GRBL_IRQ_SAVE();
  test_sw[i].axis = axis;
  test_sw[i].dir = dir;
  test_sw[i].steps = lroundf(mm * settings.steps_per_mm[axis]);
  test_sw[i].channel = channel;
  test_sw_n = 1;
  GRBL_IRQ_RESTORE(sreg);
  return(STATUS_OK);
}
