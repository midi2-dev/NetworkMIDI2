/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2019 Ha Thach (tinyusb.org)
 * Copyright (c) 2026 Michael Loh (AmeNote.com)
 *
 * NOTE: Local tusb_config.h for NetworkMIDI2_Bridge's HOST role
 * (NM2_BRIDGE_USB_ROLE=HOST, see CMakeLists.txt) -- configures TinyUSB in
 * HOST mode, in place of the shared ../../Common/include/tusb_config.h
 * (hardcoded to CFG_TUSB_RHPORT0_MODE = OPT_MODE_DEVICE, used by this app's
 * default DEVICE role and every other UUT device-role app). This directory
 * is only added to the include path ahead of Common/include when the HOST
 * role is selected -- see UUT/USB_Host_UMP_Test/tusb_config.h for the same
 * pattern this is copied from, which is the source of truth for these
 * settings' rationale; keep the two in sync.
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
 */

#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
 extern "C" {
#endif

//--------------------------------------------------------------------
// COMMON CONFIGURATION
//--------------------------------------------------------------------

#ifndef CFG_TUSB_MCU
  #error CFG_TUSB_MCU must be defined
#endif

#ifndef CFG_TUSB_OS
  #define CFG_TUSB_OS               OPT_OS_NONE
#endif

// See UUT/USB_Host_UMP_Test/tusb_config.h's matching comment: capped at 1,
// not TinyUSB's default of 2, because hcd_rp2040.c's ISR-context TU_LOG(2,
// ...) calls were observed to break enumeration timing. Configure with
// `cmake -DLOG=1` to actually see it (CFG_TUSB_DEBUG is force-injected by
// the SDK's family.cmake, this define alone does nothing).
#ifndef CFG_TUSB_DEBUG
  #define CFG_TUSB_DEBUG            0
#endif
#ifndef CFG_TUH_LOG_LEVEL
  #define CFG_TUH_LOG_LEVEL         1
#endif

#ifndef CFG_TUSB_MEM_SECTION
  #define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
  #define CFG_TUSB_MEM_ALIGN        __attribute__ ((aligned(4)))
#endif

#ifndef CFG_TUH_MEM_SECTION
  #define CFG_TUH_MEM_SECTION
#endif

#ifndef CFG_TUH_MEM_ALIGN
  #define CFG_TUH_MEM_ALIGN         __attribute__ ((aligned(4)))
#endif

//--------------------------------------------------------------------
// HOST CONFIGURATION
//--------------------------------------------------------------------

#define CFG_TUH_ENABLED             1

#ifndef BOARD_TUH_RHPORT
  #define BOARD_TUH_RHPORT          0
#endif

#ifndef BOARD_TUH_MAX_SPEED
  #define BOARD_TUH_MAX_SPEED       OPT_MODE_FULL_SPEED
#endif

#define CFG_TUH_MAX_SPEED           BOARD_TUH_MAX_SPEED

// Same rationale as USB_Host_UMP_Test's tusb_config.h: must be >= the
// largest attached device's full Configuration Descriptor wTotalLength.
#define CFG_TUH_ENUMERATION_BUFSIZE 512

// Hub support, same rationale/values as USB_Host_UMP_Test's tusb_config.h --
// carried over as-is per "make it work with USB Host as is now" rather than
// narrowed to a single directly-attached device, since a bridge deployment
// may reasonably sit behind a hub too. See that file's comments and
// UUT/USB_Host_UMP_Test/README.md's Known Issues for the current
// hub-support caveats (in particular: OAKTONE Oakboard Mini fails to
// enumerate through any hub).
#define CFG_TUH_HUB                 3
#define CFG_TUH_DEVICE_MAX          10

//--------------------------------------------------------------------
// UMP HOST CLASS DRIVER CONFIGURATION
//--------------------------------------------------------------------

// CFG_TUH_MIDI intentionally left undefined -- see USB_Host_UMP_Test's
// tusb_config.h for why (a TinyUSB core build gap under the merged-call
// enumeration shape, not a functional loss for this driver).
// #define CFG_TUH_MIDI             1

#define CFG_TUH_UMP                 20
#define CFG_TUH_UMP_MAX_GTB         8

#ifdef __cplusplus
 }
#endif

#endif /* _TUSB_CONFIG_H_ */
