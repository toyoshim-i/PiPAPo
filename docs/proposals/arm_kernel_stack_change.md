# ARM Per-Process Kernel Stack

**Status:** partially implemented; completion requires a different switch
primitive.  The per-process kernel-stack infrastructure has landed in the
current unpushed series, but the original activation step, "raise PendSV above
SVC", is known to crash and is no longer considered viable.  The latest
committed revision also has a hardware-visible regression: a booted shell does
not accept keyboard input, so the current tip is not shippable even though
`qemu_arm` tests pass.

**Target arches:** arm_m (Cortex-M0+ on RP2040, Cortex-M33 on RP2350).
**Motivation:** unblock pico1calc TTY/blocking-syscall hang; align ARM with the
other arches that already have per-process kernel stacks.

## 1. Problem

On ARM, `arch_sched_switch()` only pends PendSV.  PendSV is configured at the
lowest priority (0xFF) and **cannot preempt the SVC handler** (priority 0x80).
A blocking syscall that calls `mod_core.sched_switch()` from inside its SVC
body therefore does *not* yield: it returns immediately, the loop spins, and
PendSV never fires until the SVC handler exits.

This breaks the simple "block in syscall → idle polls device → wake reader"
model whenever the device is software-polled (e.g. PicoCalc I2C keyboard) —
because idle never runs.  UART works only because the hardware ISR fires
asynchronously; software-polled backends starve.

The post-`5720940` "internal loop" pattern in `tty.c` / `pipe.c` was written
assuming `sched_switch()` actually yields synchronously; on ARM it doesn't,
which is why pico1calc deadlocks during `push` reading from `/dev/tty1`.

## 2. Current State Across Arches

| Arch       | Kernel stack       | Yield from kernel   | Notes |
|------------|--------------------|---------------------|-------|
| arm_m      | per-proc kernel stack partly landed; activation incomplete | pend PendSV (no-op from SVC) | Needs synchronous kernel switch |
| riscv      | per-proc `kernel_sp` (PCB +52) | lazy alloc, trap-based switch | reference impl |
| ia16/pcxt  | per-proc `kernel_stack_top` (PCB +4) | trap-based switch | reference impl |
| m68k       | per-proc kernel SSP | `TRAP #1`           | reference impl |
| xtensa     | per-proc kernel sp  | windowed save/restore | reference impl |

ARM remains the outlier because, even with the per-process kernel-stack
groundwork partly landed, it still lacks a synchronous-yield-from-kernel
primitive.

## 3. Design

Give ARM each process its own kernel stack, mirroring the other arches.  This
makes the SVC handler's stack state proc-local, which is still necessary for
blocking in the middle of a syscall: the kernel must be able to park the
current C call chain, locals, and already-performed side effects, then resume
that exact continuation later.

The original plan used that stack to let PendSV preempt SVC.  That path has
failed in testing.  The remaining completion work is to add a synchronous
kernel-context switch that runs at the `sched_switch()` call site while already
inside the kernel/SVC body, without relying on PendSV preempting SVC.

### 3.1 PCB additions

In `src/kernel/common/core/proc_info.h`, inside the ARM register-save block:

```c
#if defined(__ARM_ARCH) || defined(__arm__) || defined(__thumb__)
  uint32_t r4, r5, r6, r7;
  uint32_t r8, r9, r10, r11;
  uint32_t sp;          /* saved PSP            (offset 32) */
  uint32_t kernel_sp;   /* saved MSP for this proc (offset 36) */
#endif
```

`kernel_sp` is the *top* of the proc's per-proc kernel stack page when fresh,
or the live kernel-mode SP when the proc is suspended in SVC/PendSV.

### 3.2 Allocation: borrow ia16's fixed-region layout

ia16 already has the right shape: a single kernel-stack region carved into
fixed-size slots, indexed by PID.  No per-proc page allocation, no run-time
sizing, no lazy first-switch fallback.  See
[`proc.c:99-109`](../../src/kernel/core/proc/proc.c#L99-L109):

```c
#define I16_KSTACK_REGION_BASE 0xE380u /* must match pcxt_kernel.ld */
#define I16_KSTACK_SLOT0_SIZE 128u     /* idle loop only          */
#define I16_KSTACK_SIZE 1024u          /* slots 1-7               */
```

Total ia16 budget at `PROC_MAX=8`: `128 + 7 × 1024 = 7296 B ≈ 7 KB` — far
less than my earlier 4 KB-per-page-times-8 estimate of 32 KB.

For ARM, do the same shape with arch-tunable slot sizes:

```c
/* in src/kernel/common/config.h or per-arch config: */
#ifndef PROC_KSTACK_SIZE
#define PROC_KSTACK_SIZE 1024u  /* per-proc kernel stack, slot 1+ */
#endif
#ifndef PROC_KSTACK_IDLE_SIZE
#define PROC_KSTACK_IDLE_SIZE 128u  /* PID 0 idle */
#endif
```

ARM's per-syscall depth is shallower than ia16's (no segment juggling, no
far-call save area), so 1024 B is comfortable; 512 B might also be fine
once measured with `KSTACK_USAGE_TRACK`.  Pick a per-arch default in the
proposal and refine after measurement.

Reserve the region in the ARM linker script (`pico1calc.ld` etc.) the same
way ia16 reserves it in `pcxt_kernel.ld`:

```ld
.kstack (NOLOAD) : ALIGN(8) {
    __kstack_region_base = .;
    . += PROC_KSTACK_IDLE_SIZE;
    . += (PROC_MAX - 1) * PROC_KSTACK_SIZE;
    __kstack_region_end = .;
} > RAM_KERNEL
```

`proc_init` plants canaries at slot boundaries (already done on ia16 — same
mechanism).  No run-time allocation; PID *N*'s kernel stack lives at a
compile-time-known address.

After this lands, RISC-V's lazy `kernel_sp` allocation (`riscv_common.c:209`)
becomes redundant — RISC-V can adopt the same fixed-region layout as a
follow-up.  Same for any other arch that wants it.

Per [coding_rules](../getting_started/coding_rules.md): no per-arch `#ifdef`
blocks for the body of allocation logic; only `ARCH_HAS_KERNEL_STACK` and
the size constants are conditional.

### 3.2.1 Budget on pico1calc

Worst case at `PROC_MAX=8` and `PROC_KSTACK_SIZE=1024`:
`128 + 7 × 1024 = 7296 B ≈ 7 KB` carved from `RAM_KERNEL` (currently 32 KB,
already reserves stack within it).  No impact on `RAM_PAGES` (the 192 KB
page pool stays intact).  This fits comfortably without shrinking
user-stack pages.

### 3.3 SVC entry: swap MSP to per-proc kernel stack

`src/arch/arm_m/kernel/core/trap.S` `SVC_Handler` first action becomes:

```asm
push  {r3, r4, r5, r6, r7, lr}     @ existing — saves callee-saved on the
                                   @ OUTGOING (interrupted) MSP frame
ldr   r3, =current_core
ldr   r3, [r3]                     @ r3 = current PCB (TODO: per-core)
ldr   r3, [r3, #36]                @ r3 = current->kernel_sp
mrs   r4, msp
str   r4, [sp, #-4]!               @ stash old MSP at top of new stack
msr   msp, r3                      @ swap MSP to per-proc kernel stack
```

…and `SVC_Handler` exit reverses the swap before `pop {…, pc}`.

Concretely: SVC handler runs on `current->kernel_sp` for its body; the OLD
MSP value lives in a one-word slot at the bottom of the old kernel stack
frame.  This isolates each process's in-flight kernel call chain, but by
itself it does not make PendSV-preempts-SVC safe.

### 3.4 PendSV: save/restore per-proc kernel SP

`switch.S` PendSV body adds the kernel-SP swap:

```asm
@ outgoing: save current MSP into PCB
mrs  r4, msp
str  r4, [r3, #36]                @ current->kernel_sp = MSP

@ ...sched_next, mpu_switch, etc...

@ incoming: load next->kernel_sp into MSP
ldr  r4, [r0, #36]
msr  msp, r4
```

The HW PSP frame still drives Thread-mode resumption.  The new piece is the
MSP swap so the next proc's SVC stack (if any was suspended) is restored.

### 3.5 Rejected: make PendSV preempt SVC

This was the original Phase 4 and it is known broken.

Changing PendSV priority from `0xFF` to `0x40` so it can preempt SVC at `0x80`
reproducibly crashes `runtests` during startup.  The failure happens even after
IRQ masking was added around the SVC body, so the crash is not explained by
ordinary IRQ nesting.  See `docs/notes/arm_phase4_status.md`,
`docs/notes/arm_phase4_debug.md`, and `docs/notes/arm_handoff.md`.

Do not implement this step.  It may still be useful as a debugging case, but
it is not the completion path for this proposal.

### 3.6 Required: synchronous kernel-context switch

`arch_sched_switch()` must become a true mid-kernel switch when called from an
SVC/syscall body:

```text
arch_sched_switch():
    save the current kernel continuation on current->kernel_sp
    save the user PSP/trap-frame pointer in current->sp
    choose sched_next()
    restore next->kernel_sp and next->sp
    resume the next saved kernel continuation, or first-enter its user frame
```

The important contract is stronger than "schedule soon": when
`mod_core.sched_switch()` returns to its caller, the current process must have
already been off-CPU and later resumed.  This preserves blocking syscalls with
multiple wait points or irreversible side effects.  Restart/replay is still
valid for syscalls deliberately written that way, but it is not a substitute
for this primitive.

PendSV can remain the asynchronous timer/user-preemption mechanism while this
is developed.  It must stay at the lowest priority and must not be required to
preempt SVC.

## 4. Effect on Existing Code

After the synchronous switch work lands:

- `tty_read_canon` / `tty_read_raw` / `tty_write` / `pipe_read` / `pipe_write`
  internal-loop pattern works on ARM as written.  No more spinning in SVC.
  No `svc_set_restart` dance in these paths.
- The temporary `block_self`/`unblock_self` helpers (`kernel/common/core/wait.h`)
  introduced during the regression hunt are removed; the loops use plain
  `wait_channel` / `state` assignment as the other arches do.
- Bridges (DOS/CP/M/Human68k) keep their current code; the post-`5720940`
  `-EAGAIN`/`-EINTR` semantics work uniformly across arches.  No phantom EOF.

The ongoing "move DOS bridge to userland" effort is unaffected: bridges
become user procs, hit the same SVC entry that any other user proc does, and
the per-proc kernel stack already covers them.

## 5. Verification (qemu_arm-first)

Develop and validate against `qemu_arm` (Cortex-M3 on mps2-an500) before
flashing pico1calc:

1. `./scripts/build.sh --test qemu_arm` — kernel test suite must remain
   24/24 green throughout, but this is not sufficient by itself.
2. Add an integration test that exercises the SVC-blocking-yield path:
   one process reads `/dev/ttyS0` with no data; another process is
   `RUNNABLE` and must be scheduled while the first blocks.  Verify the
   reader resumes correctly when input arrives.
3. `./scripts/build.sh --test qemu_arm` with KSTACK_USAGE_TRACK enabled
   to validate kernel stack budget per proc.
4. Once green on QEMU, flash pico1calc and confirm:
   - A freshly booted shell accepts keyboard input.  This is a current
     regression at the latest committed revision and must be fixed before the
     branch is considered healthy.
   - `push` on tty1 blocks correctly (no busy-spin).
   - Idle runs, fbcon flush keeps LCD updating.
   - I2C kbd polling fills the ring.
   - Ctrl-C / SIGINT delivery works as before.
5. Cross-target build sanity: pico1, pico2, pico2rv, qemu_arm — all
   should build clean (only arm_m affected).

## 6. Cleanup / Stage 2 (out of scope here, separate change)

- Rename ia16 `kernel_stack_top` → `kernel_sp` (keep uint16_t in the ia16
  block).  Single field name across all arches that have one.
- Migrate RISC-V from lazy `kernel_sp` allocation (`riscv_common.c:209`)
  to the same fixed-region layout used by ia16 and (after this proposal)
  ARM.  Single mechanism across the three arches that need it.
- Remove the temporary `wait.h` helpers and any `svc_set_restart`-related
  workarounds left in tty.c / pipe.c from the pico1calc regression hunt.

## 7. Risks / Open Questions

- **Stack budget.**  ~7 KB total at `PROC_MAX=8` with the ia16 layout
  (128 B idle + 7 × 1 KB).  Carved from `RAM_KERNEL` (32 KB on pico1calc
  per the post-`7f55781` layout); fits without shrinking user pages or
  touching `RAM_PAGES`.  Slot size is configurable per arch
  (`PROC_KSTACK_SIZE`); start at 1 KB for ARM, refine downward to 512 B
  if `KSTACK_USAGE_TRACK` shows the high-watermark stays well under that.
- **MSP swap timing in SVC entry.**  The SVC entry must save the OLD MSP
  somewhere reachable on exit; doing this with only the small Cortex-M0+
  ISA window requires careful register juggling.  Pre-write the asm and
  review before integration.
- **Direct switch frame shape.**  The new synchronous switch must distinguish
  two incoming cases: a process suspended inside a kernel continuation, and a
  process whose next resume is a normal exception return to user mode.  The
  frame layout must make that distinction explicit instead of inferring it from
  a PendSV-only path.
- **Booted-shell keyboard regression.**  The current committed revision passes
  qemu tests but does not accept keyboard input at the booted shell.  This is a
  release blocker and likely overlaps the original pico1calc motivation:
  qemu-only verification cannot prove this work complete.
- **Core 1.**  The same kernel-stack and switch-continuation rules must hold
  on Core 1.  Verify SMP boot still works after the change.
- **GDB/debug.**  Per-proc MSP changes how stack unwinding works in GDB.
  May need to update OpenOCD-side scripts or debug docs.

## 8. Implementation Phases

| # | Change                                                              | Verify |
|---|---------------------------------------------------------------------|--------|
| 1 | Add `kernel_sp` to ARM PCB block; allocate kernel stack page in `proc_setup_stack` (gated). | qemu_arm builds, all tests pass, no behavioral change yet. |
| 2 | SVC entry/exit: swap MSP to/from `current->kernel_sp`.              | qemu_arm builds + tests; sanity-check stack usage stays bounded. |
| 3 | PendSV: save/restore `kernel_sp`.                                   | qemu_arm tests including `test_signal`, `test_sleep_intr`. |
| 4 | Add ARM synchronous kernel-context switch for `sched_switch()` from inside SVC.  Keep PendSV low-priority for timer/user preemption. | qemu_arm tests + new blocking-yield integration test. |
| 5 | Fix/verify booted-shell keyboard input on pico1calc-class hardware. | Hardware boot: shell accepts input; idle polling and wakeup path observed. |
| 6 | Remove `wait.h` helpers and any temporary `svc_set_restart` calls; revert tty/pipe blocking paths to clean internal-loop. | qemu_arm + pico1calc hardware test. |

Each phase should be a separate commit and bisectable before this branch is
pushed.  The current unpushed history is known to contain broken intermediate
commits; see `docs/notes/arm_handoff.md`.

## 9. References

- [trap.S](../../src/arch/arm_m/kernel/core/trap.S) — current ARM SVC handler.
- [switch.S](../../src/arch/arm_m/kernel/core/switch.S) — current ARM PendSV.
- [riscv/trap.S:238](../../src/arch/riscv/kernel/core/trap.S) — RISC-V kernel-stack swap pattern.
- [i16/trap.S:148](../../src/arch/i16/kernel/core/trap.S) — ia16 kernel-stack swap pattern.
- [pico1calc_regression.md](../notes/pico1calc_regression.md) — regression hunt that surfaced this.
