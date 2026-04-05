/*
 * ppap_m68k_bridge.h — PPAP cross-arch personality for eCPU-m68k
 *
 * When running m68k ELF binaries on a non-m68k host (e.g. ARM),
 * TRAP #0 is intercepted by the eCPU trap mechanism and translated
 * to PPAP syscalls using the m68k calling convention:
 *   d0 = syscall number
 *   d1–d5 = arguments 1–5
 *   a0 = argument 6
 *   return value in d0
 *
 * See docs/ecpu/m68k.md §Step 8 for the full design.
 */

#ifndef PPAP_KERNEL_CORE_SUBSYS_PPAP_M68K_BRIDGE_H
#define PPAP_KERNEL_CORE_SUBSYS_PPAP_M68K_BRIDGE_H

#include "kernel/core/cpu/cpu.h"
#include "kernel/core/cpu/ecpu_m68k.h"
#include "kernel/core/subsys/subsys.h"

/* ── Per-process eCPU-m68k execution state ─────────────────────────────── */

typedef struct {
  m68k_state_t m68k;
} ppap_m68k_exec_state_t;

/* ── Trap handler (personality layer) ──────────────────────────────────── */

/*
 * ppap_m68k_trap_handler — intercept TRAP #0 for PPAP syscalls.
 *
 * On CPU_TRAP_SWI with param == 0:
 *   Reads d0 = syscall number, d1–d5 = args, a0 = arg6.
 *   For SYS_EXIT/SYS_EXIT_GROUP: returns CPU_TRAP_EXIT.
 *   Otherwise: dispatches to syscall_dispatch() and writes result to d0.
 *
 * On CPU_TRAP_HALT: returns CPU_TRAP_EXIT.
 * All others: CPU_TRAP_UNHANDLED.
 */
int ppap_m68k_trap_handler(cpu_state_t *cpu, int trap_type, uint32_t param,
                           void *ctx);

#ifdef PPAP_KERNEL

#include "kernel/core/proc/proc.h"

/* Kernel-mode entry point for emulated m68k processes.
 * Called via proc_setup_stack — runs the eCPU interpreter loop. */
void ppap_m68k_run_process(void);

/* Subsystem ops (currently NULL — emulated m68k uses default behavior) */
extern const subsys_ops_t ppap_m68k_subsys_ops;

#endif /* PPAP_KERNEL */

#endif /* PPAP_KERNEL_CORE_SUBSYS_PPAP_M68K_BRIDGE_H */
