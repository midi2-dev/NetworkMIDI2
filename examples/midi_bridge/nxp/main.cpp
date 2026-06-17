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

#include "lwip/tcpip.h"
#include "FreeRTOS.h"
#include "task.h"

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

    // Start lwIP's tcpip_thread.  This creates the thread; the netif is added
    // below after tcpip_init() returns.
    tcpip_init(nullptr, nullptr);

    // Add the ENET_QOS netif to lwIP and start DHCP.
    // Must run after tcpip_init() and before vTaskStartScheduler() so the
    // netif is valid when vSessionTask polls dhcp_supplied_address().
    NM2_NetifInit(nullptr); // nullptr → use default MAC address

    // Session task handles everything after this point.
    xTaskCreate(vSessionTask, "session",
                kSessionStackWords, nullptr, kSessionPriority, nullptr);

    vTaskStartScheduler();

    // Should never reach here.
    for (;;) {}
}
