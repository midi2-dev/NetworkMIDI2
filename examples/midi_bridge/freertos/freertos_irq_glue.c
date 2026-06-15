/*
 * The GCC_ARM_CM33_NTZ_NONSECURE port defines its interrupt handlers under
 * CMSIS names (SVC_Handler, PendSV_Handler, SysTick_Handler), but the Pico
 * SDK's crt0 vector table populates entries via isr_svcall / isr_pendsv /
 * isr_systick.  Without these thunks the vector entries point to the default
 * BKPT stubs in crt0.S, so vTaskStartScheduler()'s first SVC hits a breakpoint
 * instead of the FreeRTOS handler.
 *
 * Each naked function is a single-instruction tail-call branch.  It does not
 * modify any registers or touch the exception stack frame, so the FreeRTOS
 * handlers receive the processor state exactly as if they had been placed
 * directly in the vector table.
 */

extern void SVC_Handler(void);
extern void PendSV_Handler(void);
extern void SysTick_Handler(void);

void __attribute__((naked)) isr_svcall(void)  { __asm volatile ("b SVC_Handler");   }
void __attribute__((naked)) isr_pendsv(void)  { __asm volatile ("b PendSV_Handler"); }
void __attribute__((naked)) isr_systick(void) { __asm volatile ("b SysTick_Handler"); }
