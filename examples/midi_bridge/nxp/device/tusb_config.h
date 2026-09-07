/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2019 Ha Thach (tinyusb.org)
 * Copyright (c) 2026 AmeNote Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
 extern "C" {
#endif

//--------------------------------------------------------------------+
// Board Specific Configuration — FRDM-MCXN947, USB1 (ChipIdea High-Speed)
//--------------------------------------------------------------------+

// USB0 on MCX N9 is KHCI Full-Speed; USB1 is the ChipIdea High-Speed
// controller wired to the FRDM-MCXN947's USB-C connector. rhport = 1.
#ifndef BOARD_TUD_RHPORT
#define BOARD_TUD_RHPORT      1
#endif

#ifndef BOARD_TUD_MAX_SPEED
#define BOARD_TUD_MAX_SPEED   OPT_MODE_HIGH_SPEED
#endif

//--------------------------------------------------------------------
// COMMON CONFIGURATION
//--------------------------------------------------------------------

#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined
#endif

// Verbose TinyUSB device-stack logging over the LPUART debug console.
// Override at configure time: -DCFG_TUSB_DEBUG=<0-3>. Unlike the RP2040
// examples, this build does not go through TinyUSB's hw/bsp/family.cmake
// helper (which force-injects CFG_TUSB_DEBUG=0 for Release builds on that
// platform) -- this #define takes effect directly.
#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG 0
#endif

#if   BOARD_TUD_RHPORT == 0
  #define CFG_TUSB_RHPORT0_MODE     (OPT_MODE_DEVICE | BOARD_TUD_MAX_SPEED)
#elif BOARD_TUD_RHPORT == 1
  #define CFG_TUSB_RHPORT1_MODE     (OPT_MODE_DEVICE | BOARD_TUD_MAX_SPEED)
#else
  #error "Incorrect RHPort configuration"
#endif

// This example runs its USB device task from a dedicated FreeRTOS task
// (see main.cpp / vUsbDeviceTask), same as the rest of this bridge.
#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS               OPT_OS_FREERTOS
#endif

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN          __attribute__ ((aligned(4)))
#endif

//--------------------------------------------------------------------
// DEVICE CONFIGURATION
//--------------------------------------------------------------------

#ifndef CFG_TUD_ENDPOINT0_SIZE
#define CFG_TUD_ENDPOINT0_SIZE    64
#endif

//------------- CLASS -------------//
// No USB CDC here — console is the LPUART4 debug console (see board_init.cpp);
// this USB port is dedicated to the MIDI 2.0 (UMP) interface.
#define CFG_TUD_CDC               0
#define CFG_TUD_MSC               0
#define CFG_TUD_HID               0
#define CFG_TUD_MIDI              0 // app-specific UMP driver (third_party/tusb_ump) used instead
#define CFG_TUD_VENDOR            0
#define CFG_TUD_UMP               1 // conforming to tinyUSB design, 1 UMP device endpoint

// UMP FIFO size of TX and RX. NOTE: bulk endpoints in usb_descriptors.cpp are
// still declared 64 bytes (full-speed-compatible) even though this port runs
// at High Speed — a 512-byte-max-packet HS configuration descriptor is a
// follow-up throughput optimisation, not required for correct operation.
#define CFG_TUD_UMP_RX_BUFSIZE  512  // Must be modulo 4, 32 bits per UMP message or segment
#define CFG_TUD_UMP_TX_BUFSIZE  512  // Must be modulo 4, 32 bits per UMP message or segment

#ifdef __cplusplus
}
#endif

#endif /* _TUSB_CONFIG_H_ */
