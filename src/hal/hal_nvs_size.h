/*
  hal_nvs_size.h - size of the emulated EEPROM

  Shared by the C core (settings.c, eeprom.c) and the C++ Data Flash back end so
  the two can never disagree about the image size. 1024 bytes matches the AVR
  ATmega328P EEPROM that Grbl's address map was laid out for:

    0x000  settings version byte
    0x001  global settings block
    0x200  coordinate system offsets
    0x300  startup blocks
    0x3AE  build info
*/

#ifndef hal_nvs_size_h
#define hal_nvs_size_h

#define GRBL_NVS_SIZE   1024

#endif
