/**
 * @file FreeRTOSConfig.h
 * @brief FreeRTOS configuration for nm2_pico_rtos (Pico 2 W, RP2350).
 *
 * Includes the pico-examples common config then overrides the heap size and
 * enables stack-overflow detection during development.
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

#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include "FreeRTOSConfig_examples_common.h"

// RP2350 has 520 KB SRAM; 192 KB for FreeRTOS heap leaves plenty for lwIP pools.
#undef  configTOTAL_HEAP_SIZE
#define configTOTAL_HEAP_SIZE           (192 * 1024)

// Catch stack overflows at task boundary during development.
#undef  configCHECK_FOR_STACK_OVERFLOW
#define configCHECK_FOR_STACK_OVERFLOW  2

#undef  configUSE_MALLOC_FAILED_HOOK
#define configUSE_MALLOC_FAILED_HOOK    1

// CPU clock frequency — used by the ARM_CM33_NTZ port to configure SysTick.
// RP2350 default is 150 MHz; update if you overclock via set_sys_clock_khz().
#define configCPU_CLOCK_HZ              ( ( uint32_t ) 150000000 )

// ARM Cortex-M33 port (GCC_ARM_CM33_NTZ_NONSECURE) required settings.
// These are normally under #if PICO_RP2350 in the common config, but
// PICO_RP2350 is not propagated when FreeRTOS builds via its own CMakeLists.txt
// (before pico_sdk_init defines it).  We define them unconditionally here since
// this project always targets RP2350 in non-TrustZone mode.
#ifndef configENABLE_FPU
#define configENABLE_FPU                        1
#endif
#ifndef configENABLE_MPU
#define configENABLE_MPU                        0
#endif
#ifndef configENABLE_TRUSTZONE
#define configENABLE_TRUSTZONE                  0
#endif
#ifndef configRUN_FREERTOS_SECURE_ONLY
#define configRUN_FREERTOS_SECURE_ONLY          1
#endif
#ifndef configMAX_SYSCALL_INTERRUPT_PRIORITY
#define configMAX_SYSCALL_INTERRUPT_PRIORITY    16
#endif

// Pico SDK sync/time interop is specific to the Raspberry Pi ThirdParty ports
// (RP2040, RP2350_ARM_NTZ).  Disable when using the generic ARM_CM33_NTZ port.
#undef  configSUPPORT_PICO_SYNC_INTEROP
#define configSUPPORT_PICO_SYNC_INTEROP         0
#undef  configSUPPORT_PICO_TIME_INTEROP
#define configSUPPORT_PICO_TIME_INTEROP         0

#endif /* FREERTOS_CONFIG_H */
