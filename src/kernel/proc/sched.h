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
 * SysTick reload value — derived from config.h.
 * Hardware (RP2040): PPAP_SYS_HZ / PPAP_TICK_HZ − 1 = 1,329,999 (10 ms)
 * QEMU (mps2-an500): SysTick counter runs but TICKINT is never asserted,
 * so preemptive scheduling on QEMU uses cooperative sched_yield() instead.
 */
#define SYSTICK_RELOAD  (PPAP_SYS_HZ / PPAP_TICK_HZ - 1u)

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

/*
 * Put the current process to sleep for `ticks` SysTick ticks.
 * Marks current as PROC_SLEEPING and triggers PendSV so another RUNNABLE
 * process takes the CPU.  sched_tick() wakes the process when the tick
 * count reaches sleep_until.  Called from sys_nanosleep() (sys_time.c).
 */
void sched_sleep(uint32_t ticks);

/*
 * Register an input-available callback, polled every 20 ms from SysTick.
 * When fn() returns non-zero, tty_rx_notify() is called to wake blocked
 * tty readers.  Used by PicoCalc to poll the STM32 keyboard controller.
 */
void sched_set_input_poll(int (*fn)(void));

/*
 * Wake all processes blocked on the given channel.
 * Scans proc_table for PROC_BLOCKED processes whose wait_channel matches,
 * sets them to PROC_RUNNABLE, and clears their wait_channel.
 * Used by pipe_read/pipe_write/pipe_close to wake blocked counterparts.
 */
void sched_wakeup(void *channel);

/*
 * Return the current SysTick tick count.
 * Used by time syscalls (clock_gettime, gettimeofday) to derive wall time.
 */
uint32_t sched_get_ticks(void);

/*
 * Per-core CPU jiffy counters — updated in SysTick_Handler.
 * Indexed by core_id() (0 or 1).
 * Used by procfs to generate /proc/stat and /proc/uptime.
 */
extern uint32_t cpu_user_ticks[2];
extern uint32_t cpu_system_ticks[2];
extern uint32_t cpu_idle_ticks[2];

#endif /* PPAP_PROC_SCHED_H */
