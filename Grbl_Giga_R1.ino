/*
  Grbl_Giga_R1.ino - Arduino sketch entry point for Grbl-Giga

  A port of Grbl 1.1h to the Arduino GIGA R1 WiFi (STM32H747): Grbl runs on
  the Cortex-M7 at 480 MHz, the GIGA Display Shield is driven by the
  Cortex-M4 co-processor.

  Grbl is free software: you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation, either version 3 of the License, or (at your option) any later
  version. Distributed WITHOUT ANY WARRANTY; see the GNU GPL for details.

  -------------------------------------------------------------------------------

  This one sketch is uploaded twice, once per core (Tools > Target core), both
  times with Tools > Flash split = "1.5MB M7 + 0.5MB M4":

    Main Core (M7)       Grbl: motion, USB serial to the sender, settings
    M4 Co-processor      the status display on the GIGA Display Shield

  Everything of substance lives in src/. Each core compiles all of it; the
  files for the other core are fenced off with CORE_CM7 / CORE_CM4, and the
  Grbl core itself is plain C that the M4 build compiles but never calls, so
  the linker drops it. The two cores talk only through the shared memory
  block described in src/grbl_shm.h.

  The Arduino core owns main(): on the M7, setup() runs Grbl's one-time
  initialisation and loop() runs one pass of what was the outer for(;;) in
  upstream main.c, which is also where Grbl lands again after a system abort.
*/

#if defined(CORE_CM7)

extern "C" void grbl_init(void);
extern "C" void grbl_loop(void);

void setup()
{
  grbl_init();
}

void loop()
{
  grbl_loop();
}

#elif defined(CORE_CM4)

void display_m4_setup(void);
void display_m4_loop(void);

void setup()
{
  display_m4_setup();
}

void loop()
{
  display_m4_loop();
}

#endif
