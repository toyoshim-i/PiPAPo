/*
 * sched.c — Round-robin preemptive scheduler
 *
 * SysTick fires every SYSTICK_RELOAD+1 CPU cycles and calls sched_tick().
 * sched_tick() triggers PendSV when the current process's time-slice
 * expires.  PendSV_Handler (switch.S) calls sched_next() and swaps context.
 *
 * Priority setup:
 *   - PendSV must be the LOWEST priority exception (priority byte = 0xFF)
 *     so that it never preempts a real interrupt handler (e.g. UART IRQ).
 *     PendSV fires only when Thread mode would regain the CPU.
 *   - SysTick can be any priority higher than PendSV; we leave it at
 *     the reset default (0x00 = highest) for Phase 1.
 */

#include "kernel/core/proc/sched.h" /* includes proc.h via sched.h */

#include <stddef.h>
#include <stdint.h>

#include "kernel/common/ioregs.h"
#include "kernel/common/mod/mod_vfs.h"
#include "kernel/common/spinlock.h" /* SPIN_PROC */
#include "kernel/core/arch.h"
#include "kernel/core/mm/mem_region.h"
#include "kernel/core/mm/page.h" /* PAGE_SIZE */
#include "kernel/core/signal/signal.h"
#include "target/target.h"

/* ── Tick counter ─────────────────────────────────────────────────────────────
 */

/* Incremented by SysTick_Handler every tick. */
static volatile uint32_t tick_count = 0u;

/* Shared cooperative-yield flag.  Used by every architecture whose arch.h
 * pulls in arch_yield_default.h (m68k, i16, riscv, xtensa).  ARM Cortex-M
 * yields via PendSV and never touches this variable. */
/* Type is `unsigned int` so the storage width matches the platform word
 * size: 16 bits on i16 (8086 real mode), 32 bits on m68k/riscv/arm/xtensa.
 * This lets each architecture's timer/trap asm use its natural-width
 * load/store/test instructions without padding mismatches. */
volatile unsigned int switch_pending = 0u;

/* ── Per-core CPU jiffy counters (for /proc/stat) ────────────────────────── */
uint32_t cpu_user_ticks[2] = {0, 0};
uint32_t cpu_system_ticks[2] = {0, 0};
uint32_t cpu_idle_ticks[2] = {0, 0};

uint32_t sched_get_ticks(void) { return tick_count; }

/* ── Scheduler ───────────────────────────────────────────────────────────────
 */

pcb_t *sched_next(void) {
  uint32_t saved = spin_lock_irqsave(SPIN_PROC);
  uint32_t idx = (uint32_t)(current - proc_table); /* slot of current */

  /* Idle is the last resort — skipping it here keeps a single busy
   * process on-CPU instead of alternating with idle every tick. */
  pcb_t *result = current; /* default: keep running */
  for (uint32_t i = 1u; i < PROC_MAX; i++) {
    uint32_t next = (idx + i) % PROC_MAX;
    if (proc_table[next].is_idle) continue;
    if (proc_table[next].state == PROC_RUNNABLE &&
        proc_table[next].running_on_core < 0) {
      result = &proc_table[next];
      break;
    }
  }

  /* No non-idle candidate found.  If current is still runnable, stay on
   * it; otherwise fall through to an available idle task. */
  if (result == current && current->state != PROC_RUNNABLE) {
    for (uint32_t i = 0u; i < PROC_MAX; i++) {
      if (proc_table[i].is_idle && proc_table[i].state == PROC_RUNNABLE &&
          proc_table[i].running_on_core < 0) {
        result = &proc_table[i];
        break;
      }
    }
  }

  /* Track which core is running which process. */
  if (result != current) {
    current->running_on_core = -1;
    result->running_on_core = (int8_t)core_id();
  }

  spin_unlock_irqrestore(SPIN_PROC, saved);
  return result;
}

void sched_tick(void) {
  /* Only Core 0 handles sleep/timeout wakeups (avoids double-waking).
   * Comparison uses signed subtraction to handle tick_count wrap-around:
   *   (int32_t)(tick_count - sleep_until) >= 0  is true when
   *   tick_count >= sleep_until even after the uint32_t counter wraps. */
  if (core_id() == 0) {
    for (uint32_t i = 0u; i < PROC_MAX; i++) {
      pcb_t *p = &proc_table[i];
      if (p->state == PROC_SLEEPING &&
          (int32_t)(tick_count - p->sleep_until) >= 0)
        p->state = PROC_RUNNABLE;
      /* PROC_BLOCKED + sleep_until: poll/select timeout.
       * Wake the process so syscall_restart re-checks the condition.
       * Leave sleep_until set so do_ppoll can detect the expired
       * deadline on re-entry and return 0 (timeout). */
      if (p->state == PROC_BLOCKED && p->sleep_until != 0 &&
          (int32_t)(tick_count - p->sleep_until) >= 0) {
        p->state = PROC_RUNNABLE;
        p->wait_channel = NULL;
      }
    }
  }

  /* Per-core: decrement time slice and pend PendSV when expired */
  if (!current) return;

  if (--current->ticks_remaining == 0u) {
    current->ticks_remaining = PROC_DEFAULT_TICKS;
    arch_yield(); /* trigger PendSV (runs after SysTick exits) */
  }
}

/* ── Idle-loop poll ──────────────────────────────────────────────────────────
 *
 * Called from main.c's idle loop after each hlt/wfi wake.
 * Fires VFS_EVENT_IDLE so TTY backends can check input and wake blocked
 * readers, then calls target_idle_poll() for target-specific work.
 */

int sched_idle_poll(void) {
  mod_vfs.notify(VFS_EVENT_IDLE);
  target_idle_poll();
  return arch_yield_consume();
}

/* ── Timer tick handler (shared logic) ───────────────────────────────────────
 */

/* Common tick processing: tick counter, input polling, CPU accounting.
 * Called from the architecture-specific timer ISR.
 * from_user: 1 if interrupted from user mode, 0 if from kernel/supervisor. */
void sched_timer_tick(int from_user) {
  /* CP/M processes run inside a kernel-resident emulator loop, so they
   * do not naturally return through the normal user-mode signal delivery
   * path.  Consume pending default/ignored signals at timer-preemption
   * boundaries on real hardware. */
  if (current && current->state == PROC_RUNNABLE &&
      current->subsys == SUBSYS_CPM &&
      (current->sig_pending & ~current->sig_blocked)) {
    signal_check_kernel();
  }

  /* Only Core 0 maintains the global tick counter */
  if (core_id() == 0) {
    tick_count++;
  }

  uint32_t cid = core_id();
  if (current && current->state == PROC_RUNNABLE && !current->is_idle) {
    if (from_user) {
      current->utime++;
      cpu_user_ticks[cid]++;
    } else {
      current->stime++;
      cpu_system_ticks[cid]++;
    }
  } else {
    cpu_idle_ticks[cid]++;
  }

  sched_tick();
}

/* ── Scheduler startup ──────────────────────────────────────────────────────
 *
 * Per-arch pre-IRQ setup lives in arch_sched_start_hook() (declared by
 * each arch's arch.h as a static inline).  Inlining matters on ARM:
 * sched_start() switches Thread mode MSP→PSP inside the hook, and any
 * real function call would straddle the stack switch with a push/pop LR
 * pair and corrupt the return path.  On other arches the hook has no
 * such constraint but uses the same pattern for consistency.
 */

void sched_start(void) {
  arch_sched_start_hook();
  arch_irq_enable();
}

/* ── Cooperative yield ───────────────────────────────────────────────────────
 */

/* On Xtensa, ESP-IDF's pthread library defines a non-weak sched_yield()
 * that conflicts with ours. Use a PPAP-specific scheduler API name
 * consistently on every target instead of carrying a target-local alias.
 *
 * arch_sched_switch() is ARM's arch_yield() (PendSV self-pend) by default,
 * and a direct trap-based switch on m68k/xtensa/ia16 whose arch_yield()
 * only sets a flag that nothing would consume from thread context. */
void sched_switch(void) { arch_sched_switch(); }

/* ── Channel-based wakeup ────────────────────────────────────────────────────
 */

void sched_wakeup(void *channel) {
  uint32_t saved = spin_lock_irqsave(SPIN_PROC);
  int woke = 0;
  for (uint32_t i = 0u; i < PROC_MAX; i++) {
    pcb_t *p = &proc_table[i];
    if (p->state == PROC_BLOCKED && p->wait_channel == channel) {
      p->state = PROC_RUNNABLE;
      p->wait_channel = NULL;
      woke = 1;
    }
  }
  spin_unlock_irqrestore(SPIN_PROC, saved);
  /* Trigger context switch so woken process runs promptly.
   * PendSV has lowest priority — fires after the current ISR returns. */
  if (woke) arch_yield();
}
