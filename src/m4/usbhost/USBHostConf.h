// Grbl-Giga: copied from Arduino_USBHostMbed5 0.3.1 (src/USBHost/USBHostConf.h), the ARM mbed
// USBHost library ported by Arduino; licence as below. Changes: flattened into
// one directory (include paths), compiled for the M4 core only, and the GIGA's
// USB-A port (OTG_HS) selected also in the M4 build (USBHOST_GIGA). See
// README.md in this directory.

/* mbed USBHost Library
 * Copyright (c) 2006-2013 ARM Limited
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef USBHOST_CONF_H
#define USBHOST_CONF_H

// The GIGA's USB-A port is OTG_HS with its embedded full-speed PHY (PB14/PB15),
// VBUS switched by PA15. The M7 build knows the board as TARGET_GIGA; the M4
// build is GENERIC_STM32H747_M4 with GIGA_PINS - without this the library
// would take OTG_FS, which is the USB-C port the M7 talks to the PC on.
#if defined(TARGET_GIGA) || defined(GIGA_PINS)
#define USBHOST_GIGA 1
#endif

#include "mbed_config.h"
#include "Callback.h"
#include "Arduino.h"

#if defined(TARGET_STM)
/*
* Maximum number of devices that can be connected
* to the usb host
*/
/*   hub + 2 devices */
#define MAX_DEVICE_CONNECTED        3   // Grbl-Giga: a stick, perhaps behind one hub

/*
* Maximum of Hub connected to the usb host
*/
#define MAX_HUB_NB                  1

/*
* Maximum number of ports on a USB hub
*/
#define MAX_HUB_PORT                4

/*
* Enable USBHostMSD
*/
#define USBHOST_MSD                 1

/*
* Enable USBHostKeyboard
*/
#define USBHOST_KEYBOARD        0   // Grbl-Giga: not copied

/*
* Enable USBHostMouse
*/
#define USBHOST_MOUSE           0   // Grbl-Giga: not copied

/*
* Enable USBHostSerial or USBHostMultiSerial (if set > 1)
*/
#define USBHOST_SERIAL          0   // Grbl-Giga: not copied

/*
* Enable USB3Gmodule
*/
#define USBHOST_3GMODULE        0   // Grbl-Giga: not copied

/*
* Enable USB MIDI
*/
#define USBHOST_MIDI            0   // Grbl-Giga: not copied

/*
* Maximum number of interfaces of a usb device
*/
#define MAX_INTF                    2

/*
* Maximum number of endpoints on each interface
*/
#define MAX_ENDPOINT_PER_INTERFACE  2

/*
* Maximum number of endpoint descriptors that can be allocated
*/
#define MAX_ENDPOINT               11 /*  USB FS 11 channel */

#else
/*
* Maximum number of devices that can be connected
* to the usb host
*/
#define MAX_DEVICE_CONNECTED        5

/*
* Maximum of Hub connected to the usb host
*/
#define MAX_HUB_NB                  2

/*
* Maximum number of ports on a USB hub
*/
#define MAX_HUB_PORT                4

/*
* Enable USBHostMSD
*/
#define USBHOST_MSD                 1

/*
* Enable USBHostKeyboard
*/
#define USBHOST_KEYBOARD            1

/*
* Enable USBHostMouse
*/
#define USBHOST_MOUSE               1

/*
* Enable USBHostSerial or USBHostMultiSerial (if set > 1)
*/
#define USBHOST_SERIAL              1

/*
* Enable USB3Gmodule
*/
#define USBHOST_3GMODULE            1

/*
* Enable USB MIDI
*/
#define USBHOST_MIDI                1 

/*
* Maximum number of interfaces of a usb device
*/
#define MAX_INTF                    4

/*
* Maximum number of endpoints on each interface
*/
#define MAX_ENDPOINT_PER_INTERFACE  3

/*
* Maximum number of endpoint descriptors that can be allocated
*/
#define MAX_ENDPOINT                (MAX_DEVICE_CONNECTED * MAX_INTF * MAX_ENDPOINT_PER_INTERFACE)
#endif
/*
* Maximum number of transfer descriptors that can be allocated
*/
#define MAX_TD                      (MAX_ENDPOINT*2)

/*
* usb_thread stack size
*/
#define USB_THREAD_STACK            (256*4 + 2*256*4)

#endif
