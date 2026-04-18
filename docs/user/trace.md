# Trace and Debug Subsystem

Reference documentation for the PPAP ptrace kernel API, `/bin/trace`, and `/bin/pdb`.

---

## Overview

PPAP provides a per-process runtime tracing facility built on a `ptrace`-like
syscall (`SYS_PTRACE`, 0x000F). A tracer (parent) can stop, inspect, modify,
and resume a tracee (child). Two user-space tools are built on this API:

- **`/bin/trace`** — `strace`-style syscall and subsystem-call logger
- **`/bin/pdb`** — interactive debugger with breakpoints, disassembly, and memory inspection

Tracing is scoped to a single parent/child relationship and imposes no overhead
on untraced processes.

### vfork constraint

Because PPAP uses `vfork()` (parent blocked while child shares its address
space), tracing becomes active only after `execve()` completes. The child is
stopped before its first user instruction, and the parent is woken.

---

## Kernel API: `SYS_PTRACE`

```c
long ptrace(long request, long pid, void *addr, void *data);
```

All structs and constants are defined in `src/common/ptrace.h`.

### Requests

| Request | Code | Description |
|---|---|---|
| `PTRACE_TRACEME` | 0 | Child marks itself as traceable |
| `PTRACE_PEEKDATA` | 2 | Read 32-bit word from tracee at `addr` into `*data` |
| `PTRACE_POKEDATA` | 5 | Write `*data` into tracee at `addr` |
| `PTRACE_CONT` | 7 | Resume tracee |
| `PTRACE_SINGLESTEP` | 9 | Execute one instruction, then stop |
| `PTRACE_GETREGS` | 12 | Copy registers into `struct ppap_ptrace_regs` at `data` |
| `PTRACE_SETREGS` | 13 | Load registers from `struct ppap_ptrace_regs` at `data` |
| `PTRACE_ATTACH` | 16 | Attach to a running process |
| `PTRACE_DETACH` | 17 | Detach tracer; tracee resumes |
| `PTRACE_SYSCALL` | 24 | Resume with syscall-stop tracing |
| `PTRACE_GETEVENT` | 0x5000 | Get last stop event (`struct ppap_ptrace_event`) |
| `PTRACE_SETMODE` | 0x5001 | Set trace mode bits |
| `PTRACE_GETCAPS` | 0x5002 | Query target capabilities (`struct ppap_ptrace_caps`) |
| `PTRACE_SETBP` | 0x5003 | Set breakpoint (`struct ppap_ptrace_bp`) |
| `PTRACE_CLRBP` | 0x5004 | Clear breakpoint by ID |
| `PTRACE_GETSURFACE` | 0x5005 | Get active debug surface (real or eCPU) |
| `PTRACE_SETSURFACE` | 0x5006 | Set debug surface |

#### PEEKDATA / POKEDATA ABI note

Unlike Linux (which returns data in the syscall return value), PPAP uses a
pointer-based ABI: `addr` points into the tracee, `data` points to a
tracer-owned 32-bit word. This avoids ambiguity with negative return values.

### Trace mode bits (`PTRACE_SETMODE`)

| Bit | Constant | Meaning |
|---|---|---|
| 0 | `PPAP_TRACE_MODE_PPAP_SYSCALL` | Stop on native PPAP syscall enter/exit |
| 1 | `PPAP_TRACE_MODE_SUBSYS_CALL` | Stop on subsystem call enter/exit |

### Stop events (`PTRACE_GETEVENT`)

```c
struct ppap_ptrace_event {
    uint32_t event;     /* PPAP_TRACE_EVENT_* */
    uint32_t flags;     /* PPAP_TRACE_FLAG_* / PPAP_DEBUG_STOP_* */
    uint32_t abi;       /* PPAP_TRACE_ABI_* */
    uint32_t nr;        /* syscall/function number */
    uint32_t args[6];   /* arguments */
    int32_t  ret;       /* return value (on EXIT events) */
};
```

Event types:

| Value | Constant | Description |
|---|---|---|
| 0 | `PPAP_TRACE_EVENT_NONE` | No event |
| 1 | `PPAP_TRACE_EVENT_EXEC` | Stopped after execve |
| 2 | `PPAP_TRACE_EVENT_SYSCALL_ENTER` | Syscall entry |
| 3 | `PPAP_TRACE_EVENT_SYSCALL_EXIT` | Syscall exit |
| 4 | `PPAP_TRACE_EVENT_SUBSYS_ENTER` | Subsystem call entry |
| 5 | `PPAP_TRACE_EVENT_SUBSYS_EXIT` | Subsystem call exit |
| 6 | `PPAP_TRACE_EVENT_DEBUG_STOP` | Debug stop (step, breakpoint) |

ABI tags:

| Value | Constant | Description |
|---|---|---|
| 0 | `PPAP_TRACE_ABI_PPAP` | Native PPAP syscalls |
| 1 | `PPAP_TRACE_ABI_H68K_DOS` | Human68k DOS calls |
| 2 | `PPAP_TRACE_ABI_H68K_IOCS` | Human68k IOCS calls |
| 3 | `PPAP_TRACE_ABI_CPM_BDOS` | CP/M BDOS calls |
| 4 | `PPAP_TRACE_ABI_CPM_BIOS` | CP/M BIOS calls |

Debug stop flags (in `event.flags` when event is `DEBUG_STOP`):

| Bit | Constant | Description |
|---|---|---|
| 0x0001 | `PPAP_DEBUG_STOP_STEP` | Single-step completed |
| 0x0002 | `PPAP_DEBUG_STOP_SW_BP` | Software breakpoint hit |
| 0x0004 | `PPAP_DEBUG_STOP_HW_BP` | Hardware breakpoint hit |

### Register inspection (`PTRACE_GETREGS` / `PTRACE_SETREGS`)

```c
struct ppap_ptrace_regs {
    uint32_t regset;                    /* PPAP_TRACE_REGSET_* */
    uint32_t abi;                       /* PPAP_TRACE_ABI_* */
    uint32_t words;                     /* number of valid entries in regs[] */
    uint32_t regs[PPAP_PTRACE_REGS_MAX]; /* up to 20 registers */
};
```

Register sets:

| Value | Constant | Register layout |
|---|---|---|
| 1 | `PPAP_TRACE_REGSET_ARM` | r0–r15 (r13=SP, r14=LR, r15=PC) |
| 2 | `PPAP_TRACE_REGSET_M68K` | d0–d7, a0–a7 (a7=SP), PC |
| 3 | `PPAP_TRACE_REGSET_Z80` | AF, BC, DE, HL, IX, IY, SP, PC |

### Capabilities (`PTRACE_GETCAPS`)

```c
struct ppap_ptrace_caps {
    uint32_t regset;    /* current register set */
    uint32_t abi;       /* current ABI */
    uint32_t surface;   /* current debug surface */
    uint32_t surfaces;  /* bitmask of available surfaces */
    uint32_t caps;      /* PPAP_PTRACE_CAP_* bitmask */
    uint32_t max_bps;   /* max breakpoints supported */
};
```

Capability bits:

| Bit | Constant | Description |
|---|---|---|
| 0 | `PPAP_PTRACE_CAP_GETREGS` | Register read supported |
| 1 | `PPAP_PTRACE_CAP_SETREGS` | Register write supported |
| 2 | `PPAP_PTRACE_CAP_PEEKPOKE` | Memory read/write supported |
| 3 | `PPAP_PTRACE_CAP_SINGLESTEP` | Single-step supported |
| 4 | `PPAP_PTRACE_CAP_SW_BP` | Software breakpoints supported |
| 5 | `PPAP_PTRACE_CAP_HW_BP` | Hardware breakpoints supported |

### Breakpoints (`PTRACE_SETBP` / `PTRACE_CLRBP`)

```c
struct ppap_ptrace_bp {
    int32_t  id;        /* assigned by kernel on SETBP; used to identify on CLRBP */
    uint32_t addr;      /* breakpoint address */
    uint32_t flags;     /* PPAP_PTRACE_BP_SW or PPAP_PTRACE_BP_HW */
};
```

Software breakpoints (`PPAP_PTRACE_BP_SW`) are available on writable text
segments (CP/M `.com`, Human68k `.x`/`.r`, eCPU m68k). Hardware breakpoints
(`PPAP_PTRACE_BP_HW`) are available on native m68k and ARM targets.

### Debug surfaces (`PTRACE_GETSURFACE` / `PTRACE_SETSURFACE`)

For processes running on an emulated CPU (eCPU), two debug surfaces exist:

| Value | Constant | Description |
|---|---|---|
| 0 | `PPAP_TRACE_SURFACE_REAL` | Native host CPU state |
| 1 | `PPAP_TRACE_SURFACE_ECPU` | Emulated CPU state (Z80 or m68k) |

`PTRACE_GETCAPS` reports available surfaces in the `surfaces` bitmask. The
active surface affects which registers are returned by `PTRACE_GETREGS`.

### waitpid integration

Trace stops are reported via `waitpid(pid, &status, WSTOPPED)`. The tracee
enters `PROC_TRACED_STOP` state, which is distinct from `PROC_SLEEPING`.

---

## `/bin/trace` — Syscall Tracer

Usage:

```sh
trace [options] <program> [args...]
```

Options:

| Flag | Description |
|---|---|
| `--ppap` | Trace native PPAP syscalls only |
| `--subsys` | Trace subsystem calls only (DOS/IOCS, BDOS/BIOS) |
| `--both` | Trace both PPAP syscalls and subsystem calls |
| `--regs` | Dump registers at each stop |

Default: `--both` (traces all call types).

Example output:

```text
[ppap] open("/etc/passwd", 0, 0) = 3
[subsys:h68k] DOS _OPEN("A:\AUTOEXEC.BAT", 0)
[subsys:cpm] BDOS 15 OPEN("A:FOO.TXT")
```

---

## `/bin/pdb` — PPAP Debugger

Interactive debugger supporting breakpoints, single-step, memory and register
inspection, and disassembly across ARM, m68k, RISC-V, and Z80 targets.

### Invocation

```sh
pdb <program> [args...]      # launch and trace
pdb --attach <pid>           # attach to running process
pdb -c "cmd" <program>       # run startup command
pdb -f <script> <program>    # run commands from file
pdb -q <program>             # quiet mode (no prompt)
pdb --batch <program>        # batch mode (suppress stop output)
```

Flags `-c` and `-f` can be repeated to queue multiple commands or scripts.

### Commands

#### Run Control

| Command | Alias | Description |
|---|---|---|
| `step` | `s` | Single-step one instruction |
| `next` | `n` | Step over call (Z80: sets temp breakpoint after CALL; others: single-step) |
| `cont` | `c`, `run`, `continue` | Continue execution |

#### Inspection

| Command | Description |
|---|---|
| `regs` | Show all registers |
| `reg <name\|idx>` | Show one register by name or index |
| `caps` | Show target capabilities |
| `event` | Show last stop event |
| `pc` | Show program counter |
| `sp` | Show stack pointer |
| `where` / `w` | Show PC and SP |
| `bt [count]` | Frame-pointer backtrace |
| `show abi` | Show current ABI |
| `show regset` | Show register set kind |
| `show surface` | Show active debug surface |
| `x <addr> [count]` | Read memory words |
| `x/<n><fmt> <addr>` | Read memory (fmt: `x`=word, `h`=half, `b`=byte) |
| `mem <addr> [count] [sz]` | Read memory with size (`b`/`h`/`w`/`1`/`2`/`4`) |
| `disas [addr] [count]` | Disassemble (ARM Thumb, m68k, Z80) |
| `surface <real\|ecpu>` | Switch debug surface |

#### Write

| Command | Description |
|---|---|
| `set reg <name\|idx> <value>` | Write register |
| `set mem <addr> <value> [size]` | Write memory word/half/byte |
| `restore mem <addr> <bytes...>` | Write raw byte sequence |

#### Breakpoints

| Command | Alias | Description |
|---|---|---|
| `break <addr>` | `b` | Set breakpoint (auto-selects SW or HW from caps) |
| `break <sw\|hw> <addr>` | `b` | Force breakpoint type |
| `enable <id>` | | Enable breakpoint |
| `disable <id>` | | Disable breakpoint |
| `delete <id>` | `d` | Delete breakpoint |
| `info break` | `info b` | List all breakpoints |

#### Session

| Command | Alias | Description |
|---|---|---|
| `detach` | | Clear breakpoints, detach, quit |
| `quit` | `q` | Detach (if stopped) and quit |
| `help` | `?` | Show command help |

### Architecture-specific notes

- **Z80 `next` command**: Detects `CALL` opcodes and sets a temporary
  breakpoint at the return address (PC+3), then continues. For non-call
  instructions, falls back to single-step.
- **Disassembly**: `disas` auto-detects the instruction set from the current
  register set (ARM Thumb, m68k, or Z80).
- **Software breakpoints**: Only available when text is in writable memory
  (eCPU processes, CP/M `.com` files). Native ARM flash code requires hardware
  breakpoints.

---

## Implementation Files

### Kernel

| File | Description |
|---|---|
| `src/common/ptrace.h` | All ptrace constants, structs, request codes |
| `src/kernel/syscall/sys_trace.c` | `SYS_PTRACE` implementation |
| `src/kernel/proc/proc.h` | PCB trace fields (`tracer_pid`, `trace_mode`, breakpoint tables, etc.) |

### User-space: trace

| File | Description |
|---|---|
| `src/user/trace.c` | `/bin/trace` implementation |

### User-space: pdb

| File | Description |
|---|---|
| `src/user/pdb.c` | Main loop and command dispatch |
| `src/user/pdb_internal.h` | Internal types and forward declarations |
| `src/user/pdb_cmd.c` | Startup option parsing, help text |
| `src/user/pdb_target.c` | Run control and session commands |
| `src/user/pdb_inspect.c` | Inspect and write commands |
| `src/user/pdb_break.c` | Breakpoint commands |
| `src/user/pdb_regs.c` | Register name/index mapping |
| `src/user/pdb_util.c` / `pdb_util.h` | String/number utilities |
| `src/user/pdb_trace_util.c` / `pdb_trace_util.h` | Event and capability formatting |

### Tests

| File | Description |
|---|---|
| `tests/user/test_pdb.c` | pdb integration tests (marked `TEST_SLOW`) |

---

## Known Limitations

- **Signal delivery to eCPU processes**: `SIGKILL` is only delivered at SVC
  return, not during the emulator loop. An eCPU process stuck in a tight loop
  without syscalls cannot be killed promptly.
- **No async attach across cores**: Attach requires the tracee to be in a
  kernel-visible stop point.
- **No global trace buffer**: Tracing is per-process stop/inspect/resume only;
  there is no system-wide non-stopping event recorder.
