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
preemption or wakeups.  On flag-based architectures it sets
`switch_pending`; on ARM Cortex-M it pends PendSV.

`sched_check_preempt()` consumes pending switch requests in shared idle paths.
Trap and interrupt return paths also check their architecture-specific pending
state before returning to user code.

## Restartable Syscalls

Some syscalls use the restart path instead of preserving a kernel
continuation.  These syscalls set `syscall_restart[core]`, save the original first
argument in `syscall_saved_arg0[core]`, block the process, and yield.  The trap
return path then rewinds the saved user PC to the syscall instruction and
restores the first argument, so the syscall re-executes when the process runs
again.

Restart is only correct before externally visible side effects or for syscalls
whose body is deliberately written to replay.  TTY, pipe, and subsystem code
that loops across multiple waits uses continuation blocking instead.

## Architecture Summary

| Arch | Async preemption | `sched_switch()` from kernel context | Trap-return switch |
|------|------------------|--------------------------------------|--------------------|
| `arm_m` | SysTick pends PendSV | Direct `arm_kernel_sched_switch()` for blocked non-restart syscalls in Handler mode; otherwise PendSV | PendSV tail-chain after SVC/IRQ return |
| `ia16` | PIT INT 08h checks `switch_pending` | Direct `i16_ctx_switch()` builds the same frame shape as interrupt/syscall paths | `i16_trap_after_switch` restores via the shared IRET tail |
| `m68k` | Timer/trap return checks `switch_pending` | TRAP #1 switches immediately through `m68k_trap1_handler` | TRAP/timer assembly saves SSP/USP, calls `sched_next()`, restores incoming SSP/USP |
| `riscv` | Timer interrupt sets `switch_pending` | Machine-mode `ecall` switches immediately through `riscv_ctx_switch()` | `riscv_ctx_switch(current_sp)` swaps trap-frame SPs and refreshes `mscratch` |
| `xtensa` | Timer ISR sets `switch_pending` | Direct `xtensa_do_yield()` spills register windows and switches solicited frames | Syscall/fault handlers call `sched_switch()` before returning through `rfe` |

## ARM Cortex-M

ARM uses PendSV for asynchronous preemption.  PendSV stays at the lowest
priority and must not preempt SVC.  SVC runs with the process's per-process
MSP; see [`stack.md`](stack.md).

When `sched_switch()` is called inside Handler mode and the current process is
`PROC_BLOCKED` without `syscall_restart`, `arch_sched_switch()` calls
`arm_kernel_sched_switch()` directly.  That path saves the live kernel
continuation on MSP, saves PSP in `pcb_t.sp`, marks the PCB as a kernel
continuation, calls the scheduler, reloads MPU/debug state for the incoming
process, and restores either an incoming kernel continuation or a normal
Thread/PSP frame.

If the process is not eligible for the direct blocked-syscall path,
`arch_sched_switch()` pends PendSV instead.

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
