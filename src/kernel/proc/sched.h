/*
 * sched.h — Round-robin scheduler API
 *
 * The scheduler drives preemptive multitasking:
 *
 *   SysTick_Handler  — fires every SYSTICK_RELOAD+1 CPU cycles;
 *                      decrements the current process's time-slice and
 *                      triggers PendSV when the slice expires.
 *
 *   PendSV_Handler   — (in switch.S) calls sched_next() to pick the
 *                      next process, then saves/restores the CPU context.
 *
 *   sched_start()    — switches Thread mode to PSP, configures SysTick,
 *                      and enables interrupts.  Returns to the caller
 *                      (which becomes kernel thread 0).
 *
 * No separate idle thread in Phase 1.  If no other process is RUNNABLE,
 * sched_next() returns current and the running process keeps its slice.
 */

#ifndef PPAP_PROC_SCHED_H
#define PPAP_PROC_SCHED_H

#include "proc.h"

/*
 * SysTick reload value.
 * Hardware (RP2040 @ 133 MHz): 10 ms = 133,000,000/100 − 1 = 1,329,999
 * QEMU (mps2-an500): SysTick counter runs but TICKINT is never asserted,
 * so preemptive scheduling on QEMU uses cooperative sched_yield() instead.
 */
#define SYSTICK_RELOAD  (133000000u / 100u - 1u)

/*
 * Pick the next RUNNABLE process in round-robin order starting after
 * current.  Called from PendSV_Handler in switch.S.
 * Returns current if no other RUNNABLE process exists.
 */
pcb_t *sched_next(void);

/*
 * Start the preemptive scheduler:
 *   1. Copy MSP to PSP; switch Thread mode to PSP (CONTROL.SPSEL = 1).
 *   2. Set PendSV to lowest priority so it never preempts real IRQs.
 *   3. Configure SysTick with SYSTICK_RELOAD and enable its interrupt.
 *   4. Enable interrupts (cpsie i).
 * Returns to the caller, which continues executing as kernel thread 0.
 * Must be called after proc_init() and all initial processes are set up.
 */
void sched_start(void);

/*
 * Called from SysTick_Handler every tick.
 * Decrements current->ticks_remaining; when it reaches zero, reloads the
 * slice counter and sets ICSR.PENDSVSET to trigger a context switch.
 */
void sched_tick(void);

/*
 * Voluntarily yield the CPU to the next RUNNABLE process.
 * Sets PENDSVSET so PendSV_Handler runs at the next opportunity and
 * switches context.  Safe to call from Thread mode at any time.
 * Used by QEMU smoke tests where SysTick IRQ delivery is not available.
 */
void sched_yield(void);

#endif /* PPAP_PROC_SCHED_H */
