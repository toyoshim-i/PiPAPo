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

#ifndef PPAP_KERNEL_CORE_PROC_SCHED_H
#define PPAP_KERNEL_CORE_PROC_SCHED_H

#include "kernel/common/core/sched_info.h"
#include "kernel/core/proc/proc.h"

/*
 * SysTick reload value — derived from config.h.
 * Hardware (RP2040): PPAP_SYS_HZ / PPAP_TICK_HZ − 1 = 1,329,999 (10 ms)
 * QEMU (mps2-an500): SysTick counter runs but TICKINT is never asserted,
 * so preemptive scheduling on QEMU uses cooperative sched_switch() instead.
 */
#define SYSTICK_RELOAD (PPAP_SYS_HZ / PPAP_TICK_HZ - 1u)

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
 * Common timer tick handler: tick counter, input polling, CPU accounting.
 * Called from the architecture-specific timer ISR (SysTick on ARM, timer
 * handler on m68k).
 * from_user: 1 if interrupted from user mode, 0 if from kernel/supervisor.
 */
void sched_timer_tick(int from_user);

/*
 * Voluntarily switch to the next RUNNABLE process.
 * Sets PENDSVSET so PendSV_Handler runs at the next opportunity and
 * switches context.  Safe to call from Thread mode at any time.
 * Used by QEMU smoke tests where SysTick IRQ delivery is not available.
 */
void sched_switch(void);

/*
 * Put the current process to sleep for `ticks` SysTick ticks.
 * Marks current as PROC_SLEEPING and triggers PendSV so another RUNNABLE
 * process takes the CPU.  sched_tick() wakes the process when the tick
 * count reaches sleep_until.  Called from sys_nanosleep() (sys_time.c).
 */
void sched_sleep(uint32_t ticks);

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
 * Run idle-loop work: fires VFS_EVENT_IDLE (TTY input check +
 * display flush) and calls target_idle_poll() for any target-specific
 * work.  Called from the idle thread after each hlt/wfi wake.
 * Must run in thread context — backends may do slow I/O.
 *
 * Returns non-zero if any of the polled work raised an arch_yield()
 * (e.g. tty_rx_notify woke a blocked reader).  The idle loop must
 * then call sched_switch() so the wakeup is honored without waiting
 * for the next timer tick.  On Cortex-M this is always 0 because
 * PendSV self-pends and runs on exception return.
 */
int sched_idle_poll(void);

#endif /* PPAP_KERNEL_CORE_PROC_SCHED_H */
