# Proposal: No User-Stack Copy on `vfork()`

## Summary

PPAP currently uses target-specific stack-copy logic on some architectures
to make `vfork()` safe.  The PC/XT i16 port demonstrated a simpler model:

- the child shares the parent's user stack, as `vfork()` semantics already imply
- the parent is blocked until the child calls `execve()` or `_exit()`
- only the parent's vulnerable resume frame is saved out-of-line
- that saved frame is restored when the parent resumes

This document proposes treating that model as the preferred long-term
`vfork()` design for PPAP.

The goal is not to change `fork()`.  This is about `vfork()` only.

## Motivation

The existing stack-copy approach has real costs:

- extra allocation during `vfork()`
- extra copy time on the hottest process-spawn path
- more target-specific complexity
- more ways for parent/child stack remapping bugs to appear

For true `vfork()`, the child is supposed to borrow the parent's address
space briefly and then either `execve()` or `_exit()`.  Copying the full user
stack works, but it is more than the semantics require.

The i16 implementation showed that the actual hazard is narrower:

- the child re-enters the kernel and overwrites the parent's saved return frame
- the parent later resumes with corrupted state unless that frame was preserved

Once that frame is saved elsewhere, a full user-stack copy is unnecessary.

## i16 Result

The PC/XT i16 port now uses:

- shared user stack during `vfork()`
- a reserved 34-byte slot in the per-process kernel stack (fixed 1 KB slots)
   for the parent's saved GP+IRET frame (24B) and vfork stub frame (10B)
- restore of that frame before the parent returns to user mode, on both the
   syscall and timer ISR return paths

That was enough to make:

- `init`
- `vfork()`
- `execve("/bin/sh")`
- shell startup

work correctly without allocating a separate child user stack.

This is the first concrete proof inside PPAP that a "save minimal parent
resume state, do not copy the whole user stack" approach is viable.

## i16 Technical Detail

The working i16 design is more specific than just "save a frame somewhere":

- `sys_vfork()` saves a 34-byte region from the parent's shared user stack
   to the parent's own kernel stack, below `trap_ksp`.  The 34 bytes cover:
   - 24 bytes: the GP+IRET frame (ES, DS, BP, DI, SI, DX, CX, BX, AX,
     IP, CS, FLAGS) — popped by trap.S restore + `iret`
   - 10 bytes: the vfork syscall stub's callee-saved registers and return
     address (saved DI, SI, BX, BP + `call vfork` return IP) — popped by
     `SYSCALL_RET` after `iret`
   The AX slot (offset 16) in the saved copy is patched with the child PID
   so the parent sees the correct return value when resumed.
- the child keeps using the shared user stack until `execve()` or `_exit()`
- `execve()` is allowed to rebuild that shared user stack for the new image
- when the parent becomes runnable again, `i16_vfork_restore_frame()` copies
   the saved 34 bytes back to `user_SS:user_SP` before any path returns the
   parent to user mode

### What must be saved (and why)

The user stack at the time of the `int $0x30` in the vfork stub looks like:

```
  [user_SP+0 ]  ES DS BP DI SI DX CX BX AX IP CS FLAGS   ← 24B GP+IRET
  [user_SP+24]  saved DI SI BX BP  (vfork stub pushw's)   ← 10B stub frame
  [user_SP+34]  return address from `call vfork`
  [user_SP+36]  parent's C caller frame (locals, saved regs)
```

The child returns from vfork with AX=0 and `SYSCALL_RET` pops the stub
frame (bytes 24–33), returning to the parent's C caller.  From there, the
child must call only `execve()` or `_exit()`.  Even a bare `execve(path,
argv, envp)` call pushes 3 args + return address + syscall stub prologue
onto the shared stack — overwriting the 10 bytes at `[user_SP+24..+33]`.

The kernel therefore saves 34 bytes (not just 24):

- **Lower side** `[user_SP+0..+23]` (24B): the GP+IRET frame.  The child's
  `execve` syscall trap pushes a new GP+IRET frame here, overwriting the
  parent's original values.
- **Upper side** `[user_SP+24..+33]` (10B): the vfork stub's callee-saved
  regs and return address.  The child's function calls unavoidably overwrite
  this area too.

The region above `[user_SP+34]` (the parent's C caller frame) is NOT saved.
The child must not touch it — doing so violates POSIX vfork semantics.
In particular, calling any function other than `execve()` or `_exit()` in
the vfork child path is undefined behavior.

### Kernel stack layout

Every kernel entry path (`trap.S` INT 30h handler and `switch.S` timer ISR)
reserves a 34-byte slot at a fixed position in the kernel stack:

```
  ktop - 2   user_SS   (pushed by entry code)
  ktop - 4   user_SP
  ktop - 38  34-byte vfork-save slot  (subw $34, %sp)
  ...        C call chain frames below
```

`sys_vfork()` writes the parent's saved frame into the slot at
`[ktop - 38, ktop - 4)`.  The slot is wasted (34 bytes) on the common
non-vfork path but keeps the layout uniform.

### AX return-value guard

After the syscall handler returns, `trap.S` normally writes the return value
into the saved AX slot on the user stack.  For a vfork parent whose child is
still running on the shared user stack, that write would clobber the child's
AX = 0.  `i16_trap_should_skip_ret_store()` checks `current->vfork_frame_saved`
and skips the AX write in that case.

### Two restore paths

During PC/XT bring-up, the first implementation restored the saved parent
frame on the syscall trap return path only.  That was not sufficient.  The
parent can also be resumed by the timer ISR path after scheduling, and that
path also ends in a user-mode `iret`.  If the timer return path skips the
restore, it can `iret` from the child's rewritten shared user stack and pop a
stale or corrupted `CS:IP`.

Both `trap.S` (line ~206) and `switch.S` (line ~161) now call
`i16_vfork_restore_frame()` before the final `iret`.  The restore function
checks `current->vfork_frame_saved`, and if set, copies the 34 bytes from the
kernel stack slot back to the user stack via `mem_region_page_write()`, then
clears the flag.

So the actual invariant is:

- every kernel exit path that can resume a blocked `vfork()` parent must run
   the parent-frame restore before restoring user registers and executing the
   final return instruction (`iret`, `rte`, equivalent trap return, etc.)

This is useful guidance for other targets.  The right question is not only
"what parent state must be saved?" but also "which exact kernel exit paths can
return that parent to user mode, and do all of them restore the saved state?"

## Proposed Model

### Rule

For `vfork()`:

1. The child shares the parent's user address space, including the user stack.
2. The parent is marked blocked immediately after the child is made runnable.
3. The kernel preserves only the parent state that the child's syscall/exec
   path would overwrite before the parent resumes.
4. When the child calls `execve()` or `_exit()`, the parent is made runnable.
5. Before returning the parent to user mode, the kernel restores the saved
   parent resume state.

### Non-goal

Do not apply this to `fork()`.

`fork()` still needs independent writable state.  This proposal is only about
removing unnecessary copying from the `vfork()` path.

## What Must Be Preserved

The exact saved state is architecture-dependent, but the principle is the same:

- preserve the parent frame that the child will overwrite by entering the kernel
- do not preserve unrelated user memory just because it happens to live nearby

Examples:

- i16: the interrupted user GP+return frame on the shared user stack
- m68k: the parent trap/return frame, if the child would overwrite it
- RISC-V: the user-return frame or other resume metadata, depending on trap layout
- ARM: likely less urgent if the kernel already runs on a separate stack, but the
  same analysis still applies

## Conditions for Using This Design

This model is safe only if all of the following are true:

1. The parent cannot run concurrently with the child while the address space is shared.
2. The child is restricted to `execve()` or `_exit()` style behavior expected of `vfork()`.
3. The architecture can identify exactly which parent resume state is at risk.
4. The kernel has a safe place to store that parent state temporarily.
5. The restore point is well-defined and runs before the parent returns to user mode on every possible resume path.

If a target cannot satisfy those conditions cleanly, it should keep its current
stack-copy implementation.

## Expected Benefits

- simpler `vfork()` implementation
- less target-specific allocation/remap logic
- lower spawn overhead
- behavior closer to actual `vfork()` semantics
- fewer bugs caused by partial stack-copy or pointer-remap mistakes

## Risks

- restoring the wrong frame or restoring at the wrong time can corrupt the parent
- restoring the frame on one resume path but forgetting another can leave a latent bug that appears only when scheduling changes
- some targets may have more than one vulnerable frame, not just one
- signal delivery, syscall restart, and tracing can complicate the restore path
- targets that mix kernel and user stack usage may need extra care

Because of that, this should be adopted target by target, not by a blind
global refactor.

## Current State (2026-05)

Only i16 implements the no-copy model.  Four targets still allocate a
fresh 4 KB user-stack page and `memcpy` the parent's stack during
`sys_vfork`:

| Arch | `ARCH_VFORK_COPY_PROCESS_STACK` | `ARCH_VFORK_CHILD_FRAME_POINTER` | Uses `vfork_copy_user_stack()` | Where |
|------|---|---|---|---|
| i16    | 0 | 0 | no  | reference impl |
| riscv  | 0 | 1 | yes | [sys_proc.c:1812-1827](../../src/kernel/core/syscall/sys_proc.c#L1812-L1827) |
| m68k   | 0 | 0 | yes | [sys_proc.c:1771-1796](../../src/kernel/core/syscall/sys_proc.c#L1771-L1796) |
| arm_m  | 1 | 1 | no  | full-page copy in [sys_proc.c:1742-1748](../../src/kernel/core/syscall/sys_proc.c#L1742-L1748) (`ARCH_VFORK_COPY_PROCESS_STACK`) |
| xtensa | 0 | 0 | yes | [sys_proc.c:1855-1888](../../src/kernel/core/syscall/sys_proc.c#L1855-L1888) |

The shared helper to retire is
[`vfork_copy_user_stack()`](../../src/kernel/core/syscall/sys_proc.c#L231-L243).
The `ARCH_VFORK_COPY_PROCESS_STACK` arm_m branch in
[sys_proc.c:1742](../../src/kernel/core/syscall/sys_proc.c#L1742) and the
arm_m allocation block at
[sys_proc.c:1653-1662](../../src/kernel/core/syscall/sys_proc.c#L1653-L1662)
also go away once arm_m converts.

Per-arch resume paths (from [docs/kernel/context_switch.md](../kernel/context_switch.md)
and [docs/kernel/stack.md](../kernel/stack.md)):

| Arch | Kernel exit paths that can return the parent to user mode |
|------|-----------------------------------------------------------|
| i16    | `trap.S` INT 30h return, `switch.S` PIT IRET tail — both already call `i16_vfork_restore_frame()` |
| riscv  | `trap.S` trap return (single path — both ecall and timer end through `mret`) |
| m68k   | `trap.S` TRAP #0 / TRAP #1 `rte`, timer IRQ `rte` |
| arm_m  | SVC handler exit (`bx EXC_RETURN`), SysTick exit, sentinel-SVC tail in `arm_kernel_sched_switch` |
| xtensa | ESP-IDF exception `rfe` after PPAP syscall body, new-process frame path in `switch.S` |

## Concrete Plan

The rollout is risk-ordered: least-complex frame layout first, so each
conversion calibrates the pattern before harder targets.  Each phase is one
PR with its own audit + conversion + tests.

### Phase 0 — Lift the i16 mechanism into a shared contract (preparation)

Goal: every arch has the same surface area to fill in, even though the saved
bytes differ.

Tasks:

1. Move the `vfork_frame_saved` flag handling and the
   "skip-AX-return-store" idea into shared docs.  The flag itself already
   lives in [proc_info.h:179](../../src/kernel/common/core/proc_info.h#L179);
   only doc work is needed here.
2. Add a section to [docs/kernel/context_switch.md](../kernel/context_switch.md)
   stating the invariant: *every kernel exit path that can resume a blocked
   vfork parent must restore the saved parent resume state before the final
   user-mode return instruction*.  Cite i16 as reference.
3. Define a per-arch hook name that each Phase 1–4 PR will implement:
   `<arch>_vfork_save_parent_frame(pcb_t *parent)` (called from `sys_vfork`)
   and `<arch>_vfork_restore_frame(void)` (called from each exit path).
   The names mirror `i16_vfork_restore_frame()`.

Exit criteria: docs landed; no behavior change.

### Phase 1 — RISC-V (lowest risk)

Why first: RISC-V has a single trap-return path
([trap.S](../../src/arch/riscv/kernel/core/trap.S)) and a single trap-frame
layout.  `ecall` performs no hardware push to the user stack — all register
state is captured in the trap frame on the fixed kernel slot.  The only
user-stack-resident parent state is whatever the user-mode vfork stub
pushed on top of its own SP before `ecall`.

Audit checklist (must land in the PR description):

- [ ] Inspect [src/arch/riscv/user/syscall.S](../../src/arch/riscv/user/syscall.S)
      `vfork` stub: list every word it pushes on user `sp` before the
      `ecall`.  That set is the lower bound of bytes to save.
- [ ] Confirm trap return is the only user-mode resume path for a vfork
      parent (no PMP/MMU-fault redirect, no timer-direct-to-user path).
- [ ] Confirm `TF_USER_SP` in the trap frame is sufficient to locate the
      stub-saved region.

Conversion:

1. Add `riscv_vfork_save_parent_frame()` in
   [src/arch/riscv/kernel/core/riscv_common.c](../../src/arch/riscv/kernel/core/riscv_common.c):
   read N bytes (the audited stub frame size) from user SP via
   `mem_region_page_read`, store them at a fixed offset in the parent's
   kstack slot (analogous to the i16 34-byte slot), set
   `current->vfork_frame_saved`.
2. Add `riscv_vfork_restore_frame()` (mirror of i16's): before `mret` in
   `trap.S`, if `vfork_frame_saved` then `mem_region_page_write` it back
   and clear the flag.  Single insertion point.
3. Patch the saved a0 slot with the child PID (analogous to the i16 AX
   patch).  This replaces the existing `child_tf[0] = 0` arrangement only
   for the parent's return value path; the child's a0=0 already comes
   from the copied trap frame.
4. Remove the riscv branch of `vfork_copy_user_stack` and the
   `child->user_stack_page = child_ustack` assignment in
   [sys_proc.c:1812-1827](../../src/kernel/core/syscall/sys_proc.c#L1812-L1827).
   Set `child->user_stack_page = current->user_stack_page` so child shares.
5. Update `sys_exit` cleanup
   ([sys_proc.c:1564-1567](../../src/kernel/core/syscall/sys_proc.c#L1564-L1567))
   so it no longer expects a child-owned stack page on riscv (it already
   gates on inequality; the gate will simply never fire).
6. Set `ARCH_VFORK_CHILD_FRAME_POINTER = 0` in
   [src/arch/riscv/kernel/core/arch.h:20](../../src/arch/riscv/kernel/core/arch.h#L20)
   if the conversion lets us drop the `child_frame` use.

Tests: `qemu_rv32` smoke (boot → shell → run a couple of binaries that
hit vfork/exec), plus targeted `tests/` cases under vfork-heavy paths.
This is also a chance to fix the lingering rv32 second-exec issue
(see `project_rv32_next.md`) if it was masked by the stack copy.

### Phase 2 — m68k

Why second: m68k has two exit paths instead of one (TRAP `rte` and timer
IRQ `rte`), but the trap-frame discipline is well-understood and the
saved-frame format on SSP is already the single source of register state.
The user-stack vulnerable region is bounded by what the m68k vfork stub
pushes (callee-saved a-regs / d-regs per the m68k Linux ABI).

Audit checklist:

- [ ] Inspect [src/arch/m68k/user/syscall.S](../../src/arch/m68k/user/syscall.S)
      `vfork` stub: list every word pushed on USP before `trap #0`.
- [ ] Enumerate kernel exit paths in
      [src/arch/m68k/kernel/core/trap.S](../../src/arch/m68k/kernel/core/trap.S):
      TRAP #0 return, TRAP #1 return, timer IRQ return.  Confirm any
      restore site must run on all of them.
- [ ] Explain the existing a6 patching in
      [sys_proc.c:1786-1792](../../src/kernel/core/syscall/sys_proc.c#L1786-L1792):
      is the frame pointer something the stub touched, or compiler-level
      state?  If compiler-level, it has to come from the saved user-stack
      slice and not from a separate kstack copy.

Conversion:

1. Add `m68k_vfork_save_parent_frame()` /
   `m68k_vfork_restore_frame()` in
   [src/arch/m68k/kernel/core/m68k_common.c](../../src/arch/m68k/kernel/core/m68k_common.c).
2. Insert restore calls in *both* `trap.S` exit paths (TRAP and timer).
   Treat resume-path coverage as a release blocker — copy the i16 lesson
   verbatim into the PR description.
3. Drop the m68k branch in
   [sys_proc.c:1771-1796](../../src/kernel/core/syscall/sys_proc.c#L1771-L1796):
   set `child->user_stack_page = current->user_stack_page`,
   `child->usp = current->usp`, and skip the a6 remap (no remap is needed
   because the child uses the same page).
4. Verify `sys_exit`
   ([sys_proc.c:1564-1567](../../src/kernel/core/syscall/sys_proc.c#L1564-L1567))
   correctly handles the shared user_stack_page on the m68k child-exit
   path.

Tests: `qemu_m68k` full test suite, x68k floppy boot+shell smoke.

### Phase 3 — ARM Cortex-M

Why third: ARM is the only target still using the full-page-copy path
(`ARCH_VFORK_COPY_PROCESS_STACK = 1`).  It has three exit paths
(SVC tail, SysTick exit, sentinel-SVC tail) per
[docs/kernel/context_switch.md](../kernel/context_switch.md) §ARM, and
the hardware auto-pushes an 8-word HW frame onto PSP on SVC entry.
That HW frame is what the child will clobber by entering the kernel
again.

Audit checklist:

- [ ] Inspect [src/arch/arm_m/user/syscall.S](../../src/arch/arm_m/user/syscall.S)
      `vfork` stub: list every word it pushes on PSP before `svc 0`.
- [ ] Confirm the parent's HW frame (8 words) at the saved PSP value is
      exactly what gets clobbered, plus the stub's pushed callee-saved
      r4-r7.
- [ ] Enumerate every place that returns to Thread mode via
      `bx EXC_RETURN`: SVC handler, SysTick handler exit-time switch,
      and the sentinel-SVC tail in `arm_kernel_sched_switch`.

Conversion:

1. Add `arm_vfork_save_parent_frame()` in
   [src/arch/arm_m/kernel/core/arm_common.c](../../src/arch/arm_m/kernel/core/arm_common.c)
   (or equivalent): save HW frame (32 B) + stub frame (audited size) from
   PSP into the parent's kstack slot.  Patch the saved r0 slot with the
   child PID.
2. Add `arm_vfork_restore_frame()` and call it from every Thread-mode
   exit path identified in the audit.
3. Set `ARCH_VFORK_COPY_PROCESS_STACK = 0` and
   `ARCH_VFORK_CHILD_FRAME_POINTER = 0` in
   [src/arch/arm_m/kernel/core/arch.h:22-23](../../src/arch/arm_m/kernel/core/arch.h#L22-L23).
4. Drop the now-dead `stack_region` allocation block at
   [sys_proc.c:1653-1662](../../src/kernel/core/syscall/sys_proc.c#L1653-L1662)
   and the `ARCH_VFORK_COPY_PROCESS_STACK` memcpy at
   [sys_proc.c:1742-1748](../../src/kernel/core/syscall/sys_proc.c#L1742-L1748).
5. Rebuild the child SW frame at
   [sys_proc.c:1829-1840](../../src/kernel/core/syscall/sys_proc.c#L1829-L1840)
   to point the saved PSP at the parent's PSP (shared) instead of a
   child-owned page.

Tests: `qemu_arm` full suite, `pico1calc` hardware smoke (boot to shell,
spawn `ls`, run rogue briefly).  MPU region 2 may need re-examination
since child no longer has its own user stack — region 2 now points at
the parent's stack while the child runs, which is the correct semantic.

### Phase 4 — Xtensa

Why last: windowed ABI + ESP-IDF exception frame ownership make Xtensa
the most subtle frame layout.  Existing code already remaps `a3` if it
points into the parent stack
([sys_proc.c:1881-1883](../../src/kernel/core/syscall/sys_proc.c#L1881-L1883))
— that hint about what the child clobbers must inform the audit.

Audit checklist:

- [ ] Inspect [src/arch/xtensa/user/syscall.S](../../src/arch/xtensa/user/syscall.S)
      `vfork` stub: list every word pushed before the ILL trap, plus any
      window-spill behavior triggered between the trap and the return.
- [ ] Confirm
      [xtensa_build_vfork_child_frame()](../../src/arch/xtensa/kernel/core/xtensa_common.c)
      can build the child frame without needing a separate user-stack
      page (only needs `child_user_sp` to point at the parent's stack).
- [ ] Identify every place that returns to user mode through `rfe` and
      can resume a parent: the syscall body return, and any
      timer/preempt path that exits through ESP-IDF's exception epilogue.

Conversion:

1. Add `xtensa_vfork_save_parent_frame()` /
   `xtensa_vfork_restore_frame()`.  The saved region must include any
   windowed-ABI spilled registers that the parent will need on resume.
2. Drop the xtensa branch in
   [sys_proc.c:1855-1888](../../src/kernel/core/syscall/sys_proc.c#L1855-L1888)
   that calls `vfork_copy_user_stack`; let the child share
   `current->user_stack_page` and reuse the parent's `a1`.
3. Remove the a3 remap — no remap needed.

Tests: ESP32-S3 / CardComputer smoke (boot → init parses inittab →
shell prompt).

### Phase 5 — Remove dead code

Once Phases 1–4 land:

1. Delete `vfork_copy_user_stack()` from
   [sys_proc.c:231-243](../../src/kernel/core/syscall/sys_proc.c#L231-L243)
   and its `#if defined(__m68k__) || defined(__riscv) || defined(__xtensa__)`
   gate.
2. Delete `ARCH_VFORK_COPY_PROCESS_STACK` and
   `ARCH_VFORK_CHILD_FRAME_POINTER` from every arch's `arch.h`; delete
   the `#if ARCH_VFORK_COPY_PROCESS_STACK` blocks in `sys_proc.c`.
3. Simplify the vfork-child exit cleanup in
   [sys_proc.c:1554-1567](../../src/kernel/core/syscall/sys_proc.c#L1554-L1567):
   the child no longer has a private user_stack_page distinct from the
   parent's, so the inequality gate becomes dead.

Exit criteria: no remaining call to a "copy parent user stack" routine in
the `vfork` path; only the i16-style minimal save remains, behind a
per-arch hook.

## Risks and Mitigations

- **Resume-path coverage miss**: an arch with multiple exit paths (m68k,
  arm_m) forgets a restore site.  *Mitigation*: each conversion PR lists
  every exit path in its description and adds a smoke test that exercises
  the asynchronous-resume case (timer preemption while child runs).
- **Stub-frame size underestimate**: the audit miscounts what the user
  stub pushed.  *Mitigation*: source the byte count from
  `src/arch/<arch>/user/syscall.S` directly in the PR, not from memory.
- **Signal delivery during vfork window**: if a signal targeting the
  parent is delivered while the child is still using the shared stack,
  the trampoline write could collide with the child.  *Mitigation*:
  parent stays `PROC_BLOCKED` until child exec/exit; verify
  `signal_check` skips blocked vforked parents (it does today — but
  re-check during each conversion).
- **MPU/PMP boundary surprises** (arm_m, riscv): the child running on the
  parent's stack page must still be inside the user-accessible region.
  *Mitigation*: arm_m already reprograms region 2 on context switch; on
  vfork the child's region 2 points at the parent's `user_stack_page` —
  which is correct.  Verify in Phase 3.

## Policy

After Phase 5:

- `vfork()` shares the parent's user stack; the child never gets its own.
- Each arch saves the minimum bytes of parent resume state needed and
  restores them on every user-mode exit path.
- Stack-copy fallback is not retained; if a future arch cannot satisfy
  the conditions in "Conditions for Using This Design" above, it must
  add the no-copy machinery on day one or stay off PPAP.
