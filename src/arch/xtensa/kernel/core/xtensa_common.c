/*
 * xtensa_common.c — Shared Xtensa architecture state
 *
 * Provides:
 *   - Context switch pending flag (checked by timer ISR / yield)
 *   - CCOMPARE0 timer init and ISR
 *   - xtensa_ctx_switch() — C helper called from switch.S
 *   - Syscall dispatch (via ESP-IDF exception table)
 *   - Exception/fault handler (crash reporting)
 */

#include <stdint.h>

#include "kernel/common/ioregs.h"
#include "kernel/common/mod/mod_vfs.h"
#include "kernel/core/esp_hooks.h"
#include "kernel/core/proc/proc.h" /* arch.h: switch_pending, arch_sched_switch */
#include "kernel/core/proc/sched.h"
#include "kernel/core/syscall/syscall.h"
#include "xtensa_api.h"

/* Timer ready flag — set by xtensa_timer_init().
 * arch_preempt_enable() checks this to avoid enabling the timer interrupt
 * before the ISR is registered. */
volatile uint32_t xtensa_timer_ready = 0;
volatile uint32_t xtensa_trap_ready = 0;

/* Tick counter — incremented by timer ISR. */
volatile uint32_t xtensa_tick_count = 0;

void xtensa_syscall_body(XtExcFrame *frame);
void xtensa_syscall_on_kstack(XtExcFrame *frame, uintptr_t kernel_sp);
static void xtensa_syscall_handler(XtExcFrame *frame);
static void xtensa_fault_handler(XtExcFrame *frame);

/* Combined handler for EXCCAUSE=0 (IllegalInstruction).
 *
 * PPAP user-space uses the ILL instruction (opcode 0x000000) as the syscall
 * trap instead of SYSCALL (EXCCAUSE=1), because ESP-IDF's _xt_user_exc
 * intercepts EXCCAUSE=1 with a hardcoded stub that bypasses
 * _xt_exception_table.  EXCCAUSE=0 falls through to the table dispatch.
 *
 * This handler reads the 3-byte instruction at EPC1:
 *   - If it is ILL (0x000000): dispatch as a PPAP syscall
 *   - Otherwise: fall through to the fault handler
 */
static void xtensa_ill_handler(XtExcFrame *frame) {
  /* Read the 3-byte instruction at the faulting PC.
   * The exception frame is fully saved, so EPC1 == frame->pc. */
  uint32_t pc = (uint32_t)frame->pc;
  /* ILL is 3 bytes of zero.  Read the containing 32-bit word from the
   * instruction bus (IRAM or flash — both support aligned word reads). */
  uint32_t word = *(volatile uint32_t *)(pc & ~3u);
  uint32_t shift = (pc & 3u) * 8;
  uint32_t insn = (word >> shift) & 0xFFFFFFu;

  if (insn == 0x000000u) {
    /* ILL-as-syscall: dispatch through the normal syscall path */
    xtensa_syscall_handler(frame);
    return;
  }

  /* Real illegal instruction — fall through to fault handler */
  xtensa_fault_handler(frame);
}

static void xtensa_install_exception_handlers(void) {
  /* EXCCAUSE=0 (IllegalInstruction): combined ILL-as-syscall + fault */
  xt_set_exception_handler(0, xtensa_ill_handler);
  _xt_exception_table[0] = xtensa_ill_handler;

  /* EXCCAUSE=1 (Syscall): ESP-IDF intercepts this before the table,
   * so our handler here is just a safety net — it should never fire. */
  _xt_exception_table[EXCCAUSE_SYSCALL] = xtensa_fault_handler;

  for (int i = 2; i < 30; i++) {
    if (i == (int)EXCCAUSE_LEVEL1_INT || i == (int)EXCCAUSE_ALLOCA) continue;
    xt_set_exception_handler(i, xtensa_fault_handler);
    _xt_exception_table[i] = xtensa_fault_handler;
  }
}

/* ── CCOMPARE0 timer ─────────────────────────────────────────────────────── */

/* Timer ISR — called from ESP-IDF's level-1 interrupt dispatch.
 * Rearms CCOMPARE0 and calls the shared scheduler tick handler. */
static void xtensa_timer_isr(void *arg) {
  (void)arg;
  uint32_t cmp;
  __asm__ volatile("rsr %0, ccompare0" : "=a"(cmp));
  __asm__ volatile("wsr %0, ccompare0" ::"a"(cmp + XTENSA_TICK_INTERVAL));
  __asm__ volatile("esync");
  xtensa_tick_count++;
  sched_timer_tick(0);
}

void xtensa_timer_init(void) {
  /* Disable FreeRTOS's interrupt-level context switching.
   * _frxt_int_enter/_frxt_int_exit check this flag; when 0,
   * they skip TCB save/restore and ISR stack switch.
   * PPAP manages its own context switching. */
  port_xSchedulerRunning[0] = 0;

  /* Set CCOMPARE0 for first tick */
  uint32_t cc;
  __asm__ volatile("rsr %0, ccount" : "=a"(cc));
  __asm__ volatile("wsr %0, ccompare0" ::"a"(cc + XTENSA_TICK_INTERVAL));
  __asm__ volatile("esync");

  /* Register the CCOMPARE0 timer ISR through the Xtensa helper API.
   * Direct table patching regressed startup on hardware; keep this
   * stable path while exception/timer ownership is refactored. */
  xt_set_interrupt_handler(XTENSA_TIMER0_INTNUM, xtensa_timer_isr, (void *)0);

  /* Set INTENABLE to ONLY the CCOMPARE0 bit (bit 6).
   * This replaces whatever FreeRTOS had configured (including bit 12
   * for SYSTIMER).  Do not OR — explicitly set to prevent stray
   * interrupts from firing. */
  __asm__ volatile("wsr %0, intenable" ::"a"(XTENSA_TIMER0_INTMASK));
  __asm__ volatile("rsync");

  xtensa_timer_ready = 1;
}

/* ── Context switch helper ───────────────────────────────────────────────── */

/* Called from switch.S (xtensa_do_yield) with the current SP.
 * Saves SP to old PCB, picks next process, returns new SP. */
uint32_t xtensa_ctx_switch(uint32_t current_sp) {
  if (current) current->sp = current_sp;
  pcb_t *next = sched_next();
  current_core[core_id()] = next;
  return next->sp;
}

/* ── Syscall handler ────────────────────────────────────────────────────────
 */

/* Syscall body — called from xtensa_syscall_handler when the faulting
 * instruction is ILL (PPAP's syscall trap).  The XtExcFrame has all
 * registers saved; we extract syscall args from a2-a7 and call the
 * shared syscall_dispatch(). */
void xtensa_syscall_body(XtExcFrame *frame) {
  /* Advance PC past the 3-byte ILL instruction */
  frame->pc += 3;

  /* syscall_restart_loop(frame, nr, a4, a5):
   *   frame[0..3] = a2,a3,a4,a5 (contiguous in XtExcFrame)
   *   nr          = a7 (syscall number)
   *   a4, a5      = 5th/6th args
   * The wrapper loops on current->syscall_needs_restart per process —
   * sched_switch() is synchronous on xtensa, so the global syscall_restart[]
   * / syscall_saved_arg0[] pair are not safe across processes. */
  syscall_restart_loop((uint32_t *)&frame->a2, (uint32_t)frame->a7,
                       (uint32_t)frame->a4, (uint32_t)frame->a5);

  /* exec_pending: execve built a new-process frame at current->sp.
   * Reload the frame's PC/PS/SP so rfe jumps to the new program.
   * Frame layout (byte offsets): [16]=exit, [20]=PC, [24]=PS, [28]=SP. */
  if (exec_pending[0]) {
    exec_pending[0] = 0;
    uint32_t *nf = (uint32_t *)(uintptr_t)current->sp;
    frame->pc = nf[XTENSA_SOL_PC_WORD];
    frame->ps = nf[XTENSA_SOL_PS_WORD];
    frame->a1 = nf[XTENSA_SOL_SP_WORD];
    frame->a2 = 0;
  }

  /* Context switch via cooperative yield.
   *
   * Xtensa has no PendSV — we can't modify the exception frame to switch
   * to a process with a solicited (windowed-ABI) context.  Instead, call
   * sched_switch() which invokes xtensa_do_yield() to perform a proper
   * save/restore through the windowed call chain.
   *
   * When sched_switch() returns, we're back in THIS handler, which then
   * returns to ESP-IDF's dispatcher → rfe → user code.
   *
   * Yield if:
   *  - preemption pending (timer slice expired)
   *  - current process blocked (e.g. read() with no data)
   */
  if (switch_pending ||
      (current && current->state != PROC_RUNNABLE && !current->is_idle)) {
    switch_pending = 0;
    sched_switch();
  }
}

/* ESP-IDF exception-table entry point.  Keep this wrapper separate from the
 * PPAP syscall body so the fixed-kstack migration can switch stacks before
 * running syscall_dispatch() and the cooperative yield path. */
static void xtensa_syscall_handler(XtExcFrame *frame) {
  if (current && current->kernel_sp != 0u) {
    xtensa_syscall_on_kstack(frame, (uintptr_t)current->kernel_sp);
    return;
  }
  xtensa_syscall_body(frame);
}

/* ── Fault handler ───────────────────────────────────────────────────────── */

static const char *exccause_name(uint32_t cause) {
  static const char *names[] = {
      [0] = "IllegalInsn",   [1] = "Syscall",        [2] = "InsnFetchErr",
      [3] = "LoadStoreErr",  [4] = "Level1Int",      [5] = "Alloca",
      [6] = "IntDivZero",    [8] = "Privileged",     [9] = "LoadAlign",
      [12] = "InsnPIF_Data", [13] = "InsnPIF_Addr",  [14] = "LoadPIF_Data",
      [15] = "LoadPIF_Addr", [16] = "StorePIF_Data", [17] = "StorePIF_Addr",
      [20] = "InsnTLBMiss",  [24] = "LoadTLBMiss",   [28] = "StoreTLBMiss",
      [29] = "CoprocessorN",
  };
  if (cause < sizeof(names) / sizeof(names[0]) && names[cause])
    return names[cause];
  return "Unknown";
}

/* Called for all unhandled exceptions (illegal insn, load/store error, etc.).
 * Prints crash info for debugging, kills user processes, halts on kernel
 * faults. */
static void xtensa_fault_handler(XtExcFrame *frame) {
  uint32_t cause = (uint32_t)frame->exccause;
  mod_vfs.klogf("EXCEPTION: %s (cause=%lu) pc=%lx vaddr=%lx\n",
                exccause_name(cause), (unsigned long)cause,
                (unsigned long)(uint32_t)frame->pc,
                (unsigned long)(uint32_t)frame->excvaddr);
  mod_vfs.klogf(
      "  a0=%lx a1=%lx a2=%lx a3=%lx a4=%lx a5=%lx\n",
      (unsigned long)(uint32_t)frame->a0, (unsigned long)(uint32_t)frame->a1,
      (unsigned long)(uint32_t)frame->a2, (unsigned long)(uint32_t)frame->a3,
      (unsigned long)(uint32_t)frame->a4, (unsigned long)(uint32_t)frame->a5);
  mod_vfs.klogf(
      "  a6=%lx a7=%lx a8=%lx a9=%lx a10=%lx a11=%lx\n",
      (unsigned long)(uint32_t)frame->a6, (unsigned long)(uint32_t)frame->a7,
      (unsigned long)(uint32_t)frame->a8, (unsigned long)(uint32_t)frame->a9,
      (unsigned long)(uint32_t)frame->a10, (unsigned long)(uint32_t)frame->a11);

  if (current && !current->is_idle) {
    mod_vfs.klogf("  pid=%u comm=%s — killed\n", current->pid, current->comm);
    current->state = PROC_ZOMBIE;
    current->exit_status = 128 + 11; /* SIGSEGV */
    /* This is exception cleanup, not a resumable process continuation.
     * Normal blocking syscalls switch under xtensa_syscall_on_kstack(). */
    /* Must actually switch — arch_yield() only sets a flag, which
     * wouldn't be checked before rfe returns to the faulting instr. */
    sched_switch();
    return;
  }

  /* Kernel fault: halt */
  mod_vfs.klogf("PANIC: kernel exception — halting\n");
  for (;;) __asm__ volatile("waiti 15");
}

/* ── ESP-IDF abort/assert override ──────────────────────────────────────── */

/* Override ESP-IDF's abort() and __assert_func() so kernel-level panics
 * (heap corruption, assertion failures, etc.) go through our handler
 * with a clear message instead of triggering an IllegalInstruction via
 * ESP-IDF's panic_abort(). */

/* Linked via -Wl,--wrap=abort and -Wl,--wrap=__assert_func
 * (set in CMakeLists.txt).  The __wrap_ prefix replaces the original. */

void __wrap_abort(void) {
  mod_vfs.klogf("ABORT called");
  if (current && !current->is_idle) {
    mod_vfs.klogf(" (pid=%u comm=%s)\n", current->pid, current->comm);
    current->state = PROC_ZOMBIE;
    current->exit_status = 128 + 6; /* SIGABRT */
    arch_yield();
    /* Try to let scheduler run — if we return, hang. */
  } else {
    mod_vfs.klogf(" — kernel abort, halting\n");
  }
  for (;;) __asm__ volatile("waiti 15");
}

void __wrap___assert_func(const char *file, int line, const char *func,
                          const char *expr) {
  mod_vfs.klogf("ASSERT FAILED: %s:%lu", file ? file : "?",
                (unsigned long)(uint32_t)line);
  if (func) mod_vfs.klogf(" (%s)", func);
  if (expr) mod_vfs.klogf(": %s", expr);
  mod_vfs.klogf("\n");
  __wrap_abort();
}

/* ── Trap initialization ─────────────────────────────────────────────────── */

void xtensa_trap_init(void) {
  xtensa_install_exception_handlers();
  xtensa_trap_ready = 1;
}

void xtensa_trap_reassert(void) { xtensa_install_exception_handlers(); }

/* ── Initial stack frame for new processes ──────────────────────────────── */

uint32_t *xtensa_build_vfork_child_frame(uint32_t kernel_sp, uint32_t child_pc,
                                         uint32_t child_user_sp,
                                         uint32_t child_a0, uint32_t child_a3) {
  uint32_t *sp = (uint32_t *)(uintptr_t)kernel_sp;
  sp = (uint32_t *)((uintptr_t)sp & ~0xFu);

  *--sp = child_a3;      /* [SP+36] a3 = preserved reg */
  *--sp = child_a0;      /* [SP+32] a0 = return addr */
  *--sp = child_user_sp; /* [SP+28] user SP */
  *--sp = (1u << 5);     /* [SP+24] PS: UM=1 */
  *--sp = child_pc;      /* [SP+20] entry */
  *--sp = 1u;            /* [SP+16] exit = 1 */
  *--sp = 0;             /* [SP+12] ABI scratch */
  *--sp = 0;             /* [SP+8]  ABI scratch */
  *--sp = 0;             /* [SP+4]  ABI scratch */
  *--sp = 0;             /* [SP+0]  ABI scratch */
  return sp;
}

/* Build a "new-process" frame for switch.S (exit marker = 1).
 *
 * Layout (36 bytes used, 16-byte aligned):
 *   [SP+0 ..+15] ABI scratch area (unused by PPAP metadata)
 *   [SP+16] = 1       (exit marker — triggers direct-jump path in switch.S)
 *   [SP+20] = entry   (entry address, NOT windowed-encoded)
 *   [SP+24] = PS      (0: supervisor mode, WOE=0, INTLEVEL=0)
 *   [SP+28] = user_sp (stack pointer passed to user code in a1)
 *   [SP+32] = a0      (return address; needed by vfork child, 0 for exec)
 *
 * switch.S detects exit != 0, loads entry/PS/user_sp/a0, and does jx.
 * This avoids the base-save-area overlap that would corrupt a1 with the PC.
 *
 * When 'sp' points to the argc/argv area on the user stack, this stores it as
 * user_sp.  Callers that build the frame on a fixed kstack patch SOL_SP after
 * this helper returns.
 */
uint32_t *arch_build_initial_frame(uint32_t *sp, void (*entry)(void)) {
  uint32_t user_sp = (uint32_t)(uintptr_t)sp;
  sp = (uint32_t *)((uintptr_t)sp & ~0xFu); /* 16-byte align */

  *--sp = 0;       /* [SP+36] a3 = 0 (unused by _start) */
  *--sp = 0;       /* [SP+32] a0 = 0 (unused by _start) */
  *--sp = user_sp; /* [SP+28] user SP */
  /* PS for new process entry:
   * WOE=0 (call0 ABI), INTLEVEL=0 (interrupts enabled), UM=1.
   * UM=1 routes exceptions through UserExceptionVector, which is where
   * PPAP's syscall and fault handlers are registered via
   * _xt_exception_table.  UM=0 would route to KernelExceptionVector
   * (just `break 1, 0` in ESP-IDF) — silently halting on first syscall.
   */
  *--sp = (1u << 5);                  /* [SP+24] PS: UM=1 */
  *--sp = (uint32_t)(uintptr_t)entry; /* [SP+20] entry addr */
  *--sp = 1u;                         /* [SP+16] exit = 1 */

  /* Keep the low ABI scratch area reserved to avoid metadata clobbering
   * by windowed-call spill/restore helpers. */
  *--sp = 0;
  *--sp = 0;
  *--sp = 0;
  *--sp = 0;
  return sp;
}
