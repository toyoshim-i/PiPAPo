# Context Switching

PPAP has two related context-switch paths:

- Asynchronous preemption: a timer interrupt or yield request asks the
  architecture layer to switch at the next safe return point.
- Synchronous blocking: kernel code calls `sched_switch()` and must not return
  to the caller until the current process has actually yielded and later
  resumed.

The synchronous contract is required for blocking syscalls that keep local
state, have multiple wait points, or have already performed side effects.
Restarting the syscall from the user trap instruction is a separate mechanism
for operations that are explicitly replay-safe.

## Shared Entry Points

`sched_switch()` is the common cooperative-yield entry point.  It calls
`arch_sched_switch()`, whose contract is:

1. Arrange a real context switch at the call site or at the current trap
   return point.
2. Return to the caller only after the same process has been switched away and
   later resumed.

`arch_yield()` is weaker.  It only requests a switch, usually for timer/user
preemption or wakeups.  All architectures now set the cross-arch
`switch_pending` flag; the trap-return / IRQ-exit path checks it.

`sched_check_preempt()` consumes pending switch requests in shared idle paths.
Trap and interrupt return paths also check their architecture-specific pending
state before returning to user code.

## Architecture Summary

| Arch | Async preemption | `sched_switch()` from kernel context | Trap-return switch |
|------|------------------|--------------------------------------|--------------------|
| `arm_m` | SysTick sets `switch_pending` and swaps inline on exit | Direct `arm_kernel_sched_switch()` from Handler mode; Thread-mode callers issue `svc #0xFF` sentinel | SysTick exit checks `switch_pending`; SVC sentinel dispatches into `arm_kernel_sched_switch` |
| `ia16` | PIT INT 08h checks `switch_pending` | Direct `i16_ctx_switch()` builds the same frame shape as interrupt/syscall paths | `i16_trap_after_switch` restores via the shared IRET tail |
| `m68k` | Timer/trap return checks `switch_pending` | TRAP #1 switches immediately through `m68k_trap1_handler` | TRAP/timer assembly saves SSP/USP, calls `sched_next()`, restores incoming SSP/USP |
| `riscv` | Timer interrupt sets `switch_pending` | Machine-mode `ecall` switches immediately through `riscv_ctx_switch()` | `riscv_ctx_switch(current_sp)` swaps trap-frame SPs and refreshes `mscratch` |
| `xtensa` | Timer ISR sets `switch_pending` | Direct `xtensa_do_yield()` spills register windows and switches solicited frames | Syscall/fault handlers call `sched_switch()` before returning through `rfe` |

## ARM Cortex-M

ARM uses a single 10-word SW frame ({r4-r11, saved lr, saved PSP}) on MSP
for every suspended process.  `arm_kernel_sched_switch()` is the one
switch primitive — SVC entry, the SysTick exit-time check, and the
sentinel-SVC trampoline all push this frame and call it.  See
[`stack.md`](stack.md) for the slot layout.

From Handler mode (the in-syscall sched_switch case),
`arch_sched_switch()` calls `arm_kernel_sched_switch()` directly.  It
pushes the SW frame at the current MSP depth, swaps MSP+PSP between
PCBs, and pops the incoming frame.  Saved-lr dispatch handles the
return target: EXC_RETURN for fresh / SysTick-saved processes (unwinds
through HW frame on PSP into Thread mode); C return address for
syscall-body callers (continues the syscall body, eventually unwinding
to SVC normal_return).

From Thread mode (idle yield, kernel-resident subsystem yields like
`cpm_run_process` reaching `sys_exit`), `arch_sched_switch()` issues
`svc #0xFF`.  The SVC handler detects the sentinel immediate from the
stacked PC and calls `arm_kernel_sched_switch()` directly without
running the syscall dispatch — same shape m68k uses with TRAP #1.

`arch_yield()` from a context that can wait (timer ISR, sched_wakeup
from a peripheral IRQ) sets `switch_pending`; the SysTick handler
honors it on its next exit.  Latency is bounded by the tick period.

## ia16

ia16 uses a fixed SS=0 kernel-stack region.  Timer, syscall, and cooperative
yield paths all build a compatible frame shape: saved interrupted `SS:SP`,
general registers on the interrupted stack, and a kernel-stack frame saved in
`pcb_t.sp`.

`i16_ctx_switch()` is called directly from `arch_sched_switch()`.  It builds
a synthetic IRET-compatible frame for a kernel-originated yield, saves the
current kernel SP and entry-stub shadows into the outgoing PCB, calls
`sched_next()`, restores the incoming PCB state, and jumps to the same restore
tail used by syscall/timer exits.

## m68k

m68k has separate USP and SSP.  Each process's SSP frame lives in the fixed
kstack region; `pcb_t.kernel_sp` stores the slot top and `pcb_t.sp` stores the
saved frame pointer.  Cooperative `sched_switch()` triggers TRAP #1, which
saves registers on SSP, saves SSP/USP into the outgoing PCB, calls
`sched_next()`, restores the incoming SSP/USP, and returns with the same frame
layout used by timer and syscall paths.

`arch_yield()` only sets `switch_pending`; that is appropriate for interrupt
or trap return but not enough for an immediate thread-context yield, which is
why `arch_sched_switch()` uses TRAP #1.

TRAP #1 can be entered from either user mode or supervisor mode.  In
particular, a blocking syscall such as `vfork()` calls `sched_switch()` while
already running in the kernel, so the TRAP #1 frame resumes supervisor-mode
syscall code rather than user code.  m68k vfork parent-frame restoration must
therefore check the saved SR and run only for user-mode `rte` frames; otherwise
a kernel-mode cooperative switch can consume the saved parent stack slot before
the syscall epilogue reaches the real user-mode return.

### Per-process stacks and the initial frame

Because USP and SSP are physically separate, each m68k process owns **two**
stack pages: a kernel stack (`p->stack_page`, the SSP target — exception
frames, switch save area, syscall handling) and a user stack
(`p->user_stack_page`, the USP target — argc/argv/envp/auxv and user calls).
`execve()` allocates the SSP page **before** the data pages so a LIFO
page-pool free does not block later `brk` expansion.

`proc_setup_stack()` seeds a fresh process by building, on its kernel stack,
exactly the frame the switch-restore tail pops — `d0-d7`, `a0-a6`, a 16-bit
SR, and a 32-bit PC (15×4 + 2 + 4 = 66 bytes), with `pcb_t.sp` pointing at
the saved `d0`.  Slot 13 (a5) is patched to the process GOT base (see
[targets/m68k.md §6](../targets/m68k.md#got-relocation-and-the-a5-base-register)).
The SR is `SR_USER` (S=0, IPL=0), so the restore tail
(`movem.l %sp@+,%d0-%d7/%a0-%a6; rte`) pops the registers and then `rte`
switches to user mode, making the hardware a7 the USP.  The same `movem`/`rte`
shape is what the timer and TRAP #1 paths restore, so a fresh process and a
preempted one resume identically.

### Exec-restore protection

There is a window in `execve()` between building the new exception frame and
the trap-return path consuming it.  A timer ISR firing here would context-
switch and save the kernel's current SSP over `current->sp`, destroying the
freshly built frame.  A **PCB-local** `exec_restore_pending` flag guards it:
`execve()` sets it (interrupts disabled) once the frame and GOT base are in
place, the timer ISR skips the switch while it is set, and the trap-return
`.Lexec_restore` path clears it after restoring the new context.  The flag
lives in the PCB (not a CPU-global) so that init starting several getties in
quick succession cannot let one process consume another's pending restore.

## RISC-V

RISC-V uses a trap-frame switch.  Trap entry swaps `sp` with `mscratch` to get
onto the current process's kernel stack, saves the full trap frame, and then
handles syscalls or interrupts.

When `switch_pending` is set, trap return calls `riscv_ctx_switch(current_sp)`.
The helper saves the outgoing trap-frame SP in `current->sp`, selects the next
process, updates `current_core`, and returns the incoming trap-frame SP.
`kernel_sp` is initialized by the common fixed-region kstack helper, and the
assembly reloads `mscratch` from the incoming `kernel_sp` before `mret`.

## Xtensa

Xtensa uses the windowed ABI, so a context switch must spill register windows
before SP can be saved.  `arch_sched_switch()` calls `xtensa_do_yield()`
directly.  That assembly builds a solicited frame, saves return PC and PS,
spills windows, disables interrupts around the SP handoff, calls
`xtensa_ctx_switch(current_sp)`, and restores the incoming frame.

`pcb_t.sp` points at this solicited frame.  The frame keeps the first 16 bytes
for ABI scratch, then stores an exit marker, PC, PS, optional user SP, saved
`a0`, and saved `a3`.  Exit marker `0` restores with `retw`; exit marker `1`
is the exec/vfork-child path and jumps directly to the entry PC after loading
the user SP.

Syscalls are handled from the illegal-instruction exception path.  If a
syscall blocks or a preemption is pending, the PPAP syscall body calls
`sched_switch()` while running on `pcb_t.kernel_sp` through the
`xtensa_syscall_on_kstack()` wrapper.  The ESP-IDF exception frame and final
`rfe` return path stay on the original exception stack.

Xtensa also enables `ARCH_EXIT_SWITCH_IN_SYSCALL_EPILOGUE`.  `sys_exit()`
marks the process zombie and returns to the Xtensa syscall epilogue, which
observes `current->state != PROC_RUNNABLE` and performs the switch from the
fixed-kstack syscall body.  Other architectures leave this capability disabled
and switch directly inside `sys_exit()`.

## vfork Parent Resume

When the kernel handles `vfork()`, the child shares the parent's user
address space — including the user stack — until the child calls `execve()`
or `_exit()`.  The parent stays `PROC_BLOCKED` for that window.  The child
re-enters the kernel for its own syscalls, and any user code it runs before
that point overwrites the part of the shared user stack that holds the
parent's resume state.  At minimum that includes the user-mode return frame
(per-arch hardware push) and the vfork stub's saved registers; the exact
size is arch-specific.

The kernel therefore saves the parent's vulnerable user-stack slice into the
parent's own kernel-stack slot before the child runs, and restores it before
the parent ever returns to user mode.  The reference implementation is i16
(see [stack.md §ia16](stack.md#ia16) and
[`i16_vfork_restore_frame()`](../../src/arch/i16/kernel/core/i16_common.c)).

### Invariant

Every kernel exit path that can resume a blocked vfork parent must call
`<arch>_vfork_restore_frame()` before its final user-mode return
instruction (`iret`, `rte`, `mret`, `bx EXC_RETURN`, `rfe`, etc.).

The resume paths to enumerate per arch are:

- syscall-trap return — the path that handled the child's `execve` /
  `_exit` and now schedules the parent next.
- timer-IRQ return — the parent may be picked asynchronously after the
  child has already exited and another preempt fires.
- cooperative-yield return — e.g. m68k `TRAP #1`, arm_m `svc #0xFF`
  sentinel.

A single skipped restore site leaves the parent `iret`ing into a clobbered
frame.  The corruption is data-dependent and hard to reproduce: i16 was
bitten by exactly this during PC/XT bring-up when only the syscall-trap
path was wired and the timer path popped junk.  Treat resume-path coverage
as a release-blocker checklist, not an afterthought.

The restore site must also be the final user-mode return, not merely any
context-switch return.  On m68k, `sched_switch()` itself is implemented with
TRAP #1 and may return to supervisor-mode syscall code.  The m68k restore
helper receives the saved register frame and skips frames whose SR has the
supervisor bit set.

### Per-arch hook contract

Each arch that uses the no-copy vfork model implements two functions,
mirroring the i16 reference:

| Hook | Caller | Job |
|------|--------|-----|
| `<arch>_vfork_save_parent_frame(pcb_t *parent)` | `sys_vfork()` in the per-arch branch | Read the parent's vulnerable user-stack slice via `mem_region_page_read()` into a fixed slot in the parent's kstack, patch the saved return-value slot with the child PID, set `parent->vfork_frame_saved = 1`. |
| `<arch>_vfork_restore_frame(void)` | every user-mode exit path that can resume `current` | If `current->vfork_frame_saved`, copy the saved slice back to user memory via `mem_region_page_write()` and clear the flag. |

The saved-slice size and layout are arch-specific; the contract is the
same: save once in `sys_vfork`, restore exactly once per vfork cycle, on
every possible resume path.

The `vfork_frame_saved` flag itself lives in `pcb_t`
([proc_info.h](../../src/kernel/common/core/proc_info.h)) and is shared
across arches; only the save/restore body is per-arch.

### Skipping the syscall return-value store

Some arches' syscall return paths write the dispatch result back into the
saved return-register slot on the user stack (e.g. i16's trap.S writing
AX).  For a vfork parent whose child is still running on the shared user
stack, that write would clobber the child's return value — the child's
in-progress code is sitting on the same byte the kernel wants to overwrite
with the parent's return.

The shared rule: if `current->vfork_frame_saved` is set, the per-arch
syscall return path must skip its own "store return value to user-stack
register slot" step.  The patched child-PID value in the saved frame is
the authoritative parent return; it lands back on the user stack when
`<arch>_vfork_restore_frame()` writes the saved slice out.

i16 exposes this via `i16_trap_should_skip_ret_store()`
([i16_common.c](../../src/arch/i16/kernel/core/i16_common.c)).  Other
arches add an analogous predicate only if their return path stores into
user memory (arm_m does not — r0 is restored from the HW frame, which the
restore already rewrites; riscv does not — a0 is in the trap frame on
kstack; m68k does not — d0 is in the saved frame on SSP).
