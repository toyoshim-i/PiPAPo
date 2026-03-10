# Human68k Subsystem — Design and Implementation Plan

Load and execute Sharp X68000 Human68k binaries on PPAP, bridging
Human68k DOS calls to PPAP's native syscall interface.

---

## Table of Contents

1. [Goals and Scope](#1-goals-and-scope)
2. [Background: Human68k and X68000](#2-background-human68k-and-x68000)
3. [Architecture](#3-architecture)
4. [Binary Loader, Blocks, and Process Model](#4-binary-loader-blocks-and-process-model)
5. [OS Personality: DOS Call Bridge](#5-os-personality-dos-call-bridge)
6. [IOCS Bridge (TRAP #15)](#6-iocs-bridge-trap-15)
7. [Memory Model](#7-memory-model)
8. [File System Mapping](#8-file-system-mapping)
9. [Abstraction Layer for eCPU Portability](#9-abstraction-layer-for-ecpu-portability)
10. [Implementation Plan](#10-implementation-plan)
11. [Testing Strategy](#11-testing-strategy)
12. [Open Questions](#12-open-questions)

---

## 1. Goals and Scope

### 1.1 Primary Goal

Run existing Human68k command-line programs (.x executables) on PPAP,
starting with simple utilities and working toward more complex
applications.

### 1.2 Execution Modes

| Host Architecture | Execution Method | Personality Layer |
|---|---|---|
| **m68k (native)** | Direct execution — no CPU emulation | F-line exception handler in kernel |
| **ARM / other (future)** | ecpu-m68k interpreter (kernel-embedded) | Kernel-side personality bridge |

On m68k hosts, the subsystem is a thin kernel-side shim: the F-line
exception handler intercepts `$FFxx` opcodes and translates them to
PPAP syscalls. No CPU emulation is needed — the binary runs natively.

On non-m68k hosts, the subsystem uses the ecpu-m68k emulator core
(see `feature-eCPU.md`) paired with the same personality logic, all
embedded in the kernel.

### 1.3 Scope

**In scope (initial):**
- X-format (.x) binary loader
- Human68k DOS call bridge (file I/O, console I/O, process management)
- Basic IOCS passthrough on X68000 hardware (TRAP #15)
- File path translation (Human68k drive:path → PPAP /path)

**In scope (later):**
- eCPU-based execution on non-m68k architectures
- R-format (.r) relocatable loader (device drivers, TSRs)
- Extended DOS calls (memory management, directory operations)

**Out of scope:**
- Full Human68k environment emulation (environment process, CONFIG.SYS)
- Graphics/sound IOCS calls (beyond basic text I/O)
- TSR programs that hook interrupt vectors
- Undocumented Human68k kernel internals

### 1.4 Design Constraints

- On m68k, the subsystem must coexist with PPAP's own `trap #0`
  syscall mechanism — Human68k uses F-line exceptions ($FFxx), not
  TRAP instructions, so there is no conflict
- The personality layer must be structured so the same translation
  logic works for both:
  (a) native m68k execution (F-line exception handler), and
  (b) eCPU emulation on other architectures (interpreter callback)
  Both paths are kernel-embedded; they differ only in the memory
  access abstraction (real pointers vs emulated memory)
- Memory overhead must be minimal — the bridge is pure translation
  logic with no large tables or buffers

---

## 2. Background: Human68k and X68000

### 2.1 Human68k Overview

Human68k is the native OS for the Sharp X68000 series (1987–1993).
It is a single-tasking, single-user DOS with a command-line interface
similar to MS-DOS but designed for the Motorola 68000 CPU.

Key characteristics:
- DOS call interface via F-line exceptions (`dc.w $FFxx`)
- IOCS (firmware I/O) via `TRAP #15`
- X-format executable files (.x)
- Drive-letter file paths (`A:\DIR\FILE.EXT`)
- 8.3 filenames (v1–v2), 18.3 filenames (v3), unlimited with
  community TwentyOne.sys driver — see §8.2
- No memory protection, flat address space

### 2.2 DOS Call Mechanism

Human68k programs invoke DOS functions by executing an F-line
instruction — a 16-bit word in the range `$FF00–$FFFF`. The 68000
CPU treats any instruction matching `1111xxxx xxxxxxxx` as an illegal
opcode and traps to vector 11 (the "line 1111 emulator" exception at
address `$002C`).

The DOS call number is the low byte of the F-line opcode. Arguments
are pushed onto the stack **before** the F-line instruction:

```asm
; Human68k: _OPEN(path, mode)
    pea     path_string     ; push path pointer
    move.w  #mode, -(sp)    ; push mode
    dc.w    $FF3D           ; _OPEN DOS call
    addq.l  #6, sp          ; clean up stack (4+2 bytes)
    ; d0.l = file handle or negative error
```

Return value is in `d0.l`. Negative values indicate errors.

### 2.3 X-Format Binary

The X-format executable (.x) is the standard binary format for
Human68k. It is considerably simpler than ELF:

```
Offset  Size  Field
------  ----  -----
0x00    2     Magic: 0x4855 ("HU")
0x02    1     Reserved (load mode)
0x03    1     Reserved (load address)
0x04    4     Base address (usually 0)
0x08    4     Exec entry point (offset from base)
0x0C    4     Text segment size
0x10    4     Data segment size
0x14    4     BSS segment size
0x18    4     Relocation table size
0x1C    4     Symbol table size
0x20    4     SCD line number table size (debug)
0x24    4     SCD symbol table size (debug)
0x28    4     SCD string table size (debug)
0x2C    4     Reserved (0)
0x30    4     Reserved (0)
0x34    4     Reserved (0)
0x38    4     Reserved (0)
0x3C    4     Bind list address (0 if none)
------  ----
0x40    ...   Text segment
        ...   Data segment
        ...   Relocation table
        ...   Symbol table (optional)
```

**Total header size: 64 bytes (0x40).**

The relocation table contains a list of fixup locations within the
text+data segments that need the load address added. The format is:
- First entry: 2-byte displacement from the start of the image
- Subsequent entries: 2-byte relative displacement from the previous
  fixup; if the value is 0x0001, the next 4 bytes are an extended
  displacement
- Odd displacements indicate word-sized (16-bit) relocations; even
  displacements indicate long-sized (32-bit) relocations
- Terminated when the table is exhausted (size from header field)

### 2.4 R-Format Binary

The R-format (.r) is the other executable format used by Human68k,
primarily for device drivers and TSR (Terminate and Stay Resident)
programs. It is simpler than X-format:

- **No magic header** — the file does not start with `HU` or any
  other signature. It is a raw binary image.
- **No explicit segment separation** — the entire file is loaded
  contiguously into memory as-is (text+data combined).
- **No BSS section** — there is no header field specifying an
  uninitialized data region.
- **No relocation table** — R-format binaries are either
  position-independent or pre-relocated for a fixed load address.

Detection: a file with `.r` extension that does not begin with
`HU` (0x4855) is treated as R-format. Since R-format has no magic
bytes, extension-based detection is the primary method.

R-format loading is straightforward:
1. Read the entire file into memory
2. Set PC to the start of the loaded image
3. No relocation processing needed

**Scope in PPAP:** R-format is lower priority than X-format. Most
user-facing Human68k applications use X-format. R-format support
enables device drivers and TSR utilities, but these typically
depend on direct hardware access (interrupt vector installation,
memory-resident hooks) that is harder to bridge.

### 2.5 Human68k Error Codes

Human68k uses negative error codes in d0. Common values:

| Code | Name | Meaning | PPAP errno |
|---|---|---|---|
| -1 | Invalid function | Bad DOS call number | ENOSYS |
| -2 | File not found | | ENOENT |
| -3 | Directory not found | | ENOENT |
| -4 | Too many open files | | EMFILE |
| -5 | Not a directory/volume | | ENOTDIR |
| -6 | File handle invalid | | EBADF |
| -7 | Memory control block destroyed | | ENOMEM |
| -8 | Not enough memory | | ENOMEM |
| -12 | Invalid access mode | | EACCES |
| -13 | Invalid file handle | | EBADF |
| -15 | Invalid drive | | ENOENT |
| -19 | Write protected | | EROFS |
| -22 | Invalid data | | EINVAL |
| -23 | Invalid filename | | EINVAL |
| -26 | Disk full | | ENOSPC |
| -80 | File already exists | | EEXIST |

The bridge must translate between PPAP errno values and Human68k
error codes in both directions.

---

## 3. Architecture

### 3.1 Native m68k Path (No Emulation)

On m68k PPAP, Human68k binaries run natively on the real CPU. The
subsystem consists of:

1. **Binary detection** in `exec()` — recognises `HU` magic
2. **X-format loader** — loads text/data, applies relocations
3. **F-line exception handler** — kernel vector 11 handler that
   decodes the DOS call and dispatches to the personality bridge
4. **Personality bridge** — translates Human68k DOS calls to PPAP
   syscalls

```
Human68k .x binary (runs natively on 68000)
    │
    │ dc.w $FFxx (F-line instruction)
    │
    ▼
68000 CPU: line 1111 exception → vector 11
    │
    ▼
F-line handler (kernel)
    │ decode $FFxx opcode
    │ read arguments from user stack
    │
    ▼
human68k_dos_call(nr, args)  ← personality bridge
    │ translate to PPAP syscall
    │ translate return code
    │
    ▼
PPAP kernel syscall (sys_open, sys_read, etc.)
```

The F-line handler is installed alongside PPAP's TRAP #0 handler.
There is no conflict: PPAP uses TRAP #0, Human68k uses F-line.
A process can even mix both calling conventions (though there is no
practical reason to do so).

### 3.2 eCPU Path (Cross-Architecture)

On non-m68k hosts, the subsystem runs the ecpu-m68k interpreter
**inside the kernel**, as part of the process's execution context.
The `exec()` path detects the Human68k binary, sets up emulated
memory, and enters the eCPU interpreter loop. The scheduler
preempts the emulator like any other process — from the kernel's
perspective, the emulator loop is just the process's "user mode"
execution.

```
Human68k .x binary
    │
    │ exec() detects HU magic → kernel loads X-format
    │ kernel enters ecpu-m68k interpreter loop
    │
    ▼
ecpu-m68k (kernel): F-line instruction detected
    │
    ▼
human68k_dos_call(nr, args)  ← same bridge as native path
    │ translate to PPAP syscall
    │
    ▼
PPAP kernel syscall (sys_open, sys_read, etc.)
```

**Why kernel-embedded, not /subsys/?**

The `feature-subsystem.md` general framework places emulators as
user-space binaries under `/subsys/`. For the Human68k subsystem,
kernel-embedded is preferable:

- **Native path is already kernel-side.** The F-line handler is a
  CPU exception (vector 11), the X-format loader extends `exec()`,
  and the bridge calls internal syscall handlers. Placing the eCPU
  path in kernel too keeps a single code path for both.
- **No re-exec overhead.** A user-space emulator requires the kernel
  to `exec()` a separate binary, set up argv, and start a new
  process image. Kernel-embedded avoids this.
- **Direct syscall dispatch.** The bridge calls `sys_open()`,
  `sys_read()`, etc. directly — no trap instruction overhead for
  each translated DOS call.
- **Pointer handling is simpler.** The kernel can access emulated
  memory (which is just kernel-allocated pages) directly. A
  user-space emulator would need to translate guest pointers to
  host pointers for every syscall argument.
- **Consistent with PPAP's design.** PPAP is a monolithic kernel
  with no MMU on most targets. The kernel/user boundary is thin.
  Adding ~4 KB of bridge code to the kernel is negligible.
- **No romfs bloat.** No separate emulator ELF needed in romfs.

The eCPU interpreter (ecpu-m68k) is compiled into the kernel and
conditionally enabled per-target via `ENABLE_SUBSYS_HUMAN68K` in
the target's build configuration. On m68k targets, only the bridge
and loader are compiled (no eCPU needed). On ARM targets, the
ecpu-m68k interpreter is also included. Targets that do not enable
the flag pay no code size cost.

### 3.3 Process Model

Each Human68k binary runs as a regular PPAP process. It has:
- A PID in the PPAP process table
- Normal file descriptors (mapped from Human68k file handles)
- Standard PPAP signal handling (SIGINT → Human68k Ctrl-C)
- Per-process Human68k state (current drive, PDB, environment)
  stored in a `h68k_state_t` extension in the PCB

On native m68k, the process runs its own code directly. The kernel
intervenes only on F-line exceptions (DOS calls) and TRAP #15
(IOCS calls).

On eCPU hosts, the process "runs" the ecpu-m68k interpreter loop
in kernel context. The interpreter reads instructions from the
process's emulated memory pages and executes them. When the
scheduler's timer fires, it preempts the interpreter and switches
to another process — the emulator state (emulated registers, PC)
is saved in the PCB alongside the host register context.

Human68k is single-tasking, but PPAP can run multiple Human68k
binaries concurrently (each in its own process with independent
emulator state). The bridge translates per-process state
independently.

### 3.4 Component Layering

All components live in the kernel. The memory abstraction allows
the same bridge code to work for both native and eCPU paths (§9).

```
┌─────────────────────────────────────────────────────┐
│  PPAP kernel                                        │
│                                                     │
│  ┌───────────────────────────────────────────────┐  │
│  │  human68k_bridge.c (shared)                   │  │
│  │  - DOS call dispatch table                    │  │
│  │  - Argument decoding (from stack frame)       │  │
│  │  - File path translation                      │  │
│  │  - Error code translation                     │  │
│  │  - File handle ↔ fd mapping                   │  │
│  ├──────────────────────┬────────────────────────┤  │
│  │  Native m68k path    │  eCPU path (ARM etc.)  │  │
│  │                      │                        │  │
│  │  fline_handler.S     │  ecpu_m68k.c           │  │
│  │  (vector 11 entry)   │  (interpreter loop)    │  │
│  │                      │                        │  │
│  │  Reads args from     │  Reads args from       │  │
│  │  real user stack     │  emulated memory       │  │
│  ├──────────────────────┴────────────────────────┤  │
│  │  exec_x68k.c — X-format / R-format loader     │  │
│  ├───────────────────────────────────────────────┤  │
│  │  Memory abstraction interface                 │  │
│  │  h68k_read/write_u8/u16/u32(ctx, addr)        │  │
│  │  h68k_translate_ptr(ctx, addr, size)           │  │
│  └───────────────────────────────────────────────┘  │
│                                                     │
│  sys_open, sys_read, sys_write, ...  (existing)     │
└─────────────────────────────────────────────────────┘
```

---

## 4. Binary Loader, Blocks, and Process Model

### 4.1 Detection

In the `exec()` detection chain (after native ELF and before
raw binary fallback):

```c
/* First two bytes of file */
if (file[0] == 0x48 && file[1] == 0x55)  /* "HU" (big-endian) */
    → Human68k X-format subsystem

/* Extension-based fallback (no magic match) */
if (extension == ".r")
    → Human68k R-format subsystem
```

X-format is detected by magic bytes (`HU` = 0x4855). R-format has
no magic — it is detected by the `.r` file extension. The `.x`
extension can serve as a secondary hint for X-format, but the magic
is authoritative.

### 4.2 X-Format Load Procedure

```
1. Parse 64-byte X-format header
2. Validate: magic == 0x4855, sizes are sane
3. Calculate total memory: text + data + bss + stack
4. Allocate memory:
   - Native m68k: allocate PPAP pages via page_alloc()
   - eCPU: allocate within emulated address space via brk()
5. Copy text segment from file to memory
6. Copy data segment from file to memory
7. Zero BSS region
8. Apply relocations:
   - Read relocation table
   - For each entry: *(uint32_t*)(base + offset) += load_address
9. Set up initial state:
   - PC = load_address + entry_offset
   - SP = top of allocated stack
   - USP (user stack pointer) for argument passing
```

### 4.3 Initial Register State

Human68k sets specific register values before entering a program:

| Register | Value |
|---|---|
| a0 | Memory management pointer (メモリ管理ポインタ) |
| a1 | Program end + 1 address |
| a2 | Command line address |
| a3 | Environment address (-1 if none) |
| a4 | Program execution start address |
| SR | User mode |
| USP | Parent stack pointer |
| d0–d7 | Undefined |
| a5–a6 | Undefined |

All available memory is allocated to the process at load time
(see §7.3). The program is expected to call `_SETBLOCK` to
release unneeded memory.

**PPAP setup:** the loader populates a0–a4 to match. a0 points
to a synthesized memory management block (§4.4). a2 points to
the command line in LASCIIZ format (first byte = length, then the
string, then null terminator). a3 = -1 initially (no environment).

### 4.4 Memory Management Block (メモリ管理ポインタ)

Human68k tracks memory allocations using 16-byte-aligned blocks
that form a doubly-linked list. Every `_MALLOC` allocation and
every loaded program has an associated block:

```
Offset  Size  Field
------  ----  -----
0x00    4     Previous block pointer (0 = first block)
0x04    4     Owner process pointer; high byte = attributes
0x08    4     Block end address + 1
0x0C    4     Next block pointer (0 = last block)
------  ----
0x10    ...   Usable memory starts here
```

**Owner high byte attributes:**
- `$00` — normal allocation
- `$FF` — resident process (`_KEEPPR`)
- `$FD` — sub-memory block (child allocation)

PPAP emulates this structure at the start of each process's
memory region so that programs reading a0 at startup see a valid
block. In the initial (single-process) implementation, there is
only one block per process — the "all memory" block set up by
the loader, later shrunk by `_SETBLOCK`.

### 4.5 Process Management Block (プロセス管理ポインタ)

The process management block (PMB) is a 256-byte structure at the
start of a process's memory. It is a superset of the memory
management block — the first 16 bytes are the MMB header, followed
by process-specific fields. Programs access it via `_GETPDB`.

```
Offset  Size  Field
------  ----  -----
0x00    16    Memory management block header (§4.4)
0x10    4     Environment address (-1 if unallocated)
0x14    4     Exit return address
0x18    4     Ctrl-C handler address
0x1C    4     Error abort handler address
0x20    4     Command line address
0x24    12    File handle bitmap (96 handles, 1 bit each)
0x30    4     BSS start address
0x34    4     Heap start address
0x38    4     Initial stack address (heap end + 1)
0x3C    4     Parent USP
0x40    4     Parent SSP
0x44    2     Parent SR
0x46    2     Abort SR
0x48    4     Abort SSP
0x4C    20    Saved TRAP #10–#14 vectors (5 × 4 bytes)
0x60    4     Shell flag (0 = normal, -1 = shell)
0x64    1     Module number
0x68    4     Child process memory pointer
0x80    2     Execution file drive
0x82    66    Execution file path
0xC4    24    Execution file name
------  ----
0x100   ...   Program text starts here
```

**PPAP implementation:** the bridge allocates 256 bytes at the
start of the process's memory and populates the PMB. Key fields:
- 0x10: a3 value (environment pointer, -1 initially)
- 0x14: kernel exit handler address
- 0x20: a2 value (command line pointer)
- 0x24: file handle bitmap (initialised with stdin/stdout/stderr)
- 0x30–0x38: segment addresses from X-format header
- 0x82/0xC4: path and name of the executed binary

Programs that don't inspect the PMB work without it. Programs
that read their own PMB via `_GETPDB` ($FF51) get the address
of offset 0x00 (the MMB header at the start of the 256-byte block).

### 4.6 Process Teams (Future)

Human68k is single-tasking, but it supports **process chains** —
parent→child relationships tracked via the MMB owner pointer
(offset 0x04) and the child pointer (PMB offset 0x68). A chain
of `_EXEC`'d processes shares drive/directory state.

PPAP initially runs each Human68k binary as an isolated PPAP
process — a single PMB with no chain. Future work will introduce
**process teams**: multiple Human68k processes (parent + children
via `_EXEC`) that share:
- The same virtual address space (native m68k) or emulated memory
- File handle bitmaps and drive state
- Chained MMB/PMB linked lists

This matches Human68k's model where a parent `_EXEC`'s a child
that inherits the parent's environment and memory context. From
PPAP's perspective, a process team is still one PPAP process
(one PID, one set of fd's) with multiple Human68k execution
contexts chained internally.

### 4.7 Command Line Format (LASCIIZ)

Human68k uses "LASCIIZ" format for the command line passed to
programs (pointed to by a2):

```
Offset  Content
------  -------
0x00    Length byte (string length, max 255)
0x01    Command line string (space-separated arguments)
0x01+N  Null terminator (0x00)
```

The first byte is the string length (not counting the null),
followed by the argument string, followed by a null byte.

### 4.8 Environment Format

The environment block (pointed to by a3) has:

```
Offset  Content
------  -------
0x00    4-byte size of environment area
0x04    VAR1=value1\0
        VAR2=value2\0
        \0              (double null = end of environment)
```

PPAP initially passes a3 = -1 (no environment). When environment
support is added, the bridge will allocate an environment block
and populate it from the PPAP process's environ.

### 4.9 R-Format Load Procedure

R-format loading is simpler than X-format — no header parsing or
relocation:

```
1. Read entire file into memory (no header to skip)
2. Allocate memory: file_size + stack
   - Native m68k: allocate PPAP pages via page_alloc()
   - eCPU: allocate within emulated address space
3. Copy file contents directly into allocated memory
4. Set up initial state:
   - PC = load_address (start of loaded image)
   - SP = top of allocated stack
```

No BSS zeroing (R-format has no explicit BSS), no relocation.
The binary must be either position-independent or linked for a
specific load address. In practice, most R-format binaries are
device drivers that install themselves at fixed addresses — PPAP
loads them at the base of the allocated region.

**Limitations:** R-format binaries that install interrupt handlers,
hook vectors, or go memory-resident (TSR) via `_KEEPPR` will not
work correctly. These require deep Human68k kernel emulation that
is out of scope. R-format support is primarily useful for simple
programs that happen to use the .r format rather than .x.

### 4.10 Native vs eCPU Loading

| Aspect | Native m68k | eCPU |
|---|---|---|
| Memory allocation | PPAP kernel pages | `brk()`/`mmap()` in emulator process |
| Text segment | Loaded into PPAP pages | Loaded into `ecpu_state.memory[]` |
| Relocation fixups | Writes to real memory | Writes to emulated memory |
| Entry point | Set in exception frame | Set in `ecpu_state.pc` |
| Stack setup | Set USP via `proc_setup_stack()` | Set emulated SP |

The loader code itself can be shared — the difference is where memory
reads/writes go, which is abstracted by the memory interface (§9).

---

## 5. OS Personality: DOS Call Bridge

### 5.1 DOS Call Dispatch Table

The bridge intercepts F-line exceptions and dispatches based on the
low byte of the `$FFxx` opcode:

The following table lists all known Human68k DOS calls (based on
Human68k v3.02 and the run68x emulator). The "Priority" column
indicates which PPAP implementation phase targets each call.

| DOS Call | Number | PPAP Translation | Status |
|---|---|---|---|
| **Console I/O** | | | |
| `_EXIT` | $FF00 | `sys_exit(0)` | ✅ |
| `_GETCHAR` | $FF01 | `sys_read(0, &ch, 1)` + echo | ✅ |
| `_PUTCHAR` | $FF02 | `sys_write(1, &ch, 1)` | ✅ |
| `_COMINP` | $FF03 | `sys_read(0, &ch, 1)` (raw, no echo) | ✅ |
| `_COMOUT` | $FF04 | `sys_write(1, &ch, 1)` (raw) | ✅ |
| `_MOVE` | $FF05 | `sys_write(1, &ch, 1)` | ✅ |
| `_INPOUT` | $FF08 | Output or check input (0xFF=check) | ✅ |
| `_PRINT` | $FF09 | `sys_write(1, str, len)` | ✅ |
| `_GETS` | $FF0A | Line-buffered `sys_read(0, ...)` | ✅ |
| `_KFLUSH` | $FF0C | Flush keyboard + input subfunc | ✅ |
| `_CONCTRL` | $FF10 | Console control (putc, print, color, ^C) | ✅ |
| `_INKEY` | $FF07 | `sys_read(0, &ch, 1)` (raw) | ☐ |
| `_KEYSNS` | $FF0B | Non-blocking key sense | ☐ |
| `_CHGDRV` | $FF0E | Change current drive | ☐ |
| `_DRVCTRL` | $FF0F | Return 0x02 (drive ready) | ☐ |
| **File Handle I/O** | | | |
| `_FGETC` | $FF1B | `sys_read(fd, &ch, 1)` | ✅ |
| `_FGETS` | $FF1C | Line read from fd | ✅ |
| `_FPUTC` | $FF1D | `sys_write(fd, &ch, 1)` | ✅ |
| `_FPUTS` | $FF1E | `sys_write(fd, str, len)` | ✅ |
| `_NAMECK` | $FF18 | Parse filename into components | ✅ |
| `_ALLCLOSE` | $FF1A | Close all file handles (fd 3+) | ✅ |
| **System** | | | |
| `_SUPER` | $FF20 | Logical supervisor mode toggle | ✅ |
| `_FFLUSH` | $FF24 | No-op (unbuffered I/O) | ✅ |
| `_GETDATE` | $FF2A | Fixed date (2026-01-01) | ✅ |
| `_SETDATE` | $FF2B | No-op, return 0 | ✅ |
| `_GETTIME` | $FF2C | Fixed time (00:00:00) | ✅ |
| `_SETTIME` | $FF2D | No-op, return 0 | ✅ |
| `_VERNUM` | $FF30 | Return 0x48550302 (HU + v3.02) | ✅ |
| `_KEEPPR` | $FF31 | TSR — just exits | ✅ |
| `_BREAKCK` | $FF33 | Return 1 (break check on) | ✅ |
| `_INTVCG` | $FF35 | Get interrupt vector — stub | ✅ |
| `_DSKFRE` | $FF36 | Free space from page allocator | ✅ |
| `_GETENV` | $FF38 | Return -1 (not found) | ✅ |
| `_FNCKEY` | $FF21 | Function key string get/set | ☐ |
| `_INTVCS` | $FF25 | Set interrupt vector | ☐ |
| `_GETDPB` | $FF32 | Return -1 (no DPB) | ☐ |
| **File Operations** | | | |
| `_MKDIR` | $FF39 | `sys_mkdir(path, 0755)` | ✅ |
| `_RMDIR` | $FF3A | `sys_rmdir(path)` | ✅ |
| `_CHDIR` | $FF3B | `sys_chdir(path)` | ✅ |
| `_CREATE` | $FF3C | `sys_open(path, O_CREAT\|O_TRUNC\|O_WRONLY)` | ✅ |
| `_OPEN` | $FF3D | `sys_open(path, flags)` | ✅ |
| `_CLOSE` | $FF3E | `sys_close(fd)` | ✅ |
| `_READ` | $FF3F | `sys_read(fd, buf, len)` | ✅ |
| `_WRITE` | $FF40 | `sys_write(fd, buf, len)` | ✅ |
| `_DELETE` | $FF41 | `sys_unlink(path)` | ✅ |
| `_SEEK` | $FF42 | `sys_lseek(fd, off, whence)` | ✅ |
| `_CHMOD` | $FF43 | Synthesize attrs from `sys_stat()` (query-only) | ✅ |
| `_IOCTRL` | $FF44 | Device info query (console vs file) | ✅ |
| `_DUP` | $FF45 | `sys_dup(fd)` | ✅ |
| `_DUP2` | $FF46 | `sys_dup2(old, new)` | ✅ |
| `_CURDIR` | $FF47 | `sys_getcwd(buf, size)` | ✅ |
| `_CURDRV` | $FF19 | Return 0 (drive A:) | ✅ |
| `_RENAME` | $FF56 | Stub — returns -ENOSYS (no sys_rename) | ⚠️ |
| `_FILEDATE` | $FF57 | Get: fixed date; Set: no-op | ✅ |
| **Memory** | | | |
| `_MALLOC` | $FF48 | Contiguous page allocation + availability | ✅ |
| `_MFREE` | $FF49 | Free tracked allocation blocks | ✅ |
| `_SETBLOCK` | $FF4A | Resize block (shrink/grow pages) | ✅ |
| `_MALLOC2` | $FF58 | Same as `_MALLOC` (v2 mode selector) | ☐ |
| **Process** | | | |
| `_EXEC` | $FF4B | Mode 0: `proc_alloc` + `do_execve` + wait | ✅ |
| `_EXIT2` | $FF4C | `sys_exit(code)` | ✅ |
| `_ASSIGN` | $FF4D | Drive assignment — stub (return 0) | ✅ |
| `_FILES` | $FF4E | Directory search with wildcard matching | ✅ |
| `_NFILES` | $FF4F | Continue directory search | ✅ |
| `_GETPDB` | $FF51 | Return PMB address | ✅ |

**48 DOS calls implemented** (44 functional + 4 stubs returning safe defaults).

**Note:** DOS call numbers $FF80–$FFAF map to $FF50–$FF7F (subtract
$30); this aliasing is handled in the dispatcher.

Unimplemented DOS calls fall through to the default handler which
returns -ENOSYS and logs a trace message (when H68K_DEBUG is enabled).

### 5.2 Console I/O Translation

Human68k's console I/O calls are tightly coupled to the X68000's
keyboard and display hardware. The bridge maps them to PPAP's
`/dev/ttyS0` (or the process's controlling terminal):

- `_GETCHAR`/`_COMINP` → `read(0, &ch, 1)`
- `_PUTCHAR`/`_COMOUT` → `write(1, &ch, 1)`
- `_PRINT` → scan for null terminator, `write(1, str, len)`
- `_GETS` → line-buffered read with the `linebuf` structure:
  ```
  struct linebuf {
      uint8_t max;      /* max chars */
      uint8_t len;      /* actual chars read (filled by DOS) */
      char    buf[];    /* data */
  };
  ```

The bridge handles the `linebuf` structure translation: read up to
`max` bytes from stdin, write `len` and fill `buf`.

### 5.3 File Handle Translation

Human68k file handles are small integers (0–15 typically), similar
to UNIX file descriptors. The mapping is straightforward:

| Human68k Handle | PPAP fd |
|---|---|
| 0 (stdin) | 0 |
| 1 (stdout) | 1 |
| 2 (stderr) | 2 |
| 3 (stdaux) | Map to fd 2 (or /dev/null) |
| 4 (stdprn) | Map to /dev/null |
| 5+ (user files) | Direct 1:1 mapping |

Human68k handle numbers happen to align with UNIX fd numbers for
the common cases, so no complex translation table is needed.

### 5.4 Argument Decoding

Human68k DOS calls pass arguments on the stack. The F-line handler
must read arguments from the user stack at the point of the exception.

On entry to the F-line exception, the CPU has pushed `{SR, PC}` onto
the supervisor stack. The user stack pointer (USP) still points to
the arguments the program pushed before the `dc.w $FFxx`:

```
USP → [arg_n]    (last argument pushed, lowest address)
       [arg_n-1]
       ...
       [arg_1]   (first argument pushed, highest address)
```

The bridge reads arguments from USP based on the DOS call number's
known signature. Each DOS call has a fixed number and type of
arguments (word, long, pointer).

### 5.5 Error Code Translation

The bridge must translate between PPAP errno values (POSIX-style
negative returns) and Human68k error codes:

```c
static int ppap_to_human68k_error(int ppap_err) {
    switch (ppap_err) {
        case -ENOENT:     return -2;   /* File not found */
        case -EMFILE:     return -4;   /* Too many open files */
        case -ENOTDIR:    return -5;   /* Not a directory */
        case -EBADF:      return -6;   /* Invalid handle */
        case -ENOMEM:     return -8;   /* Out of memory */
        case -EACCES:     return -12;  /* Access denied */
        case -EINVAL:     return -22;  /* Invalid data */
        case -ENOSPC:     return -26;  /* Disk full */
        case -EEXIST:     return -80;  /* File exists */
        case -EROFS:      return -19;  /* Write protected */
        case -ENOSYS:     return -1;   /* Invalid function */
        default:          return -1;   /* Generic error */
    }
}
```

For successful operations, Human68k typically returns 0 or a positive
value (file handle, byte count, etc.) — same convention as PPAP.

---

## 6. IOCS Bridge (TRAP #15)

### 6.1 IOCS on X68000 Hardware

On real X68000 hardware running PPAP, the IPL ROM's IOCS is resident
in ROM and handles `TRAP #15` directly. Human68k programs that use
IOCS calls work without any bridge — the ROM handles them natively.

### 6.2 IOCS on Non-X68000 Hosts

On QEMU or non-m68k hosts, IOCS is not available. The subsystem must
provide stubs for commonly used IOCS calls:

| IOCS Call | d0 Value | Function | Status |
|---|---|---|---|
| `_B_KEYINP` | $00 | Wait for key input | ✅ `sys_read(0, &ch, 1)` |
| `_B_KEYSNS` | $04 | Key sense (non-blocking) | ✅ returns 0 |
| `_SKEY_MOD` | $0E | Shift key status | ✅ returns 0 |
| `_B_UP` | $19 | Cursor up | ✅ ANSI `ESC[A` |
| `_B_DOWN` | $1A | Cursor down | ✅ ANSI `ESC[B` |
| `_B_RIGHT` | $1B | Cursor right | ✅ ANSI `ESC[C` |
| `_B_LEFT` | $1C | Cursor left | ✅ ANSI `ESC[D` |
| `_B_PUTC` | $20 | Output character | ✅ `sys_write(1, &ch, 1)` |
| `_B_PRINT` | $21 | Print string | ✅ `sys_write(1, str, len)` |
| `_B_COLOR` | $22 | Set text color | ✅ stub (returns 0) |
| `_B_LOCATE` | $23 | Set cursor position | ✅ stub (returns 0) |
| `_B_CLRST` | $2A | Clear screen | ✅ ANSI `ESC[nJ` |
| `_B_ERA_AL` | $2B | Clear to end of line | ✅ ANSI `ESC[0K` |
| `_DATEGET` | $5A | Get BCD date | ✅ fixed 2026-01-01 |
| `_TIMEGET` | $5B | Get BCD time | ✅ fixed 00:00:00 |
| `_ONTIME` | $7F | Uptime (1/100s) | ✅ `sched_get_ticks()` |
| Others | — | — | returns -1 (logged when H68K_DEBUG) |

**15 IOCS calls implemented.**

On the native m68k path (QEMU), the TRAP #15 vector is hooked
to route to these stubs. On the eCPU path, the emulator intercepts
`TRAP #15` instructions the same way it intercepts F-line.

### 6.3 IOCS on X68000 PPAP (Passthrough)

When PPAP itself runs on X68000 hardware:
- IOCS ROM is present and functional
- Human68k programs that call TRAP #15 work directly — the ROM
  handles them
- The kernel must ensure IOCS calls are serialized (disable
  interrupts around IOCS, as IOCS is non-reentrant)
- No IOCS bridge is needed in this case

---

## 7. Memory Model

### 7.1 Human68k Memory Expectations

A typical Human68k program expects:

```
$000000  Vector table (shared with OS)
$000400  System area
$006800  Human68k DOS work area
$008000+ Program load area (text → data → bss → heap)
  ...
$BFxxxx  Stack (grows down)
$C00000  GVRAM (graphics)
$E00000  TVRAM (text)
```

### 7.2 PPAP Memory Layout for Human68k Processes

The subsystem provides a simplified memory layout. On the QEMU
m68k target, all memory is RAM starting at address 0.

Note: addresses increase downward in this diagram (low addresses at top).

```
          RAM (0x00000000, 16 MB on QEMU virt)
┌──────────────────────────────────────┐
│  Vector table            (1 KB)      │  0x00000000  256 vectors × 4 bytes
│  Kernel .text + .rodata              │  0x00000400  Kernel code
│  Kernel .data + .bss                 │              Kernel data
│  Kernel stack (SSP)      (16 KB)     │              Supervisor stack
├──────────────────────────────────────┤
│  Page pool               (remainder) │  __page_pool_start (4K-aligned)
│                                      │
│  ┌──────────────────────────────┐    │
│  │  PMB header (256 bytes)     │    │  base (= process page allocation)
│  │  ┌──────────────────────┐   │    │
│  │  │  MMB: prev/owner/    │   │    │  0x00–0x0F
│  │  │       end/next       │   │    │
│  │  │  env, cmdline, fds   │   │    │  0x10–0x2F
│  │  │  bss, heap, usp      │   │    │  0x30–0x3F
│  │  │  directory, filename  │   │    │  0x80–0xDF
│  │  └──────────────────────┘   │    │
│  ├──────────────────────────────┤    │  base + 0x100
│  │  .text  (code)              │    │  a4 = text start
│  │  .rodata                    │    │
│  ├──────────────────────────────┤    │  base + 0x100 + text_size
│  │  .data  (initialized data)  │    │  a5 = data base (PIC register)
│  │  .bss   (zeroed)            │    │
│  ├──────────────────────────────┤    │
│  │  Heap (grows toward high)   │    │  Managed via _SETBLOCK / _MALLOC
│  │        ...                  │    │
│  │  (free space)               │    │
│  │        ...                  │    │
│  │  User stack (grows toward low)│   │  USP starts at base + total_bytes
│  └──────────────────────────────┘    │  base + total_bytes (high addr)
│                                      │
│  Kernel stack page (SSP, per-proc)   │  Separate page for trap handling
│                                      │
│  (remaining free pages)              │
└──────────────────────────────────────┘
```

The first 256 bytes (0x100) are the PMB, which includes the MMB
as its header. Program code starts at offset 0x100 — matching
the Human68k convention.

**Key PMB fields populated at load time:**

| Offset | Size | Field | Value |
|--------|------|-------|-------|
| 0x00 | 4 | MMB prev block | 0 (first block) |
| 0x04 | 4 | MMB owner | PMB base address (self) |
| 0x08 | 4 | MMB block end+1 | total_bytes |
| 0x0C | 4 | MMB next block | 0 (last block) |
| 0x10 | 4 | Environment pointer | -1 (none) |
| 0x20 | 4 | Cmdline address | PMB base + 0x6C |
| 0x24 | 4 | File handle bitmap | 0x07 (stdin/stdout/stderr open) |
| 0x30 | 4 | BSS start address | base + 0x100 + text_size + data_size |
| 0x34 | 4 | Heap start address | base + 0x100 + text_size + data_size + bss_size |
| 0x38 | 4 | Initial USP | base + total_bytes (top of block) |
| 0x82 | 65 | Directory path | Current working directory |
| 0xC4 | 23 | Filename | Program filename |

**Initial register values at program entry:**

| Register | Value |
|----------|-------|
| a0 | PMB base address |
| a1 | Block end (base + total_bytes) |
| a2 | Cmdline pointer (PMB base + 0x6C, empty NUL string) |
| a3 | Environment pointer (-1 = none) |
| a4 | Program text start (base + 0x100) |
| USP | Top of block (base + total_bytes) |

**Key points:**

- Both text and data are in RAM (no XIP on m68k).
- The PMB at the base provides Human68k compatibility. Programs
  receive `a0 = PMB base` and `a1 = block end` at entry.
- Heap and user stack share the free space between BSS end and block
  top. The heap grows toward higher addresses; the stack starts at the
  highest address (`base + total_bytes`) and grows toward lower
  addresses (push = `--sp`). There is no guard page — overflow is
  not detected.
- `_SETBLOCK` ($FF4A) can shrink the block to release pages back to
  the system. Programs typically call `_SETBLOCK` at startup to
  return unused memory.
- The kernel (supervisor) stack is a separate page, used only during
  trap and exception handling.

On native m68k, `base` is wherever the kernel's page allocator placed
the memory. The program doesn't know or care about the absolute
address (relocations have been applied).

On eCPU, the emulated 68000 sees a flat address space starting at
a chosen base (e.g., $010000) to avoid conflicts with the emulated
vector table area.

### 7.3 Initial Memory Allocation and _SETBLOCK Protocol

On real Human68k, the OS allocates **all available memory** to the
newly loaded process. The program is expected to call `_SETBLOCK`
early in its startup to release unneeded memory back to the OS. This
is required before `_EXEC` (loading a child process), since there
would be no free memory left otherwise. Well-behaved programs do:

```
    move.l  #needed_size,-(sp)    ; keep only what we need
    move.l  a5,-(sp)              ; our memory block address
    dc.w    $FF4A                 ; _SETBLOCK
    addq.l  #8,sp
```

PPAP follows the same protocol, bounded by the existing per-process
page limit (`USER_PAGES_MAX`, defined in `config.h`):

1. **At load time**, the loader allocates `USER_PAGES_MAX` pages
   (or all available pages, whichever is less) for the new process.
   The process sees this entire region as its own.
2. **`_SETBLOCK` shrinks** — the process calls `_SETBLOCK` to release
   the tail of the allocation. PPAP reclaims the freed pages back to
   the page allocator immediately, making them available to other
   processes.
3. **`_SETBLOCK` grows** — if the process later needs more memory
   (up to its original allocation), PPAP re-allocates pages if
   available. Growing beyond `USER_PAGES_MAX` pages fails with
   Human68k error -8 (not enough memory).

No Human68k-specific memory limit is needed — PPAP's per-process
`USER_PAGES_MAX` already serves this purpose.

### 7.4 Memory Management Calls

Human68k's `_MALLOC`/`_MFREE`/`_SETBLOCK` provide a simple heap:

- `_MALLOC(size)` — allocate a memory block from the free pool.
  If `size == -1` ($FFFFFFFF), returns the largest available block
  size (standard Human68k "query free memory" idiom). Otherwise,
  allocates from pages not currently claimed by any process.
- `_MFREE(ptr)` — free a previously allocated block. Returns its
  pages to the free pool. In the minimal implementation, this is
  a no-op (memory is reclaimed on process exit). A more complete
  implementation tracks per-block allocations.
- `_SETBLOCK(ptr, size)` — resize a memory block. This is the
  primary mechanism programs use to release startup memory (see
  §7.3). The bridge adjusts the process's page allocation:
  shrinking frees pages immediately; growing re-allocates if pages
  are available.

### 7.5 Memory Management Block and Process Management Block

See §4.4 (MMB) and §4.5 (PMB) for the full block formats.

The MMB/PMB relationship to `_MALLOC`/`_MFREE`/`_SETBLOCK`:
- Each `_MALLOC` allocation creates a new MMB linked into the chain
- `_MFREE` unlinks the MMB and returns its pages
- `_SETBLOCK` adjusts the MMB's end pointer (offset 0x08) and
  frees or reclaims pages accordingly
- The process's PMB (at the start of its memory) is always the
  first block in the chain; `_GETPDB` returns its address

---

## 8. File System Mapping

### 8.1 Drive Letter Translation

Human68k uses drive letters (`A:`, `B:`, etc.). The bridge translates
these to PPAP paths:

```
A:\DIR\FILE.X   →  /a/DIR/FILE.X
B:\FOLDER\DATA  →  /b/FOLDER/DATA
```

- Backslash (`\`) → forward slash (`/`)
- Drive letter → lowercase directory under root
- If no drive letter: relative to current working directory

### 8.2 Filename Length and Format

Human68k filename support evolved across versions and community
extensions. This is a key compatibility concern because the bridge
must decide what filenames to accept, how to report them back to
programs, and how they interact with PPAP's UNIX-style filesystem.

**Standard Human68k (v1–v2): 8+3 filenames**

The original Human68k uses MS-DOS-style 8.3 filenames:
- Base name: up to 8 characters
- Extension: up to 3 characters, separated by `.`
- Case-insensitive (stored uppercase on disk)
- Characters: ASCII alphanumeric + selected symbols + Shift-JIS

The `struct files` returned by `_FILES`/`_NFILES` has a 23-byte
`name` field, sufficient for `12345678.123` + null = 13 bytes.

**Human68k v3: 18+3 filenames (official extension)**

Human68k version 3.02 extended the base name limit from 8 to 18
characters, giving a maximum of `18 + 1 + 3 + 1 = 23` bytes
(exactly filling the `name[23]` field in `struct files`). This is
sometimes referred to as "21-character filenames" (18+3, counting
the dot). All Sharp-shipped Human68k v3.x systems support this.

**Community extension: unlimited long filenames (TwentyOne.sys)**

The popular community driver **TwentyOne.sys** (and its variants
like Sixty.sys) patches Human68k to support filenames longer than
18+3. These drivers:
- Hook DOS calls to intercept filename handling
- Extend `_FILES`/`_NFILES` to return longer names
- Support mixed-case filenames
- Support deeper directory nesting
- Are essentially a long filename layer (similar to Windows 95 LFN
  over FAT)

Programs compiled with TwentyOne.sys awareness may pass or expect
filenames longer than 23 bytes.

**PPAP strategy:**

PPAP's UNIX filesystems (romfs, UFS) natively support filenames up
to 63 bytes (`VFS_NAME_MAX`). The bridge handles the mismatch as
follows:

| Context | Behavior |
|---|---|
| **Path arguments to DOS calls** (`_OPEN`, `_CHDIR`, etc.) | Accept any length up to `VFS_PATH_MAX` (128 bytes). No 8.3 or 18.3 truncation — pass through to PPAP VFS as-is. |
| **`_FILES`/`_NFILES` name output** | Truncate to 22 chars + null (23 bytes) for strict v3.02 compat. Longer names are truncated with `~` notation (e.g., `LONGFILENAME~1.TXT`) if needed. |
| **`_NAMESTS`/`_NAMECK`** | Parse filenames accepting any length. Return components truncated to buffer sizes. |
| **Case** | PPAP stores filenames as-is (case-preserving). The bridge does case-insensitive lookup (§8.3). |

This approach maximizes compatibility: programs that only read
filenames via `_FILES` see names that fit in the 23-byte buffer.
Programs that pass explicit pathnames (e.g., from command-line
arguments or config files) can use any length the VFS supports.

**Open question:** should the bridge report itself as Human68k v3.02
(the last official version) or emulate TwentyOne.sys-level long
filename support? v3.02 is safer — programs expecting TwentyOne.sys
may also expect other driver-specific behaviors. The `_VERNUM` call
returns 0x0302 by default. A future flag could enable extended
filename mode for programs that need it.

### 8.3 Case Sensitivity

Human68k is case-insensitive. PPAP (with romfs/UFS) is case-sensitive.
The bridge performs case-insensitive path matching:

1. Try the path as-is (exact match)
2. If not found, try uppercase conversion
3. If not found, scan the directory for a case-insensitive match

This matches how Wine handles case sensitivity differences.

### 8.4 File Attributes

Human68k file attributes (archive, read-only, hidden, system,
directory, volume) don't map directly to UNIX permissions. The bridge:

- `_CHMOD` get: synthesize attributes from `stat()` mode bits
  (directory bit from `S_ISDIR`, read-only from write permission)
- `_CHMOD` set: translate to `chmod()` where possible
- `_FILES`/`_NFILES`: populate the `struct files` with synthesized
  attributes from `stat()`

### 8.5 Directory Search (_FILES / _NFILES)

Human68k's find-first/find-next pattern (`_FILES`/`_NFILES`) with
wildcard matching:

```c
/* _FILES populates a 53-byte struct: */
struct files {
    char    search[21];  /* reserved for _NFILES */
    uint8_t attr;        /* file attributes */
    uint16_t time;       /* modification time (DOS format) */
    uint16_t date;       /* modification date (DOS format) */
    uint32_t size;       /* file size */
    char    name[23];    /* filename (null-terminated, see §8.2) */
};
```

The bridge implements this using `opendir()`/`readdir()` on the
translated path, with wildcard pattern matching (`?` and `*`) applied
to each entry. The directory handle is stored in the `search` reserved
area of the `struct files` to maintain state between `_FILES` and
`_NFILES` calls.

### 8.6 Date/Time Conversion

Human68k uses DOS-format packed date/time:

```
Time: HHHHHMMMMMMSSSSS  (hours:5, minutes:6, seconds/2:5)
Date: YYYYYYYMMMMDDDDD  (year-1980:7, month:4, day:5)
```

The bridge converts between this format and PPAP's `struct timespec`
(seconds since boot — no real-time clock on most PPAP targets).

---

## 9. Abstraction Layer for eCPU Portability

### 9.1 Motivation

The core bridge logic (DOS call dispatch, path translation, error
mapping) is identical whether running natively or under emulation.
The only difference is **how memory is accessed**:

- **Native m68k**: real pointers — just dereference them
- **eCPU**: guest addresses — must translate through emulated memory

An abstraction layer allows the same bridge code to compile for both
contexts.

### 9.2 Memory Access Interface

```c
/* Opaque context — either kernel state or ecpu_state_t* */
typedef struct h68k_ctx h68k_ctx_t;

/* Read from guest memory */
uint8_t  h68k_read_u8 (h68k_ctx_t *ctx, uint32_t addr);
uint16_t h68k_read_u16(h68k_ctx_t *ctx, uint32_t addr);
uint32_t h68k_read_u32(h68k_ctx_t *ctx, uint32_t addr);

/* Write to guest memory */
void h68k_write_u8 (h68k_ctx_t *ctx, uint32_t addr, uint8_t  val);
void h68k_write_u16(h68k_ctx_t *ctx, uint32_t addr, uint16_t val);
void h68k_write_u32(h68k_ctx_t *ctx, uint32_t addr, uint32_t val);

/* Translate guest pointer to host pointer (for bulk operations) */
/* Returns NULL if addr+size is out of bounds */
void *h68k_translate_ptr(h68k_ctx_t *ctx, uint32_t addr, uint32_t size);

/* Read null-terminated string from guest memory */
/* Copies to host buffer, returns length */
int h68k_read_string(h68k_ctx_t *ctx, uint32_t addr, char *buf, int max);
```

### 9.3 Syscall Dispatch

Since both paths are kernel-embedded, the bridge always calls
kernel syscall implementations directly:

```c
/* Issue a PPAP syscall from the bridge (always kernel context) */
long h68k_syscall(h68k_ctx_t *ctx, int nr,
                  long a1, long a2, long a3, long a4, long a5, long a6);
```

This calls `sys_open()`, `sys_read()`, etc. internally — no trap
instruction overhead. The bridge runs in kernel context on both
the native and eCPU paths.

### 9.4 Native Implementation (m68k)

```c
/* h68k_native.c — compiled into the kernel for m68k targets */

struct h68k_ctx {
    pcb_t   *proc;       /* current process */
    uint32_t usp;        /* user stack pointer at F-line trap */
};

uint32_t h68k_read_u32(h68k_ctx_t *ctx, uint32_t addr) {
    /* Direct memory access — addr is a real pointer */
    return *(volatile uint32_t *)addr;
}

void *h68k_translate_ptr(h68k_ctx_t *ctx, uint32_t addr, uint32_t size) {
    /* On native m68k, guest address == host address */
    return (void *)addr;
}
```

### 9.5 eCPU Implementation (ARM and other hosts)

```c
/* h68k_ecpu.c — compiled into the kernel for non-m68k targets */

struct h68k_ctx {
    ecpu_state_t *cpu;   /* emulated CPU state */
    pcb_t        *proc;  /* current process */
};

uint32_t h68k_read_u32(h68k_ctx_t *ctx, uint32_t addr) {
    /* Read from emulated memory (big-endian byte order) */
    uint8_t *p = ctx->cpu->memory + addr;
    return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

void *h68k_translate_ptr(h68k_ctx_t *ctx, uint32_t addr, uint32_t size) {
    if (addr + size > ctx->cpu->mem_size) return NULL;
    return ctx->cpu->memory + addr;
}
```

Both implementations are kernel code — the only difference is
whether addresses are real (native) or offsets into emulated
memory (eCPU). The `h68k_syscall()` function is shared: it calls
the internal kernel syscall dispatch in both cases.

### 9.6 Bridge Code (Shared)

```c
/* human68k_bridge.c — kernel code, shared by native and eCPU paths */

int human68k_dos_call(h68k_ctx_t *ctx, uint16_t opcode, uint32_t usp) {
    uint8_t func = opcode & 0xFF;

    switch (func) {
        case 0x00: { /* _EXIT */
            int code = (int16_t)h68k_read_u16(ctx, usp);
            h68k_syscall(ctx, SYS_EXIT, code, 0, 0, 0, 0, 0);
            return 0; /* unreachable */
        }
        case 0x02: { /* _PUTCHAR */
            uint16_t ch = h68k_read_u16(ctx, usp);
            uint8_t c = (uint8_t)ch;
            void *buf = &c;
            /* For eCPU: must write char to guest memory first */
            h68k_syscall(ctx, SYS_WRITE, 1, (long)buf, 1, 0, 0, 0);
            return 0;
        }
        case 0x3D: { /* _OPEN */
            uint32_t path_ptr = h68k_read_u32(ctx, usp);
            uint16_t mode = h68k_read_u16(ctx, usp + 4);
            char path_buf[128];
            h68k_read_string(ctx, path_ptr, path_buf, sizeof(path_buf));
            char ppap_path[128];
            h68k_translate_path(path_buf, ppap_path, sizeof(ppap_path));
            int flags = h68k_mode_to_flags(mode);
            long ret = h68k_syscall(ctx, SYS_OPEN, (long)ppap_path, flags, 0644, 0, 0, 0);
            return (ret < 0) ? ppap_to_human68k_error(ret) : ret;
        }
        /* ... more DOS calls ... */
    }
    return -1; /* Invalid function */
}
```

---

## 10. Implementation Status

### Phase 1 — X-Format Loader + Minimal DOS ✅

**Status:** Complete.

- X-format (.x) binary loader with HU magic detection, relocation
- PMB setup (MMB header, segment addresses, command line, file handles)
- Initial register setup (a0–a4, USP) and user-mode entry
- F-line exception handler dispatching `$FFxx` opcodes
- `_EXIT` ($FF00), `_EXIT2` ($FF4C), `_SETBLOCK` ($FF4A)
- `_MALLOC` ($FF48) with contiguous allocation + availability query
- `_MFREE` ($FF49) with tracked multi-block freeing
- `page_max_contiguous()` for accurate free-space reporting

**Files:**
- `src/kernel/exec/exec_x68k.c` — X-format loader + PMB setup
- `src/arch/m68k/fline.S` / `src/arch/m68k/trap.S` — exception entry
- `src/kernel/subsys/human68k_bridge.c` — DOS call bridge
- `src/kernel/subsys/human68k_bridge.h` — per-process state

### Phase 2 — Console I/O ✅

**Status:** Complete.

- `_PUTCHAR` ($FF02), `_COMOUT` ($FF04) — character output
- `_GETCHAR` ($FF01) — character input with echo
- `_COMINP` ($FF03) — raw character input (no echo)
- `_PRINT` ($FF09) — string output
- `_GETS` ($FF0A) — line-buffered input
- `_FGETC` ($FF1B), `_FGETS` ($FF1C) — per-handle input
- `_FPUTC` ($FF1D), `_FPUTS` ($FF1E) — per-handle output
- `_MOVE` ($FF05) — character output with cursor advance
- `_INPOUT` ($FF08) — character I/O with input check
- `_KFLUSH` ($FF0C) — flush keyboard buffer + input subfunc
- `_CONCTRL` ($FF10) — console control (putc, print, color, Ctrl-C)

### Phase 3 — File I/O ✅

**Status:** Complete.

- Path translation: `A:\DIR\FILE` → `/a/DIR/FILE` (drive lowercase, `\`→`/`)
- Error code translation: PPAP errno → Human68k error codes
- `_CREATE` ($FF3C), `_OPEN` ($FF3D), `_CLOSE` ($FF3E)
- `_READ` ($FF3F), `_WRITE` ($FF40), `_SEEK` ($FF42)
- `_DELETE` ($FF41)
- `_DUP` ($FF45), `_DUP2` ($FF46) — via `sys_dup`/`sys_dup2`
- `_CHDIR` ($FF3B), `_CURDIR` ($FF47), `_CURDRV` ($FF19)
- `_RENAME` ($FF56) — stub (returns -ENOSYS, no `sys_rename` yet)

**Host tests:** `test_h68k_path` — 26 assertions for path translation
and error code mapping.

### Phase 4 — Directory and Metadata ✅

**Status:** Complete.

- `_MKDIR` ($FF39), `_RMDIR` ($FF3A)
- `_CHMOD` ($FF43) — query-only (synthesizes attrs from `stat()`)
- `_FILES` ($FF4E), `_NFILES` ($FF4F) — directory search with
  case-insensitive wildcard matching (`*`, `?`)
- `_FILEDATE` ($FF57) — get returns fixed date, set ignored
- FILBUF structure fill with file attributes, size, name

### Phase 5 — System Utilities and Date/Time ✅

**Status:** Complete.

- `_VERNUM` ($FF30) — returns Human68k v3.02 (`0x48550302`)
- `_BREAKCK` ($FF33) — break check stub
- `_INTVCG` ($FF35) — get interrupt vector stub
- `_GETDATE` ($FF2A), `_SETDATE` ($FF2B) — fixed date / no-op
- `_GETTIME` ($FF2C), `_SETTIME` ($FF2D) — fixed time / no-op
- `_DSKFRE` ($FF36) — disk free space from page allocator
- `_GETENV` ($FF38) — returns "not found"
- `_IOCTRL` ($FF44) — minimal device query (console vs file)
- `_FFLUSH` ($FF24) — no-op (unbuffered I/O)

### Phase 6 — Process Management ✅

**Status:** Complete (basic).

- `_EXEC` ($FF4B) — mode 0 LOADEXEC via `proc_alloc` + `do_execve`
  + polling `waitpid` (WNOHANG + yield)
- `_GETPDB` ($FF51) — return PMB address
- `_SUPER` ($FF20) — logical supervisor mode toggle
- `_KEEPPR` ($FF31) — TSR stub (just exits)
- `_ASSIGN` ($FF4D) — drive assignment stub
- `_ALLCLOSE` ($FF1A) — close all file handles (fd 3+)
- `_NAMECK` ($FF18) — parse pathname into components
- DOS call aliasing: $FF80–$FFAF → $FF50–$FF7F

### Phase 7 — IOCS Stubs (TRAP #15) ✅

**Status:** Complete.

- `_B_KEYINP` ($00) — blocking key input from stdin
- `_B_KEYSNS` ($04) — keyboard check (returns 0)
- `_SKEY_MOD` ($0E) — shift key status (returns 0)
- `_B_UP` ($19), `_B_DOWN` ($1A), `_B_RIGHT` ($1B), `_B_LEFT` ($1C)
  — cursor movement via ANSI escape sequences
- `_B_PUTC` ($20) — character output via stdout
- `_B_PRINT` ($21) — string output via stdout
- `_B_COLOR` ($22) — text color stub
- `_B_LOCATE` ($23) — cursor position stub
- `_B_CLRST` ($2A) — clear screen via ANSI `ESC[nJ`
- `_B_ERA_AL` ($2B) — clear to EOL via ANSI `ESC[0K`
- `_DATEGET` ($5A) — BCD date (fixed 2026-01-01)
- `_TIMEGET` ($5B) — BCD time (fixed 00:00:00)
- `_ONTIME` ($7F) — real uptime from scheduler tick counter (100 Hz)

### Future — Abstraction Layer + eCPU Port ☐

**Goal:** Run Human68k binaries on ARM PPAP via kernel-embedded
ecpu-m68k.

Steps:
1. Factor the bridge into the memory abstraction interface (§9)
2. Implement `h68k_ecpu.c` (eCPU memory accessors, kernel-side)
3. Implement `h68k_native.c` (native m68k memory accessors)
4. Integrate ecpu-m68k interpreter into the kernel (conditionally
   compiled for non-m68k targets)
5. Extend `exec()` to detect HU magic on ARM and enter the eCPU
   interpreter loop instead of native execution
6. Test: run same Human68k binary on ARM PPAP via emulation

---

## 11. Testing Strategy

### 11.1 Test Binaries

Build minimal Human68k test programs using a 68000 cross-assembler
(or the `m68k-elf-gcc` toolchain with a custom linker script that
produces X-format output):

| Test | DOS Calls Used | Validates |
|---|---|---|
| `exit0.x` | `_EXIT` | Loader + F-line handler + exit |
| `hello.x` | `_PUTCHAR`, `_PRINT`, `_EXIT` | Console output |
| `echo.x` | `_GETCHAR`, `_PUTCHAR`, `_EXIT` | Console input |
| `cat.x` | `_OPEN`, `_READ`, `_WRITE`, `_CLOSE`, `_EXIT` | File I/O |
| `ls.x` | `_FILES`, `_NFILES`, `_PRINT`, `_EXIT` | Directory listing |
| `malloc.x` | `_MALLOC`, `_MFREE`, `_EXIT` | Memory management |
| `run.x` | `_EXEC`, `_WAIT`, `_EXIT` | Process management |

### 11.2 mkxfile Host Tool

A simple host tool `mkxfile` to produce X-format binaries from raw
68000 object code (or convert from ELF to X-format). This avoids
needing a full Human68k development toolchain.

Alternatively, a linker script for `m68k-elf-ld` that outputs
X-format directly, or a post-processing script that converts ELF
to X-format.

### 11.3 QEMU Testing

All phases 1–5 can be tested on `qemu_m68k` (PPAP's existing QEMU
m68k target). The Human68k test binaries are included in the romfs
image under `/x68k/` or `/human68k/`.

### 11.4 Real X68000 Software

After phases 1–5, test with actual X68000 command-line utilities:
- Simple file managers
- Text viewers
- Command-line games

These provide real-world validation of DOS call compatibility.

---

## 12. Open Questions

1. **R-format TSR behavior**: The R-format loader (§4.4) handles
   simple .r binaries, but TSR programs that call `_KEEPPR` to go
   resident and hook interrupt vectors need deeper emulation. How
   much TSR support is practical? Likely: none initially, revisit
   if specific useful TSRs are identified.

2. **Kanji (Shift-JIS) support**: Human68k uses Shift-JIS encoding.
   File paths may contain Shift-JIS characters. The bridge should
   pass bytes through transparently (no encoding conversion) — the
   terminal handles display. Filenames with Shift-JIS in PPAP's
   romfs/UFS need testing.

3. **_EXEC modes**: Human68k's `_EXEC` supports multiple modes
   (load+exec, load-only, exec overlay). Initially only mode 0
   (load+exec, equivalent to fork+exec) is implemented.

4. **Memory protection**: Human68k programs expect a flat address
   space with no protection. On m68k PPAP, this is naturally
   the case (no MMU). On ARM via eCPU, the emulated memory is
   a single buffer — no protection issues.

5. **Interrupt-driven I/O**: Some Human68k programs install
   interrupt handlers (e.g., for Ctrl-C). The bridge should
   translate `SIGINT` to the equivalent Human68k behavior (calling
   the program's Ctrl-C handler if registered via `_CTRLVC`).

6. **IOCS graphics calls**: Many X68000 programs use IOCS for
   graphics (sprites, scroll, palette). These are out of scope
   for the initial implementation. A future graphics bridge could
   translate them to PPAP's framebuffer console, but this is a
   major undertaking.

7. **Kernel size impact**: The bridge (~4 KB code), X-format loader
   (~2 KB), and eCPU interpreter (~16 KB for ecpu-m68k) are all
   kernel-embedded. On ARM targets with limited flash, the eCPU
   can be conditionally compiled out if Human68k support is not
   needed. On m68k targets, only the bridge and loader are needed
   (no eCPU), adding ~6 KB.

8. **mkxfile vs cross-compiler**: Need to decide between building
   a custom `mkxfile` tool or using the existing `m68k-elf-gcc`
   toolchain with appropriate linker scripts. The latter is more
   flexible but produces ELF, which then needs conversion.

---

## References

### External Projects

- **run68x** (https://github.com/kg68k/run68x) — Human68k CUI emulator.
  A host-based emulator that interprets m68k instructions and translates
  Human68k DOS calls to host OS calls. The source provides a complete
  DOS call dispatch table (`doscall.c`), X-format loader (`load.c`),
  IOCS call stubs (`iocscall.c`), and memory management (`dos_memory.c`).
  Invaluable reference for DOS call signatures and edge cases.
  License: GPL v2+.

- **puni** (https://github.com/kg68k/puni) — X680x0 programming reference
  collection. Contains detailed documentation of Human68k DOS calls
  (`doscall.txt`), IOCS calls (`iocscall.txt`), trap exception handling,
  memory-mapped I/O registers, and assembly language syntax. Primary
  reference for Human68k v3.02 API behavior.
  **Note:** The puni repository's README prohibits AI processing of its
  contents. This project does not feed puni documents to AI tools
  directly. When puni information is needed, a human developer reads
  the document and writes the prompt or code based on their own
  understanding.

### Related PPAP Documentation

- [feature-subsystem.md](feature-subsystem.md) — Subsystem framework (general design; Human68k uses kernel-embedded model instead of user-space `/subsys/`)
- [feature-eCPU.md](feature-eCPU.md) — CPU emulation layer
- [target-68000.md](target-68000.md) — m68k target reference (§7: X68000 details)
- [syscall.md](syscall.md) — PPAP system call reference
- [kernel.md](kernel.md) — Kernel internals