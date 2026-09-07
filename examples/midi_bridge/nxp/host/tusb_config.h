/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2019 Ha Thach (tinyusb.org)
 * Copyright (c) 2026 AmeNote Inc.
 *
 * NOTE: Local tusb_config.h for this example's HOST role
 * (NM2_BRIDGE_USB_ROLE=HOST, see CMakeLists.txt) -- configures TinyUSB in
 * HOST mode, in place of ../device/tusb_config.h (hardcoded to
 * CFG_TUSB_RHPORT1_MODE = OPT_MODE_DEVICE, used by the default DEVICE
 * role). This directory is only added to the include path ahead of
 * device/ when the HOST role is selected -- same split as
 * examples/midi_bridge/pico/host/tusb_config.h, which this is adapted
 * from (see that file's header comment for the config values' origin).
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
// controller wired to the FRDM-MCXN947's USB-C connector. rhport = 1,
// same physical port the DEVICE role uses (see ../device/tusb_config.h).
#ifndef BOARD_TUH_RHPORT
#define BOARD_TUH_RHPORT      1
#endif

#ifndef BOARD_TUH_MAX_SPEED
#define BOARD_TUH_MAX_SPEED   OPT_MODE_HIGH_SPEED
#endif

//--------------------------------------------------------------------
// COMMON CONFIGURATION
//--------------------------------------------------------------------

#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined
#endif

// Verbose TinyUSB host-stack logging over the LPUART debug console.
// Override at configure time: -DCFG_TUSB_DEBUG=<0-3>.
#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG 0
#endif
#ifndef CFG_TUH_LOG_LEVEL
#define CFG_TUH_LOG_LEVEL 1
#endif

#if   BOARD_TUH_RHPORT == 0
  #define CFG_TUSB_RHPORT0_MODE     (OPT_MODE_HOST | BOARD_TUH_MAX_SPEED)
#elif BOARD_TUH_RHPORT == 1
  #define CFG_TUSB_RHPORT1_MODE     (OPT_MODE_HOST | BOARD_TUH_MAX_SPEED)
#else
  #error "Incorrect RHPort configuration"
#endif

// This example runs its USB host task from a dedicated FreeRTOS task
// (see main.cpp / vUsbDeviceTask's HOST-role branch), same as the
// DEVICE role.
#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS               OPT_OS_FREERTOS
#endif

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN          __attribute__ ((aligned(4)))
#endif

#ifndef CFG_TUH_MEM_SECTION
#define CFG_TUH_MEM_SECTION
#endif

#ifndef CFG_TUH_MEM_ALIGN
#define CFG_TUH_MEM_ALIGN           __attribute__ ((aligned(4)))
#endif

//--------------------------------------------------------------------
// HOST CONFIGURATION
//--------------------------------------------------------------------

#define CFG_TUH_ENABLED             1
#define CFG_TUH_MAX_SPEED           BOARD_TUH_MAX_SPEED

// Must be >= the largest attached device's full Configuration Descriptor
// wTotalLength -- same rationale/value as examples/midi_bridge/pico's
// HOST role.
#define CFG_TUH_ENUMERATION_BUFSIZE 512

// Hub support and device/endpoint limits -- UNLIKE examples/midi_bridge/
// pico's HOST role (which carries over generous RP2040 defaults,
// CFG_TUH_HUB=3/CFG_TUH_DEVICE_MAX=10/endpoint max 16), these are trimmed
// hard here: TinyUSB's shared EHCI core (portable/ehci/ehci.c) allocates
// a static `ehci_data_t` sized by
// CFG_TUH_DEVICE_MAX*CFG_TUH_ENDPOINT_MAX + CFG_TUH_HUB queue-head/qTD
// pools, and the Pico-borrowed values (163 queue heads) alone cost ~17 KB
// of this chip's 384 KB SRAM -- enough by itself to blow the link budget
// once lwIP/mDNS/session buffers are also accounted for (confirmed: build
// failed with a ~21 KB SRAM overflow before this was trimmed). This
// bridge only ever tracks one downstream USB MIDI device at a time (see
// s_usbHostMounted/s_usbHostDaddr/s_usbHostItfNum in SessionTask.cpp), so
// there is no functional loss in sizing for "one device, optionally
// behind one hub tier" instead of Pico's ample-headroom defaults.
#define CFG_TUH_ENDPOINT_MAX        4
#define CFG_TUH_HUB                 1
#define CFG_TUH_DEVICE_MAX          2

//--------------------------------------------------------------------
// UMP HOST CLASS DRIVER CONFIGURATION
//--------------------------------------------------------------------

// CFG_TUH_MIDI intentionally left undefined -- see
// UUT/USB_Host_UMP_Test/tusb_config.h for why (a TinyUSB core build gap
// under the merged-call enumeration shape, not a functional loss for
// this driver).
// #define CFG_TUH_MIDI             1

#define CFG_TUH_UMP                 20
#define CFG_TUH_UMP_MAX_GTB         8

#ifdef __cplusplus
 }
#endif

#endif /* _TUSB_CONFIG_H_ */
