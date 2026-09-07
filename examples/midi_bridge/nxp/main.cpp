/**
 * @file main.cpp
 * @brief Network MIDI 2.0 — NXP FRDM-MCXN947 FreeRTOS example entry point.
 *
 * Initialises the board (clocks, pin mux, debug UART), starts the lwIP
 * tcpip_thread via tcpip_init(), adds the ENET_QOS netif and starts DHCP,
 * then creates vSessionTask before handing off to the FreeRTOS scheduler.
 *
 * The Ethernet + lwIP stack is brought up synchronously before the scheduler
 * starts so vSessionTask can call transport methods immediately.
 *
 * ## Flashing
 * Build produces nm2_nxp_mcxn947.elf.  Flash via:
 *   pyocd flash --target mcxn947 nm2_nxp_mcxn947.elf
 *   or:
 *   J-Link Commander: loadfile nm2_nxp_mcxn947.elf
 *
 * ## Console
 * Open the J-Link virtual COM port at 115200 baud (USB connector on MCU-Link
 * probe, typically /dev/tty.usbmodem* on macOS or COMx on Windows).
 *
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
 */

#include "board_init.h"
#include "SessionTask.h"
#include "fsl_cmc.h"

#include "lwip/tcpip.h"
#include "FreeRTOS.h"
#include "task.h"

#include "tusb.h"
#if NM2_BRIDGE_USB_HOST
#include "host/hcd.h"
#endif

#include <cstdio>
#include <cstddef>

// Route C++ operator new/delete through the FreeRTOS heap so that
// allocations made by NetworkMidiSession (and any other C++ code) draw from
// the 256 KB pvPortMalloc pool rather than the tiny newlib _sbrk region.
void *operator new(size_t size)   { return pvPortMalloc(size); }
void *operator new[](size_t size) { return pvPortMalloc(size); }
void  operator delete(void *p)   noexcept { vPortFree(p); }
void  operator delete[](void *p) noexcept { vPortFree(p); }
void  operator delete(void *p, size_t) noexcept { vPortFree(p); }

// Session task: Ethernet wait, mDNS, auth, MIDI session, CLI.
// 6 KB stack: generous headroom for lwIP mDNS, SHA-256, and printf.
static constexpr uint32_t    kSessionStackWords = 1536; // 1536 * 4 = 6144 bytes
static constexpr UBaseType_t kSessionPriority   = 2;

// USB task: runs tud_task() (DEVICE role) or tuh_task() (HOST role,
// NM2_BRIDGE_USB_ROLE=HOST, see CMakeLists.txt) forever. Must run at a
// priority high enough to service USB promptly; tusb_init() itself must be
// called from task context after the scheduler starts (TinyUSB's IRQ
// handling uses FreeRTOS queue/semaphore APIs internally once
// CFG_TUSB_OS=OPT_OS_FREERTOS).
static constexpr uint32_t    kUsbStackWords = 512; // 2048 bytes
static constexpr UBaseType_t kUsbPriority   = configMAX_PRIORITIES - 1;

#if NM2_BRIDGE_USB_HOST
extern "C" void vUsbDeviceTask(void * /*params*/)
{
    tusb_rhport_init_t hostInit{};
    hostInit.role  = TUSB_ROLE_HOST;
    hostInit.speed = TUSB_SPEED_AUTO;
    tusb_init(BOARD_TUH_RHPORT, &hostInit);

    // The chipidea HS host controller, like RP2040's, only notifies TinyUSB
    // of a device via an edge-triggered connect interrupt -- if a device
    // was already plugged in before tusb_init() ran, there's no edge to
    // catch and it's silently never enumerated. Same workaround as
    // examples/midi_bridge/pico/main.cpp's HOST role /
    // UUT/USB_Host_UMP_Test.
    if (hcd_port_connect_status(BOARD_TUH_RHPORT)) {
        printf("USB device already attached at boot -- forcing enumeration\r\n");
        hcd_event_device_attach(BOARD_TUH_RHPORT, false);
    }

    for (;;) {
        tuh_task();
    }
}
#else
extern "C" void vUsbDeviceTask(void * /*params*/)
{
    tusb_rhport_init_t devInit{};
    devInit.role  = TUSB_ROLE_DEVICE;
    devInit.speed = TUSB_SPEED_AUTO;
    tusb_init(BOARD_TUD_RHPORT, &devInit);

    // A debugger-issued reset (MCU-Link/pyOCD during bench bring-up) resets
    // the chip's logic but can leave the USB pull-up state change too brief
    // for the host/hub to register as a real detach -- the host then keeps
    // its stale, previously-enumerated descriptor set instead of re-reading
    // the new one. A real power-cycle or cable replug doesn't have this
    // problem; this pulse makes a bare debugger reset behave the same way.
    // Same fix as the Pico DEVICE-role build (examples/midi_bridge/pico/main.cpp).
    tud_disconnect();
    vTaskDelay(pdMS_TO_TICKS(250));
    tud_connect();

    for (;;) {
        tud_task();
    }
}
#endif // NM2_BRIDGE_USB_HOST

// The NXP startup file (startup_mcxn947_cm33_core0.c) gates the
// __libc_init_array() call on #if defined(__cplusplus), which is false when
// the startup is compiled as C.  Call it explicitly here — data/bss are
// already initialised by the time main() is entered, so this is safe.
extern "C" void __libc_init_array(void);
extern "C" void _init(void) {}  // required stub for newlib __libc_init_array
extern "C" void *__dso_handle __attribute__((weak)) = nullptr; // for __cxa_atexit

int main(void)
{
    __libc_init_array(); // runs C++ static constructors (vtable init, etc.)

    // Board init: clocks → 150 MHz, pin mux, debug UART retarget.
    NM2_BoardInit();

    // Diagnostic: print why the MCU last reset, decoded from CMC0->SRS
    // (System Reset Status). Added while investigating a reproducible reset
    // triggered by opening the composite USB CDC console port -- this tells
    // us definitively whether it's the watchdog, a core lockup, a software
    // reset request, or something else, instead of guessing from symptoms.
    {
        uint32_t srs = CMC_GetSystemResetStatus(CMC0);
        printf("\r\n[boot] Reset cause (CMC0->SRS = 0x%08lX):", (unsigned long) srs);
        if (srs & (uint32_t) kCMC_WakeUpReset)              printf(" WAKEUP");
        if (srs & (uint32_t) kCMC_PORReset)                 printf(" POR");
        if (srs & (uint32_t) kCMC_VDReset)                  printf(" VOLTAGE_DETECT");
        if (srs & (uint32_t) kCMC_PinReset)                 printf(" PIN");
        if (srs & (uint32_t) kCMC_DAPReset)                 printf(" DAP/DEBUG");
        if (srs & (uint32_t) kCMC_LowPowerAcknowledgeTimeoutReset) printf(" LOW_POWER_ACK_TIMEOUT");
        if (srs & (uint32_t) kCMC_SCGReset)                 printf(" CLOCK_LOSS");
        if (srs & (uint32_t) kCMC_WindowedWatchdog0Reset)   printf(" WATCHDOG0");
        if (srs & (uint32_t) kCMC_SoftwareReset)            printf(" SOFTWARE");
        if (srs & (uint32_t) kCMC_LockUoReset)              printf(" CPU_LOCKUP");
        printf("\r\n\r\n");
    }

    // Power up and clock the USB1 High-Speed controller/PHY. tusb_init()
    // itself is deferred to vUsbDeviceTask, which runs after the scheduler
    // starts (see kUsbStackWords comment above).
    NM2_UsbHsInit();

    // Start lwIP's tcpip_thread.  This creates the thread; the netif is added
    // below after tcpip_init() returns.
    tcpip_init(nullptr, nullptr);

    // Add the ENET_QOS netif to lwIP (left down -- DHCP vs. static IP is a
    // runtime setup-menu choice vSessionTask makes later, see board_init.h).
    // Must run after tcpip_init() and before vTaskStartScheduler().
    NM2_NetifAdd(nullptr); // nullptr → use default MAC address

    // USB device task: services the MIDI 2.0 (UMP) interface.
    xTaskCreate(vUsbDeviceTask, "usbd",
                kUsbStackWords, nullptr, kUsbPriority, nullptr);

    // Session task handles everything after this point.
    xTaskCreate(vSessionTask, "session",
                kSessionStackWords, nullptr, kSessionPriority, nullptr);

    vTaskStartScheduler();

    // Should never reach here.
    for (;;) {}
}
