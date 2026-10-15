/*
  hal_serial.cpp - PC link over the GIGA's native USB (mbed USBCDC)

  Part of Grbl-Giga, a port of Grbl 1.1 to the Arduino GIGA R1 WiFi.

  Grbl is free software: you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation, either version 3 of the License, or (at your option) any later
  version. Distributed WITHOUT ANY WARRANTY; see the GNU GPL for details.

  -------------------------------------------------------------------------------

  The USB-C socket is the STM32H747's own full-speed USB, run by mbed's USB
  device stack behind the core's `Serial` (_SerialUSB). Two properties of that
  stack shape this file:

  - Receive: when an OUT packet arrives, the USB interrupt calls the callbacks
    attached to _SerialUSB. The core's own callback moves the packet into a
    256-byte ring; ours runs right after it and hands every byte to
    grbl_serial_rx_char(), so `!`, `~`, `?` and Ctrl-X act from interrupt
    context exactly as on AVR. Bytes stay in the core's ring (and, behind it,
    in the endpoint, which then NAKs the host) only while Grbl's own receive
    buffer is full; hal_serial_pump() picks them up as soon as there is room.
    The USB stack runs its event handling inside an mbed critical section,
    which is also what makes the two callers of drain() mutually exclusive
    (see hal_h7.cpp for why that section no longer masks Grbl's timers).

  - Transmit: _SerialUSB.write() blocks until the host has collected each
    packet. Grbl must never block on output, so it writes into a ring here and
    hal_serial_pump() hands the endpoint one packet at a time with send_nb(),
    which only copies into the stack's buffer and starts the transfer. Packets
    are kept at 63 bytes: a full 64-byte packet does not end a USB transfer,
    and the host would sit on the data until the next one arrived.

  Opening the port at 1200 baud still resets the board into the bootloader
  for uploads; that is the core's thread, untouched by this file.
*/

#if defined(CORE_CM7)

#include <Arduino.h>
#include "USB/PluggableUSBSerial.h"
#include "stm32h7xx.h"

#include "hal.h"

extern "C" uint16_t serial_get_rx_buffer_available(void);

#define TX_RING_SIZE      8192            // power of two
#define TX_PACKET         63              // see above
#define TX_STALL_MS       3000            // connected but not reading: give up after this

static uint8_t tx_ring[TX_RING_SIZE];
static uint32_t tx_head = 0;              // written by serial_write (main loop)
static uint32_t tx_tail = 0;              // advanced by the pump (main loop)
static uint32_t tx_discarded = 0;
static uint32_t tx_blocked_since = 0;

static bool was_connected = false;
static volatile bool connect_event = false;
static uint32_t pump_last = 0;


// Moves bytes from the core's receive ring to Grbl's while Grbl has room.
// Runs inside an mbed critical section: from the USB callback (the stack
// holds one) and from the pump (which takes one itself).
static void drain(void)
{
  bool any = false;
  while (serial_get_rx_buffer_available() > 0) {
    int c = _SerialUSB.read();
    if (c < 0) { break; }
    grbl_serial_rx_char((uint8_t)c);
    any = true;
  }
  if (any) { hal_actled_rx(); }
}

static void usb_rx_callback(void) { drain(); }


extern "C" void hal_serial_init(uint32_t baud)
{
  (void)baud;   // native USB: the rate the host selects is irrelevant
  // main() has already run _SerialUSB.begin(), which attached the core's own
  // receive callback and the 1200 baud bootloader trigger. Ours goes after it,
  // once: the core keeps at most four callbacks and never removes one.
  static bool attached = false;
  if (attached) { return; }
  attached = true;
  _SerialUSB.attach(usb_rx_callback);
}


extern "C" bool hal_serial_connected(void)
{
  return _SerialUSB.connected();
}


static uint32_t tx_used(void) { return tx_head - tx_tail; }

// Hands the next packet to the endpoint if it is idle.
static void tx_kick(void)
{
  uint32_t used = tx_used();
  if (used == 0) { return; }
  uint8_t pkt[TX_PACKET];
  uint32_t n = (used > TX_PACKET) ? TX_PACKET : used;
  for (uint32_t i = 0; i < n; i++) { pkt[i] = tx_ring[(tx_tail + i) & (TX_RING_SIZE - 1)]; }
  uint32_t actual = 0;
  _SerialUSB.send_nb(pkt, n, &actual, true);
  if (actual) {
    tx_tail += actual;
    tx_blocked_since = 0;
    hal_actled_tx();
  }
}


extern "C" void hal_serial_pump(void)
{
  // The pump is called from every one of Grbl's wait loops, hundreds of
  // thousands of times a second. Each USB call masks interrupts briefly, so
  // do the work at most every 20 us.
  uint32_t now = DWT->CYCCNT;
  if ((uint32_t)(now - pump_last) < 20UL * (SystemCoreClock / 1000000UL)) { return; }
  pump_last = now;

  bool conn = _SerialUSB.connected();
  if (conn && !was_connected) { connect_event = true; }
  if (!conn) {
    // Nobody has the port open: output goes nowhere, so do not let it pile up
    // and stall Grbl.
    tx_discarded += tx_used();
    tx_tail = tx_head;
  }
  was_connected = conn;

  if (conn) { tx_kick(); }

  // Input that the receive callback had to leave behind for want of room.
  // An mbed critical section keeps the USB interrupt (and its callback, the
  // other caller of drain()) out, without holding off Grbl's own timers.
  core_util_critical_section_enter();
  if (serial_get_rx_buffer_available() > 0) { drain(); }
  core_util_critical_section_exit();

  // A host that has the port open but stopped reading would otherwise block
  // Grbl in serial_write() forever. The timeout is long on purpose: a host
  // that is reading never stalls for seconds, and on the UNO R4 a 250 ms
  // value threw away output during ordinary PC scheduling hiccups.
  if (tx_used() >= TX_RING_SIZE - 1) {
    uint32_t ms = hal_millis();
    if (tx_blocked_since == 0) { tx_blocked_since = ms ? ms : 1; }
    else if ((uint32_t)(ms - tx_blocked_since) > TX_STALL_MS) {
      tx_discarded += tx_used();
      tx_tail = tx_head;
      tx_blocked_since = 0;
    }
  }
}


extern "C" uint16_t hal_serial_tx_free(void)
{
  uint32_t used = tx_used();
  uint32_t free_bytes = TX_RING_SIZE - 1 - used;
  return (free_bytes > 0xFFFF) ? 0xFFFF : (uint16_t)free_bytes;
}


extern "C" void hal_serial_write_byte(uint8_t b)
{
  if (tx_used() >= TX_RING_SIZE - 1) { return; }   // caller checked tx_free
  tx_ring[tx_head & (TX_RING_SIZE - 1)] = b;
  tx_head++;
}


extern "C" void hal_serial_flush(void)
{
  uint32_t start = hal_millis();
  while (tx_used() && _SerialUSB.connected()) {
    pump_last = DWT->CYCCNT - 0x80000000UL;   // defeat the pump's rate limit
    hal_serial_pump();
    if ((uint32_t)(hal_millis() - start) > 500) { break; }
  }
}


// True once after a host opened the port. serial.c answers with the welcome
// banner, which AVR boards print because opening the port resets them.
extern "C" bool hal_serial_connect_event(void)
{
  if (!connect_event) { return false; }
  connect_event = false;
  return true;
}


extern "C" uint32_t hal_serial_tx_stall_count(void) { return tx_discarded; }

#endif // CORE_CM7
