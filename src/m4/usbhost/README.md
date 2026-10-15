# usbhost - USB host for the USB-A port (M4 core)

The parts of [Arduino_USBHostMbed5](https://github.com/arduino-libraries/Arduino_USBHostMbed5)
0.3.1 (the ARM mbed USBHost library) that a USB stick needs: the host stack,
the hub and mass-storage classes and the STM32 target. MIT / Apache 2.0
licence, see LICENSE and the file headers.

Why a copy and not the library: the library picks the GIGA's USB-A port
(OTG_HS) only when compiled for the M7 (TARGET_GIGA). This port runs the USB
stick on the M4, whose build is GENERIC_STM32H747_M4; there the library would
fall back to OTG_FS - the USB-C port Grbl uses on the M7. The copy is
flattened into this directory, compiled for the M4 only, and selects the GIGA
target through USBHOST_GIGA (USBHostConf.h). Nothing else is changed.
