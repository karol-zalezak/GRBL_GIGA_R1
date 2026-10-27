/*
  hal_h7.cpp - GPIO, timer, PWM and DAC back end for the STM32H747 Cortex-M7 (GIGA R1)

  Part of Grbl-Giga, a port of Grbl 1.1 to the Arduino GIGA R1 WiFi.

  Grbl is free software: you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation, either version 3 of the License, or (at your option) any later
  version. Distributed WITHOUT ANY WARRANTY; see the GNU GPL for details.

  -------------------------------------------------------------------------------

  Timer allocation on the M7 (all count at 240 MHz):

    TIM6   16-bit  step pulse termination          IRQ prio 0   (preempts TIM7)
    EXTI4          central STOP (D49)              IRQ prio 0
    TIM7   16-bit  stepper driver interrupt        IRQ prio 1
    TIM17  16-bit  input scan                      IRQ prio 2
    TIM4   16-bit  spindle PWM on D6 (TIM4_CH2)    no interrupt

  The two 32-bit timers are taken: mbed OS runs its microsecond ticker on TIM5
  on the M7 and on TIM2 on the M4. So the stepper period, which Grbl computes
  as a 32-bit tick count, is stretched here: a period that fits 16 bits is
  loaded as is, a longer one is split into n equal timer cycles and the
  wrapper below calls Grbl's ISR on every n-th. That only happens below
  3.7 kHz, where an extra interrupt costs nothing.

  Interrupt priorities matter: the pulse-off timer must be able to preempt the
  stepper ISR the way AVR Grbl's `sei()` inside ISR(TIMER1_COMPA_vect) allowed
  ISR(TIMER0_OVF_vect) to fire on time, and the step pulse must not wait for
  the input scan. Everything else on the M7 (USB, the mbed ticker) is pushed
  below them by hal_init().

  Vectors are installed with NVIC_SetVector(): mbed copies the vector table to
  DTCM at boot, so no strong IRQHandler symbols are needed - which also keeps
  this file from planting vectors in the M4 build of the same sketch.
*/

#if defined(CORE_CM7)

#include <Arduino.h>
#include <string.h>
#include "stm32h7xx.h"

#include "hal.h"
#include "../cpu_map.h"

// ---------------------------------------------------------------------------
// Pin helpers
// ---------------------------------------------------------------------------

// Arduino pin number -> STM32 port base and bit, through the core's PinName.
static GPIO_TypeDef *pin_port(uint8_t pin)
{
  PinName pn = digitalPinToPinName(pin);
  return (GPIO_TypeDef *)(GPIOA_BASE + 0x400UL * STM_PORT(pn));
}
static uint16_t pin_mask(uint8_t pin)
{
  PinName pn = digitalPinToPinName(pin);
  return (uint16_t)(1U << STM_PIN(pn));
}
static uint32_t pin_index(uint8_t pin)
{
  return STM_PIN(digitalPinToPinName(pin));
}

// Mode registers are shared with the M4 (it configures its own pins at
// start-up), so every read-modify-write is skipped when the pin is already
// set up, and done with interrupts masked otherwise. Grbl re-runs its init
// functions on every soft reset; after the first time they touch nothing.
static void pin_configure(uint8_t pin, uint32_t moder, uint32_t pupdr, uint32_t af)
{
  GPIO_TypeDef *port = pin_port(pin);
  uint32_t n = pin_index(pin);
  uint32_t idx = STM_PORT(digitalPinToPinName(pin));
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  RCC->AHB4ENR |= (1UL << idx);
  (void)RCC->AHB4ENR;
  if (moder == 2) {
    uint32_t afr = port->AFR[n >> 3];
    uint32_t sh = (n & 7) * 4;
    if (((afr >> sh) & 0xF) != af) { port->AFR[n >> 3] = (afr & ~(0xFUL << sh)) | (af << sh); }
  }
  if (moder == 1 || moder == 2) {
    uint32_t sp = port->OSPEEDR;
    if (((sp >> (2 * n)) & 3) != 2) { port->OSPEEDR = (sp & ~(3UL << (2 * n))) | (2UL << (2 * n)); } // high speed
    if (port->OTYPER & (1UL << n)) { port->OTYPER &= ~(1UL << n); }                                // push-pull
  }
  uint32_t pu = port->PUPDR;
  if (((pu >> (2 * n)) & 3) != pupdr) { port->PUPDR = (pu & ~(3UL << (2 * n))) | (pupdr << (2 * n)); }
  uint32_t mo = port->MODER;
  if (((mo >> (2 * n)) & 3) != moder) { port->MODER = (mo & ~(3UL << (2 * n))) | (moder << (2 * n)); }
  if (!primask) { __enable_irq(); }
}

extern "C" void hal_pin_output(uint8_t pin)              { pin_configure(pin, 1, 0, 0); }
extern "C" void hal_pin_input(uint8_t pin, uint8_t mode) { pin_configure(pin, 0, (mode == HAL_PIN_INPUT_PULLUP) ? 1 : 0, 0); }
extern "C" void hal_pin_write(uint8_t pin, bool high)
{
  uint16_t m = pin_mask(pin);
  pin_port(pin)->BSRR = high ? (uint32_t)m : ((uint32_t)m << 16);
}
extern "C" bool hal_pin_read(uint8_t pin) { return (pin_port(pin)->IDR & pin_mask(pin)) != 0; }
extern "C" volatile const uint32_t *hal_pin_idr(uint8_t pin) { return &pin_port(pin)->IDR; }
extern "C" uint16_t hal_pin_mask(uint8_t pin) { return pin_mask(pin); }
extern "C" void hal_pin_pull_down(uint8_t pin) { pin_configure(pin, 0, 2, 0); }


// Both set init functions build the new table in a local and publish it with
// one interrupt-free copy: the sets are read from interrupts (the scan runs at
// 10 kHz) and Grbl re-runs limits_init() and system_init() on every soft
// reset while the scan keeps running. A half-built table would be a null
// pointer dereference in the scan ISR.
extern "C" void hal_outset_init(hal_outset_t *set, const uint8_t *pins, uint8_t n_pins)
{
  hal_outset_t tmp;
  memset(&tmp, 0, sizeof(tmp));
  if (n_pins > HAL_MAX_GROUP_PINS) { n_pins = HAL_MAX_GROUP_PINS; }

  for (uint8_t i = 0; i < n_pins; i++) {
    GPIO_TypeDef *port  = pin_port(pins[i]);
    uint16_t      hw_bit = pin_mask(pins[i]);
    hal_pin_output(pins[i]);

    // Find or create the group for this MCU port.
    volatile uint32_t *bsrr = &port->BSRR;
    int8_t gi = -1;
    for (uint8_t g = 0; g < tmp.n_groups; g++) {
      if (tmp.group[g].bsrr == bsrr) { gi = (int8_t)g; break; }
    }
    if (gi < 0) {
      if (tmp.n_groups >= HAL_MAX_PORT_GROUPS) { continue; }   // silently drop; see cpu_map.h
      gi = (int8_t)tmp.n_groups++;
      tmp.group[gi].bsrr = bsrr;
    }
    tmp.group[gi].hw_mask |= hw_bit;

    // Fold this pin into the group's lookup table.
    for (uint16_t v = 0; v < HAL_OUT_LUT_SIZE; v++) {
      if (v & (1u << i)) { tmp.group[gi].lut[v] |= hw_bit; }
    }
  }

  uint32_t saved = hal_irq_disable();
  memcpy(set, &tmp, sizeof(tmp));
  hal_irq_restore(saved);
}

extern "C" void hal_inset_init(hal_inset_t *set, const uint8_t *pins, uint8_t n_pins, uint8_t mode)
{
  hal_inset_t tmp;
  memset(&tmp, 0, sizeof(tmp));
  if (n_pins > HAL_MAX_GROUP_PINS) { n_pins = HAL_MAX_GROUP_PINS; }
  tmp.n_pins = n_pins;

  for (uint8_t i = 0; i < n_pins; i++) {
    hal_pin_input(pins[i], mode);
    tmp.idr[i] = &pin_port(pins[i])->IDR;
    tmp.bit[i] = pin_mask(pins[i]);
  }

  uint32_t saved = hal_irq_disable();
  memcpy(set, &tmp, sizeof(tmp));
  hal_irq_restore(saved);
}


// ---------------------------------------------------------------------------
// Timers
// ---------------------------------------------------------------------------

#define STEPPER_TIM         TIM7
#define PULSE_TIM           TIM6
#define SCAN_TIM            TIM17

#define STEPPER_TIMER_IRQ   TIM7_IRQn
#define PULSE_TIMER_IRQ     TIM6_DAC_IRQn
#define SCAN_TIMER_IRQ      TIM17_IRQn

#define PULSE_TIMER_PRIO    0
// The stepper sits above the input scan: the scan reads seven GPIO ports and
// works through twenty channels, and if it held the stepper interrupt back
// between its timer event and the start of the step pulse, that pulse would
// end late and run into the next one at 200 kHz. The central STOP does not
// depend on the scan (its own EXTI at priority 0); the scan, for limits and
// the rest, cannot be starved - the period floor leaves the stepper at most a
// fraction of the CPU - and is delayed by at most one stepper interrupt.
#define STEPPER_TIMER_PRIO  1
#define SCAN_TIMER_PRIO     2
// Everything else on this core is moved down to here or below.
#define OTHER_IRQ_PRIO      3

static void (*stepper_isr_cb)(void) = 0;
static void (*pulse_isr_cb)(void)   = 0;
static void (*pulse_delay_isr_cb)(void) = 0;
static void (*scan_isr_cb)(void)    = 0;

volatile uint32_t hal_scan_ticks = 0;
volatile uint32_t hal_diag_stepper_ticks = 0;
volatile uint32_t hal_diag_pulse_ticks = 0;
volatile uint32_t hal_diag_pulse_overrun = 0;
volatile uint32_t hal_diag_period_floor = 0;
volatile uint32_t hal_diag_isr_lat_max = 0;
volatile uint32_t hal_diag_isr_lat_over = 0;
volatile uint32_t hal_diag_scan_cycles_max = 0;
volatile uint32_t hal_diag_stepper_cycles_max = 0;
static uint32_t stepper_period_floor = 2 * HAL_TICKS_PER_US;

// Period extension: the stepper ISR runs on every ext_reload-th timer update.
static volatile uint32_t ext_reload = 1;
static volatile uint32_t ext_count = 1;

// One stretched stepper period (hal_stepper_timer_stretch_once): the values
// to restore when it is over.
static volatile uint8_t stretch_pending = 0;
static uint32_t stretch_arr = 0;
static uint32_t stretch_reload = 1;

// When a pre-step delay is configured the pulse timer fires twice per step: the
// first expiry starts the pulse, the second ends it. `pulse_phase` tracks which.
static volatile uint8_t pulse_phase = 0;
static uint32_t pulse_delay_ticks = 0;
static uint32_t pulse_width_ticks = 0;

volatile uint32_t hal_diag_stepper_spurious = 0;

static void stepper_timer_isr(void)
{
  // CNT restarted from zero at the update event that raised this interrupt,
  // so it now reads the entry latency in ticks.
  uint32_t lat = STEPPER_TIM->CNT;
  // An entry without an update: the interrupt pending again from the last one,
  // whose flag had not yet been cleared in the timer when the handler ended -
  // seen about once per segment load with a stretched period. Counted as a
  // timer cycle it made the step that followed come one cycle early: 100
  // steps/s ran at 101.8 (measured on the oscilloscope, 2 ms segments).
  if (!(STEPPER_TIM->SR & TIM_SR_UIF)) { hal_diag_stepper_spurious++; return; }
  STEPPER_TIM->SR = ~TIM_SR_UIF;
  if (--ext_count) { return; }      // a stretched period is not over yet
  if (stretch_pending) {
    // The one long period is over: back to the period the ISR last set.
    stretch_pending = 0;
    ext_reload = stretch_reload;
    STEPPER_TIM->ARR = stretch_arr;
  }
  ext_count = ext_reload;
  if (lat > hal_diag_isr_lat_max) { hal_diag_isr_lat_max = lat; }
  if (lat > 8 * HAL_TICKS_PER_US) { hal_diag_isr_lat_over++; }
  hal_diag_stepper_ticks++;
  uint32_t t0 = DWT->CYCCNT;
  if (stepper_isr_cb) { stepper_isr_cb(); }
  uint32_t dt = DWT->CYCCNT - t0;
  if (dt > hal_diag_stepper_cycles_max) { hal_diag_stepper_cycles_max = dt; }
}

// Set by hal_pulse_timer_arm_delayed(): this one pulse starts with a delay.
static volatile uint8_t pulse_oneshot_delay = 0;

static void pulse_timer_isr(void)
{
  PULSE_TIM->SR = ~TIM_SR_UIF;
  hal_diag_pulse_ticks++;
  // One-pulse mode has already stopped the counter.
  if ((pulse_delay_ticks || pulse_oneshot_delay) && pulse_phase == 0) {
    // End of the direction-setup delay: raise the step pins and re-arm for the
    // pulse width itself.
    pulse_oneshot_delay = 0;
    pulse_phase = 1;
    if (pulse_delay_isr_cb) { pulse_delay_isr_cb(); }
    hal_pulse_timer_arm(pulse_width_ticks);
    return;
  }
  pulse_phase = 0;
  if (pulse_isr_cb) { pulse_isr_cb(); }
}

static void scan_timer_isr(void)
{
  SCAN_TIM->SR = ~TIM_SR_UIF;
  hal_scan_ticks++;
  uint32_t t0 = DWT->CYCCNT;
  if (scan_isr_cb) { scan_isr_cb(); }
  uint32_t dt = DWT->CYCCNT - t0;
  if (dt > hal_diag_scan_cycles_max) { hal_diag_scan_cycles_max = dt; }
}

// Puts a timer into a known state: stopped, prescaler loaded through EGR.UG
// with the resulting update flag cleared again, update interrupt enabled.
static void timer_setup(TIM_TypeDef *tim, uint32_t psc, uint32_t arr, uint32_t cr1)
{
  tim->CR1  = 0;
  tim->DIER = 0;
  tim->PSC  = psc;
  tim->ARR  = arr;
  tim->CNT  = 0;
  tim->EGR  = TIM_EGR_UG;          // load PSC
  tim->SR   = 0;
  tim->CR1  = cr1;
  tim->DIER = TIM_DIER_UIE;
}

static void irq_attach(IRQn_Type irq, void (*handler)(void), uint32_t prio)
{
  NVIC_DisableIRQ(irq);
  NVIC_SetVector(irq, (uint32_t)handler);
  NVIC_SetPriority(irq, prio);
  NVIC_ClearPendingIRQ(irq);
  NVIC_EnableIRQ(irq);
}

extern "C" void hal_stepper_timer_init(void (*isr)(void))
{
  stepper_isr_cb = isr;
  RCC->APB1LENR |= RCC_APB1LENR_TIM7EN;
  (void)RCC->APB1LENR;
  // URS clear on purpose: hal_stepper_timer_set_period() relies on EGR.UG
  // raising the update interrupt. ARPE clear: ARR writes act at once.
  timer_setup(STEPPER_TIM, 0, 0xFFFF, 0);
  irq_attach(STEPPER_TIMER_IRQ, stepper_timer_isr, STEPPER_TIMER_PRIO);
}

extern "C" void hal_stepper_timer_set_period(uint32_t period_ticks)
{
  // A tick period shorter than the step pulse would re-arm the pulse timer
  // before the pulse had ended and merge two steps into one on the driver, so
  // the HAL keeps the period a margin above the pulse. The step rate then
  // falls short of the plan rather than the pulses being lost.
  if (period_ticks < stepper_period_floor) { period_ticks = stepper_period_floor; hal_diag_period_floor++; }
  uint32_t n = 1;
  if (period_ticks > 0x10000UL) {
    n = (period_ticks + 0xFFFFUL) >> 16;
    period_ticks /= n;   // the remainder is under n ticks of 4 ns: immaterial
  }
  ext_reload = n;
  ext_count = n;
  STEPPER_TIM->ARR = period_ticks - 1;
  // Written right after the update that started this period, so CNT is only
  // the entry latency. Should an interrupt storm have held the ISR back past
  // the new period, the counter would otherwise run on to 0xFFFF: restart it,
  // which raises the update (and with it the next tick) at once.
  if (STEPPER_TIM->CNT >= period_ticks - 1) { STEPPER_TIM->EGR = TIM_EGR_UG; }
}

extern "C" void hal_stepper_timer_stretch_once(uint32_t min_ticks)
{
  uint32_t now_ticks = (STEPPER_TIM->ARR + 1) * ext_reload;
  if (now_ticks >= min_ticks) { return; }
  if (!stretch_pending) {      // remember the period to go back to
    stretch_arr = STEPPER_TIM->ARR;
    stretch_reload = ext_reload;
  }
  uint32_t n = 1;
  if (min_ticks > 0x10000UL) { n = (min_ticks + 0xFFFFUL) >> 16; }
  uint32_t arr = (min_ticks + n - 1) / n;
  stretch_pending = 1;
  ext_reload = n;
  ext_count = n;
  STEPPER_TIM->ARR = arr - 1;
}

extern "C" void hal_stepper_timer_start(uint32_t period_ticks)
{
  stretch_pending = 0;
  STEPPER_TIM->CNT = 0;
  hal_stepper_timer_set_period(period_ticks);
  STEPPER_TIM->SR = ~TIM_SR_UIF;
  STEPPER_TIM->CR1 |= TIM_CR1_CEN;
}

extern "C" void hal_stepper_timer_stop(void)
{
  // st_go_idle() is reached from inside the stepper ISR, so this must be a
  // plain register write. Clearing UIF afterwards drops an overflow that may
  // have landed while the ISR was running.
  STEPPER_TIM->CR1 &= ~TIM_CR1_CEN;
  STEPPER_TIM->SR = ~TIM_SR_UIF;
  NVIC_ClearPendingIRQ(STEPPER_TIMER_IRQ);
}

extern "C" void hal_pulse_timer_init(void (*isr)(void))
{
  pulse_isr_cb = isr;
  RCC->APB1LENR |= RCC_APB1LENR_TIM6EN;
  (void)RCC->APB1LENR;
  timer_setup(PULSE_TIM, 0, 0xFFFF, TIM_CR1_OPM | TIM_CR1_URS);
  irq_attach(PULSE_TIMER_IRQ, pulse_timer_isr, PULSE_TIMER_PRIO);
}

extern "C" void hal_pulse_timer_arm(uint32_t ticks)
{
  if (PULSE_TIM->CR1 & TIM_CR1_CEN) { hal_diag_pulse_overrun++; }
  if (ticks > 0xFFFF) { ticks = 0xFFFF; }   // 273 us; $0 tops out at 255 us
  PULSE_TIM->CNT = 0;
  PULSE_TIM->ARR = ticks;
  PULSE_TIM->CR1 |= TIM_CR1_CEN;   // one-pulse mode: stops itself
}

extern "C" void hal_pulse_timer_set_delay_isr(void (*isr_delay)(void)) { pulse_delay_isr_cb = isr_delay; }
extern "C" void hal_pulse_timer_arm_delayed(uint32_t delay_ticks)
{
  pulse_phase = 0;
  pulse_oneshot_delay = 1;
  hal_pulse_timer_arm(delay_ticks);
}
extern "C" void hal_pulse_timer_set_delay(uint32_t ticks)              { pulse_delay_ticks = ticks; }

// Called by stepper.c so the two-phase pulse timer knows the pulse width, and
// so the stepper period floor follows the $0 setting.
extern "C" void hal_pulse_timer_set_width(uint32_t ticks)
{
  pulse_width_ticks = ticks;
  stepper_period_floor = ticks + pulse_delay_ticks + STEP_PERIOD_MARGIN_US * HAL_TICKS_PER_US;
}

extern "C" void hal_scan_timer_init(void (*isr)(void), uint32_t hz)
{
  scan_isr_cb = isr;
  // TIM17 hangs off APB2, whose timer clock need not equal APB1's.
  uint32_t period = hal_timer2_clock_hz() / hz;
  uint32_t psc = 0;
  while (period / (psc + 1) > 0x10000UL) { psc++; }
  RCC->APB2ENR |= RCC_APB2ENR_TIM17EN;
  (void)RCC->APB2ENR;
  timer_setup(SCAN_TIM, psc, period / (psc + 1) - 1, TIM_CR1_URS);
  irq_attach(SCAN_TIMER_IRQ, scan_timer_isr, SCAN_TIMER_PRIO);
  SCAN_TIM->CR1 |= TIM_CR1_CEN;
}


// ---------------------------------------------------------------------------
// Central STOP: EXTI line of the pin, both edges, NVIC priority 0
// ---------------------------------------------------------------------------

#define ESTOP_IRQ_PRIO  0

static void (*estop_isr_cb)(void) = 0;
static uint32_t estop_line = 0;
static IRQn_Type estop_irq = EXTI4_IRQn;

static void estop_exti_isr(void)
{
  EXTI_D1->PR1 = (1UL << estop_line);    // write 1 to clear
  if (estop_isr_cb) { estop_isr_cb(); }
}

extern "C" bool hal_estop_init(uint8_t pin, void (*isr)(void))
{
  PinName pn = digitalPinToPinName(pin);
  uint32_t line = STM_PIN(pn);
  uint32_t port = STM_PORT(pn);
  if (line > 4) { return false; }        // lines 5-15 share vectors
  estop_isr_cb = isr;
  estop_line = line;
  estop_irq = (IRQn_Type)(EXTI0_IRQn + line);   // EXTI0..EXTI4 are consecutive
  RCC->APB4ENR |= RCC_APB4ENR_SYSCFGEN;
  (void)RCC->APB4ENR;
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  uint32_t sh = (line & 3) * 4;
  SYSCFG->EXTICR[line >> 2] = (SYSCFG->EXTICR[line >> 2] & ~(0xFUL << sh)) | (port << sh);
  EXTI->RTSR1 |= (1UL << line);
  EXTI->FTSR1 |= (1UL << line);
  EXTI_D1->PR1 = (1UL << line);
  EXTI_D1->IMR1 |= (1UL << line);
  if (!primask) { __enable_irq(); }
  irq_attach(estop_irq, estop_exti_isr, ESTOP_IRQ_PRIO);
  return true;
}


// ---------------------------------------------------------------------------
// Spindle PWM: D6 is PD13, TIM4 channel 2 on alternate function 2.
// ---------------------------------------------------------------------------
//
// With the PWM off ($38=0) D6 is the output channel OUT13 and TIM4 stands;
// the period is still worked out, because the DAC's level is computed from
// the same duty value.

static uint32_t spindle_pwm_period = 0;
static bool spindle_pwm_on = false;
static bool dac_ready = false;
static uint8_t  spindle_pwm_pin = 0xFF;
static uint32_t spindle_pwm_freq = 0;
static uint8_t  spindle_pwm_mode = 0xFF;

extern "C" uint32_t hal_spindle_pwm_init(uint8_t pin, uint32_t frequency_hz, uint8_t mode)
{
  // spindle_init() runs on every reset and on every $30/$31 change, so make
  // repeat calls with unchanged parameters free.
  if (spindle_pwm_period && pin == spindle_pwm_pin && frequency_hz == spindle_pwm_freq &&
      mode == spindle_pwm_mode) {
    return spindle_pwm_period;
  }
  if (frequency_hz == 0 || digitalPinToPinName(pin) != PD_13) { return 0; }   // only D6 is wired to TIM4_CH2

  uint32_t period = HAL_TIMER_HZ / frequency_hz;
  uint32_t psc = 0;
  while (period / (psc + 1) > 0x10000UL) { psc++; }   // TIM4 is 16-bit
  period /= (psc + 1);
  if (period < 2) { period = 2; }

  if (mode == HAL_PWM_OFF) {
    // Hand the pin back as a plain output, low (io.c puts OUT13's level on it).
    if (spindle_pwm_on) {
      hal_pin_write(pin, false);
      pin_configure(pin, 1, 0, 0);
      TIM4->CR1 = 0;
      TIM4->CCER = 0;
    }
    spindle_pwm_on = false;
    spindle_pwm_period = period;
    spindle_pwm_pin = pin;
    spindle_pwm_freq = frequency_hz;
    spindle_pwm_mode = mode;
    return period;
  }

  RCC->APB1LENR |= RCC_APB1LENR_TIM4EN;
  (void)RCC->APB1LENR;
  TIM4->CR1  = 0;
  TIM4->CCER = 0;
  TIM4->PSC  = psc;
  TIM4->ARR  = period - 1;
  TIM4->CCR2 = 0;
  TIM4->CCMR1 = (TIM4->CCMR1 & ~(TIM_CCMR1_OC2M | TIM_CCMR1_CC2S))
              | (6U << TIM_CCMR1_OC2M_Pos) | TIM_CCMR1_OC2PE;   // PWM mode 1, preloaded
  // Inverted: active low, so the pin is high at duty 0 - right for a TTL PWM
  // input behind an output that sinks to 0 V when the pin is high.
  TIM4->CCER = TIM_CCER_CC2E | ((mode == HAL_PWM_INVERTED) ? TIM_CCER_CC2P : 0);
  TIM4->CR1  = TIM_CR1_ARPE | TIM_CR1_URS;
  TIM4->EGR  = TIM_EGR_UG;
  TIM4->CR1 |= TIM_CR1_CEN;

  pin_configure(pin, 2, 0, 2);   // AF2 = TIM4

  spindle_pwm_on = true;
  spindle_pwm_period = period;
  spindle_pwm_pin = pin;
  spindle_pwm_freq = frequency_hz;
  spindle_pwm_mode = mode;
  return period;
}

extern "C" void hal_spindle_pwm_set(uint32_t duty_counts)
{
  if (!spindle_pwm_on) { return; }
  if (duty_counts > spindle_pwm_period) { duty_counts = spindle_pwm_period; }
  TIM4->CCR2 = duty_counts;
}

// What the PWM and DAC hardware is actually set to, for $I: duty in 0.1 %
// read back from TIM4, and the DAC output register (0-4095).
extern "C" uint32_t hal_spindle_pwm_readback(void)
{
  if (!spindle_pwm_on || !(TIM4->CR1 & TIM_CR1_CEN)) { return 0xFFFF; }
  return (uint32_t)(((uint64_t)TIM4->CCR2 * 1000ULL) / (TIM4->ARR + 1));
}

extern "C" uint32_t hal_spindle_dac_readback(void)
{
  return dac_ready ? (DAC1->DOR1 & 0xFFF) : 0xFFFF;
}

extern "C" void hal_spindle_pwm_off(void)
{
  if (!spindle_pwm_on) { return; }
  TIM4->CCR2 = 0;
}

// DAC1 channel 1 on PA4 (A12): 0-3.3 V proportional to the spindle speed,
// buffered output. A x3 non-inverting stage makes the 0-10 V a VFD expects.
extern "C" void hal_spindle_dac_init(uint8_t pin)
{
  if (dac_ready) { return; }
  if (digitalPinToPinName(pin) != PA_4) { return; }
  pin_configure(pin, 3, 0, 0);            // analog mode
  RCC->APB1LENR |= RCC_APB1LENR_DAC12EN;
  (void)RCC->APB1LENR;
  DAC1->DHR12R1 = 0;
  DAC1->CR |= DAC_CR_EN1;
  dac_ready = true;
}

extern "C" void hal_spindle_dac_set(uint32_t value_12bit)
{
  if (!dac_ready) { return; }
  if (value_12bit > 4095) { value_12bit = 4095; }
  DAC1->DHR12R1 = value_12bit;
}


// Analog inputs (the operator panel's potentiometers on A0/A1, ADC1) through
// the core's analogRead(): mbed sets up the ADC and its kernel clock. A
// conversion takes some microseconds; main loop only.
extern "C" uint16_t hal_analog_read(uint8_t pin)
{
  static bool resolution_set = false;
  if (!resolution_set) { analogReadResolution(12); resolution_set = true; }
  int v = analogRead(pin);
  return (uint16_t)((v < 0) ? 0 : ((v > 4095) ? 4095 : v));
}


// ---------------------------------------------------------------------------
// Handwheel: TIM8 counts the quadrature of D54 (PI5 = TIM8_CH1) and D59
// (PI6 = TIM8_CH2), both on alternate function 3. Nothing else uses TIM8.
// ---------------------------------------------------------------------------

extern "C" void hal_mpg_init(uint8_t pin_a, uint8_t pin_b)
{
  pin_configure(pin_a, 2, 1, 3);
  pin_configure(pin_b, 2, 1, 3);
  RCC->APB2ENR |= RCC_APB2ENR_TIM8EN;
  (void)RCC->APB2ENR;
  TIM8->CR1 = TIM_CR1_CKD_1;                 // tDTS = 4 timer clocks, for the filter
  TIM8->SMCR = 0;
  TIM8->CCER = 0;
  // Both channels inputs on their own pin (CCxS = 01), filter fDTS/32, N = 8:
  // an edge counts once it has held some 4 us.
  TIM8->CCMR1 = (1UL << TIM_CCMR1_CC1S_Pos) | (1UL << TIM_CCMR1_CC2S_Pos) |
                (0xFUL << TIM_CCMR1_IC1F_Pos) | (0xFUL << TIM_CCMR1_IC2F_Pos);
  TIM8->SMCR = 3UL << TIM_SMCR_SMS_Pos;      // encoder mode 3: both edges of both inputs
  TIM8->ARR = 0xFFFF;
  TIM8->CNT = 0;
  TIM8->CR1 |= TIM_CR1_CEN;
}

extern "C" uint16_t hal_mpg_count(void) { return (uint16_t)TIM8->CNT; }

extern "C" void hal_mpg_test_levels(uint8_t pin_a, uint8_t pin_b, bool a_high, bool b_high)
{
  pin_configure(pin_a, 2, a_high ? 1 : 2, 3);
  pin_configure(pin_b, 2, b_high ? 1 : 2, 3);
}


// ---------------------------------------------------------------------------
// RS485 to the spindle VFD: USART2 on D18 (PD5 = TX) and D19 (PD6 = RX),
// alternate function 7, the transceiver's DE + ~RE on a GPIO (D17).
// ---------------------------------------------------------------------------
//
// The interrupt sits at OTHER_IRQ_PRIO, below the Grbl timers; with the
// 16-byte FIFOs on, the microseconds mbed's critical sections or a stepper
// interrupt hold it back cost nothing. A Modbus request (8 bytes) fits the
// transmit FIFO whole.

static volatile uint8_t rs_tx_buf[HAL_RS485_FRAME_MAX];
static volatile uint8_t rs_tx_len = 0, rs_tx_pos = 0;
static volatile bool rs_sending = false;
static volatile uint8_t rs_rx_buf[HAL_RS485_FRAME_MAX];
static volatile uint8_t rs_rx_len = 0;
static volatile bool rs_rx_done = false;     // a frame is complete and waits to be read
static volatile bool rs_rx_bad = false;      // ...and something in it went wrong
static volatile uint32_t rs_last_cycles = 0; // DWT at the last byte either way
static uint32_t rs_clock = 0, rs_baud = 0;
static uint8_t rs_format = 0xFF, rs_de_pin = 0xFF;

#define RS485_RX_ERRORS  (USART_ISR_ORE | USART_ISR_FE | USART_ISR_NE | USART_ISR_PE)
#define RS485_RX_CLEAR   (USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NECF | USART_ICR_PECF)

// The USART2 kernel clock. Its source is shared with USART3, UART4/5/7/8,
// so it is only read, never changed (mbed leaves it at PCLK1, 120 MHz).
static uint32_t rs485_kernel_clock(void)
{
  switch (RCC->D2CCIP2R & RCC_D2CCIP2R_USART28SEL) {
    case 0: return HAL_RCC_GetPCLK1Freq();
    case 1: { PLL2_ClocksTypeDef c; HAL_RCCEx_GetPLL2ClockFreq(&c); return c.PLL2_Q_Frequency; }
    case 2: { PLL3_ClocksTypeDef c; HAL_RCCEx_GetPLL3ClockFreq(&c); return c.PLL3_Q_Frequency; }
    case 3: return HSI_VALUE >> ((RCC->CR & RCC_CR_HSIDIV) >> RCC_CR_HSIDIV_Pos);
    case 4: return CSI_VALUE;
    case 5: return LSE_VALUE;
  }
  return 0;
}

static void rs485_isr(void)
{
  // Receive: drain the FIFO. The error flags belong to the byte at its head.
  uint32_t s;
  while ((s = USART2->ISR) & USART_ISR_RXNE_RXFNE) {
    if (s & RS485_RX_ERRORS) {
      USART2->ICR = RS485_RX_CLEAR;
      rs_rx_bad = true;
    }
    uint8_t b = (uint8_t)USART2->RDR;
    if (!rs_rx_done) {
      if (rs_rx_len < HAL_RS485_FRAME_MAX) { rs_rx_buf[rs_rx_len++] = b; } else { rs_rx_bad = true; }
    }
    rs_last_cycles = DWT->CYCCNT;
  }
  if (s & USART_ISR_ORE) { USART2->ICR = USART_ICR_ORECF; rs_rx_bad = true; }
  // 3.5 characters of silence: the frame is complete.
  if (s & USART_ISR_RTOF) {
    USART2->ICR = USART_ICR_RTOCF;
    if (rs_rx_len) { rs_rx_done = true; }
  }

  // Transmit: keep the FIFO filled; after the last byte, wait for TC.
  uint32_t cr1 = USART2->CR1;
  if (cr1 & USART_CR1_TXEIE_TXFNFIE) {
    while ((rs_tx_pos < rs_tx_len) && (USART2->ISR & USART_ISR_TXE_TXFNF)) {
      USART2->TDR = rs_tx_buf[rs_tx_pos++];
    }
    if (rs_tx_pos >= rs_tx_len) { USART2->CR1 = (cr1 & ~USART_CR1_TXEIE_TXFNFIE) | USART_CR1_TCIE; }
  } else if ((cr1 & USART_CR1_TCIE) && (USART2->ISR & USART_ISR_TC)) {
    // Last stop bit out: the bus belongs to the VFD again. What the receiver
    // might have picked up meanwhile is thrown away.
    hal_pin_write(rs_de_pin, false);
    USART2->CR1 = cr1 & ~USART_CR1_TCIE;
    USART2->ICR = USART_ICR_TCCF | USART_ICR_RTOCF | RS485_RX_CLEAR;
    USART2->RQR = USART_RQR_RXFRQ;
    rs_rx_len = 0;
    rs_rx_done = false;
    rs_rx_bad = false;
    USART2->CR1 |= USART_CR1_RE;
    rs_last_cycles = DWT->CYCCNT;
    rs_sending = false;
  }
}

extern "C" void hal_rs485_stop(void)
{
  if (!rs_clock) { return; }
  NVIC_DisableIRQ(USART2_IRQn);
  USART2->CR1 = 0;
  hal_pin_write(rs_de_pin, false);
  rs_clock = 0;
  rs_sending = false;
  rs_rx_len = 0;
  rs_rx_done = false;
}

extern "C" bool hal_rs485_init(uint32_t baud, uint8_t format, uint8_t de_pin)
{
  if (rs_clock && (baud == rs_baud) && (format == rs_format) && (de_pin == rs_de_pin)) { return true; }
  hal_rs485_stop();
  if ((baud == 0) || (format >= HAL_RS485_FORMATS)) { return false; }

  // Oversampling by 16: BRR = clock / prescaler / baud, 16 to 65535.
  static const uint16_t presc_div[12] = { 1, 2, 4, 6, 8, 10, 12, 16, 32, 64, 128, 256 };
  uint32_t clk = rs485_kernel_clock();
  uint32_t p, div = 0;
  for (p = 0; p < 12; p++) {
    div = (clk / presc_div[p] + baud / 2) / baud;
    if (div <= 0xFFFFUL) { break; }
  }
  if ((p == 12) || (div < 16)) { return false; }

  bool parity = (format == 2) || (format == 3) || (format == 4) || (format == 5);
  bool odd = (format == 3) || (format == 5);
  bool two_stop = (format == 1) || (format == 4) || (format == 5);
  uint32_t char_bits = 10 + (parity ? 1 : 0) + (two_stop ? 1 : 0);
  // End of frame after 3.5 characters, or the fixed 1.75 ms Modbus asks for
  // above 19200 baud. RTO counts bit times after the last stop bit.
  uint32_t rto = (baud <= 19200) ? (35 * char_bits + 9) / 10
                                 : (uint32_t)((1750ULL * baud + 999999ULL) / 1000000ULL);

  RCC->APB1LENR |= RCC_APB1LENR_USART2EN;
  (void)RCC->APB1LENR;
  USART2->CR1 = 0;                       // UE off: word length, parity, FIFO only change now
  USART2->PRESC = p;
  USART2->BRR = div;
  USART2->CR2 = (two_stop ? USART_CR2_STOP_1 : 0) | USART_CR2_RTOEN;
  USART2->CR3 = 0;
  USART2->RTOR = rto;
  uint32_t cr1 = USART_CR1_FIFOEN | USART_CR1_TE | USART_CR1_RE |
                 USART_CR1_RXNEIE_RXFNEIE | USART_CR1_RTOIE;
  if (parity) { cr1 |= USART_CR1_PCE | USART_CR1_M0 | (odd ? USART_CR1_PS : 0); }   // 8 data + parity = 9 bits
  USART2->CR1 = cr1;

  // Driver enable low (receive) before the pin becomes an output, so the
  // shield's pull-down hands over without a glitch.
  hal_pin_write(de_pin, false);
  pin_configure(de_pin, 1, 0, 0);
  pin_configure(RS485_TX_PIN, 2, 1, 7);  // pulled up while UE is off
  pin_configure(RS485_RX_PIN, 2, 1, 7);  // pulled up while the transceiver's ~RE is high

  USART2->ICR = 0xFFFFFFFFUL;
  USART2->CR1 = cr1 | USART_CR1_UE;
  rs_sending = false;
  rs_rx_len = 0;
  rs_rx_done = false;
  rs_rx_bad = false;
  rs_last_cycles = DWT->CYCCNT;
  rs_clock = clk;
  rs_baud = baud;
  rs_format = format;
  rs_de_pin = de_pin;
  irq_attach(USART2_IRQn, rs485_isr, OTHER_IRQ_PRIO);
  return true;
}

extern "C" void hal_rs485_send(const uint8_t *frame, uint8_t len)
{
  if (!rs_clock || !len) { return; }
  if (len > HAL_RS485_FRAME_MAX) { len = HAL_RS485_FRAME_MAX; }
  NVIC_DisableIRQ(USART2_IRQn);
  USART2->CR1 &= ~(USART_CR1_RE | USART_CR1_TCIE | USART_CR1_TXEIE_TXFNFIE);
  for (uint8_t i = 0; i < len; i++) { rs_tx_buf[i] = frame[i]; }
  rs_tx_len = len;
  rs_tx_pos = 0;
  rs_rx_len = 0;
  rs_rx_done = false;
  rs_rx_bad = false;
  rs_sending = true;
  hal_pin_write(rs_de_pin, true);
  hal_delay_us(RS485_DE_SETUP_US);      // driver on (through the isolator) before the start bit
  USART2->ICR = USART_ICR_TCCF;
  USART2->CR1 |= USART_CR1_TXEIE_TXFNFIE;
  NVIC_EnableIRQ(USART2_IRQn);
}

extern "C" bool hal_rs485_sending(void) { return rs_sending; }

extern "C" int hal_rs485_frame(uint8_t *buf, uint8_t max)
{
  if (!rs_clock || !rs_rx_done) { return 0; }
  NVIC_DisableIRQ(USART2_IRQn);
  int n = rs_rx_len;
  bool bad = rs_rx_bad;
  if (n > max) { n = max; bad = true; }
  for (int i = 0; i < n; i++) { buf[i] = rs_rx_buf[i]; }
  rs_rx_len = 0;
  rs_rx_done = false;
  rs_rx_bad = false;
  NVIC_EnableIRQ(USART2_IRQn);
  return bad ? -1 : n;
}

extern "C" uint32_t hal_rs485_quiet_us(void)
{
  if (rs_sending) { return 0; }
  uint32_t us = (DWT->CYCCNT - rs_last_cycles) / hal_cycles_per_us();
  return (us > 1000000UL) ? 1000000UL : us;
}

extern "C" uint32_t hal_rs485_clock_hz(void) { return rs_clock; }


// ---------------------------------------------------------------------------
// Start-up, clocks, delays
// ---------------------------------------------------------------------------

static uint32_t cycles_per_us = 480;
static uint64_t ms_cycles = 0;     // see hal_millis()
static uint32_t ms_last = 0;

static void nvic_police(void);              // defined with the critical sections below
static void cs_switch_to_priority(void);

extern "C" uint32_t hal_timer_clock_hz(void)
{
  // APB1 timer kernel clock: PCLK1, doubled when the APB1 prescaler divides.
  uint32_t pclk1 = HAL_RCC_GetPCLK1Freq();
  if ((RCC->D2CFGR & RCC_D2CFGR_D2PPRE1) != RCC_D2CFGR_D2PPRE1_DIV1) { pclk1 *= 2; }
  return pclk1;
}

// APB2 timer kernel clock (TIM1/8/15/16/17), likewise.
extern "C" uint32_t hal_timer2_clock_hz(void)
{
  uint32_t pclk2 = HAL_RCC_GetPCLK2Freq();
  if ((RCC->D2CFGR & RCC_D2CFGR_D2PPRE2) != RCC_D2CFGR_D2PPRE2_DIV1) { pclk2 *= 2; }
  return pclk2;
}

// Interrupt stack. mbed's linker script leaves handlers 1 kB of MSP, and Grbl
// nests four interrupt levels on top of the USB stack's own handler (pulse,
// scan, stepper, USB), each frame up to 104 bytes with the FPU in use. The
// threads all run on their own stacks (PSP), so the MSP can simply be moved.
static uint64_t isr_stack[1024];   // 8 kB

extern "C" void hal_init(void)
{
  if (__get_CONTROL() & CONTROL_SPSEL_Msk) {   // thread mode on PSP: MSP is idle
    __set_MSP((uint32_t)&isr_stack[sizeof(isr_stack) / sizeof(isr_stack[0])]);
  }

  // Cycle counter for the busy-wait delays. The M7's DWT sits behind a
  // CoreSight lock that has to be opened first.
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->LAR = 0xC5ACCE55;
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
  ms_cycles = (uint64_t)millis() * 480000ULL;   // carry on from mbed's count since boot
  ms_last = DWT->CYCCNT;
  cycles_per_us = SystemCoreClock / 1000000UL;
  if (cycles_per_us == 0) { cycles_per_us = 480; }

  // Make room at the top of the priority range for the three Grbl timers.
  // mbed leaves its own interrupts (USB, the microsecond ticker) at their
  // defaults; none of them is time-critical at the microsecond level. From
  // here on mbed's critical sections mask by priority (see above).
  nvic_police();
  cs_switch_to_priority();

  #ifdef ACTIVITY_LEDS
    hal_pin_write(LED_RED_PIN, true);     // active low: off
    hal_pin_write(LED_GREEN_PIN, true);
    hal_pin_write(LED_BLUE_PIN, true);
    hal_pin_output(LED_RED_PIN);
    hal_pin_output(LED_GREEN_PIN);
    hal_pin_output(LED_BLUE_PIN);
  #endif
}

extern "C" void hal_delay_us(uint32_t us)
{
  // Spins on the cycle counter, which runs regardless of interrupt masking.
  // Split so the 32-bit tick count cannot overflow (8.9 s at 480 MHz).
  while (us) {
    uint32_t chunk = (us > 1000000UL) ? 1000000UL : us;
    uint32_t start = DWT->CYCCNT;
    uint32_t ticks = chunk * cycles_per_us;
    while ((DWT->CYCCNT - start) < ticks) { }
    us -= chunk;
  }
}

extern "C" uint32_t hal_cycles_per_us(void) { return cycles_per_us; }

extern "C" void hal_delay_ms(uint32_t ms)
{
  while (ms--) { hal_delay_us(1000); }
}

// Milliseconds since start, from the cycle counter. mbed's millis() was
// measured running 4 % slow against the PC (step rates timed by the PC are
// exact to 0.1 %, so the core clock is right; the ticker behind millis() is
// not), and uptime, the display's error timeout and the scan-rate checks in
// the tests all lean on this. CYCCNT wraps every 8.9 s at 480 MHz, so it is
// folded into a 64-bit count on every call; the serial pump calls it far
// more often than that.
extern "C" uint32_t hal_millis(void)
{
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  uint32_t now = DWT->CYCCNT;
  ms_cycles += (uint32_t)(now - ms_last);
  ms_last = now;
  uint64_t c = ms_cycles;
  if (!primask) { __enable_irq(); }
  return (uint32_t)(c / ((uint64_t)cycles_per_us * 1000ULL));
}

// ---------------------------------------------------------------------------
// mbed's critical sections, by priority instead of by PRIMASK
// ---------------------------------------------------------------------------
//
// mbed protects its own data with core_util_critical_section_enter(), whose
// default back end masks every interrupt. The USB device stack runs its whole
// event handling inside one - measured up to 13 us - and that held the step
// pulse timer back far enough to merge pulses above 100 kHz. These three are
// weak in the core; this version masks by BASEPRI instead, so everything at
// OTHER_IRQ_PRIO and below (all of mbed, the USB stack included) is held off
// exactly as before, while the three Grbl timers above it keep running. They
// never touch mbed state, which is what makes that safe.
//
// One side effect: WFI in mbed's idle loop is only woken by an unmasked
// interrupt. Grbl's main loop never blocks, and the 10 kHz input scan is
// unmasked, so the worst case is a 100 us late wake-up.

#define CS_BASEPRI   (OTHER_IRQ_PRIO << (8U - __NVIC_PRIO_BITS))

static volatile uint32_t cs_saved_basepri = 0;
static volatile bool cs_saved_irq_on = false;
static volatile bool cs_saved = false;
// Until hal_init() has moved mbed's interrupts below the Grbl timers they sit
// at priority 0, where BASEPRI cannot reach them; mask by PRIMASK until then.
static volatile bool cs_by_priority = false;

extern "C" void hal_critical_section_enter(void)
{
  if (!cs_by_priority) {
    bool on = (__get_PRIMASK() == 0);
    __disable_irq();
    if (cs_saved) { return; }
    cs_saved_irq_on = on;
    cs_saved_basepri = __get_BASEPRI();
    cs_saved = true;
    return;
  }
  uint32_t bp = __get_BASEPRI();
  __set_BASEPRI_MAX(CS_BASEPRI);     // only ever raises the mask
  __ISB();
  if (cs_saved) { return; }          // nested: the outermost state is already kept
  cs_saved_basepri = bp;
  cs_saved_irq_on = false;
  cs_saved = true;
}

extern "C" void hal_critical_section_exit(void)
{
  uint32_t bp = cs_saved_basepri;
  bool irq_on = cs_saved_irq_on;
  cs_saved = false;
  __set_BASEPRI(bp);
  __ISB();
  if (irq_on) { __enable_irq(); }
}

// Keeps every interrupt but the three Grbl timers at OTHER_IRQ_PRIO or below,
// also ones a driver enables later at the reset priority of 0. Cheap; called
// from the serial pump a few times a second.
static void nvic_police(void)
{
  for (int irq = 0; irq < 150; irq++) {
    if (irq == STEPPER_TIMER_IRQ || irq == PULSE_TIMER_IRQ || irq == SCAN_TIMER_IRQ) { continue; }
    if (estop_isr_cb && (irq == (int)estop_irq)) { continue; }
    if (NVIC_GetPriority((IRQn_Type)irq) < OTHER_IRQ_PRIO) {
      NVIC_SetPriority((IRQn_Type)irq, OTHER_IRQ_PRIO);
    }
  }
}

static void cs_switch_to_priority(void) { cs_by_priority = true; }

extern "C" bool hal_in_critical_section(void)
{
  return cs_saved;
}


extern "C" void hal_housekeeping(void)
{
  static uint32_t last = 0;
  uint32_t now = DWT->CYCCNT;
  if ((uint32_t)(now - last) < 100000UL * cycles_per_us) { return; }   // every 100 ms
  last = now;
  nvic_police();
  (void)hal_millis();   // keeps the cycle count folded even when nobody asks
}


extern "C" uint32_t hal_irq_disable(void)
{
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  return primask;
}

extern "C" void hal_irq_restore(uint32_t primask)
{
  if (!primask) { __enable_irq(); }
}


#ifdef ACTIVITY_LEDS
// ---------------------------------------------------------------------------
// Activity LEDs on the board's RGB LED (active low): green = receive,
// blue = transmit, red = alarm.

#define ACTLED_ON_US     25000UL
#define ACTLED_GAP_US    25000UL

typedef struct {
  uint8_t           pin;
  volatile uint8_t  pending;
  uint8_t           lit;
  uint32_t          since;
} actled_t;

static actled_t actled_rx_state = { LED_GREEN_PIN, 0, 0, 0 };
static actled_t actled_tx_state = { LED_BLUE_PIN, 0, 0, 0 };

extern "C" void hal_actled_rx(void) { actled_rx_state.pending = 1; }
extern "C" void hal_actled_tx(void) { actled_tx_state.pending = 1; }

static void actled_step(actled_t *l, uint32_t now_us)
{
  if (l->lit) {
    if ((uint32_t)(now_us - l->since) >= ACTLED_ON_US) {
      hal_pin_write(l->pin, true);
      l->lit = 0;
      l->since = now_us;
    }
  } else if (l->pending && (uint32_t)(now_us - l->since) >= ACTLED_GAP_US) {
    l->pending = 0;
    hal_pin_write(l->pin, false);
    l->lit = 1;
    l->since = now_us;
  }
}

extern "C" void hal_actled_service(bool alarm)
{
  // Called on every serial pump, i.e. hundreds of thousands of times a
  // second; turned away on elapsed time almost always.
  static uint32_t last_check = 0;
  static uint32_t now_us = 0;     // microseconds, wraps after 71 minutes
  static bool red = false;
  uint32_t now = DWT->CYCCNT;
  uint32_t delta = now - last_check;
  if (delta < 1000UL * cycles_per_us) { return; }
  last_check = now;
  now_us += delta / cycles_per_us;
  actled_step(&actled_rx_state, now_us);
  actled_step(&actled_tx_state, now_us);
  if (alarm != red) { red = alarm; hal_pin_write(LED_RED_PIN, !alarm); }
}
#endif

#endif // CORE_CM7
