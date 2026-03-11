# PPAP Trace Proposal

This document proposes a runtime tracing facility for PPAP that can replace
most compile-time debug logging around syscalls and subsystem bridges.

The short version is:

- Keep `klog()` and compile-time debug macros for early bring-up.
- Add a parent/child tracing syscall for normal syscall logging and debugging.
- Make the first implementation `ptrace`-like, not a global `ktrace` log sink.
- Integrate it with `waitpid()` stop notifications so user space can build
  `strace`-style and debugger-style tools.

## Problem

Today syscall and bridge tracing mostly relies on compile-time switches such as:

- `SYSCALL_DEBUG` in `src/kernel/syscall/syscall.c`
- `H68K_DEBUG` in `src/kernel/subsys/human68k_bridge.c`

That is useful during kernel bring-up, but it has clear limits:

- It is global, not per-process.
- It is enabled at build time, not at runtime.
- It floods the UART/LCD log path.
- It cannot stop a child at syscall entry or exit.
- It gives the parent no chance to inspect registers or memory.
- It does not scale into a real debugger or `strace`-like tool.

In other words, the current logging is diagnostic output, not a tracing API.

## Goals

- Let a parent trace one child at runtime.
- Support `strace`-style syscall logging.
- Support debugger-style inspection of registers and memory while the child is stopped.
- Work across native ARM, native m68k, the eCPU m68k personality, and the
  eCPU Z80 personality used by CP/M.
- Avoid mandatory global logging in the kernel fast path.
- Reuse existing PPAP process and wait semantics where possible.

## Non-goals for the first version

- Full Linux `ptrace` compatibility.
- Hardware breakpoints or single-step support.
- A high-throughput system-wide event recorder.
- Async attach to arbitrary running processes on another core.

## Recommendation

Add a new syscall, preferably `SYS_PTRACE`, as the primary tracing primitive.

Recommended initial placement:

- `src/common/syscall_nr.h`: `#define SYS_PTRACE 0x000F`

Why `ptrace`-like instead of `ktrace` first:

- It gives a direct path to a user-space `strace`.
- It gives a direct path to a debugger.
- It keeps tracing scoped to a traced child instead of turning UART into a global trace bus.
- It fits PPAP's small process table and simple scheduler better than a system-wide event stream.

A buffered `ktrace` mode can still be added later on top of the same syscall
entry/exit hooks.

## PPAP-specific constraint: `vfork()` changes the design

PPAP does not implement a real `fork()`. `fork()` and `clone(SIGCHLD, 0)` both
route to `sys_vfork()`, so the parent is blocked while the child still shares
the parent's address space.

This means classic Linux `PTRACE_TRACEME` behavior is not safe before `execve()`:

- the parent cannot safely run while the child still shares its memory
- the child cannot be stopped and inspected in the middle of the `vfork` window

Because of that, the initial tracing design should be "trace on exec", not
"trace immediately after fork".

Recommended rule:

- `PTRACE_TRACEME` marks the current child as traceable
- the kernel does not stop the child immediately
- after a successful `execve()`, when the child owns a fresh image, the kernel
  stops it before its first user instruction and wakes the parent

That preserves PPAP's current `vfork` safety model while still enabling
`strace(child -> execve(target))`.

## Proposed API surface

### Syscall

```c
long ptrace(long req, long pid, void *addr, void *data);
```

Recommended request set across the initial phases:

- `PTRACE_TRACEME`
- `PTRACE_CONT`
- `PTRACE_SYSCALL`
- `PTRACE_DETACH`
- `PTRACE_GETREGS`
- `PTRACE_PEEKDATA`
- `PTRACE_POKEDATA`
- `PTRACE_SETOPTIONS`

Requests that can wait:

- none in the first cut; waiting should happen through `waitpid()`

### `waitpid()` integration

Extend `waitpid()` to report traced-stop events.

Recommended additions:

- add `WSTOPPED`
- later optionally add `WCONTINUED`
- add `SIGTRAP` to `src/kernel/signal/signal.h`
- return a normal stopped status using `SIGTRAP`

That lets a tracer follow the familiar loop:

1. child does `PTRACE_TRACEME`
2. child does `execve()`
3. parent gets a `waitpid(..., WSTOPPED)` stop
4. parent inspects state with `ptrace()`
5. parent resumes with `PTRACE_SYSCALL`
6. child stops again on syscall entry or exit

## Event model

The kernel should treat tracing as explicit stop points, not as direct logging.

Recommended first stop reasons:

- `TRACE_STOP_EXEC`
- `TRACE_STOP_SYSCALL_ENTER`
- `TRACE_STOP_SYSCALL_EXIT`
- `TRACE_STOP_EXIT` (optional; normal zombie wait may be enough at first)

Recommended future stop reasons:

- signal-delivery stop
- subsystem-call enter/exit for Human68k DOS/IOCS and CP/M BDOS

For syscall tracing, the kernel should snapshot a stable event record in the
target PCB before waking the parent.

Example event structure:

```c
struct trace_syscall_info {
    uint32_t stop_reason;   /* enter / exit / exec */
    uint32_t abi;           /* native ARM, native m68k, eCPU m68k, eCPU z80 */
    uint32_t nr;
    uint32_t args[6];
    int32_t  ret;
    uint32_t pc;
    uint32_t flags;         /* restarted, error, etc. */
};
```

`PTRACE_GETREGS` remains useful for debugger work, but `strace` should not need
to decode raw arch register frames just to print `open("/etc/passwd", ...)`.

## Trace classes

The tracer should be able to enable two logically separate classes of events:

- native PPAP syscall trace
- subsystem-call trace

This matters most for subsystem processes.

Examples:

- a native PPAP ELF only needs native PPAP syscall trace
- an eCPU m68k process running the PPAP ABI also only needs native PPAP
  syscall trace
- a Human68k process may want subsystem-call trace for DOS/IOCS visibility
- a CP/M process may want subsystem-call trace for BDOS/BIOS visibility

The useful part is that these classes should be independently selectable.

For a subsystem process, the tracer may want:

- only subsystem-call trace
  this gives a clean DOS/IOCS or BDOS/BIOS view
- only native PPAP syscall trace
  this shows what the bridge ultimately does inside PPAP
- both
  this lets the user correlate a subsystem call with the native PPAP syscalls
  the bridge issued on its behalf

Recommended trace mode bits:

- `TRACE_MODE_PPAP_SYSCALL`
- `TRACE_MODE_SUBSYS_CALL`

And optionally later:

- `TRACE_MODE_SIGNAL`
- `TRACE_MODE_EXEC`

## Kernel state changes

### PCB additions

`src/kernel/proc/proc.h` will need tracer state, for example:

```c
pid_t    tracer_pid;
uint8_t  trace_mode;        /* none / syscall / continue-stop */
uint8_t  trace_pending;     /* child should stop on next trace point */
uint8_t  trace_stopped;     /* child currently stopped for tracer */
uint8_t  trace_phase;       /* syscall enter vs exit */
struct trace_syscall_info trace_info;
```

A dedicated process state is cleaner than overloading `PROC_BLOCKED`.

Recommended new state:

- `PROC_TRACED_STOP`

That keeps stopped tracees out of normal wakeup paths such as poll timeouts or
pipe wakeups.

### Scheduler and wait interaction

`src/kernel/proc/sched.c` and `src/kernel/syscall/sys_proc.c` need three changes:

1. `sched_next()` must never pick `PROC_TRACED_STOP`.
2. `waitpid()` must detect traced-stop children when `WSTOPPED` is set.
3. resume requests (`PTRACE_CONT`, `PTRACE_SYSCALL`, `PTRACE_DETACH`) must
   transition the child back to `PROC_RUNNABLE`.

`/proc/<pid>/stat` should also report a trace-stop state, ideally `T`, so
userland tools can distinguish a sleeping task from a traced task.

## Syscall path hook points

The right place to drive this is around `syscall_dispatch()`, not by printing
from each individual syscall implementation.

Recommended flow inside `src/kernel/syscall/syscall.c`:

1. Decode syscall number and arguments.
2. If the process is in syscall-trace mode, snapshot an ENTER event.
3. Stop the child and wake the tracer.
4. When resumed, dispatch the syscall normally.
5. Snapshot an EXIT event with the return value.
6. Stop the child and wake the tracer again.

That gives the parent the same two observation points that `strace` expects.

This design also naturally covers:

- native ARM trap path
- native m68k trap path
- eCPU m68k running the native PPAP ABI through `ppap_m68k_bridge.c`

## eCPU m68k split: native PPAP ABI vs Human68k ABI

The eCPU m68k case needs to be split into two sub-cases.

### eCPU m68k with native PPAP ABI

When an emulated m68k process is using the native PPAP ABI, `TRAP #0` is
translated into `syscall_dispatch()` by:

- `src/kernel/subsys/ppap_m68k_bridge.c`

That means the main syscall entry/exit trace hooks are already the right place
to observe the interesting events. In this mode, eCPU m68k behaves much more
like native ARM or native m68k from the tracer's perspective.

The only extra work here is register export:

- arguments and return values still follow the m68k syscall ABI
- full register inspection comes from the emulated CPU state in `pcb->subsys_data`,
  not from a native kernel trap frame

### eCPU m68k with Human68k ABI

Human68k is different. The interesting OS-facing operations do not primarily
show up as native PPAP syscalls. They enter the kernel-facing personality layer
through the Human68k bridge:

- `src/kernel/subsys/human68k_bridge.c`

So if the goal is `strace`-like visibility into Human68k DOS and IOCS calls,
the kernel needs subsystem trace events in addition to native syscall trace
events.

Recommended Human68k event families:

- `TRACE_STOP_SUBSYS_ENTER`
- `TRACE_STOP_SUBSYS_EXIT`

With an m68k-subsystem ABI tag, the event payload should capture at least:

- DOS or IOCS call type
- function number or opcode
- key input registers and stack-derived arguments
- guest `PC`
- return registers after the bridge call

This is the same class of requirement as eCPU Z80 CP/M:

- native PPAP syscalls are only part of the story
- subsystem bridge entry/exit is the real compatibility boundary that tools
  want to observe

## eCPU Z80 / CP/M path

The Z80 case needs an explicit addition to this proposal because it does not
reach `syscall_dispatch()` directly.

CP/M programs running on eCPU Z80 enter the kernel-facing personality layer via:

- `ECPU_TRAP_CALL` for `CALL 0x0005` (BDOS) and `CALL 0x0000` (warm boot / BIOS path)
- `ECPU_TRAP_IO_IN` and `ECPU_TRAP_IO_OUT` for port-based hooks

So `strace`-style coverage for CP/M cannot be implemented only around native
syscall entry/exit. The trace design should treat the Z80 personality layer as
another ABI producer of trace events.

Recommended event families for eCPU Z80:

- `TRACE_STOP_SUBSYS_ENTER`
- `TRACE_STOP_SUBSYS_EXIT`

With `abi = eCPU z80`, the event payload should capture at least:

- BDOS or BIOS call type
- function number
  CP/M BDOS uses register `C` for the function number
- key argument registers
  commonly `DE`, `HL`, and `A`
- guest `PC`
- return registers after the bridge call

That gives a parent enough information to build a CP/M-aware trace tool, for
example printing BDOS `fn 9` string output or file-open calls, without forcing
all CP/M activity through fake native syscall numbers.

Implementation-wise, the hook point is the CP/M bridge and eCPU trap callback
path, not the native syscall dispatcher:

- `src/kernel/subsys/cpm_bridge.c`
- `src/kernel/ecpu/ecpu_z80.c`
- the per-process emulated CPU state stored in `pcb->subsys_data`

The same parent/child stop, inspect, and resume mechanism should still be used.
Only the event source changes.

## User-space tracer plan

The kernel API is only half the design. The proposal should explicitly plan for
at least one small user-space tracing tool.

Recommended first tool:

- `trace`

Initial usage model:

```sh
trace /bin/sh
trace --ppap /bin/ls
trace --subsys /subsys/human68k/foo.x
trace --ppap --subsys /subsys/cpm/mbasic.com
```

Recommended command-line flags:

- `--ppap`
  enable native PPAP syscall trace
- `--subsys`
  enable subsystem-call trace
- `--regs`
  show register state at each stop
- `--raw`
  print undecoded numbers for bring-up

Reasonable default policy:

- for native PPAP targets: default to `--ppap`
- for subsystem targets: default to `--subsys`

That keeps the output aligned with what the user usually cares about.

### Behavior for subsystem processes

When the target is running inside a subsystem, the user should be able to turn
PPAP trace and subsystem trace on independently.

Examples:

- Human68k:
  `trace --subsys hello.x`
  prints DOS/IOCS calls
- Human68k:
  `trace --ppap hello.x`
  prints the native PPAP syscalls issued by the bridge
- Human68k:
  `trace --ppap --subsys hello.x`
  prints both views
- CP/M:
  `trace --subsys foo.com`
  prints BDOS/BIOS calls
- CP/M:
  `trace --ppap --subsys foo.com`
  prints BDOS/BIOS plus the underlying PPAP file and TTY operations

For combined mode, the event record should identify its class clearly so the
tool can render output like:

```text
[subsys:h68k] DOS _OPEN("A:\\AUTOEXEC.BAT", 0)
[ppap] open("/a/AUTOEXEC.BAT", O_RDONLY, 0) = 3
```

or:

```text
[subsys:cpm] BDOS 15 OPEN("/a/FOO.TXT")
[ppap] open("/a/FOO.TXT", O_RDONLY, 0644) = 4
```

### Recommended user-space phases

Phase A:

- minimal tracer that forks, requests tracing, execs target, and prints stops
- support `--ppap`
- decode native PPAP syscall names and basic arguments

Phase B:

- add subsystem decoding for Human68k and CP/M
- support `--subsys`
- print DOS/IOCS and BDOS/BIOS symbolic names

Phase C:

- support combined mode `--ppap --subsys`
- correlate nested events for a single subsystem call
- add filtering by syscall or subsystem function number

Phase D:

- optional debugger-style commands
- optional attach mode if kernel support is later added

## `execve()` hook

The "trace on exec" rule makes `execve()` the most important lifecycle hook.

Recommended behavior in `sys_execve()`:

- if the process is marked `TRACEME`, keep the current `vfork` parent blocked
  until `execve()` finishes loading the new image
- once the new image is ready, stop the child in `PROC_TRACED_STOP`
- populate a `TRACE_STOP_EXEC` record
- wake the parent instead of immediately running the child

This is the point where parent and child no longer share the old address space,
so inspection is safe again.

For eCPU Z80 children, the same rule still applies at the process level:

- tracing begins after the PPAP-side `execve()` or loader transition has created
  the child image
- subsequent trace stops may then occur on CP/M BDOS/BIOS events instead of
  native syscall entry/exit

## Register and memory inspection

To support debugging in addition to logging, the kernel should allow inspection
while the tracee is stopped.

Recommended minimum:

- `PTRACE_GETREGS`
- `PTRACE_PEEKDATA`
- `PTRACE_POKEDATA`

Notes:

- On ARM, raw registers are split between the saved exception frame and the PCB.
- On native m68k, the saved trap frame already contains a richer register set.
- On eCPU m68k, registers live in the emulated CPU state stored in `pcb->subsys_data`.
- On eCPU Z80, registers also live in `pcb->subsys_data`, but the exported
  register set is completely different: `AF`, `BC`, `DE`, `HL`, `IX`, `IY`,
  `SP`, `PC`, `IFF1`, `IFF2`, and `IM`.

The implementation should normalize these into architecture-specific export
structs, not expose raw kernel stack layout directly.

## Restarted syscalls

PPAP already restarts some blocking syscalls through `svc_restart`.

Tracing needs a clear rule here. Recommended first rule:

- emit `SYSCALL_ENTER`
- if the syscall blocks and will be restarted internally, do not emit a final
  `SYSCALL_EXIT` yet
- when the syscall re-enters after wakeup, emit another `SYSCALL_ENTER` with a
  `TRACE_FLAG_RESTART`
- emit `SYSCALL_EXIT` only when the syscall produces a user-visible return value

That is easier to reason about than inventing an internal pseudo-return code
that user space never otherwise sees.

## Why not start with `ktrace`?

A pure `ktrace` design would mean:

- append events to a ring buffer
- expose that buffer to user space, probably through a special file or syscall
- let the target continue running without stop/resume control

That is attractive for low-overhead logging, but it does not solve the debugger
use case, and it still leaves open the question of how a parent inspects child
state at a precise syscall boundary.

So the recommendation is:

- first build the stop/inspect/resume control plane
- later add an optional non-stopping ring-buffer backend using the same event hooks

## Extension to subsystem bridges

This mechanism should not be limited to native PPAP syscalls forever.

The same model can replace `H68K_DEBUG` and similar compile-time bridge traces:

- Human68k DOS call enter/exit
- Human68k IOCS call enter/exit
- CP/M BDOS/BIOS bridge calls

That would let a tracer say "show me Human68k file calls from this child"
without rebuilding the kernel with `#ifdef` logging.

## Suggested implementation phases

### Phase 1: core tracing

- add `SYS_PTRACE`
- add tracer fields to `pcb_t`
- add `PROC_TRACED_STOP`
- add `WSTOPPED` handling to `waitpid()`
- implement `PTRACE_TRACEME`, `PTRACE_SYSCALL`, `PTRACE_CONT`, `PTRACE_DETACH`
- stop on `execve()`, syscall entry, and syscall exit
- add a small `trace_syscall_info` snapshot
- define a parallel event snapshot path for eCPU Z80 CP/M BDOS/BIOS stops

### Phase 2: inspection

- implement `PTRACE_GETREGS`
- implement `PTRACE_PEEKDATA` and `PTRACE_POKEDATA`
- implement Z80 register export for traced CP/M children
- add user-space test coverage

### Phase 3: polish

- add `PTRACE_SETOPTIONS`
- support `PTRACE_O_TRACESYSGOOD`
- add `/proc/<pid>/stat` trace-stop reporting
- document syscall ABI in `docs/syscall.md`

### Phase 4: optional buffered mode

- add per-process or global trace ring buffer
- expose it through a dedicated read interface
- allow "log only, do not stop" tracing

## Files likely to change

- `src/common/syscall_nr.h`
- `src/kernel/proc/proc.h`
- `src/kernel/proc/sched.c`
- `src/kernel/syscall/syscall.h`
- `src/kernel/syscall/syscall.c`
- `src/kernel/syscall/sys_proc.c` or a new `src/kernel/syscall/sys_trace.c`
- `src/kernel/fs/procfs.c`
- `src/kernel/subsys/cpm_bridge.c`
- `src/kernel/subsys/human68k_bridge.c`
- `src/kernel/subsys/ppap_m68k_bridge.c`
- `src/kernel/ecpu/ecpu_z80.c`
- `src/kernel/ecpu/ecpu_z80.h`
- `src/user/syscall.h`
- `docs/syscall.md`
- new tests under `tests/user/`

## Recommendation summary

Yes, PPAP should move from compile-time syscall logging to a real runtime trace
mechanism.

The best first step is a minimal `ptrace`-like syscall with parent/child stop,
inspect, and resume semantics.

The important PPAP-specific adjustment is that tracing should become active only
after `execve()` completes, because the current `vfork`-only process model does
not make a pre-exec traced stop safe.
