/*
  hal.h - Hardware abstraction layer for the STM32H747 Cortex-M7 (Arduino GIGA R1 WiFi)

  Part of Grbl-Giga, a port of Grbl 1.1 to the Arduino GIGA R1 WiFi.

  Grbl is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Grbl is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  -------------------------------------------------------------------------------

  This header is included by the plain-C Grbl core as well as by the C++ HAL
  implementation files, so everything here is C-linkage and it deliberately
  avoids the STM32 CMSIS / mbed headers: registers are reached through plain
  volatile pointers so the core never has to see them.

  The API is the one the UNO R4 and UNO Q ports expose, so the Grbl core above
  it stays close to theirs. What is particular to the GIGA:

  1) Grbl owns the Cortex-M7 at 480 MHz; the Cortex-M4 runs the display (see
     hal_shm.cpp). The Arduino core is mbed OS, whose microsecond ticker holds
     TIM5 on the M7 and TIM2 on the M4 - the two 32-bit timers. So step
     generation uses 16-bit basic timers: TIM7 for the stepper interrupt,
     TIM6 for the end of the step pulse, TIM17 for the input scan. All count
     at 240 MHz. Step periods longer than 65536 counts (273 us) are stretched
     over several timer cycles by the HAL, which the stepper code never sees.

  2) Port output uses the STM32 BSRR register. Bits 15:0 set pins, bits 31:16
     reset them, and one 32-bit store does both atomically with no
     read-modify-write.

  3) Limit, driver alarm, control and probe inputs are polled by a timer, not
     by EXTI interrupts, so every pin can carry every function.
*/

#ifndef hal_h
#define hal_h

#include <stdint.h>
#include <stdbool.h>

// The HAL has build options of its own in config.h (timing margins, LEDs,
// display). Without this include they would expand to nothing here and
// silently do nothing, since config.h otherwise only reaches the C core
// through grbl.h.
#include "../config.h"

#ifdef __cplusplus
extern "C" {
#endif

// Timer kernel clock. The GIGA's mbed startup runs the M7 at 480 MHz with
// HCLK = 240 MHz and both APB1/APB2 at 120 MHz; timers on a divided APB run at
// twice the bus clock, so every TIMx counts at 240 MHz. hal_init() checks this
// against the RCC and $I reports what it found (tim=).
#define HAL_TIMER_HZ            240000000UL
#define HAL_TICKS_PER_US        (HAL_TIMER_HZ / 1000000UL)

// Maximum number of distinct MCU ports a single logical pin group may span.
#define HAL_MAX_PORT_GROUPS     4
// Maximum number of pins in a logical group (four axes).
#define HAL_MAX_GROUP_PINS      4
#define HAL_OUT_LUT_SIZE        (1 << HAL_MAX_GROUP_PINS)


// ---------------------------------------------------------------------------
// Start-up
// ---------------------------------------------------------------------------

// Cycle counter, interrupt priorities, board LEDs. Called first in grbl_init().
void hal_init(void);
// What the timers really count at, read back from the RCC (APB1 and APB2 timers).
uint32_t hal_timer_clock_hz(void);
uint32_t hal_timer2_clock_hz(void);
// The display core's own status word (2 = running; bit 31 set = DMA2D error flags).
uint32_t hal_display_status(void);


// ---------------------------------------------------------------------------
// Fast grouped GPIO output
// ---------------------------------------------------------------------------

typedef struct {
  volatile uint32_t *bsrr;            // &GPIOx->BSRR
  uint16_t hw_mask;                   // hardware bits owned by this group
  uint16_t lut[HAL_OUT_LUT_SIZE];     // logical bit pattern -> hardware bits to SET
} hal_port_group_t;

typedef struct {
  uint8_t n_groups;
  hal_port_group_t group[HAL_MAX_PORT_GROUPS];
} hal_outset_t;

// Builds an output set from Arduino pin numbers. Bit i of the logical value
// passed to hal_outset_write() drives pins[i]. Configures the pins as outputs.
void hal_outset_init(hal_outset_t *set, const uint8_t *pins, uint8_t n_pins);

// Drives every pin of the set in one atomic store per port.
static inline void hal_outset_write(const hal_outset_t *set, uint8_t logical_bits)
{
  uint8_t i;
  for (i = 0; i < set->n_groups; i++) {
    const hal_port_group_t *g = &set->group[i];
    uint32_t hw_set = g->lut[logical_bits & (HAL_OUT_LUT_SIZE - 1)];
    // BS = bits to drive high, BR = bits to drive low. One store, no RMW.
    *(g->bsrr) = hw_set | ((uint32_t)(g->hw_mask & ~hw_set) << 16);
  }
}


// ---------------------------------------------------------------------------
// Grouped GPIO input
// ---------------------------------------------------------------------------

typedef struct {
  uint8_t n_pins;
  volatile const uint32_t *idr[HAL_MAX_GROUP_PINS];   // &GPIOx->IDR
  uint16_t bit[HAL_MAX_GROUP_PINS];                    // hardware bit within that port
} hal_inset_t;

// mode: one of HAL_PIN_INPUT / HAL_PIN_INPUT_PULLUP
#define HAL_PIN_INPUT           0
#define HAL_PIN_INPUT_PULLUP    1

void hal_inset_init(hal_inset_t *set, const uint8_t *pins, uint8_t n_pins, uint8_t mode);

// Returns the raw (non-inverted) logical bit pattern; bit i mirrors pins[i].
static inline uint8_t hal_inset_read(const hal_inset_t *set)
{
  uint8_t i, v = 0;
  for (i = 0; i < set->n_pins; i++) {
    if (*(set->idr[i]) & set->bit[i]) { v |= (1 << i); }
  }
  return v;
}


// ---------------------------------------------------------------------------
// Single pin helpers (low frequency use: enable, outputs, LEDs)
// ---------------------------------------------------------------------------

void hal_pin_output(uint8_t pin);
void hal_pin_input(uint8_t pin, uint8_t mode);
void hal_pin_write(uint8_t pin, bool high);   // one BSRR store; safe from interrupts
bool hal_pin_read(uint8_t pin);
// Where a pin is read: its port's input data register and its bit there.
volatile const uint32_t *hal_pin_idr(uint8_t pin);
uint16_t hal_pin_mask(uint8_t pin);
// Input with the pull-down on (test hook: reads like an optocoupler pulling low).
void hal_pin_pull_down(uint8_t pin);


// ---------------------------------------------------------------------------
// Timers
// ---------------------------------------------------------------------------

// Stepper driver interrupt (TIM7). Period is in HAL_TIMER_HZ ticks, any 32-bit value.
void hal_stepper_timer_init(void (*isr)(void));
void hal_stepper_timer_start(uint32_t period_ticks);
void hal_stepper_timer_stop(void);
// Called from inside the stepper ISR, right after the update event that
// started the period it applies to - the semantics of AVR Grbl's OCR1A write.
void hal_stepper_timer_set_period(uint32_t period_ticks);

// Step pulse termination timer (TIM6, one-pulse mode), armed by the stepper
// ISR and stopped by the hardware itself at the end of the pulse. Mirrors AVR
// Grbl's Timer0 usage.
void hal_pulse_timer_init(void (*isr)(void));
void hal_pulse_timer_arm(uint32_t ticks);
// Optional pre-step delay (STEP_PULSE_DELAY); fires isr_delay then isr.
void hal_pulse_timer_set_delay_isr(void (*isr_delay)(void));
void hal_pulse_timer_set_delay(uint32_t ticks);
// Pulse width, needed by the two-phase pulse timer when a delay is configured
// and for the shortest step period the HAL lets through.
void hal_pulse_timer_set_width(uint32_t ticks);
// One step with a delay before it: arms the delay phase once (the delay ISR
// raises the step pins, then the pulse width follows). For the direction
// setup time after a direction change ($56).
void hal_pulse_timer_arm_delayed(uint32_t delay_ticks);
// Lengthens the stepper period that is running now to at least min_ticks,
// for this one period only; the next one is back to what was set. Called from
// the stepper ISR after hal_stepper_timer_set_period() for that tick.
void hal_stepper_timer_stretch_once(uint32_t min_ticks);

// Input scan timer (TIM17) used to poll limits/driver alarms/control/probe.
void hal_scan_timer_init(void (*isr)(void), uint32_t hz);

// Central STOP: an EXTI interrupt on both edges of `pin` (lines 0-4, which
// have vectors of their own), at NVIC priority 0. False if not possible.
bool hal_estop_init(uint8_t pin, void (*isr)(void));

// Diagnostic: how often the pulse timer was re-armed while the previous pulse
// had not ended yet, i.e. a step pulse merged with the next one. With the
// period floor in place this should stay at zero.
extern volatile uint32_t hal_diag_pulse_overrun;
// Diagnostic: how often the stepper period had to be lifted to that floor.
extern volatile uint32_t hal_diag_period_floor;
// Diagnostic: worst stepper interrupt entry latency seen, in timer ticks, and
// how many entries were later than 8 us.
extern volatile uint32_t hal_diag_isr_lat_max;
extern volatile uint32_t hal_diag_isr_lat_over;
// Diagnostic: longest input scan and stepper interrupt, in core cycles, from
// entry to exit (preemption by higher priorities included).
extern volatile uint32_t hal_diag_scan_cycles_max;
extern volatile uint32_t hal_diag_stepper_cycles_max;
// Diagnostic: input scans performed, stepper interrupts and pulse
// terminations since power-up. While stepping the last two must agree.
extern volatile uint32_t hal_scan_ticks;
extern volatile uint32_t hal_diag_stepper_ticks;
// Stepper timer interrupts entered without an update (ignored; see hal_h7.cpp).
extern volatile uint32_t hal_diag_stepper_spurious;
extern volatile uint32_t hal_diag_pulse_ticks;


// ---------------------------------------------------------------------------
// Spindle PWM (TIM4_CH2 on D6) and analog speed output (DAC1 on A12)
// ---------------------------------------------------------------------------

// Returns the number of timer counts in one PWM period, i.e. the maximum duty
// value. Zero if the pin could not be configured for PWM. mode HAL_PWM_OFF
// leaves the pin a plain output (the period is returned all the same: the
// DAC's level is computed from the duty), HAL_PWM_INVERTED is active low.
#define HAL_PWM_OFF       0
#define HAL_PWM_ON        1
#define HAL_PWM_INVERTED  2
uint32_t hal_spindle_pwm_init(uint8_t pin, uint32_t frequency_hz, uint8_t mode);
void hal_spindle_pwm_set(uint32_t duty_counts);
void hal_spindle_pwm_off(void);
void hal_spindle_dac_init(uint8_t pin);
// An analog input (the operator panel's potentiometers): 0-4095, ratiometric
// to the 3.3 V reference. Main loop only (it blocks for a conversion).
uint16_t hal_analog_read(uint8_t pin);
void hal_spindle_dac_set(uint32_t value_12bit);
// Read back from the hardware, for $I: PWM duty in 0.1 %, DAC code 0-4095.
uint32_t hal_spindle_pwm_readback(void);
uint32_t hal_spindle_dac_readback(void);


// ---------------------------------------------------------------------------
// Handwheel: TIM8 in encoder mode on D54 (PI5, CH1) and D59 (PI6, CH2)
// ---------------------------------------------------------------------------

// Pins to TIM8 (alternate function 3, pull-ups on), counting every edge of
// both inputs through the input filter (~4 us).
void hal_mpg_init(uint8_t pin_a, uint8_t pin_b);
uint16_t hal_mpg_count(void);
// Test hook: the pulls of the two pins (true = up), leaving them on TIM8.
void hal_mpg_test_levels(uint8_t pin_a, uint8_t pin_b, bool a_high, bool b_high);


// ---------------------------------------------------------------------------
// RS485 to the spindle VFD: USART2 on D18 (TX, PD5) and D19 (RX, PD6), both
// alternate function 7, and the transceiver's driver enable on a GPIO
// ---------------------------------------------------------------------------
//
// Half duplex, whole frames. hal_rs485_send() raises the driver enable,
// switches the receiver off (no echo, whatever the transceiver does with its
// ~RE) and hands the frame to the interrupt, which drops the enable again
// once the last stop bit is out (TC). What comes back is collected by the
// interrupt; the USART's receiver timeout marks the end of a frame after
// 3.5 characters of silence (1.75 ms above 19200 baud), as Modbus RTU frames
// it.

// format: 0 8N1, 1 8N2, 2 8E1, 3 8O1, 4 8E2, 5 8O2 (Delta's 09-04 = 12 + format).
#define HAL_RS485_FORMATS       6
#define HAL_RS485_FRAME_MAX     64

// False if the kernel clock cannot make the baud rate. Repeat calls with the
// same parameters touch nothing.
bool hal_rs485_init(uint32_t baud, uint8_t format, uint8_t de_pin);
// USART2 off, its interrupt off, the driver enable low.
void hal_rs485_stop(void);
// Starts a frame (at most HAL_RS485_FRAME_MAX bytes) out. Drops anything
// received so far.
void hal_rs485_send(const uint8_t *frame, uint8_t len);
// True until the last stop bit of the frame has left and the enable is low.
bool hal_rs485_sending(void);
// A complete received frame: its length (copied to buf), 0 if none yet, -1 if
// it was broken (parity, framing, noise, overrun, longer than the buffer).
int hal_rs485_frame(uint8_t *buf, uint8_t max);
// Microseconds the line has been quiet (since the last byte either way),
// saturating at 1 s.
uint32_t hal_rs485_quiet_us(void);
// USART2 kernel clock found by hal_rs485_init(), Hz (0: not running).
uint32_t hal_rs485_clock_hz(void);


// ---------------------------------------------------------------------------
// Non-volatile storage (internal flash, bank 2)
// ---------------------------------------------------------------------------

// Loads the shadow image from flash. Returns false if no valid image was
// found, in which case the shadow is filled with 0xFF and Grbl will restore
// its defaults exactly as it would on a blank AVR EEPROM.
bool hal_nvs_init(void);
// False if the settings flash cannot be used (sketch too large); settings
// then live in RAM only.
bool hal_nvs_ok(void);
uint8_t hal_nvs_read(uint16_t addr);
void hal_nvs_write(uint16_t addr, uint8_t value);
bool hal_nvs_dirty(void);
// Appends the shadow to the flash log; occasionally erases a sector first,
// which blocks for up to a couple of seconds. Only called while idle.
bool hal_nvs_commit(void);


// ---------------------------------------------------------------------------
// Serial (native USB CDC)
// ---------------------------------------------------------------------------

void hal_serial_init(uint32_t baud);

// Free space in the transmit ring. Callers must check this before writing;
// hal_serial_write_byte() drops the byte rather than blocking, because a
// blocking write would stall protocol_execute_realtime() and with it feed hold,
// reset and status reporting.
uint16_t hal_serial_tx_free(void);
void hal_serial_write_byte(uint8_t b);

// Pushes queued output into the USB endpoint and collects input the receive
// callback could not deliver. Called from protocol_execute_realtime().
void hal_serial_pump(void);

// Pumps until the transmit ring has drained, or a host is not listening.
void hal_serial_flush(void);
bool hal_serial_connected(void);

// Provided by serial.c. Called once per received character.
void grbl_serial_rx_char(uint8_t c);

// Diagnostic: output bytes discarded because nobody was reading the port.
uint32_t hal_serial_tx_stall_count(void);

// True once each time a host opens the port (DTR rises).
bool hal_serial_connect_event(void);


// ---------------------------------------------------------------------------
// The display co-processor (Cortex-M4) and the shared memory block
// ---------------------------------------------------------------------------

// Maps the shared block and clears it (and, once per board, turns off the
// M4's factory autoboot, which costs one reset).
void hal_display_init(void);
// Starts the M4 if its flash area holds a program. Called once all Grbl pins
// are configured.
void hal_display_start(void);
// True while the M4 is running the display program (its heartbeat is fresh).
bool hal_display_present(void);
// Frames the M4 has drawn since it started.
uint32_t hal_display_frames(void);
// Next real-time command byte from the touch screen, or -1.
int hal_display_cmd_read(void);
// The panel's framebuffer as the M4 left it (cache invalidated), or 0 when
// the display is not running.
const uint8_t *hal_display_framebuffer(void);
// Orders memory accesses around the shared block.
void hal_memory_barrier(void);


// ---------------------------------------------------------------------------
// Misc
// ---------------------------------------------------------------------------

// Busy-wait delays on the cycle counter: Grbl calls them from interrupt
// context (st_go_idle() via mc_reset()), where nothing interrupt-driven runs.
void hal_delay_ms(uint32_t ms);
void hal_delay_us(uint32_t us);
uint32_t hal_millis(void);
// The core's cycle counter (DWT CYCCNT, 480 MHz, wraps every 8.9 s) for
// timing measurements; and cycles per microsecond.
static inline uint32_t hal_cycles(void) { return *(volatile uint32_t *)0xE0001004UL; }
uint32_t hal_cycles_per_us(void);

// Transmit / receive / alarm indication on the board's RGB LED.
// hal_actled_rx()/tx() only set a flag, so they are safe from an interrupt;
// hal_actled_service() does the timing and the pin writes.
#ifdef ACTIVITY_LEDS
  void hal_actled_rx(void);
  void hal_actled_tx(void);
  void hal_actled_service(bool alarm);
#else
  #define hal_actled_rx()       do { } while (0)
  #define hal_actled_tx()       do { } while (0)
  #define hal_actled_service(a) do { } while (0)
#endif

// Periodic upkeep (interrupt priorities); called from the serial pump.
void hal_housekeeping(void);

// Critical sections. Nestable, mirrors AVR Grbl's `sreg = SREG; cli(); ... SREG = sreg;`
uint32_t hal_irq_disable(void);
void hal_irq_restore(uint32_t primask);
// The same, inlined, for the stepper interrupt.
static inline uint32_t hal_irq_save_fast(void)
{
  uint32_t pm;
  __asm volatile ("mrs %0, primask" : "=r" (pm) :: "memory");
  __asm volatile ("cpsid i" ::: "memory");
  return pm;
}
static inline void hal_irq_restore_fast(uint32_t pm)
{
  __asm volatile ("msr primask, %0" :: "r" (pm) : "memory");
}

#ifdef __cplusplus
}
#endif

#endif
