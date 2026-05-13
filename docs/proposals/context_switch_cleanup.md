# Context Switch Cleanup

**Status:** proposed architecture cleanup.  The current context-switch
contract is documented in
[`../kernel/context_switch.md`](../kernel/context_switch.md); kernel-stack
mechanics are documented in [`../kernel/stack.md`](../kernel/stack.md).

## Goal

Establish one long-term context-switch model for all architectures:

- Every process has a kernel stack that can hold a suspended kernel
  continuation.
- That kernel stack comes from the common fixed-region kstack helper where the
  target has a suitable fixed kernel-memory window.
- `sched_switch()` performs a real cooperative switch even when called while
  the process is already executing kernel code.
- When the blocked process is scheduled again, it resumes the same kernel call
  chain at the instruction after `sched_switch()`.
- Restartable syscalls remain a separate replay mechanism for operations that
  are explicitly safe to re-execute.

This makes blocking syscalls, VFS waits, pipes, TTY waits, subsystem bridges,
and future kernel services use the same continuation-blocking contract across
ARM, ia16, m68k, RISC-V, Xtensa, and later ports.  The final convergence target
is that no architecture borrows a user/process data page as its kernel
continuation stack.

An architecture may also keep a native interrupt stack, such as ARM MSP, for
fast system interrupt handling.  That is an optional optimization.  It must not
be required for correctness, and it must not make the process-kernel-stack
continuation path architecture-specific from the shared kernel's point of
view.

## Target Contract

Each architecture should provide:

1. A per-process kernel stack, preferably allocated by the common
   fixed-region helper.
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

## Final Fixed-Kstack Model

The desired steady state is:

1. Each target reserves a fixed `__kstack_region_base` window in its linker
   script or equivalent target memory map.
2. `proc_kstack_init_slot()` initializes `pcb_t.kernel_sp` for every process
   slot and installs canaries / optional `KSTACK_USAGE_TRACK` painting.
3. Architecture trap, syscall, and cooperative switch paths use
   `pcb_t.kernel_sp` as the process kernel-continuation stack.
4. User stacks, PSP/USP stacks, data pages, heap, and mmap pages are tracked
   independently from fixed kernel stacks.
5. `execve()` does not need arch-specific deferred freeing for the old kernel
   stack, because the live kernel continuation does not reside in a page-pool
   stack page.

Architectures may temporarily keep an equivalent saved-continuation frame while
being migrated, but that is an intermediate compatibility state, not the final
goal.

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
- `sys_vfork()` no longer uses an opt-out architecture list for its copied
  process-stack path.  Each architecture now declares the vfork stack-frame
  behavior it needs through positive `ARCH_VFORK_*` capability macros.
- m68k now uses fixed per-process kernel-stack slots on `qemu_m68k` and
  `x68k`; native m68k ELF and Human68k loaders no longer allocate
  `stack_page_id` as SSP storage.
- m68k `execve()` no longer needs the deferred old-SSP-page free hook because
  syscall return switches between fixed kstack frames.
- Kernel-stack-use reduction for deep subsystem paths is deferred in
  [`kernel_stack_use.md`](kernel_stack_use.md), because userland subsystem
  work may obsolete much of that path.

Remaining:

- Xtensa still carries user, kernel, and solicited switch frames on one process
  stack.  The `xtensa_cc` build is reliable again and now uses the shared
  Xtensa kernel source list; the remaining work is the functional fixed-kstack
  migration.
- Optional naming cleanup remains for arch switch helpers (`*_ctx_switch`).
- ARM stack-size reduction is deferred until measurements are useful after, or
  independent of, userland subsystem migration.

## Context Switch Work

1. DONE: audit ARM-M, ia16, m68k, RISC-V, and Xtensa against the target
   contract in this proposal and the stack/context-switch docs.
2. PARTIAL: move every architecture to the fixed per-process kstack model.
   ARM-M, ia16, m68k, and RISC-V already use fixed kstack slots.  Xtensa is
   now buildable again and remains the only functional migration target.
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
  - Xtensa now uses `xtensa_ctx_switch` for the C handoff helper
6. DONE: keep the userland blocking-pipe test as coverage for one process
   blocking inside a syscall while another runnable process executes before
   the blocked syscall returns.
7. DONE: document the context-switch contract for new architectures in
   `docs/getting_started/porting.md`.

## Kernel Stack Work

The stack items support the single continuation-blocking model.  They should
stay behavior-preserving unless a subtask explicitly calls for
measurement-driven sizing.

1. DONE: establish the common fixed-region helper for per-process kernel
   stacks.  ARM-M, ia16, and RISC-V use it today.
2. DONE: move RISC-V away from lazy `kernel_sp` initialization from the
   process stack page.  `qemu_rv32` and `pico2rv` now reserve fixed kstack
   regions and initialize slots through `proc_kstack_init_slot`.
3. DONE: treat architectures that use a native interrupt stack as having two
   stack roles: an interrupt stack for short handlers and a process kernel
   stack for continuations.
4. DONE: use the optional common fixed-region `KSTACK_USAGE_TRACK` helper on
   ARM when measuring kernel-stack high-water marks.
5. DONE: migrate m68k SSP storage from `stack_page_id` to fixed kstack
   slots while preserving the existing USP and full-frame switch contract.
6. DONE: set m68k fixed-kstack user-process slots to 2 KB on `qemu_m68k` and
   `x68k`; TODO: measure high-water marks and shrink to 1 KB if safe.
7. REMAINING: migrate Xtensa solicited switch frames to a fixed per-process
   kernel stack.  The build prerequisite is done.
8. DEFERRED: measure pico1calc stack usage with CP/M and other deep subsystem
   paths.  See [`kernel_stack_use.md`](kernel_stack_use.md).
9. DEFERRED: shrink pico1calc `PROC_KSTACK_SIZE` from 2 KB only if
   measurements show a safe margin.

## User Stack and Tracking

The user-mode stack page is a separate concern from the per-process kernel
stack that the rest of this proposal addresses.  It is a page-pool-backed
page owned by the process for the duration of its life (or until `execve()`
replaces the image).  Each architecture currently tracks it differently:

| arch    | user-stack page lives in                                   |
|---------|------------------------------------------------------------|
| `arm_m` | `pcb_t::stack_page_id` (originally PSP, now solely the user stack after the kernel-stack migration) |
| `ia16`  | n/a — real-mode does not split user and kernel stacks      |
| `m68k`  | `pcb_t::user_stack_page` (named field) plus `pcb_t::usp`   |
| `riscv` | `user_pages[USER_PAGES_MAX - 1]` (magic slot, no name)     |
| `xtensa`| `user_pages[USER_PAGES_MAX - 1]` for the copied vfork child stack; the normal process stack still uses `stack_page_id` while live switch frames remain on the shared process stack |

The free sites in `sys_proc.c` mirror that fragmentation: every reference to
`user_stack_page` in `sys_exit()`, the vfork-child cleanup branch, and the
post-`execve()` old-stack cleanup is guarded `#if defined(__m68k__)`, RISC-V
and Xtensa rely on the magic tracked slot being walked by
`proc_release_tracked_pages()`.

When this work is picked up, the four post-Phase-2 architectures
(`arm_m`, `m68k`, `riscv`, `xtensa`) converge on a single named field
(`pcb_t::user_stack_page` is the obvious candidate, since it already
exists for m68k) and the `#if defined(__m68k__)` guards on the lifecycle
sites are dropped.  `ia16` keeps its current real-mode arrangement
unchanged — the strict memory limit makes adding a separate user-stack
page expensive, and there is no kernel/user split to reflect.

This unification interacts with
[`no_stack_copy_on_vfork.md`](no_stack_copy_on_vfork.md), which proposes
adopting the ia16 model — child shares the parent's user stack and only
the parent's vulnerable resume frame is saved out-of-line — across all
architectures.  If that proposal lands first, `vfork_copy_user_stack()`
disappears along with the per-vfork page allocation, and the user-stack
page on `arm_m` / `m68k` / `riscv` / `xtensa` is again a single per-process
page (allocated at `execve()`, freed at `wait()`-reap) with no
vfork-specific second copy.  The naming-and-tracking cleanup is still
needed in that world, just simplified — there is one user-stack page per
process to track, not one per process plus a per-vfork copy.

The original Xtensa per-vfork page leak has been fixed standalone by tracking
the copied child stack in the same magic slot used by RISC-V.  The broader
unification is still useful because that slot does not name the page's role,
and it will be easier to reason about after live Xtensa switch frames move off
the process stack.

## Step-by-Step Fixed-Kstack Migration

### Phase 1: Keep The Contract Green

1. Keep the userland empty-pipe blocking test passing on every QEMU target
   that can run it.
2. Build each neighboring architecture after shared process, exec, or procfs
   changes.
3. Keep restart/replay state separate from continuation blocking state.

### Phase 2: Finish Page-Backed Kernel Stack Ports

1. DONE: migrate m68k first because its SSP model already cleanly separates
   user USP from kernel SSP.
2. DONE: add fixed kstack reservations for m68k targets.
3. DONE: initialize m68k `pcb_t.kernel_sp` via `proc_kstack_init_slot()`.
4. DONE: build initial m68k frames on the fixed kstack slot and keep
   `pcb_t.usp` / `user_stack_page` as user stack storage.
5. DONE: update TRAP #0, TRAP #1, timer, trace, and emulator switch paths so
   SSP is saved/restored from fixed kstack frames instead of `stack_page_id`.
6. DONE: remove m68k's deferred exec old-stack hook once `stack_page_id` is no
   longer the live SSP.
7. DONE: remove native m68k `stack_page_id` allocation from ELF and Human68k
   process loaders.  Subsystems may still use it as page-backed process
   storage, but not as SSP continuation storage.
8. DONE: update stack, memory, target, and procfs docs to describe fixed m68k
   kstacks.

### Phase 3: Finish Shared Accounting And Diagnostics

1. Keep `/proc/meminfo` and `/proc/<pid>/stat` reporting page-pool-backed
   memory only.
2. Document fixed kstack memory as target-reserved kernel memory, not process
   page-pool memory.
3. Use common `KSTACK_USAGE_TRACK` for fixed-kstack high-water measurements on
   every fixed-kstack architecture that has a suitable syscall/trap hook.

### Phase 4: Migrate Xtensa

1. DONE: restore a reliable `xtensa_cc` build before functional migration.
2. DONE: document the current solicited-frame layout and all places where
   user, kernel, and switch frames share the process stack.
3. DONE: add `pcb_t.kernel_sp` for Xtensa.
4. DONE: reserve a fixed kstack region for Xtensa and initialize the Xtensa
   `kernel_sp` field from it.  Because ESP-IDF owns the final linker script,
   the region is aligned arch-owned BSS rather than a target `.ld` section.
5. Move syscall continuation and solicited yield frames onto fixed kstack
   slots, keeping register-window spilling hidden inside the arch helper.
   DONE for manufactured new-process frames (`exec` initial frames and
   `vfork` child frames); live cooperative yield frames still remain.
6. Split the ESP-IDF exception-table syscall entry wrapper from the PPAP
   syscall body so an arch trampoline can switch to `pcb_t.kernel_sp` before
   running `syscall_dispatch()` or `sched_switch()`.
7. REMAINING: add the Xtensa syscall-stack trampoline.  A direct `a1` switch
   to `pcb_t.kernel_sp` builds cleanly, but hardware testing double-excepts at
   the first user process.  The accepted version must preserve ESP-IDF's
   exception-entry and Xtensa window state instead of only changing the C
   stack pointer.
8. REMAINING: move live syscall-path solicited yield frames onto the fixed
   kstack as the normal blocked continuation.
9. Make non-syscall cooperative switch paths use fixed kstack or stay
   explicitly limited to kernel-idle/target bootstrap paths.
10. Make timer/fault return paths restore either normal user state or suspended
   kernel continuations from the fixed kstack.
11. Add or enable a userland blocking-continuation test for Xtensa.

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
- Native m68k ELF and Human68k loading build initial SSP frames on the fixed
  kstack slot.  Native ELF still allocates a separate user stack page for USP.
- `arch_sched_switch()` executes TRAP #1, so cooperative `sched_switch()` is
  immediate and satisfies continuation blocking.
- TRAP #1, TRAP #0 return, timer interrupts, trace exceptions, and boot
  emulator paths all save full register frames and swap SSP/USP through the
  PCB.
- `execve()` can free old page-backed stack storage immediately when present;
  the syscall return path no longer runs through a page-backed SSP.
- Syscall restart is more per-process than ARM/RISC-V: the trap path comments
  note that global `syscall_restart` is unsafe for nested m68k trap handling.

Plan:

1. Preserve the full-frame SSP/USP switch contract while moving SSP storage.
2. DONE: extract the duplicated switch bodies behind `m68k_ctx_switch`,
   without changing frame format.
3. Keep TRAP #1 as the synchronous cooperative switch trigger.
4. DONE: add fixed kstack regions to m68k target linker scripts and initialize
   `pcb_t.kernel_sp` from the common helper.
5. DONE: build the initial m68k SSP frame on the fixed kstack slot.  Keep
   `user_stack_page` and `pcb_t.usp` as separate user storage.
6. DONE: update TRAP #0, TRAP #1, timer, trace, and emulator switch paths so
   they save/restore SSP from the fixed slot.
7. DONE: remove `m68k_exec_old_stack` / `m68k_exec_free_old_stack()` after
   `execve()` no longer returns through a page-backed SSP.
8. DONE: remove native m68k kernel-stack `stack_page_id` allocation from ELF,
   `.x`, and `.r` loaders.  Keep subsystem-owned `stack_page_id` uses where
   they represent page-backed process storage rather than SSP.
9. Move remaining restart naming toward `syscall_*`, but keep the actual
   restart state per-process where m68k already needs it.

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
  `xtensa_ctx_switch(current_sp)`.
- `pcb_t.sp` is the saved solicited-frame SP.  `pcb_t.kernel_sp` is populated
  from the common fixed-region helper, backed by aligned Xtensa BSS because
  ESP-IDF owns the target linker script.  The live solicited frames have not
  moved to that region yet.
- The current process stack still carries user frames, kernel call frames, and
  live solicited switch frames.  Manufactured new-process frames are now built
  on the fixed kstack slot.
- The solicited frame lives below the active `sched_switch()` caller.  The low
  16 bytes are left as ABI scratch.  Offsets are: `+16` exit marker,
  `+20` PC, `+24` PS, `+28` user SP for new-process frames, `+32` saved `a0`,
  and `+36` saved `a3`.
- Exit marker `0` means a normal solicited frame and restores through `retw`.
  Exit marker `1` means an exec/vfork child frame and restores by loading
  entry, PS, `a0`, `a3`, and user SP, then jumping directly with `jx`.
- `arch_build_initial_frame()` builds the marked new-process frame.  Exec and
  vfork child paths place that frame on the fixed kstack slot and record the
  user argc/argv or copied vfork stack pointer in the frame's user-SP slot.
- `xtensa_ctx_switch(current_sp)` stores the outgoing frame pointer in
  `current->sp`, picks `sched_next()`, installs `current_core[0]`, and returns
  the incoming process's saved frame pointer.
- Syscalls enter through the illegal-instruction exception path.  When a
  syscall blocks or a preemption is pending, the handler calls
  `sched_switch()`, which performs a real `xtensa_do_yield()`.
- The ESP-IDF exception-table syscall wrapper is separate from the PPAP
  syscall body.  This remains the insertion point for a future fixed-kstack
  handoff.
- A direct syscall trampoline that only switches `a1` to `pcb_t.kernel_sp`
  was tried and rejected: it builds, but `xtensa_cc` hardware testing
  double-excepts before user tests start.  The next version must account for
  Xtensa window spill/restore state and ESP-IDF's exception-entry contract.
- Timer ISR currently sets the shared pending flag through
  `sched_timer_tick()`, but the real switch occurs when code reaches the
  cooperative yield path.
- The ESP-IDF `ppap_kernel` component consumes `KERNEL_XTENSA_COMMON_SOURCES`
  from `cmake/kernel.cmake`; target-local sources are limited to ESP32-S3
  device glue and Xtensa-CC stubs.  This avoids repeating the common kernel
  source list in the target component.

Plan:

1. DONE: restore a reliable `xtensa_cc` build before changing stack semantics.
2. DONE: keep the ESP-IDF kernel component on the shared Xtensa/common source
   list so future core sources are not missed.
3. DONE: document the current solicited-frame layout and the invariants that
   keep user frames, kernel frames, and switch frames recoverable.
4. Keep register-window spilling hidden inside the architecture helper.
5. DONE: add `pcb_t.kernel_sp` for Xtensa.
6. DONE: initialize `pcb_t.kernel_sp` from a target-reserved fixed kstack
   region.  `xtensa_cc` uses 2 KB user-process slots for the staging region.
7. Move solicited yield frames and syscall continuations onto the fixed kstack
   while preserving the window-spill discipline.  DONE for manufactured
   new-process frames; remaining work is the live cooperative yield frame.
8. DONE: split the ESP-IDF exception-table syscall entry wrapper from the
   PPAP syscall body, leaving a clean insertion point for a kstack-switch
   trampoline.
9. Add the kstack-switch trampoline before `xtensa_syscall_body()`.  It must
   save the ESP-IDF exception-entry state, preserve the Xtensa window ABI
   invariants needed by spill/restore, switch to `pcb_t.kernel_sp`, call the
   syscall body, and restore the exception-entry state before returning to
   ESP-IDF.
10. Move syscall-path solicited yield frames onto the fixed kstack by making
   `sched_switch()` run only after that accepted trampoline is active.
11. Audit non-syscall `sched_switch()` callers.  Keep target bootstrap and
   idle-only uses explicit, and move any process continuation path onto the
   fixed kstack before it can block.
12. Make exception return restore either a normal user frame or a suspended
   kernel continuation from the fixed kstack.
13. DONE: rename the low-level C handoff helper to `xtensa_ctx_switch`.
   Keep `xtensa_do_yield()` as the public arch scheduling trigger.
14. Add a test that blocks inside the syscall handler and resumes through the
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
