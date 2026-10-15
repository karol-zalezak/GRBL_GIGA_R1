/*
  cpu_map.h - pin mapping for the Arduino GIGA R1 WiFi (STM32H747, Cortex-M7 core)

  Part of Grbl-Giga, a port of Grbl 1.1 to the Arduino GIGA R1 WiFi.

  Grbl is free software: you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation, either version 3 of the License, or (at your option) any later
  version. Distributed WITHOUT ANY WARRANTY; see the GNU GPL for details.

  -------------------------------------------------------------------------------

  The approved pin table (PINOUT.md, v2). Each block of the peripheral shield
  sits on one stretch of the GIGA's headers, in the order of its terminals:

      2x18 header, outer (odd) column          2x18 header, inner (even) column
      D23  spare                               D22  IN1   (X- limit)
      D25  X step     PJ0                      D24  IN2   (X+ limit)
      D27  X dir      PJ1                      D26  IN3   (Y- limit)
      D29  Y step     PJ2                      D28  IN4   (Y+ limit)
      D31  Y dir      PJ3                      D30  IN5   (Z- limit)
      D33  Z step     PJ4                      D32  IN6   (Z+ limit)
      D35  Z dir      PJ5                      D34  IN7   (A- limit)
      D37  A step     PJ6                      D36  IN8   (A+ limit)
      D39  A dir      PI14                     D38  IN9   (X servo alarm)
      D41  enable                              D40  IN10  (Y servo alarm)
      D43  IN17 (door)                         D42  IN11  (Z servo alarm)
      D45  IN18 (spindle at speed)             D44  IN12  (A servo alarm)
      D47  IN19 (spindle fault)                D46  IN13  (probe)
      D49  STOP  PE4 / EXTI4 (fixed)           D48  IN14  (reset button)
      D51  IN20 (free)                         D50  IN15  (hold button)
      D53  spare                               D52  IN16  (start button)

      top header, left to right
      D13 OUT1  D12 OUT2  D11 OUT3  D10 OUT4  D9 OUT5  D8 OUT6
      D7 spare (input only)  D6 spindle PWM
      D5 OUT7  D4 OUT8  D3 OUT9  D2 OUT10   D14 OUT11  D15 OUT12  D16 BRAKE (fixed)

      A12  DAC0, spindle speed 0-3.3 V (to the isolated 0-10 V stage)

  IN1-IN20 and OUT1-OUT12 are universal channels: what each one does is a
  setting ($60-$93, see io.h); the functions in brackets are the defaults.
  Fixed are only the step/direction/enable signals, STOP, BRAKE, PWM and DAC.

  The four step pins share GPIO port J (PJ0/2/4/6), so a step is a single
  32-bit store; three of the four direction pins are on port J too.

  The GIGA is a 3.3 V board. The shield puts 24 V signals behind optocouplers:
  an input channel is active when its optocoupler conducts and pulls the pin
  (internal pull-up) low; NC/NO per channel is $79. Outputs drive the LEDs of
  the output optocouplers; no output pin is a JTAG/SWD or boot pin, and PB4
  (D7, pulled up by JTAG after reset) is not used as an output.
*/

#ifndef cpu_map_h
#define cpu_map_h

#ifdef CPU_MAP_GIGA_R1

  // Step pulse outputs.
  #define X_STEP_PIN            25
  #define Y_STEP_PIN            29
  #define Z_STEP_PIN            33
  #define A_STEP_PIN            37

  // Step direction outputs.
  #define X_DIRECTION_PIN       27
  #define Y_DIRECTION_PIN       31
  #define Z_DIRECTION_PIN       35
  #define A_DIRECTION_PIN       39

  // Servo / stepper driver enable (active low on common drivers; invert with $4).
  #define STEPPERS_DISABLE_PIN  41

  // Universal input channels IN1-IN20, in channel order.
  #define IN_CHANNEL_PINS  { 22, 24, 26, 28, 30, 32, 34, 36, 38, 40, \
                             42, 44, 46, 48, 50, 52, 43, 45, 47, 51 }

  // Universal output channels OUT1-OUT12, in channel order.
  #define OUT_CHANNEL_PINS { 13, 12, 11, 10, 9, 8, 5, 4, 3, 2, 14, 15 }

  // Central STOP: signal from the safety relay. Fixed pin with its own EXTI
  // line (PE4 -> EXTI4, a dedicated interrupt vector nothing else uses).
  #define ESTOP_PIN             49

  // Driver power: supply voltage of the drives through an optocoupler of its
  // own (low = supply present). Fixed pin, read by the input scan ($94).
  #define DRVPWR_PIN            53

  // Handwheel pendant (mpg.h) on the camera pins, which the Display Shield
  // passes through its socket J3. A/B must stay on PI5/PI6: TIM8_CH1/CH2.
  #define MPG_A_PIN             54    // PI5
  #define MPG_B_PIN             59    // PI6
  #define MPG_SELECT_PINS       { 55, 56, 57, 58, 60, 61, 62 }   // X Y Z 4, x1 x10 x100
  #define MPG_LED_PIN           63    // PH11

  // Z axis brake: fixed power output (5 A MOSFET on the shield). On = coil
  // energised = brake released.
  #define BRAKE_PIN             16

  // Spindle speed: PWM carrier on D6 and an analog level on DAC0 (A12).
  #define SPINDLE_PWM_PIN       6
  #define SPINDLE_DAC_PIN       84    // A12 / DAC0, PA4. Comment out to leave the DAC alone.

  // Board RGB LED (active low): green = receive, blue = transmit, red = alarm.
  #define LED_RED_PIN           86
  #define LED_GREEN_PIN         87
  #define LED_BLUE_PIN          88

  // Bit positions inside the logical masks Grbl passes around internally. These
  // are axis indices, not hardware bits; the HAL maps them to real pins.
  #define X_STEP_BIT            X_AXIS
  #define Y_STEP_BIT            Y_AXIS
  #define Z_STEP_BIT            Z_AXIS
  #define A_STEP_BIT            A_AXIS
  #define X_DIRECTION_BIT       X_AXIS
  #define Y_DIRECTION_BIT       Y_AXIS
  #define Z_DIRECTION_BIT       Z_AXIS
  #define A_DIRECTION_BIT       A_AXIS

  #define X_LIMIT_BIT           X_AXIS
  #define Y_LIMIT_BIT           Y_AXIS
  #define Z_LIMIT_BIT           Z_AXIS
  #define A_LIMIT_BIT           A_AXIS
  #define LIMIT_MASK            ((1<<X_LIMIT_BIT)|(1<<Y_LIMIT_BIT)|(1<<Z_LIMIT_BIT)|(1<<A_LIMIT_BIT))
  // Logical mask covering every axis, used where AVR Grbl used the hardware
  // STEP_MASK to test whether any homing axis is still unlocked.
  #define AXIS_STEP_MASK        ((1<<X_AXIS)|(1<<Y_AXIS)|(1<<Z_AXIS)|(1<<A_AXIS))
  #define MOTOR_ALARM_MASK      AXIS_STEP_MASK

  #define CONTROL_RESET_BIT         0
  #define CONTROL_FEED_HOLD_BIT     1
  #define CONTROL_CYCLE_START_BIT   2
  #define CONTROL_SAFETY_DOOR_BIT   3
  #define CONTROL_MASK          ((1<<CONTROL_RESET_BIT)|(1<<CONTROL_FEED_HOLD_BIT)| \
                                 (1<<CONTROL_CYCLE_START_BIT)|(1<<CONTROL_SAFETY_DOOR_BIT))

  #define PROBE_MASK            1

  // Spindle PWM. TIM4 counts at 240 MHz behind a prescaler chosen to fit its
  // 16-bit range, so the duty resolution at the 1 kHz default is 60000 counts
  // (240 MHz / 4 / 1 kHz), against the 8 bits AVR Grbl had.
  // SPINDLE_PWM_MAX_VALUE is filled in at run time by spindle_init() from what
  // the timer actually achieved.
  #ifndef SPINDLE_PWM_FREQUENCY
    #define SPINDLE_PWM_FREQUENCY 1000
  #endif
  // Duty floor for a non-zero commanded RPM, in raw timer counts. One count is
  // effectively "as low as the hardware goes"; raise it if a VFD or laser
  // driver needs a minimum on-time to respond.
  #ifndef SPINDLE_PWM_MIN_VALUE
    #define SPINDLE_PWM_MIN_VALUE 1
  #endif

#endif

#endif
