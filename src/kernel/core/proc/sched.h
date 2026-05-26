/*
 * sched.h — Round-robin scheduler API
 *
 * The scheduler drives preemptive multitasking:
 *
 *   SysTick_Handler  — fires every SYSTICK_RELOAD+1 CPU cycles;
 *                      decrements the current process's time-slice and
 *                      raises switch_pending when the slice expires.
 *                      The per-arch exception-exit path consumes the
 *                      flag to perform the context swap.
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
 * current.  Called from the per-arch context-switch path (arm_m's
 * arm_kernel_sched_switch, m68k's m68k_ctx_switch, etc.).
 * Returns current if no other RUNNABLE process exists.
 */
pcb_t *sched_next(void);

/*
 * Start the preemptive scheduler:
 *   1. Copy MSP to PSP; switch Thread mode to PSP (CONTROL.SPSEL = 1).
 *   2. Lower SVCall priority so hardware IRQs can preempt syscalls.
 *   3. Configure SysTick with SYSTICK_RELOAD and enable its interrupt.
 *   4. Enable interrupts (cpsie i).
 * Returns to the caller, which continues executing as kernel thread 0.
 * Must be called after proc_init() and all initial processes are set up.
 */
void sched_start(void);

/*
 * Common timer tick handler: tick counter, input polling, CPU accounting.
 * Called from the architecture-specific timer ISR (SysTick on ARM, timer
 * handler on m68k).
 * from_user: 1 if interrupted from user mode, 0 if from kernel/supervisor.
 */
void sched_timer_tick(int from_user);

/*
 * Voluntarily switch to the next RUNNABLE process.  Calls
 * arch_sched_switch(), which performs a synchronous swap when called
 * from a Handler-mode context that owns its kernel stack, and falls
 * back to a per-arch trap mechanism (m68k TRAP #1, ARM `svc #0xFF`)
 * from Thread mode.  Safe from anywhere.
 */
void sched_switch(void);

/*
 * Mark the current process blocked on a wait channel.
 * Caller is responsible for checking its blocking condition first, then
 * calling sched_switch() after releasing any resource lock.
 */
void sched_block_current(void *channel);

/*
 * Block the current process on a wait channel, then switch away.
 * The caller must have already checked the blocking condition.
 */
void sched_sleep_current(void *channel);

/*
 * Block the current process on a wait channel, release a caller-held spinlock,
 * then switch away.  Use this after checking a blocking condition under the
 * same resource lock so wakeups cannot slip between the check and the block.
 */
void sched_sleep_current_unlock(void *channel, uint32_t lock_num,
                                uint32_t saved);

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
 * for the next timer tick.
 */
int sched_idle_poll(void);

#endif /* PPAP_KERNEL_CORE_PROC_SCHED_H */
