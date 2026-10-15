# FatFs R0.15 (w/patch1) for the USB stick

ChaN's FatFs, <http://elm-chan.org/fsw/ff/>, copied from the Arduino
renesas_uno 1.6.0 core (`libraries/FATFilesystem`). Arduino's one change
there is kept: the directory object is `FATFS_DIR`, not `DIR`, which POSIX
(and mbed) already has.

The mbed core of the GIGA has FatFs R0.13 built into `libmbed.a`, configured
without exFAT and without GPT partition tables. Many USB sticks come with
one or the other, so the M4 uses this copy instead, directly through the f_*
calls (`src/m4/usbstick.cpp` has the disk glue). mbed's FATFileSystem is not
used (it is linked all the same, see below).

Changes:

- `ff.h`: its global names (f_*, ff_*, disk_*, get_fattime) renamed to
  gff_* by macros. The core links `libmbed.a` whole (`--whole-archive`),
  mbed's FatFs included, so the same names would clash.
- `ff.c`, `ffunicode.c`: wrapped in `#if defined(CORE_CM4)`. The sketch's
  `src/` is compiled for both cores, and only the M4 has the stick.
- `ffconf.h`: exFAT and 64-bit LBA (GPT) on, LFN with UTF-8 names, code
  page 852 (Central European) for 8.3 names, one volume, 512-byte sectors,
  f_utime() and f_mkfs() on, no relative paths; FF_MULTI_PARTITION on, only
  so that f_mkfs() can format a partition the M4 has laid out itself (see
  format_stick() in usbstick.cpp - FatFs's own MBR has no disk signature,
  and Windows does not mount a stick without one).

License (FatFs):

    Copyright (C) 2022, ChaN, all right reserved.

    FatFs module is an open source software. Redistribution and use of FatFs
    in source and binary forms, with or without modification, are permitted
    provided that the following condition is met:

    1. Redistributions of source code must retain the above copyright notice,
       this condition and the following disclaimer.

    This software is provided by the copyright holder and contributors "AS
    IS" and any warranties related to this software are DISCLAIMED. The
    copyright owner or contributors be NOT LIABLE for any damages caused by
    use of this software.
