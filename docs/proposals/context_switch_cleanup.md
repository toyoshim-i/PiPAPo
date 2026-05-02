# Context Switch Cleanup

**Status:** proposed cleanup.  The current context-switch contract is
documented in [`../kernel/context_switch.md`](../kernel/context_switch.md);
kernel-stack mechanics are documented in [`../kernel/stack.md`](../kernel/stack.md).

## Context Switch Work

1. Rename shared restart symbols from ARM-flavored `svc_*` names to syscall
   names:
   - `svc_restart[]` -> `syscall_restart[]`
   - `svc_saved_a0[]` -> `syscall_saved_arg0[]`
   - `svc_set_restart()` -> `syscall_set_restart()`
   - `mod_core.svc_set_restart` -> `mod_core.syscall_set_restart`
2. Keep ARM-only names for ARM-only state, such as `svc_exc_return[]`.
3. Normalize context-switch helper names where useful:
   - `riscv_do_switch` -> `riscv_ctx_switch`
   - extract m68k inline switch bodies behind `m68k_ctx_switch`
   - decide whether `i16_sched_yield` should stay public or become
     `i16_ctx_switch`
   - extract/rename Xtensa helper glue as `xtensa_ctx_switch` if it improves
     readability
4. Add a blocking-yield test that proves one process can block inside a
   syscall loop while another runnable process executes before the blocked
   syscall returns.
5. Document the context-switch contract for new architectures in
   `docs/getting_started/porting.md`.

## Kernel Stack Work

The stack items support the same continuation-blocking model and should stay
behavior-preserving unless a subtask explicitly calls for measurement-driven
sizing.

1. Decide whether RISC-V should keep its lazy `kernel_sp` initialization from
   the process stack page or move to the fixed-region helper used by ia16 and
   ARM targets with `PPAP_ARM_KSTACK_REGION`.
2. Add ARM kernel-stack high-water tracking equivalent to ia16
   `KSTACK_USAGE_TRACK`.
3. Measure pico1calc stack usage with CP/M and other deep subsystem paths.
4. Shrink pico1calc `PROC_KSTACK_SIZE` from 2 KB only if measurements show a
   safe margin.

## Constraints

- Do not reintroduce PendSV preempting SVC on ARM.  ARM's accepted synchronous
  path is `arm_kernel_sched_switch()`.
- Keep each rename or extraction bisectable and behavior-neutral.
- Treat restart and continuation blocking as separate mechanisms; do not
  convert side-effectful blocking syscalls to replay-based restart.
- Do not shrink ia16's 1 KB user-process kernel stack as part of this cleanup.
