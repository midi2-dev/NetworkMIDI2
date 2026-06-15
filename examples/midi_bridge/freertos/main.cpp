/**
 * @file main.cpp
 * @brief Network MIDI 2.0 — Pico 2 W FreeRTOS example entry point.
 *
 * Creates one FreeRTOS task before handing off to the scheduler:
 *
 *   vSessionTask — application task: WiFi, mDNS, auth, MIDI session, and CLI.
 *
 * tud_task() is NOT driven from a dedicated FreeRTOS task.  The pico SDK
 * installs a USBCTRL_IRQ shared handler in stdio_usb_init() that triggers a
 * low-priority user IRQ, which in turn calls tud_task_ext() whenever USB
 * hardware events arrive.  Adding a second caller from task context is not
 * safe: TinyUSB's event loop is not re-entrant, and the user IRQ can preempt
 * a task-context tud_task() call on Cortex-M with the IRQ priority above
 * configMAX_SYSCALL_INTERRUPT_PRIORITY.
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

#include "pico/stdlib.h"
#include "pico/flash.h"
#include "FreeRTOS.h"
#include "task.h"
#include "SessionTask.h"
#include <cstdio>

// Session task — WiFi, mDNS, auth, MIDI session, and CLI.
// 4096 words = 16 KB: generous headroom for WiFi, mDNS, SHA-256, and printf.
static constexpr uint32_t    kSessionStackWords = 4096;
static constexpr UBaseType_t kSessionPriority   = 2;

int main()
{
    // stdio_init_all() calls tusb_init() (registers USBCTRL_IRQ) and
    // stdio_usb_init() (installs the background user-IRQ that drives tud_task).
    // Must run before the scheduler so the IRQ handler is in place.
    stdio_init_all();

    // Allow the host 2 s to enumerate the CDC device before FreeRTOS
    // reconfigures SysTick.  The SDK's background IRQ mechanism drives
    // tud_task() inside sleep_ms — no scheduler conflict at this point.
    sleep_ms(2000);

    // flash_safe_execute_core_init() must precede vTaskStartScheduler() on
    // dual-core RP2350: registers the Core 1 handler that quiesces it before
    // any flash erase/program (XIP stalls both cores otherwise).
    flash_safe_execute_core_init();

    xTaskCreate(vSessionTask, "session", kSessionStackWords, nullptr, kSessionPriority, nullptr);

    vTaskStartScheduler();

    for (;;) {}
}

// ---------------------------------------------------------------------------
// FreeRTOS hook implementations
// ---------------------------------------------------------------------------

extern "C" {

void vApplicationStackOverflowHook(TaskHandle_t /*task*/, char *taskName)
{
    printf("[FATAL] Stack overflow in task: %s\r\n", taskName);
    for (;;) {}
}

void vApplicationMallocFailedHook()
{
    printf("[FATAL] FreeRTOS heap exhausted (pvPortMalloc returned NULL)\r\n");
    for (;;) {}
}

} // extern "C"
