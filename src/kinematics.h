/*
  kinematics.h - selectable machine kinematics (Cartesian / CoreXY)

  Part of Grbl-Giga, a port of Grbl 1.1 to the Arduino GIGA R1 WiFi (from the Grbl-R4 port).

  Grbl is free software: you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation, either version 3 of the License, or (at your option) any later
  version. Distributed WITHOUT ANY WARRANTY; see the GNU GPL for details.

  -------------------------------------------------------------------------------

  Upstream Grbl selects CoreXY with a compile-time #define, so switching a
  machine between Cartesian and CoreXY means rebuilding and reflashing. The
  STM32H747 has the flash and the cycles to carry both, so here it is a stored
  setting instead:

      $40=0   Cartesian (default)
      $40=1   CoreXY

  The change takes effect immediately and survives power cycles. Homing is the
  one thing that still needs attention from the user: CoreXY cannot home X and
  Y in the same cycle, so $40=1 requires HOMING_CYCLE_0 and HOMING_CYCLE_1 in
  config.h to home X and Y separately. settings.c reports an error if $40=1 is
  set while the build homes both axes at once.

  CoreXY belt geometry, with motor A on the X driver and motor B on the Y driver:

      A = X + Y        X = (A + B) / 2
      B = X - Y        Y = (A - B) / 2

  Moving both motors the same way translates in X; moving them oppositely
  translates in Y.
*/

#ifndef kinematics_h
#define kinematics_h

#define KINEMATICS_CARTESIAN  0
#define KINEMATICS_COREXY     1

// CoreXY motor assignments. DO NOT ALTER: the equations above depend on them.
#define A_MOTOR X_AXIS
#define B_MOTOR Y_AXIS

// True when the machine is configured for CoreXY belt geometry.
#define kinematics_is_corexy()  (settings.kinematics == KINEMATICS_COREXY)

// What the fourth motor is ($42). As an independent axis A is either rotary -
// degrees, never scaled by G20 - or linear like X/Y/Z. As a slave it has no
// coordinate of its own: the stepper ISR copies the master axis's step and
// direction onto the A driver (with A's own $2/$3 invert bits), and homing
// squares the pair using the A limit input as the second switch.
#define A_MODE_ROTARY   0
#define A_MODE_LINEAR   1
#define A_MODE_SLAVE_X  2
#define A_MODE_SLAVE_Y  3

#define axis_a_is_slave()     (settings.a_mode >= A_MODE_SLAVE_X)
#define axis_a_master()       ((settings.a_mode == A_MODE_SLAVE_Y) ? Y_AXIS : X_AXIS)
#define axis_is_rotary(idx)   (((idx) == A_AXIS) && (settings.a_mode == A_MODE_ROTARY))

// Convert CoreXY motor step counts to Cartesian axis step counts.
int32_t kinematics_motors_to_x_steps(int32_t *steps);
int32_t kinematics_motors_to_y_steps(int32_t *steps);

#endif
