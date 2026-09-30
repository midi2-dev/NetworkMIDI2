/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2019 Ha Thach (tinyusb.org)
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
 * This file is part of the TinyUSB stack.
 */

// LOCAL OVERRIDE (examples/midi_bridge/nxp/tinyusb_overrides/hcd_ci_hs.c):
// third_party/tinyusb's hcd_ci_hs.c (pinned at 0.18.0) has no OPT_MCU_MCXN9
// branch -- only its DEVICE-role sibling, dcd_ci_hs.c, does (see that
// file's `#elif TU_CHECK_MCU(OPT_MCU_MCXN9) #include "ci_hs_mcx.h"`
// branch). ci_hs_mcx.h itself already defines both CI_DCD_INT_ENABLE/
// DISABLE (device) and CI_HCD_INT_ENABLE/DISABLE (host) macros, so it was
// clearly written with host-mode support in mind -- this is a one-line
// upstream gap, not a genuine MCX-N9-HOST-mode limitation. Same pattern as
// examples/midi_bridge/pico/tinyusb_overrides/hcd_rp2040.c: swap in a
// locally patched copy of a single upstream file, scoped to this example's
// HOST-role build only, rather than editing the third_party/tinyusb
// submodule (upstream hathach/tinyusb, not a fork we control) in place.
// The only change from the stock file is the added MCXN9 branch below --
// diff against the submodule's copy if this needs to be re-synced after a
// TinyUSB version bump.

#include "tusb_option.h"

// Chipidea Highspeed USB IP implement EHCI for host functionality

#if CFG_TUH_ENABLED && defined(TUP_USBIP_EHCI)

//--------------------------------------------------------------------+
// INCLUDE
//--------------------------------------------------------------------+
#include "common/tusb_common.h"
#include "host/hcd.h"
#include "portable/ehci/ehci_api.h"
#include "ci_hs_type.h"

#if CFG_TUSB_MCU == OPT_MCU_MIMXRT1XXX

#include "ci_hs_imxrt.h"

#if CFG_TUH_MEM_DCACHE_ENABLE
bool hcd_dcache_clean(void const* addr, uint32_t data_size) {
  return imxrt_dcache_clean(addr, data_size);
}

bool hcd_dcache_invalidate(void const* addr, uint32_t data_size) {
  return imxrt_dcache_invalidate(addr, data_size);
}

bool hcd_dcache_clean_invalidate(void const* addr, uint32_t data_size) {
  return imxrt_dcache_clean_invalidate(addr, data_size);
}
#endif

#elif TU_CHECK_MCU(OPT_MCU_LPC18XX, OPT_MCU_LPC43XX)

#include "ci_hs_lpc18_43.h"

#elif TU_CHECK_MCU(OPT_MCU_MCXN9)
// MCX N9 only port 1 uses this controller -- same header dcd_ci_hs.c uses
// for the DEVICE role (see this file's header comment).
#include "ci_hs_mcx.h"

#else
#error "Unsupported MCUs"
#endif

//--------------------------------------------------------------------+
// MACRO CONSTANT TYPEDEF
//--------------------------------------------------------------------+

//--------------------------------------------------------------------+
// Controller API
//--------------------------------------------------------------------+

bool hcd_init(uint8_t rhport, const tusb_rhport_init_t* rh_init) {
  (void) rh_init;
  ci_hs_regs_t *hcd_reg = CI_HS_REG(rhport);

  // Reset controller
  hcd_reg->USBCMD |= USBCMD_RESET;
  while ( hcd_reg->USBCMD & USBCMD_RESET ) {}

  // Set mode to device, must be set immediately after reset
#if CFG_TUSB_MCU == OPT_MCU_LPC18XX || CFG_TUSB_MCU == OPT_MCU_LPC43XX
  // LPC18XX/43XX need to set VBUS Power Select to HIGH
  // RHPORT1 is fullspeed only (need external PHY for Highspeed)
  hcd_reg->USBMODE = USBMODE_CM_HOST | USBMODE_VBUS_POWER_SELECT;
  if (rhport == 1) {
    hcd_reg->PORTSC1 |= PORTSC1_FORCE_FULL_SPEED;
  }
#else
  hcd_reg->USBMODE = USBMODE_CM_HOST;
#endif

  // Backport of upstream hathach/tinyusb fc43eedd: the unconditional force to
  // full speed was a workaround for attach bouncing, which usbh.c's
  // ENUM_DEBOUNCING_DELAY_MS now handles. Forced FS capped a Pi gadget on the
  // FRDM-MCXN947's HS port at full speed. Only force it when the host is not
  // configured for high speed.
  #if !TUH_OPT_HIGH_SPEED
  hcd_reg->PORTSC1 |= PORTSC1_FORCE_FULL_SPEED;
  #endif

  // Interrupt Threshold Control: deliver transfer-complete interrupts
  // immediately instead of at the reset default of 8 micro-frames (1 ms).
  // TinyUSB keeps one IN transfer queued per endpoint and re-arms it from the
  // completion interrupt, so the default threshold capped a bulk IN endpoint
  // at one transfer per millisecond. A device that sends each message as its
  // own short transfer (a Linux f_midi2 gadget writing a chord note by note)
  // then had its 10-note chord spread over 9 ms on the network side: measured
  // on the wire as 10 datagrams exactly 1.000 ms apart, with the IN endpoint
  // armed and waiting the whole time. dcd_ci_hs.c already clears this for the
  // device role; ehci.c never touches it for the host role. Must be written
  // while the controller is halted (EHCI 2.3.1), i.e. here, after reset and
  // before ehci_init() sets Run.
  hcd_reg->USBCMD &= ~USBCMD_INTR_THRESHOLD_MASK;

  return ehci_init(rhport, (uint32_t) &hcd_reg->CAPLENGTH, (uint32_t) &hcd_reg->USBCMD);
}

void hcd_int_enable(uint8_t rhport) {
  CI_HCD_INT_ENABLE(rhport);
}

void hcd_int_disable(uint8_t rhport) {
  CI_HCD_INT_DISABLE(rhport);
}

#endif
