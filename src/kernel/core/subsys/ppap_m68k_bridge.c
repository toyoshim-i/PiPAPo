/*
 * ppap_m68k_bridge.c — PPAP cross-arch personality for eCPU-m68k
 *
 * Translates m68k TRAP #0 into PPAP syscalls when running m68k ELF
 * binaries on a non-m68k host via the eCPU interpreter.
 *
 * m68k syscall ABI:
 *   d0 = syscall number
 *   d1 = arg1, d2 = arg2, d3 = arg3, d4 = arg4, d5 = arg5
 *   a0 = arg6
 *   return value → d0
 */

#include "kernel/core/subsys/ppap_m68k_bridge.h"

#include "common/syscall_nr.h"

/* ── Trap handler ──────────────────────────────────────────────────────── */

int ppap_m68k_trap_handler(cpu_state_t *state, int trap_type, uint32_t param,
                           void *ctx) {
  (void)ctx;
  m68k_state_t *cpu = (m68k_state_t *)state;

  if (trap_type == CPU_TRAP_SWI && param == 0) {
    /* TRAP #0 = PPAP syscall */
    uint32_t nr = cpu->d[0];

    /* Fast path: exit */
    if (nr == SYS_EXIT || nr == SYS_EXIT_GROUP) return CPU_TRAP_EXIT;

#ifdef PPAP_KERNEL
    /* Build argument frame for syscall_dispatch.
     * syscall_dispatch(frame, nr, a4, a5) where:
     *   frame[0..3] = args 1–4  (d1–d4)
     *   a4 = arg5 (d5)
     *   a5 = arg6 (a0) */
    uint32_t frame[4];
    frame[0] = cpu->d[1];
    frame[1] = cpu->d[2];
    frame[2] = cpu->d[3];
    frame[3] = cpu->d[4];

    extern void syscall_dispatch(uint32_t * frame, uint32_t nr, uint32_t a4,
                                 uint32_t a5);
    syscall_dispatch(frame, nr, cpu->d[5], cpu->a[0]);

    /* Return value is in frame[0] */
    cpu->d[0] = frame[0];
#endif
    return CPU_TRAP_HANDLED;
  }

  if (trap_type == CPU_TRAP_HALT) return CPU_TRAP_EXIT;

  return CPU_TRAP_UNHANDLED;
}

/* ── Kernel-only: process entry point and subsystem ops ────────────────── */

#ifdef PPAP_KERNEL

#include "kernel/core/proc/proc.h"
#include "kernel/core/proc/sched.h"
#include "kernel/core/syscall/syscall.h"

/*
 * ppap_m68k_run_process — kernel-mode entry for emulated m68k processes.
 *
 * The scheduler "returns" into this function after proc_setup_stack().
 * It runs the m68k emulator loop until SYS_EXIT causes CPU_TRAP_EXIT,
 * then exits the process with d1 as the exit status.
 */
void ppap_m68k_run_process(void) {
  pcb_t *p = current;
  m68k_state_t *m68k = (m68k_state_t *)p->subsys_data;

  for (;;) {
    int do_step_stop = p->trace_step_pending != 0;
    int run_with_step =
        do_step_stop || (p->tracer_pid != 0 && trace_has_swbp());

    if (run_with_step) {
      p->trace_step_pending = 0;

      if (trace_maybe_stop_at_swbp(PPAP_TRACE_ABI_PPAP, m68k->pc)) {
        if (p->state == PROC_TRACED_STOP) sched_switch();
        continue;
      }

      /* Single-step: set budget=1 and call run() */
      m68k->step_budget = 1;
      ecpu_m68k_ops.run((cpu_state_t *)m68k);
      if (m68k->step_trap_exit) break;
      if (do_step_stop && p->tracer_pid != 0 && p->state == PROC_RUNNABLE)
        trace_debug_stop(PPAP_TRACE_ABI_PPAP, m68k->pc, PPAP_DEBUG_STOP_STEP);
      if (p->state == PROC_TRACED_STOP) sched_switch();
      continue;
    }

    ecpu_m68k_ops.run((cpu_state_t *)m68k);
    break;
  }

  /* Exit status is in d1 (first arg to SYS_EXIT) */
  sys_exit((long)m68k->d[1]);
  /* not reached */
}

/* Subsystem ops — emulated m68k uses default kernel behavior */
const subsys_ops_t ppap_m68k_subsys_ops = {
    .on_crash = (void *)0,
    .on_signal = (void *)0,
    .on_init = (void *)0,
};

#endif /* PPAP_KERNEL */
