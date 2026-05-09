# Kernel Stacks

PPAP lets a process block while it is already inside the kernel.  When that
happens, the architecture layer must preserve both the user resume point and
the kernel continuation that called `sched_switch()`.  The exact frame shape is
architecture-specific, but the contract is the same: after a blocked process is
scheduled again, it resumes from the same kernel call chain unless the syscall
explicitly chose the restart/replay path.

## Common Concepts

`pcb_t.sp` is the saved architecture context pointer.  On some architectures
it points at a kernel stack frame; on others it points at a trap frame or a
solicited switch frame.  Architectures with a separate kernel stack also store
a top or live kernel-stack pointer in `pcb_t.kernel_sp`.

The fixed-region kernel-stack helper in `src/kernel/core/proc/kstack.c` is
used when `PROC_HAS_FIXED_REGION_KSTACK` is defined.  It expects the target
linker script to reserve `__kstack_region_base` and splits that region into:

| Slot | Owner | Size |
|------|-------|------|
| 0 | idle process | `PROC_KSTACK_IDLE_SIZE` |
| 1..`PROC_MAX - 1` | user processes | `PROC_KSTACK_SIZE` each |

For fixed-region stacks, the helper plants a sentinel at the slot base and a
4-byte guard at the true top.  `kernel_sp` is initialized to
`true_top - PROC_KSTACK_GUARD_BYTES`, so the active stack should not overwrite
the top guard.  `proc_check_kstack_canary_panic()` checks those sentinels.

The common defaults are:

```c
#define PROC_KSTACK_SIZE 1024u
#define PROC_KSTACK_IDLE_SIZE 128u
```

## Architecture Summary

| Arch | Kernel stack owner | Saved PCB fields | Switch mechanism |
|------|--------------------|------------------|------------------|
| `arm_m` | Fixed region per process, MSP kernel stack | `sp` = PSP/user context, `kernel_sp` = MSP, `svc_msp` = SVC entry MSP, `kernel_context` distinguishes suspended kernel continuations | PendSV for async preemption; direct `arm_kernel_sched_switch()` for blocked non-restart syscalls inside SVC |
| `ia16` | Fixed region per process, SS=0 kernel stack | `sp` = saved kernel-stack SP, `kernel_sp` = slot top, plus entry-stub shadows | Timer INT 08h and `i16_ctx_switch()` save `SS:SP` on the kernel stack and restore through a shared IRET tail |
| `m68k` | Per-process supervisor stack from the process stack page | `sp` = SSP, `usp` = user stack pointer | TRAP/timer paths save full frames on SSP, call `sched_next()`, then restore incoming SSP/USP |
| `riscv` | Per-process kernel stack at the top of the process stack page | `sp` = trap-frame SP, `kernel_sp` = `mscratch` kernel-stack top | Trap entry swaps `sp` with `mscratch`; `riscv_ctx_switch()` swaps trap-frame SPs and refreshes `mscratch` |
| `xtensa` | One process stack carries user and kernel/switch frames | `sp` = solicited switch frame | `xtensa_do_yield()` spills register windows, saves SP, calls `sched_next()`, and restores the incoming solicited frame |

## ARM Cortex-M

ARM uses two hardware stack pointers: PSP for Thread/user mode and MSP for
Handler/kernel mode.  Each process has a fixed MSP slot and
`pcb_t.kernel_sp` tracks the live MSP for that process.

`SVC_Handler` saves the original MSP in `current->svc_msp`, switches MSP to
`current->kernel_sp` when needed, and runs the syscall body on that per-process
kernel stack.  On SVC exit it restores the saved entry MSP before touching the
original entry frame.

PendSV remains the low-priority async preemption path.  Its switch code saves
the outgoing MSP into `current->kernel_sp` and restores the incoming MSP from
`next->kernel_sp`, so later exceptions for that process run on its own slot.
PendSV must stay below SVC priority; it is not used to preempt an active SVC.

When `sched_switch()` is called inside Handler mode and the current process is
`PROC_BLOCKED` without SVC restart, `arch_sched_switch()` calls
`arm_kernel_sched_switch()` directly.  That path saves the in-flight kernel
continuation on MSP, saves PSP in `pcb_t.sp`, marks the PCB as a kernel
continuation, selects the next runnable process, and restores either the next
kernel continuation or the next normal user/PSP context.

All ARM Cortex-M targets use 2 KB user-process slots:

```cmake
PROC_KSTACK_SIZE=2048u
```

At `PROC_MAX=8`, this reserves `128 + 7 * 2048 = 14464` bytes from
`RAM_KERNEL`.

## ia16

ia16 always uses a fixed SS=0 kernel-stack region.  Slot 0 is the idle process
and slots 1..7 are user processes.  The mature slot geometry is 128 bytes for
idle and 1024 bytes for each user process.

On syscall or timer entry, the CPU first pushes the interrupt frame on the
interrupted stack.  The handler saves general registers there, captures the
interrupted `SS:SP`, switches to the current process's SS=0 kernel stack if it
came from user mode, and pushes the captured `SS:SP` on the kernel stack.
`pcb_t.sp` stores the saved kernel-stack SP.  `pcb_t.kernel_sp` stores the slot
top used by entry code and by the vfork frame helpers.

`i16_current_ksp` mirrors the current process's kernel-stack top so interrupt
entry can switch stacks before it is safe to dereference `current_core`.
Context switches also shadow the core/VFS entry-stub return globals into the
PCB, making those stubs effectively per-process while a process is suspended.

The restore path is shared across timer, syscall, and cooperative yield:
restore `SS:SP`, pop the saved general registers from the interrupted stack,
and `iret`.

`KSTACK_USAGE_TRACK` is implemented by the common fixed-region kstack helper.
ia16 and ARM syscall paths can paint unused stack space and report high-water
marks while keeping the feature compile-time optional.

## m68k

m68k user code runs with a user stack pointer (USP), while supervisor/kernel
code uses the supervisor stack pointer (SSP).  PPAP stores the suspended SSP in
`pcb_t.sp` and the current USP in `pcb_t.usp`.

TRAP and timer paths save the register frame on SSP.  When a switch is needed,
they save SSP/USP into the outgoing PCB, call `sched_next()`, load the
incoming PCB's SSP/USP, restore registers from the incoming SSP, and return
with `rte`.

Cooperative `sched_switch()` uses TRAP #1 so the switch happens immediately
instead of waiting for a later interrupt to consume `switch_pending`.

## RISC-V

RISC-V uses `mscratch` as the current process's kernel-stack top.  Trap entry
starts with `csrrw sp, mscratch, sp`, which swaps the user SP into `mscratch`
and moves execution onto the process's kernel stack.  The trap frame then
saves the full register set and the original user SP.

`pcb_t.sp` points at the saved trap frame.  `pcb_t.kernel_sp` is the kernel
stack top loaded back into `mscratch` when that process becomes current.
`riscv_ctx_switch(current_sp)` saves the outgoing trap-frame SP, calls
`sched_next()`, makes the selected process current, ensures `kernel_sp` is
initialized from the process stack page if necessary, and returns the incoming
trap-frame SP.  Trap return refreshes `mscratch` from the incoming
`kernel_sp`.

## Xtensa

Xtensa does not have a separate per-process kernel stack field.  `pcb_t.sp`
points at a solicited switch frame on the process stack.  Because the ESP32-S3
uses the windowed ABI, a cooperative switch must spill all register windows
before saving SP.

`xtensa_do_yield()` builds a solicited frame, saves return PC and PS, spills
windows via the Xtensa HAL, disables interrupts for the SP handoff, calls
`xtensa_do_switch(current_sp)`, and restores the incoming frame.  A fresh
process uses a marked "new-process" frame so the restore path can jump to the
entry point directly instead of using `retw`.

Syscalls are dispatched from the Xtensa illegal-instruction exception handler.
If the syscall blocks or a preemption is pending, the handler calls
`sched_switch()`, which runs the same solicited-frame switch path and then
returns to the exception dispatcher for `rfe`.

## Restart Versus Continuation

The kernel has two blocking models:

- Continuation blocking: the syscall blocks inside kernel code, switches away,
  and later resumes after `sched_switch()` returns.
- Restart blocking: the syscall records original arguments, rewinds the trap
  PC, and re-executes when scheduled again.

Restart is only for syscalls that are deliberately written to be replay-safe.
For syscalls with multiple waits or side effects, the architecture must support
continuation blocking.
