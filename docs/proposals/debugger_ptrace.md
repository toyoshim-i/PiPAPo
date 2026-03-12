# Debugger Plan (`ptrace`-based)

This document defines the next implementation step after the current
`trace`/`ptrace` support: a userland debugger with register and memory edit,
single-step execution, and breakpoints.

## Scope

In scope:

- Extend `ptrace` from trace logging to debugger control.
- Support native and guest (eCPU) processes through one parent/child API.
- Expose per-tracee capabilities so userland tools can adapt at runtime.

Out of scope (first debugger phase):

- Full gdb remote protocol.
- Global system tracing.
- Arbitrary process attach across unrelated parents.

## Baseline (already implemented)

Current kernel already supports:

- `PTRACE_TRACEME`, `PTRACE_CONT`, `PTRACE_DETACH`, `PTRACE_SYSCALL`
- `PTRACE_GETEVENT`, `PTRACE_SETMODE`
- `PTRACE_GETREGS`
- `PTRACE_PEEKDATA` / `PTRACE_POKEDATA`
- trace-stop reporting via `waitpid(..., WSTOPPED)`

Files:

- `src/kernel/syscall/sys_proc.c`
- `src/common/ptrace.h`
- `src/user/trace.c`

## Key constraint: software breakpoints are mapping-dependent

Software breakpoints require writable executable bytes in the tracee image.
Architecture alone is not enough.

Support policy:

| Tracee type | Text location | SW breakpoint |
|---|---|---|
| Native ARM PPAP ELF | XIP flash / romfs | No |
| Native m68k PPAP ELF from romfs | romfs mapping (shared, not private writable text) | No |
| Human68k `.x` / `.r` | Per-process allocated RAM image | Yes |
| CP/M `.com` (Z80) | Per-process allocated Z80 RAM | Yes |
| m68k eCPU process (`exec_m68k_emu`) | Per-process allocated emulator RAM | Yes |

Implication: debugger must query capabilities per target process.

## Proposed API additions

### `PTRACE_GETCAPS` (PPAP-specific)

Add request:

- `PTRACE_GETCAPS` (PPAP-specific value, e.g. `0x5002`)

Proposed payload:

```c
struct ppap_ptrace_caps {
    uint32_t regset;      /* PPAP_TRACE_REGSET_* */
    uint32_t abi;         /* PPAP_TRACE_ABI_* */
    uint32_t caps;        /* PPAP_PTRACE_CAP_* */
    uint32_t max_bps;     /* software+hardware combined budget */
};
```

Proposed capability bits:

```c
#define PPAP_PTRACE_CAP_GETREGS     (1u << 0)
#define PPAP_PTRACE_CAP_SETREGS     (1u << 1)
#define PPAP_PTRACE_CAP_PEEKPOKE    (1u << 2)
#define PPAP_PTRACE_CAP_SINGLESTEP  (1u << 3)
#define PPAP_PTRACE_CAP_SW_BP       (1u << 4)
#define PPAP_PTRACE_CAP_HW_BP       (1u << 5)
```

### `PTRACE_SETREGS`

Add register write path to match existing `PTRACE_GETREGS`.

Use cases:

- set PC for continue-from-here
- patch SP/argument registers during debugging

Constraints:

- only when tracee is `PROC_TRACED_STOP`
- reject unsupported regsets with `-EINVAL` / `-ENOTSUP`

### `PTRACE_SINGLESTEP`

Add explicit single-step resume request.

Behavior:

- resumes tracee for one instruction
- re-stops with `SIGTRAP`
- publishes a debug stop event

### Breakpoint requests

Add PPAP-specific requests:

- `PTRACE_SETBP`
- `PTRACE_CLRBP`

`PTRACE_SETBP` should fail with `-ENOTSUP` when `PPAP_PTRACE_CAP_SW_BP`
and `PPAP_PTRACE_CAP_HW_BP` are both unavailable.

## Event model extension

Add debug stop kind:

```c
#define PPAP_TRACE_EVENT_DEBUG_STOP  6
```

Add stop reasons in `flags`:

```c
#define PPAP_DEBUG_STOP_STEP         0x0001
#define PPAP_DEBUG_STOP_SW_BP        0x0002
#define PPAP_DEBUG_STOP_HW_BP        0x0004
```

Populate:

- `args[0]`: PC at stop
- `abi`: active ABI tag

## Backend implementation plan

### Phase 1: capability + register write

1. Add `PTRACE_GETCAPS`.
2. Add `PTRACE_SETREGS` for:
   - Z80 tracees (`SUBSYS_CPM`)
   - eCPU m68k tracees (`SUBSYS_PPAP` with `subsys_data`)
   - native m68k/ARM frame-backed tracees
3. Add user test coverage for round-trip get/set regs.

### Phase 2: single-step for eCPU backends

Add per-core step op in `ecpu_core_ops_t`:

- `int (*step)(ecpu_state_t *cpu);`

Implement for:

- Z80 core
- m68k eCPU core

Debugger path:

- `PTRACE_SINGLESTEP` marks pending step
- scheduler executes one emulator instruction
- kernel raises `PPAP_TRACE_EVENT_DEBUG_STOP | PPAP_DEBUG_STOP_STEP`

### Phase 3: software breakpoints on writable guest RAM images

Implement kernel-managed breakpoint table per tracee:

- address
- original instruction bytes
- enabled state

Initial SW-BP targets:

- Human68k (`.x/.r`)
- CP/M
- eCPU m68k

Native PPAP ELF in romfs remains unsupported for SW-BP.

### Phase 4: native breakpoint/step follow-up

Native m68k and ARM can be added independently:

- m68k: evaluate SR trace mode and available debug exceptions
- ARM M0+: hardware breakpoint support only if target backend exposes it

If unsupported on a target, capability bits stay clear.

## Userland debugger workflow

Minimal loop:

1. `vfork()` child -> child `PTRACE_TRACEME` -> `execve(target)`
2. parent `waitpid(WSTOPPED)`
3. parent `PTRACE_GETCAPS`
4. parent uses supported features (`SETREGS`, `SINGLESTEP`, `SETBP`, `CONT`)
5. parent inspects events via `PTRACE_GETEVENT`

`/bin/trace` remains syscall/subsystem tracer. A dedicated debugger app should
be built on the same kernel interface.

## Userland debugger app plan: `pdb`

Proposed binary:

- `/bin/pdb` (`pdb` = PPAP debugger)

Primary goals:

- inspect memory/registers
- disassemble around PC
- manage breakpoints
- single-step / continue
- edit memory/registers

### Target launch model

Initial mode (same safety model as current tracer):

- `pdb <program> [args...]`
- child runs `PTRACE_TRACEME`, then `execve`
- parent debugger waits for initial stop and enters REPL

Optional later mode:

- `pdb --attach <pid>` (only after explicit attach semantics are implemented)

### Surface model: `real` vs `ecpu`

Debugger must support two observation surfaces:

- `real`: native CPU register surface (trap frame / native execution context)
- `ecpu`: guest CPU surface (Z80 or emulated m68k core state)

Rationale:

- subsystem processes may have both a host scheduling context and a guest CPU
  context
- debugger user sometimes wants guest ISA stepping/disassembly, not host state

Proposed control:

- `surface real`
- `surface ecpu`
- `show surface`

Kernel interface implication:

- add a ptrace request for surface select/query (for example
  `PTRACE_SETSURFACE` / `PTRACE_GETSURFACE`)
- for unsupported surfaces return `-ENOTSUP`
- include active surface and available surfaces in `PTRACE_GETCAPS`

### REPL command set (first version)

Core process control:

- `run` (for future non-stop init)
- `cont` / `c`
- `step` / `s` (single instruction)
- `next` / `n` (optional later; step-over helper in userland)
- `detach`
- `quit`

Inspection:

- `regs` (all registers on active surface)
- `reg <name>` (single register)
- `x/<n><fmt> <addr>` memory examine
- `disas <addr> [count]`
- `bt` (optional later, native-only until unwind support exists)

Mutation:

- `set reg <name> <value>`
- `set mem <addr> <value> [size]`
- `restore mem <addr> <bytes...>` (optional scripted form)

Breakpoints:

- `break <addr>`
- `delete <id>`
- `disable <id>`
- `enable <id>`
- `info break`

Quality-of-life:

- `help`
- `show caps`
- `show abi`
- `show event`

### Disassembly strategy

Disassembler backend should follow active surface:

- `real` + ARM: Thumb decoder path
- `real` + native m68k: m68k decoder path
- `ecpu` + Z80: Z80 decoder path
- `ecpu` + m68k emu: m68k decoder path

Implementation options:

- minimal in-tree disassembler (subset first)
- optional host-side richer disassembler later

### Breakpoint behavior by surface/type

`pdb` should not assume SW breakpoints are always available.

Rules:

- if `PPAP_PTRACE_CAP_SW_BP` is clear, `break` either:
  - tries HW breakpoint when `PPAP_PTRACE_CAP_HW_BP` is set, or
  - fails with clear message (`not supported on this mapping/target`)
- this naturally blocks native ARM XIP and native m68k romfs text from SW BP
- it allows Human68k/CP/M/eCPU RAM images when kernel reports capability

### Internal architecture (`pdb`)

Modules:

- target session (wait/ptrace/event loop)
- register map per surface/regset
- memory view helpers
- disassembler front-end
- breakpoint manager (kernel IDs + local metadata)
- command parser / REPL

Recommended file layout:

- `src/user/pdb_main.c`
- `src/user/pdb_target.c`
- `src/user/pdb_regs.c`
- `src/user/pdb_mem.c`
- `src/user/pdb_disas_*.c`
- `src/user/pdb_break.c`
- `src/user/pdb_cmd.c`

### Implementation phases for `pdb`

Phase A:

- attach-on-exec flow
- `regs`, `x`, `cont`, `step`, `quit`
- `surface` command and caps display

Phase B:

- `set reg`, `set mem`
- disassembly output around PC

Phase C:

- full breakpoint commands
- robust stop-reason reporting (step vs bp)

Phase D:

- script mode (`-c "cmd"`)
- optional non-interactive output mode for tests

## Testing plan

Add user tests:

- `test_trace_debug_regs`: `SETREGS` changes PC or GP register as expected
- `test_trace_debug_step_z80`: CP/M tracee single-step
- `test_trace_debug_step_m68k_emu`: emulated m68k single-step
- `test_trace_debug_swbp_cpm`: SW-BP hit in CP/M image
- `test_trace_debug_caps`: capability matrix sanity per tracee type
- `test_pdb_smoke`: launch target under `pdb` scripted mode, run `regs`,
  `x`, and `step`

Non-goal for first pass:

- asserting native ARM SW-BP behavior (should return `-ENOTSUP`)
