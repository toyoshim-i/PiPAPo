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

#include "sched.h" /* includes proc.h via sched.h */

#include <stddef.h>
#include <stdint.h>

#include "../fd/tty.h"  /* tty_rx_notify */
#include "../mm/page.h" /* PAGE_SIZE */
#include "../signal/signal.h"
#include "../spinlock.h" /* SPIN_PROC */
#include "arch/arch.h"
#include "arch/ioregs.h"

/* ── Tick counter ─────────────────────────────────────────────────────────────
 */

/* Incremented by SysTick_Handler every tick.  Declared here (before sched_tick
 * and sched_sleep) so both functions can access it in the same translation
 * unit. */
static volatile uint32_t tick_count = 0u;

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

  pcb_t *result = current; /* default: keep running */
  for (uint32_t i = 1u; i < PROC_MAX; i++) {
    uint32_t next = (idx + i) % PROC_MAX;
    if (proc_table[next].state == PROC_RUNNABLE &&
        proc_table[next].running_on_core < 0) {
      result = &proc_table[next];
      break;
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
       * Wake the process so svc_restart re-checks the condition.
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

/* ── Periodic polling callbacks (shared across architectures) ────────────────
 */

/* Optional input-available callbacks, registered by target via
 * sched_set_input_poll(). Returns non-zero if input is available (e.g. keyboard
 * FIFO has data). */
static int (*input_poll_fn)(void);
static int input_poll_tty_idx;

/* Optional second input poll (e.g. serial TTY on dual-console targets). */
static int (*input_poll_fn2)(void);
static int input_poll_tty_idx2;

/* Keyboard polling counter and deferred flag.
 * SysTick increments the counter; when it reaches the threshold it sets
 * input_poll_due = 1 instead of calling the (slow) I2C poll function
 * directly.  The actual poll happens in sched_display_poll(), which runs
 * from the idle loop in thread context — no interrupt blocking. */
static uint32_t input_poll_counter;
static volatile uint8_t input_poll_due;

void sched_set_input_poll(int (*fn)(void), int tty_idx) {
  input_poll_fn = fn;
  input_poll_tty_idx = tty_idx;
}

void sched_set_input_poll2(int (*fn)(void), int tty_idx) {
  input_poll_fn2 = fn;
  input_poll_tty_idx2 = tty_idx;
}

/* Optional display flush callback, registered by target via
 * sched_set_display_poll(). */
static void (*display_poll_fn)(void);

void sched_set_display_poll(void (*fn)(void)) { display_poll_fn = fn; }

void sched_display_poll(void) {
  /* Run deferred input poll (keyboard I2C) in thread context so that
   * UART and other IRQs are not blocked during the slow I2C transfer. */
  if (input_poll_due && input_poll_fn) {
    input_poll_due = 0;
    if (input_poll_fn()) tty_rx_notify(input_poll_tty_idx);
    if (input_poll_fn2 && input_poll_fn2()) tty_rx_notify(input_poll_tty_idx2);
  }
  if (display_poll_fn) display_poll_fn();
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

    /* Defer input poll to idle loop every 2 ticks (20 ms).
     * The actual I2C transaction runs in sched_display_poll() (thread
     * context) so it does not block UART and other IRQs. */
    if (input_poll_fn && ++input_poll_counter >= 2u) {
      input_poll_counter = 0;
      input_poll_due = 1;
    }
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

/* ── Architecture-specific: timer ISR + scheduler startup ───────────────────
 */

#if defined(__ARM_ARCH) || defined(__arm__) || defined(__thumb__)

/* ARM Cortex-M SysTick exception handler.
 *
 * CPU tick accounting needs the EXC_RETURN value that the hardware places in
 * LR on exception entry.  A normal C function's prologue clobbers LR, so we
 * use a naked wrapper to capture it and pass it as the first argument.
 *
 * EXC_RETURN bit 3:  1 = return to Thread mode (user),
 *                    0 = return to Handler mode (kernel).
 *
 * Note: ICSR.RETTOBASE (bit 11) is RAZ on ARMv6-M / Cortex-M0+, so we
 * cannot use it for user-vs-kernel distinction. */

__attribute__((used)) static void SysTick_Handler_c(uint32_t exc_return);

__attribute__((naked)) void SysTick_Handler(void) {
  __asm__ volatile(
      "push {r0, lr}\n" /* 8-byte aligned; save EXC_RETURN for return */
      "mov  r0, lr\n"   /* pass EXC_RETURN as first argument */
      "bl   SysTick_Handler_c\n"
      "pop  {r0, pc}\n" /* pop EXC_RETURN into PC → exception return */
  );
}

static void SysTick_Handler_c(uint32_t exc_return) {
  sched_timer_tick((exc_return & (1u << 3)) != 0);
}

/* ── Scheduler startup (ARM) ────────────────────────────────────────────────
 */

void sched_start(void) {
  /* Set PendSV to lowest priority (0xFF) so it never preempts real IRQs.
   * SHPR3[23:16] is the PendSV priority byte on Cortex-M0+. */
  SCB_SHPR3 = (SCB_SHPR3 & ~PENDSV_PRIO_MASK) | PENDSV_PRIO_LOWEST;

  /* Lower SVCall priority (0x80) so hardware interrupts (SysTick, UART)
   * can preempt the SVC handler.  Without this, WFI inside blocking
   * syscalls (e.g. tty_read) would never wake — no interrupt can preempt
   * a handler at the default priority 0x00. */
  SCB_SHPR2 = (SCB_SHPR2 & ~SVCALL_PRIO_MASK) | (0x80u << SVCALL_PRIO_SHIFT);

  /*
   * Switch Thread mode from MSP to PSP using Thread 0's dedicated stack.
   */
  uint32_t psp_top = (uint32_t)(uintptr_t)proc_table[0].stack_page + PAGE_SIZE;
  __asm__ volatile(
      "msr  psp, %0      \n" /* PSP = top of Thread 0's stack page */
      "movs r0, #2       \n" /* CONTROL.SPSEL = 1 */
      "msr  control, r0  \n"
      "isb               \n" ::"r"(psp_top)
      : "r0");

  /* Configure SysTick: reload value, clear current count, start. */
  SYST_RVR = SYSTICK_RELOAD;
  SYST_CVR = 0u;
  SYST_CSR = SYST_CSR_ENABLE | SYST_CSR_TICKINT | SYST_CSR_CLKSOURCE;

  /* Enable interrupts — scheduler is now live. */
  arch_irq_enable();
}

#elif defined(__m68k__)

/* ── Scheduler startup (m68k) ───────────────────────────────────────────────
 */

/* M68K timer ISR and context switch are implemented in Phase C Steps 2-5.
 * sched_timer_tick() is called from the m68k timer ISR. */

void sched_start(void) {
  /* M68K: no PSP/MSP split, no PendSV priorities to set.
   * Timer ISR setup is done by target_late_init().
   * Just enable interrupts to start the scheduler. */
  arch_irq_enable();
}

#elif defined(__riscv)

/* ── Scheduler startup (RISC-V) ────────────────────────────────────────────
 */

void sched_start(void) {
  /* RISC-V: no PSP/MSP split, no PendSV priorities.
   * Timer ISR setup is done by target_late_init() → riscv_timer_init().
   * Just enable interrupts to start the scheduler. */
  arch_irq_enable();
}

#endif /* __ARM_ARCH / __m68k__ / __riscv */

/* ── Cooperative yield ───────────────────────────────────────────────────────
 */

void sched_yield(void) {
#if defined(__m68k__)
  /* TRAP #1 enters m68k_trap1_handler which does the context switch
   * immediately.  arch_yield() only sets a flag — insufficient from
   * thread context where no timer ISR is pending to check it. */
  __asm__ volatile("trap #1");
#else
  arch_yield(); /* pend PendSV; fires at next instruction boundary */
#endif
}

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

/* ── Sleep ────────────────────────────────────────────────────────────────────
 */

void sched_sleep(uint32_t ticks) {
  current->sleep_until = tick_count + ticks;
  current->state = PROC_SLEEPING;
#if defined(__m68k__)
  __asm__ volatile("trap #1"); /* immediate context switch */
#else
  arch_yield(); /* yield CPU; PendSV fires after caller returns */
#endif
  /* Execution resumes here after sched_tick() marks us RUNNABLE again
   * and the context switch restores our context. */
}
