# Context Switch Cleanup

**Status:** proposed architecture cleanup.  The current context-switch
contract is documented in
[`../kernel/context_switch.md`](../kernel/context_switch.md); kernel-stack
mechanics are documented in [`../kernel/stack.md`](../kernel/stack.md).

## Goal

Establish one long-term context-switch model for all architectures:

- Every process has a kernel stack that can hold a suspended kernel
  continuation.
- `sched_switch()` performs a real cooperative switch even when called while
  the process is already executing kernel code.
- When the blocked process is scheduled again, it resumes the same kernel call
  chain at the instruction after `sched_switch()`.
- Restartable syscalls remain a separate replay mechanism for operations that
  are explicitly safe to re-execute.

This makes blocking syscalls, VFS waits, pipes, TTY waits, subsystem bridges,
and future kernel services use the same continuation-blocking contract across
ARM, ia16, m68k, RISC-V, Xtensa, and later ports.

An architecture may also keep a native interrupt stack, such as ARM MSP, for
fast system interrupt handling.  That is an optional optimization.  It must not
be required for correctness, and it must not make the process-kernel-stack
continuation path architecture-specific from the shared kernel's point of
view.

## Target Contract

Each architecture should provide:

1. A per-process kernel stack or equivalent saved kernel-continuation frame.
2. A saved context pointer in `pcb_t.sp`.
3. If the architecture uses a separate kernel-stack top, a live saved value in
   `pcb_t.kernel_sp`.
4. An `arch_sched_switch()` implementation that switches immediately enough to
   satisfy the `sched_switch()` contract.
5. A trap or return path that can restore either a normal user context or a
   suspended kernel continuation.

The shared kernel should not need to know whether the architecture uses
hardware stack switching, `mscratch`, separate USP/SSP, register windows, or a
far-call real-mode stack.  Those details belong behind the architecture switch
entry points.

## Native Interrupt Stacks

Native interrupt stacks are allowed, but only as an optimization layer:

- Timer, device, and fault handlers may run on an architecture-native stack
  when that is cheaper or required by hardware.
- Blocking kernel paths still need a per-process continuation stack.
- If an interrupt stack requests rescheduling, the request may be deferred to
  a trap-return point unless the architecture already has a safe direct switch
  path.
- The interrupt stack must not become the only place where a blocked syscall
  can be suspended.

For ARM Cortex-M, this means MSP may remain useful for exception entry and
short interrupt handling, but blocked non-restart syscalls should continue to
use an explicit kernel-continuation switch path such as
`arm_kernel_sched_switch()`.  PendSV must remain an asynchronous preemption
mechanism, not a way to preempt active SVC execution.

## Progress Summary

Done:

- ARM-M uses fixed per-process kernel-stack slots on every ARM target.
- ARM-M has an explicit synchronous continuation switch,
  `arm_kernel_sched_switch()`, for blocked non-restart syscalls in SVC.
- ARM-M comments distinguish PendSV async preemption, restart-style replay,
  and synchronous SVC continuation switching.
- The common fixed-region kstack helper owns canaries and optional
  `KSTACK_USAGE_TRACK` high-water tracking for ARM-M, ia16, and RISC-V.
- The userland empty-pipe blocking test covers continuation blocking without
  adding test-only kernel code.  It passes on `qemu_arm`, `qemu_m68k`,
  `qemu_rv32`, and `pcxt --hdd`.
- RISC-V `sched_switch()` now uses a machine-mode `ecall`, so a blocking
  syscall suspends at the call site on the process kernel stack instead of
  waiting for a later trap return.
- RISC-V now uses fixed per-process kernel-stack slots on `qemu_rv32` and
  `pico2rv`; `mscratch` is loaded from `pcb_t.kernel_sp` rather than lazily
  deriving the kernel stack from `stack_page_id`.
- RISC-V native ELF processes no longer allocate the old placeholder
  `stack_page_id` page; the user stack remains a tracked page in
  `user_pages[USER_PAGES_MAX - 1]`.
- Kernel-stack-use reduction for deep subsystem paths is deferred in
  [`kernel_stack_use.md`](kernel_stack_use.md), because userland subsystem
  work may obsolete much of that path.

Remaining:

- Xtensa needs an explicit decision: accept the current solicited-frame stack
  as its kernel-continuation equivalent, or add a separate kernel stack.
- Optional naming cleanup remains for arch switch helpers (`*_ctx_switch`).
- ARM stack-size reduction is deferred until measurements are useful after, or
  independent of, userland subsystem migration.

## Context Switch Work

1. DONE: audit ARM-M, ia16, m68k, RISC-V, and Xtensa against the target
   contract in this proposal and the stack/context-switch docs.
2. PARTIAL: move architectures toward an explicit per-process
   kernel-continuation model where they do not already have one.  RISC-V now
   uses a machine-mode `ecall` continuation switch; Xtensa still needs a design
   decision.
3. DONE: rename shared restart symbols from ARM-flavored `svc_*` names to
   syscall names:
   - `svc_restart[]` -> `syscall_restart[]`
   - `svc_saved_a0[]` -> `syscall_saved_arg0[]`
   - `svc_set_restart()` -> `syscall_set_restart()`
   - `mod_core.svc_set_restart` -> `mod_core.syscall_set_restart`
4. REMAINING: keep ARM-only names for ARM-only state, such as
   `svc_exc_return[]`.
5. REMAINING: normalize context-switch helper names where useful:
   - RISC-V already uses `riscv_ctx_switch`
   - m68k switch paths now share `m68k_ctx_switch`
   - ia16 now uses `i16_ctx_switch`
   - extract/rename Xtensa helper glue as `xtensa_ctx_switch` if it improves
     readability
6. DONE: keep the userland blocking-pipe test as coverage for one process
   blocking inside a syscall while another runnable process executes before
   the blocked syscall returns.
7. DONE: document the context-switch contract for new architectures in
   `docs/getting_started/porting.md`.

## Kernel Stack Work

The stack items support the single continuation-blocking model.  They should
stay behavior-preserving unless a subtask explicitly calls for
measurement-driven sizing.

1. DONE: prefer a common fixed-region helper for per-process kernel stacks
   where it fits the target memory map.  ARM-M, ia16, and RISC-V use it today.
2. DONE: move RISC-V away from lazy `kernel_sp` initialization from the
   process stack page.  `qemu_rv32` and `pico2rv` now reserve fixed kstack
   regions and initialize slots through `proc_kstack_init_slot`.
3. DONE: treat architectures that use a native interrupt stack as having two
   stack roles: an interrupt stack for short handlers and a process kernel
   stack for continuations.
4. DONE: use the optional common fixed-region `KSTACK_USAGE_TRACK` helper on
   ARM when measuring kernel-stack high-water marks.
5. DEFERRED: measure pico1calc stack usage with CP/M and other deep subsystem
   paths.  See [`kernel_stack_use.md`](kernel_stack_use.md).
6. DEFERRED: shrink pico1calc `PROC_KSTACK_SIZE` from 2 KB only if
   measurements show a safe margin.

## Architecture Notes

### `arm_m`

Current state:

- All ARM-M targets reserve fixed per-process MSP slots in their target linker
  scripts.
- PendSV handles asynchronous preemption and saves/restores PSP.  It also
  saves/restores `pcb_t.kernel_sp`.
- `SVC_Handler` can switch MSP onto `current->kernel_sp` while running the
  syscall body, then restore the original exception-entry MSP through
  `pcb_t.svc_msp`.
- `arch_sched_switch()` calls `arm_kernel_sched_switch()` only when Handler
  mode is blocked without restart; otherwise it pends PendSV.
- `arm_kernel_sched_switch()` saves the in-flight MSP continuation, marks
  `pcb_t.kernel_context`, switches to the next process, and restores either a
  suspended kernel continuation or a normal user/PSP context.
- ARM comments now explicitly separate PendSV async preemption,
  restart-style replay, and synchronous SVC continuation switching.
- `tests/user/test_pipe.c` includes an empty-pipe blocking case that passes on
  `qemu_arm`, `qemu_m68k`, `qemu_rv32`, and `pcxt --hdd`.

Plan:

1. Keep PendSV as the low-priority asynchronous path.  Do not make PendSV
   preempt active SVC execution.
2. Treat `arm_kernel_sched_switch()` as the ARM implementation of the common
   continuation switch contract, not as a special-case workaround.
3. Keep `svc_msp` ARM-local unless another architecture needs an equivalent
   native interrupt-stack restore slot.
4. Keep using the common fixed-region `KSTACK_USAGE_TRACK` helper to measure ARM
   kernel-stack high-water marks when the optional build flag is enabled.
5. After measurement, shrink the 2 KB user-process slots only if
   CP/M, VFS, TTY, and subsystem paths leave a safe margin.

### `ia16`

Current state:

- ia16 always uses fixed SS=0 per-process kernel-stack slots from
  `__kstack_region_base`.
- `i16_current_ksp` mirrors the active process's kernel-stack top so syscall
  and timer entry can switch to SS=0 before dereferencing shared kernel data.
- Timer, syscall, and cooperative yield paths all build a compatible frame:
  interrupted `SS:SP`, saved GP registers, a synthetic or hardware IRET frame,
  and the 34-byte vfork-save reserve.
- `i16_ctx_switch()` is a true synchronous yield.  It builds a synthetic
  IRET-compatible frame and restores through `i16_trap_after_switch`.
- Context switches also shadow the core/VFS far-call entry-stub globals into
  the PCB, making the stubs effectively per-process while suspended.
- Restart is partly per-process through `pcb_t.syscall_needs_restart`, and the
  shared restart symbols now use syscall-oriented names.

Plan:

1. Keep the 1 KB user-process kernel-stack size during this cleanup.
2. Preserve the single restore tail and the vfork-save frame convention.
3. DONE: rename the synchronous cooperative helper to `i16_ctx_switch()`.
4. Keep the far-call stub shadow swap as an ia16 implementation detail.
5. Include ia16 in the blocking-yield test because it exercises the deepest
   combination of kernel continuation, VFS module calls, and real-mode stacks.
6. Rename shared restart symbols to syscall-oriented names, while keeping any
   genuinely ia16-specific restart bookkeeping per-process.

### `m68k`

Current state:

- m68k has separate USP and SSP.  `pcb_t.sp` stores SSP and `pcb_t.usp` stores
  USP.
- ELF loading allocates a kernel stack page and a separate user stack page for
  native m68k user processes.
- `arch_sched_switch()` executes TRAP #1, so cooperative `sched_switch()` is
  immediate and satisfies continuation blocking.
- TRAP #1, TRAP #0 return, timer interrupts, trace exceptions, and boot
  emulator paths all save full register frames and swap SSP/USP through the
  PCB.
- Syscall restart is more per-process than ARM/RISC-V: the trap path comments
  note that global `syscall_restart` is unsafe for nested m68k trap handling.

Plan:

1. Preserve the full-frame SSP/USP switch contract.
2. DONE: extract the duplicated switch bodies behind `m68k_ctx_switch`,
   without changing frame format.
3. Keep TRAP #1 as the synchronous cooperative switch trigger.
4. Document that the process stack page is the per-process kernel stack and
   the user stack page is separate USP storage.
5. Move remaining restart naming toward `syscall_*`, but keep the actual
   restart state per-process where m68k already needs it.
6. Consider a common fixed-region helper only if later memory measurements
   show value; the current page-backed SSP model already satisfies the target
   contract.

### `riscv`

Current state:

- RISC-V trap entry swaps `sp` with `mscratch`, saves a 144-byte trap frame on
  the process's fixed kernel-stack slot, and returns through `mret`.
- `pcb_t.sp` stores the saved trap-frame SP.  `pcb_t.kernel_sp` stores the
  kernel-stack top loaded into `mscratch`.
- `riscv_ctx_switch(current_sp)` swaps trap-frame SPs through `sched_next()`
  and relies on `pcb_t.kernel_sp` already being initialized by the common
  fixed-region helper.
- Timer preemption is correct because the trap path consumes
  `switch_pending` before returning.
- Cooperative `sched_switch()` executes a machine-mode `ecall`.  The M-mode
  trap frame sits on the live process kernel stack above the blocked kernel
  call chain, so `riscv_ctx_switch()` can save `pcb_t.sp` and later resume at
  the instruction after `sched_switch()`.
- User trap entry restores `mscratch` to the process kernel-stack top after
  saving the original user `sp`, so nested M-mode traps during syscall
  execution do not borrow the user stack.
- `qemu_rv32` reserves the fixed kstack region between `.bss` and the boot
  stack.  `pico2rv` reserves a dedicated 32 KB SRAM window before the page
  pool.  Both targets keep 4 KB user-process slots for the current RISC-V stack
  budget.

Plan:

1. DONE: add a real RISC-V `ARCH_HAS_SCHED_SWITCH` path.
2. DONE: use the M-mode trap frame as the synchronous kernel-continuation
   frame, avoiding a second RISC-V-specific saved-frame format.
3. DONE: reuse `pcb_t.kernel_sp` and `mscratch`; do not add a native interrupt
   stack unless measurement shows a need.
4. DONE: choose an M-mode trap dedicated to cooperative kernel yield; this
   shares the same restore path as timer and syscall-return switches.
5. DONE: rename the shared RISC-V switch helper to `riscv_ctx_switch()`.
6. DONE: replace lazy `kernel_sp` initialization with the common
   `proc_kstack_init_slot` fixed-region model on RISC-V targets.
7. DONE: keep the blocking-yield userland test as coverage; it passes on
   `qemu_rv32` after the M-mode `ecall` switch path.

### `xtensa`

Current state:

- Xtensa uses the windowed ABI.  `xtensa_do_yield()` builds a solicited frame,
  spills register windows, disables interrupts for the SP handoff, and calls
  `xtensa_do_switch(current_sp)`.
- `pcb_t.sp` is the saved solicited-frame SP.  There is no separate
  `pcb_t.kernel_sp` field on Xtensa today.
- The current process stack carries user frames, kernel call frames, and
  solicited switch frames.
- Syscalls enter through the illegal-instruction exception path.  When a
  syscall blocks or a preemption is pending, the handler calls
  `sched_switch()`, which performs a real `xtensa_do_yield()`.
- Timer ISR currently sets the shared pending flag through
  `sched_timer_tick()`, but the real switch occurs when code reaches the
  cooperative yield path.

Plan:

1. Keep register-window spilling hidden inside the architecture helper.
2. Decide whether the current solicited-frame stack is an acceptable
   "equivalent saved kernel-continuation frame" for the target contract, or
   whether Xtensa should gain an explicit per-process kernel stack.
3. If the current model is accepted, document why `pcb_t.sp` alone is enough
   and what invariants protect user frames from kernel continuation frames.
4. If an explicit kernel stack is required, add `pcb_t.kernel_sp` for Xtensa
   and move solicited yield frames there before broadening user-space support.
5. Rename `xtensa_do_yield()` / `xtensa_do_switch()` only after deciding the
   shared helper naming convention.
6. Add a test that blocks inside the syscall handler and resumes through the
   same windowed call chain, because this is the Xtensa-specific risk.

## Constraints

- Do not reintroduce PendSV preempting SVC on ARM.  ARM's accepted synchronous
  path is `arm_kernel_sched_switch()`.
- Keep each rename or extraction bisectable and behavior-neutral.
- Treat restart and continuation blocking as separate mechanisms; do not
  convert side-effectful blocking syscalls to replay-based restart.
- Native interrupt stacks are optional.  Allow arch-local use when it is the
  natural fit for that hardware, but introduce a shared abstraction if multiple
  architectures need the same pattern.
- Do not shrink ia16's 1 KB user-process kernel stack as part of this cleanup.
