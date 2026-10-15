/*
  kinematics.c - selectable machine kinematics (Cartesian / CoreXY)

  Part of Grbl-Giga, a port of Grbl 1.1 to the Arduino GIGA R1 WiFi (from the Grbl-R4 port).

  Grbl is free software: you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation, either version 3 of the License, or (at your option) any later
  version. Distributed WITHOUT ANY WARRANTY; see the GNU GPL for details.
*/

#include "grbl.h"


// X = (A + B) / 2
int32_t kinematics_motors_to_x_steps(int32_t *steps)
{
  return ((steps[A_MOTOR] + steps[B_MOTOR]) / 2);
}


// Y = (A - B) / 2
int32_t kinematics_motors_to_y_steps(int32_t *steps)
{
  return ((steps[A_MOTOR] - steps[B_MOTOR]) / 2);
}
