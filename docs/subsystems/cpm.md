# CP/M Subsystem — Design

Load and execute CP/M 2.2 `.COM` binaries on PPAP, bridging
CP/M BDOS calls to PPAP's native syscall interface via the
ecpu-z80 emulator core.

---

## Table of Contents

1. [Goals and Scope](#1-goals-and-scope)
2. [Background: CP/M 2.2](#2-background-cpm-22)
3. [Architecture](#3-architecture)
4. [Binary Loader and Memory Map](#4-binary-loader-and-memory-map)
5. [OS Personality: BDOS Bridge](#5-os-personality-bdos-bridge)
6. [BIOS Bridge](#6-bios-bridge)
7. [File System Mapping](#7-file-system-mapping)
8. [Terminal Handling](#8-terminal-handling)
9. [Implementation Plan](#9-implementation-plan)
10. [Testing Strategy](#10-testing-strategy)
11. [Open Questions](#11-open-questions)

---

## 1. Goals and Scope

### 1.1 Primary Goal

Run existing CP/M 2.2 `.COM` programs on PPAP — starting with simple
utilities and working toward real-world applications (MBASIC, Turbo
Pascal, WordStar, dBASE II). The subsystem bridges CP/M's BDOS API
to PPAP syscalls via the ecpu-z80 interpreter (see `docs/ecpu/z80.md`).

### 1.2 Execution Mode

CP/M is always emulated — there is no native Z80 PPAP target. The
subsystem runs on any PPAP host (ARM, m68k) using the kernel-embedded
ecpu-z80 interpreter:

| Host Architecture | Execution Method | Personality Layer |
|---|---|---|
| **ARM (RP2040)** | ecpu-z80 interpreter (kernel-embedded) | CP/M BDOS bridge |
| **m68k (QEMU/X68000)** | ecpu-z80 interpreter (kernel-embedded) | CP/M BDOS bridge |

There is only one execution path — no "native vs eCPU" split as in
the Human68k subsystem. This simplifies the design: no memory
abstraction layer needed (all memory access goes through the emulated
64 KB array).

### 1.3 Scope

**In scope (initial):**
- `.COM` binary loader (raw binary at 0x0100)
- CP/M 2.2 memory map setup (zero page, BDOS/BIOS stubs)
- BDOS console I/O functions (0–12)
- BDOS file operations via FCB (15–36)
- FCB-to-path translation
- Drive letter mapping

**In scope (later):**
- Full BDOS compatibility (user areas, disk operations)
- CP/M 3.0 (CP/M Plus) extensions if needed
- Multiple simultaneous CP/M processes

**Out of scope:**
- Full BIOS emulation (disk controller, character I/O hardware)
- CP/M system tracks / cold boot simulation
- Direct disk access (BIOS sector read/write)
- Programs that bypass BDOS for direct BIOS or hardware access
- Graphics or sound (CP/M is text-only)

### 1.4 Design Constraints

- The subsystem must coexist with other PPAP subsystems (Human68k,
  future DOS) — each runs as an independent PPAP process
- 64 KB Z80 memory fits within RP2040's 264 KB SRAM but leaves
  limited headroom; the kernel + page pool must fit in ~200 KB
- The personality bridge should be simple — CP/M's BDOS API has
  ~40 functions, most mapping directly to PPAP syscalls
- FCB-based file operations are the main complexity; the bridge
  must maintain FCB state (current position, record number) and
  map it to PPAP's stream-oriented `read()`/`write()`/`lseek()`

---

## 2. Background: CP/M 2.2

### 2.1 Overview

CP/M (Control Program for Microcomputers) was created by Gary
Kildall in 1974 and became the dominant microcomputer OS before
MS-DOS. CP/M 2.2 (1979) is the most widely used version, running
on Z80 and 8080 systems with 64 KB RAM.

Key characteristics:
- Single-user, single-tasking
- BDOS (Basic Disk Operating System) API via `CALL 5`
- FCB (File Control Block) based file operations
- 8.3 filenames, case-insensitive
- Drive letters A:–P: (up to 16 drives)
- User areas 0–15 (subdirectory substitute)
- 128-byte record-oriented file I/O
- Programs loaded at 0x0100, max ~62 KB (TPA)

### 2.2 BDOS Call Mechanism

Programs invoke BDOS by loading parameters and calling address
0x0005:

```asm
; CP/M: Print string at DE (terminated by '$')
    LD   C, 9       ; BDOS function 9: Print String
    LD   DE, msg    ; pointer to string
    CALL 5          ; invoke BDOS
    RET

msg: DB 'Hello, World!$'
```

- C register = BDOS function number (0–40)
- DE register = parameter (pointer or value)
- Return value in A (and sometimes HL)

### 2.3 FCB (File Control Block)

CP/M uses FCBs for file operations. An FCB is a 36-byte structure
in program memory:

```
Offset  Size  Field
------  ----  -----
0x00    1     Drive code (0=default, 1=A, 2=B, ...)
0x01    8     Filename (padded with spaces)
0x09    3     File type/extension (padded with spaces)
0x0C    1     Extent number (low byte)
0x0D    1     S1 (reserved)
0x0E    1     S2 (extent high byte / module number)
0x0F    1     Record count (records in current extent)
0x10    16    Allocation map (filled by BDOS)
0x20    1     Current record (0–127 within current extent)
0x21    3     Random record number (for random access, 3 bytes)
------  ----
Total: 36 bytes
```

Programs set the drive, filename, and type fields. BDOS fills the
rest during Open/Make operations. The allocation map is opaque
to the program.

### 2.4 Memory Map

```
0x0000  JP to BIOS warm boot
0x0001  (reserved)
0x0003  I/O byte (IOBYTE)
0x0004  Current drive + user number
0x0005  JP to BDOS entry
0x0006–0x0007  (BDOS address, high byte = TPA top)
0x005C  Default FCB 1 (from command line arg 1)
0x006C  Default FCB 2 (from command line arg 2)
0x0080  Default DMA buffer (128 bytes)
        Also: command-line tail (first byte = length)
0x0100  Start of TPA (Transient Program Area)
  ...   Program code and data
BDOS    End of TPA / start of BDOS (varies by system)
BIOS    After BDOS
0xFFFF  Top of memory
```

The TPA size depends on where BDOS is located. A typical 64 KB
CP/M system places BDOS around 0xE400, giving ~57 KB TPA. PPAP
can place the BDOS entry higher to maximize TPA.

### 2.5 DMA (Disk Memory Address)

CP/M uses a 128-byte DMA buffer for disk I/O. The default DMA
address is 0x0080. Programs can change it with BDOS function 26
(Set DMA Address). All sequential and random read/write operations
transfer exactly 128 bytes to/from the current DMA address.

### 2.6 User Areas

CP/M 2.2 supports 16 user areas (0–15) per drive. Each user area
acts as a simple namespace — files in different user areas don't
see each other. Most CP/M programs use user area 0.

PPAP maps user areas to subdirectories (see §7.4).

---

## 3. Architecture

### 3.1 Component Overview

All components are kernel-embedded. The subsystem combines:

1. **Binary detection** in `exec()` — recognises `.COM` files
2. **.COM loader** — loads raw binary at emulated address 0x0100
3. **Memory map setup** — zero page stubs, FCBs, command line
4. **ecpu-z80 interpreter** — executes Z80 instructions (see `docs/ecpu/z80.md`)
5. **BDOS bridge** — intercepts `CALL 0x0005`, translates to PPAP syscalls
6. **BIOS bridge** — intercepts `CALL 0x0000` (warm boot) and optional BIOS calls

```
CP/M .COM binary
    │
    │ exec() detects .COM extension
    │ kernel loads binary into emulated 64 KB memory
    │ sets up CP/M zero page, FCBs, command tail
    │ enters ecpu-z80 interpreter loop
    │
    ▼
ecpu-z80: CALL 0x0005 detected → trap fires
    │
    ▼
cpm_bdos_handler(cpu, C, DE)
    │ C = function number
    │ DE = parameter
    │ translate to PPAP syscall
    │ write result to A/HL registers
    │
    ▼
PPAP kernel syscall (sys_read, sys_write, sys_open, ...)
```

### 3.2 Process Model

Each CP/M binary runs as a regular PPAP process with:
- A PID in the PPAP process table
- Standard file descriptors (stdin/stdout/stderr)
- An emulated Z80 state (`z80_state_t`) stored in `pcb_t::subsys_data`
- 64 KB emulated memory allocated from the page pool

The ecpu-z80 interpreter loop runs in kernel context. The scheduler
preempts it via timer interrupt (or cooperative yield) — from the
kernel's perspective, the interpreter is just the process's
execution.

CP/M is single-tasking, but PPAP can run multiple CP/M processes
concurrently. Each has independent Z80 state and 64 KB memory.
(On RP2040, memory constraints likely limit this to one CP/M
process at a time.)

### 3.3 Component Layering

```
┌─────────────────────────────────────────────────────┐
│  PPAP kernel                                        │
│                                                     │
│  ┌───────────────────────────────────────────────┐  │
│  │  cpm_bridge.c (personality layer)             │  │
│  │  - BDOS function dispatch table               │  │
│  │  - FCB parsing and file handle mapping        │  │
│  │  - Drive/path translation                     │  │
│  │  - DMA buffer management                      │  │
│  │  - Console I/O bridge                         │  │
│  ├───────────────────────────────────────────────┤  │
│  │  cpm_host.c                                 │  │
│  │  - .COM file detection and loading            │  │
│  │  - Zero page setup (JMP stubs, FCBs, cmdline) │  │
│  │  - Default FCB parsing from argv              │  │
│  ├───────────────────────────────────────────────┤  │
│  │  ecpu_z80.c (CPU emulator — see docs/ecpu/z80.md)  │  │
│  │  - Fetch/decode/execute loop                  │  │
│  │  - Trap hook fires on CALL 0x0005 / CALL 0x0000 │
│  ├───────────────────────────────────────────────┤  │
│  │  64 KB emulated memory (z80_state.memory[])   │  │
│  └───────────────────────────────────────────────┘  │
│                                                     │
│  sys_open, sys_read, sys_write, ...  (existing)     │
└─────────────────────────────────────────────────────┘
```

### 3.4 Conditional Compilation

```cmake
if(ENABLE_SUBSYS_CPM)
    target_sources(ppap PRIVATE
        src/kernel/core/cpu/ecpu_z80.c
        src/kernel/core/cpu/ecpu_z80_cb.c
        src/kernel/core/cpu/ecpu_z80_ed.c
        src/kernel/core/cpu/ecpu_z80_ix.c
        src/kernel/core/cpu/ecpu_z80_iy.c
        src/kernel/core/cpu/ecpu_z80_alu.c
        src/kernel/core/subsys/cpm/cpm_bridge.c
        src/kernel/core/subsys/cpm/cpm_host.c
    )
    target_compile_definitions(ppap PRIVATE
        ENABLE_SUBSYS_CPM=1
        ENABLE_ECPU_Z80=1
    )
endif()
```

---

## 4. Binary Loader and Memory Map

### 4.1 Detection

In the `exec()` detection chain, after native ELF and magic-byte
checks:

```c
/* Raw binary with .com extension under /subsys/cpm/ */
if (path_under("/subsys/cpm/") && extension_is(".com"))
    → CP/M subsystem

/* Or: any .com file when CP/M is the default .com handler */
if (extension_is(".com") && file_size <= 0xFE00)
    → CP/M subsystem (if enabled; see docs/subsystems/overview.md §8.2)
```

CP/M `.COM` files have no magic bytes — detection is extension-based.
Disambiguation from DOS `.COM` uses the directory convention: files
under `/subsys/cpm/` are CP/M, files under `/subsys/dos/` are DOS.

### 4.2 .COM Load Procedure

```
1. Validate: file_size <= 0xFE00 (max TPA size)
2. Allocate 64 KB memory for Z80 address space
3. Zero entire 64 KB (clean slate)
4. Set up zero page (§4.3)
5. Copy .COM file to address 0x0100
6. Parse command-line arguments into default FCBs at 0x005C/0x006C
7. Copy command-line tail to 0x0080 (DMA buffer)
8. Initialize Z80 state:
   - PC = 0x0100 (program entry)
   - SP = 0xFF00 (or BDOS base, top of TPA)
   - C = 0 (default drive)
9. Register BDOS trap handler
10. Enter ecpu-z80 interpreter loop
```

### 4.3 Zero Page Setup

The first 256 bytes of memory contain CP/M system data:

```
Address  Contents
-------  --------
0x0000   C3 xx xx     JP warm_boot    (BIOS warm boot entry)
0x0003   00           IOBYTE (default: 0)
0x0004   00           Current drive (0=A) + user number (bits 4-7)
0x0005   C3 xx xx     JP bdos_entry   (BDOS entry point)
0x0008   ...          (RST vectors — unused in most CP/M programs)
0x005C   [36 bytes]   Default FCB 1 (parsed from first cmd arg)
0x006C   [36 bytes]   Default FCB 2 (parsed from second cmd arg)
0x0080   len string   Command-line tail (len byte + text + 0x00)
0x0100   ...          Program start (TPA)
```

The `JP` instructions at 0x0000 and 0x0005 jump to addresses within
the emulated BIOS/BDOS area (above TPA). The emulator intercepts
`CALL` instructions to 0x0005 (BDOS) and 0x0000 (warm boot) via the
trap hook, so these stubs don't need to contain real code — they
just need valid `JP` targets:

```c
/* Place BDOS/BIOS entry stubs near top of memory */
#define CPM_BDOS_ENTRY  0xFE00  /* BDOS entry address */
#define CPM_BIOS_ENTRY  0xFF00  /* BIOS entry address */

/* Zero page warm boot: JP BIOS_ENTRY */
mem[0x0000] = 0xC3;             /* JP */
mem[0x0001] = CPM_BIOS_ENTRY & 0xFF;
mem[0x0002] = CPM_BIOS_ENTRY >> 8;

/* Zero page BDOS entry: JP BDOS_ENTRY */
mem[0x0005] = 0xC3;             /* JP */
mem[0x0006] = CPM_BDOS_ENTRY & 0xFF;
mem[0x0007] = CPM_BDOS_ENTRY >> 8;

/* At BDOS entry: RET (trap intercepts before execution) */
mem[CPM_BDOS_ENTRY] = 0xC9;    /* RET */

/* At BIOS entry: RET (trap intercepts before execution) */
mem[CPM_BIOS_ENTRY] = 0xC9;    /* RET */
```

**TPA size reporting:** the high byte of the address at 0x0006
tells programs the TPA top. With `CPM_BDOS_ENTRY = 0xFE00`,
programs see 0xFE as the high byte → TPA extends to 0xFDFF,
giving ~63 KB of usable program space.

### 4.4 Default FCB Parsing

CP/M parses the first two command-line arguments into FCBs at
0x005C and 0x006C. The loader must replicate this:

```
Command line: "A:TEST.TXT B:OUTPUT.DAT extra args"

FCB1 at 0x005C:
  drive = 1 (A:)
  name  = "TEST    "
  type  = "TXT"

FCB2 at 0x006C:
  drive = 2 (B:)
  name  = "OUTPUT  "
  type  = "DAT"

DMA at 0x0080:
  length byte = 24
  " A:TEST.TXT B:OUTPUT.DAT extra args"
```

FCB parsing rules:
- Drive letter prefix (`A:`–`P:`) → drive code 1–16; no prefix → 0
- Filename: up to 8 chars, uppercase, space-padded
- Extension: up to 3 chars, uppercase, space-padded, after `.`
- Wildcards: `*` expands to `?` characters filling the field

```c
static void cpm_parse_fcb(uint8_t *fcb, const char *arg) {
    memset(fcb, ' ', 36);
    fcb[0] = 0;  /* default drive */

    /* Check for drive prefix */
    if (arg[1] == ':') {
        fcb[0] = toupper(arg[0]) - 'A' + 1;
        arg += 2;
    }

    /* Parse filename (up to 8 chars) */
    int i = 1;
    while (*arg && *arg != '.' && i <= 8) {
        if (*arg == '*') { memset(&fcb[i], '?', 9 - i); i = 9; break; }
        fcb[i++] = toupper(*arg++);
    }

    /* Skip to extension */
    if (*arg == '.') arg++;

    /* Parse extension (up to 3 chars) */
    i = 9;
    while (*arg && i <= 11) {
        if (*arg == '*') { memset(&fcb[i], '?', 12 - i); i = 12; break; }
        fcb[i++] = toupper(*arg++);
    }

    /* Zero remaining fields */
    memset(&fcb[12], 0, 24);
}
```

### 4.5 Command-Line Tail

The DMA buffer at 0x0080 receives the raw command-line tail:

```c
static void cpm_setup_cmdline(uint8_t *mem, const char *cmdline) {
    int len = strlen(cmdline);
    if (len > 126) len = 126;  /* max 126 chars + length byte + null */
    mem[0x0080] = (uint8_t)len;
    memcpy(&mem[0x0081], cmdline, len);
    mem[0x0081 + len] = 0x00;
}
```

---

## 5. OS Personality: BDOS Bridge

### 5.1 Trap Handler

The CP/M personality registers a trap handler with ecpu-z80 that
intercepts `CALL` instructions to specific addresses:

```c
/* Uses eCPU common interface trap types (see docs/ecpu/z80.md §3) */
static int cpm_trap_handler(ecpu_state_t *cpu, int trap_type,
                            uint32_t param, void *ctx) {
    if (trap_type == ECPU_TRAP_RST && param == 0x0000) {
        /* RST 0 from internal stub area — compute slot from address */
        int slot = (cpu->pc - 1 - CPM_STUB_BASE) / 2;
        if (slot == 0) return cpm_bdos_dispatch(cpu, ctx);
        return cpm_bios_dispatch(cpu, ctx, slot - 1);
    }
    if (trap_type == ECPU_TRAP_HALT) {
        return ECPU_TRAP_EXIT;  /* HALT = exit */
    }
    return ECPU_TRAP_UNHANDLED;
}
```

### 5.2 BDOS Function Dispatch

```c
/* Uses eCPU common interface for register access (see docs/ecpu/z80.md §3) */
static int cpm_bdos_dispatch(ecpu_state_t *cpu, void *ctx) {
    const ecpu_core_ops_t *ops = current->ecpu_ops;
    uint8_t  fn = ops->get_reg(cpu, Z80_REG_C);
    uint16_t de = ops->get_reg(cpu, Z80_REG_DE);
    cpm_state_t *cpm = (cpm_state_t *)ctx;

    int result = 0;

    switch (fn) {
        case 0:  return cpm_system_reset(cpu, cpm);
        case 1:  result = cpm_console_input(cpu, cpm); break;
        case 2:  cpm_console_output(cpu, cpm, de & 0xFF); break;
        case 6:  result = cpm_direct_console_io(cpu, cpm, de & 0xFF); break;
        case 9:  cpm_print_string(cpu, cpm, de); break;
        case 10: cpm_read_console_buffer(cpu, cpm, de); break;
        case 11: result = cpm_console_status(cpu, cpm); break;
        case 12: result = cpm_version_number(cpu, cpm); break;
        case 13: cpm_reset_disk(cpu, cpm); break;
        case 14: cpm_select_disk(cpu, cpm, de & 0xFF); break;
        case 15: result = cpm_open_file(cpu, cpm, de); break;
        case 16: result = cpm_close_file(cpu, cpm, de); break;
        case 17: result = cpm_search_first(cpu, cpm, de); break;
        case 18: result = cpm_search_next(cpu, cpm); break;
        case 19: result = cpm_delete_file(cpu, cpm, de); break;
        case 20: result = cpm_read_sequential(cpu, cpm, de); break;
        case 21: result = cpm_write_sequential(cpu, cpm, de); break;
        case 22: result = cpm_make_file(cpu, cpm, de); break;
        case 23: result = cpm_rename_file(cpu, cpm, de); break;
        case 24: result = cpm_return_login_vector(cpu, cpm); break;
        case 25: result = cpm_return_current_disk(cpu, cpm); break;
        case 26: cpm_set_dma_address(cpu, cpm, de); break;
        case 27: result = cpm_get_alloc_vector(cpu, cpm); break;
        case 29: result = cpm_get_readonly_vector(cpu, cpm); break;
        case 32: result = cpm_set_get_user_code(cpu, cpm, de & 0xFF); break;
        case 33: result = cpm_read_random(cpu, cpm, de); break;
        case 34: result = cpm_write_random(cpu, cpm, de); break;
        case 35: result = cpm_compute_file_size(cpu, cpm, de); break;
        case 36: result = cpm_set_random_record(cpu, cpm, de); break;
        case 40: result = cpm_write_random_zero_fill(cpu, cpm, de); break;
        default:
            /* Unknown function — return 0xFF (error) */
            result = 0xFF;
            break;
    }

    ops->set_reg(cpu, Z80_REG_A, result & 0xFF);
    ops->set_reg(cpu, Z80_REG_HL, result & 0xFFFF);
    return ECPU_TRAP_HANDLED;
}
```

### 5.3 BDOS Function Reference

#### Console I/O (Functions 0–12)

| Fn | Name | C reg | DE reg | Returns | PPAP Translation |
|---|---|---|---|---|---|
| 0 | System Reset | 0 | — | — | `sys_exit(0)` |
| 1 | Console Input | 1 | — | A=char | `sys_read(0, &ch, 1)` + echo |
| 2 | Console Output | 2 | E=char | — | `sys_write(1, &ch, 1)` |
| 3 | Reader Input | 3 | — | A=char | `sys_read(0, &ch, 1)` (same as 1) |
| 4 | Punch Output | 4 | E=char | — | `sys_write(1, &ch, 1)` (same as 2) |
| 5 | List Output | 5 | E=char | — | `sys_write(1, &ch, 1)` (same as 2) |
| 6 | Direct Console I/O | 6 | E=FF→input, else→output | A=char or 0 | Non-blocking read or write |
| 7 | Get I/O Byte | 7 | — | A=IOBYTE | Return `mem[0x0003]` |
| 8 | Set I/O Byte | 8 | E=byte | — | `mem[0x0003] = E` |
| 9 | Print String | 9 | DE=addr | — | Write until `$` terminator |
| 10 | Read Console Buffer | 10 | DE=buf | — | Line-buffered read |
| 11 | Get Console Status | 11 | — | A=FF if ready, 0 if not | Non-blocking input check |
| 12 | Return Version | 12 | — | HL=0x0022 | CP/M 2.2 |

#### Disk/File Operations (Functions 13–40)

| Fn | Name | DE reg | Returns | PPAP Translation |
|---|---|---|---|---|
| 13 | Reset Disk System | — | — | Reset to drive A, DMA=0x0080 |
| 14 | Select Disk | E=drive (0=A) | — | Set current drive |
| 15 | Open File | DE=FCB | A=0 ok, FF=err | `sys_open()` via FCB→path |
| 16 | Close File | DE=FCB | A=0 ok, FF=err | `sys_close()` |
| 17 | Search First | DE=FCB | A=0–3 or FF | `sys_opendir()` + `sys_readdir()` |
| 18 | Search Next | — | A=0–3 or FF | `sys_readdir()` |
| 19 | Delete File | DE=FCB | A=0 ok, FF=err | `sys_unlink()` |
| 20 | Read Sequential | DE=FCB | A=0 ok, nonzero=err | `sys_read()` 128 bytes to DMA |
| 21 | Write Sequential | DE=FCB | A=0 ok, nonzero=err | `sys_write()` 128 bytes from DMA |
| 22 | Make File | DE=FCB | A=0 ok, FF=err | `sys_open(O_CREAT)` |
| 23 | Rename File | DE=FCB | A=0 ok, FF=err | `sys_rename()` |
| 24 | Return Login Vector | — | HL=bitmap | Return 0x0001 (drive A only) |
| 25 | Return Current Disk | — | A=disk (0=A) | Return current drive |
| 26 | Set DMA Address | DE=addr | — | Update internal DMA pointer |
| 27 | Get Alloc Vector | — | HL=addr | Synthesized bitmap at 0xFD00 (all allocated) |
| 28 | Write Protect Disk | — | — | No-op |
| 29 | Get R/O Vector | — | HL=bitmap | Return 0x0000 (no R/O drives) |
| 30 | Set File Attributes | DE=FCB | A=0 ok | No-op (returns success) |
| 31 | Get Disk Parameters | — | HL=addr | Synthesized DPB at 0xFCF0 (IBM 3740) |
| 32 | Get/Set User Code | E=FF→get, else→set | A=user | Get/set current user area |
| 33 | Read Random | DE=FCB | A=0 ok, nonzero=err | `sys_lseek()` + `sys_read()` 128 bytes |
| 34 | Write Random | DE=FCB | A=0 ok, nonzero=err | `sys_lseek()` + `sys_write()` 128 bytes |
| 35 | Compute File Size | DE=FCB | — | `sys_lseek(SEEK_END)` / 128 → FCB r0/r1/r2 |
| 36 | Set Random Record | DE=FCB | — | Compute from sequential position |
| 40 | Write Random (Zero Fill) | DE=FCB | A=0 ok | Same as 34 (zero-fill gaps) |

### 5.4 Per-Process CP/M State

```c
typedef struct {
    uint8_t  current_drive;   /* 0=A, 1=B, ..., 15=P */
    uint8_t  current_user;    /* 0–15 */
    uint16_t dma_addr;        /* DMA buffer address (default 0x0080) */

    /* FCB-to-fd mapping table.
     * CP/M doesn't have file handles — it uses FCBs. We track
     * which FCB addresses map to which PPAP file descriptors. */
    struct {
        uint16_t fcb_addr;    /* Z80 address of FCB (0 = free slot) */
        int      fd;          /* PPAP file descriptor */
        uint32_t file_pos;    /* current file position in bytes */
    } open_files[CPM_MAX_OPEN_FILES];

    /* Directory search state (for fn 17/18) */
    int      search_fd;       /* directory fd for readdir */
    uint8_t  search_fcb[12];  /* filename pattern being searched */
    uint8_t  search_drive;    /* drive being searched */
    uint8_t  search_user;     /* user area being searched */
} cpm_state_t;

#define CPM_MAX_OPEN_FILES  8  /* CP/M programs rarely open many files */
```

### 5.5 FCB File Operations

#### Open File (Function 15)

```c
static int cpm_open_file(z80_state_t *cpu, cpm_state_t *cpm,
                         uint16_t fcb_addr) {
    /* Read FCB from emulated memory */
    uint8_t fcb[36];
    for (int i = 0; i < 36; i++)
        fcb[i] = z80_read8(cpu, fcb_addr + i);

    /* Build PPAP path from FCB */
    char path[128];
    cpm_fcb_to_path(cpm, fcb, path, sizeof(path));

    /* Open via PPAP */
    int fd = sys_open(path, O_RDWR, 0);
    if (fd < 0) {
        fd = sys_open(path, O_RDONLY, 0);  /* try read-only */
        if (fd < 0) return 0xFF;  /* file not found */
    }

    /* Track FCB→fd mapping */
    int slot = cpm_alloc_file_slot(cpm, fcb_addr, fd);
    if (slot < 0) { sys_close(fd); return 0xFF; }

    /* Initialize FCB fields */
    z80_write8(cpu, fcb_addr + 0x0C, 0);  /* extent = 0 */
    z80_write8(cpu, fcb_addr + 0x0F, 0);  /* record count (filled later) */
    z80_write8(cpu, fcb_addr + 0x20, 0);  /* current record = 0 */

    return 0;  /* success — directory code 0 */
}
```

#### Read Sequential (Function 20)

```c
static int cpm_read_sequential(z80_state_t *cpu, cpm_state_t *cpm,
                               uint16_t fcb_addr) {
    int slot = cpm_find_file_slot(cpm, fcb_addr);
    if (slot < 0) return 0x09;  /* invalid FCB */

    /* Read 128 bytes into DMA buffer */
    uint8_t buf[128];
    int n = sys_read(cpm->open_files[slot].fd, buf, 128);
    if (n <= 0) return 0x01;  /* end of file */

    /* Pad with 0x1A (CP/M EOF marker) if short read */
    if (n < 128) memset(&buf[n], 0x1A, 128 - n);

    /* Write to emulated DMA buffer */
    for (int i = 0; i < 128; i++)
        z80_write8(cpu, cpm->dma_addr + i, buf[i]);

    /* Advance FCB sequential position */
    cpm->open_files[slot].file_pos += 128;
    cpm_update_fcb_position(cpu, fcb_addr, cpm->open_files[slot].file_pos);

    return 0;  /* success */
}
```

#### Read Random (Function 33)

```c
static int cpm_read_random(z80_state_t *cpu, cpm_state_t *cpm,
                           uint16_t fcb_addr) {
    int slot = cpm_find_file_slot(cpm, fcb_addr);
    if (slot < 0) return 0x09;

    /* Read random record number from FCB (3 bytes, little-endian) */
    uint32_t record = z80_read8(cpu, fcb_addr + 0x21)
                    | ((uint32_t)z80_read8(cpu, fcb_addr + 0x22) << 8)
                    | ((uint32_t)z80_read8(cpu, fcb_addr + 0x23) << 16);

    /* Seek to record position */
    uint32_t offset = record * 128;
    int rc = sys_lseek(cpm->open_files[slot].fd, offset, SEEK_SET);
    if (rc < 0) return 0x06;  /* seek error */

    /* Read 128 bytes (same as sequential) */
    cpm->open_files[slot].file_pos = offset;
    return cpm_read_sequential(cpu, cpm, fcb_addr);
}
```

### 5.6 Console I/O

#### Print String (Function 9)

```c
static void cpm_print_string(z80_state_t *cpu, cpm_state_t *cpm,
                             uint16_t addr) {
    /* CP/M strings are terminated by '$' */
    char buf[256];
    int len = 0;
    while (len < (int)sizeof(buf) - 1) {
        uint8_t ch = z80_read8(cpu, addr + len);
        if (ch == '$') break;
        buf[len++] = ch;
    }
    if (len > 0)
        sys_write(1, buf, len);
}
```

#### Read Console Buffer (Function 10)

```c
static void cpm_read_console_buffer(z80_state_t *cpu, cpm_state_t *cpm,
                                    uint16_t buf_addr) {
    /* Buffer format:
     * buf[0] = max chars (set by caller)
     * buf[1] = actual chars read (set by BDOS)
     * buf[2..] = character data */
    uint8_t max = z80_read8(cpu, buf_addr);
    char line[256];
    int n = sys_read(0, line, max < 255 ? max : 255);
    if (n < 0) n = 0;

    /* Strip trailing newline */
    if (n > 0 && line[n - 1] == '\n') n--;

    z80_write8(cpu, buf_addr + 1, (uint8_t)n);
    for (int i = 0; i < n; i++)
        z80_write8(cpu, buf_addr + 2 + i, line[i]);
}
```

#### Direct Console I/O (Function 6)

```c
static int cpm_direct_console_io(z80_state_t *cpu, cpm_state_t *cpm,
                                 uint8_t e) {
    if (e == 0xFF) {
        /* Input: non-blocking read */
        uint8_t ch;
        int n = sys_read_nonblock(0, &ch, 1);
        return (n > 0) ? ch : 0x00;
    } else if (e == 0xFE) {
        /* Console status check */
        return sys_read_ready(0) ? 0xFF : 0x00;
    } else {
        /* Output */
        uint8_t ch = e;
        sys_write(1, &ch, 1);
        return 0;
    }
}
```

---

## 6. BIOS Bridge

### 6.1 Overview

CP/M's BIOS provides low-level hardware access. Most CP/M programs
never call BIOS directly — they use BDOS. However, some programs
(particularly older ones) use BIOS calls for:
- Warm boot (`CALL 0x0000` or BIOS function 0)
- Console status/I/O (BIOS functions 2–4)
- Direct disk access (BIOS functions 9–14)

### 6.2 BIOS Function Table

The BIOS jump table starts at `CPM_BIOS_ENTRY` with 3-byte entries:

```
BIOS+0x00  JP BOOT       Function 0: Cold boot
BIOS+0x03  JP WBOOT      Function 1: Warm boot
BIOS+0x06  JP CONST      Function 2: Console status
BIOS+0x09  JP CONIN      Function 3: Console input
BIOS+0x0C  JP CONOUT     Function 4: Console output
BIOS+0x0F  JP LIST       Function 5: List device output
BIOS+0x12  JP PUNCH      Function 6: Punch output
BIOS+0x15  JP READER     Function 7: Reader input
BIOS+0x18  JP HOME       Function 8: Move to track 0
BIOS+0x1B  JP SELDSK     Function 9: Select disk
BIOS+0x1E  JP SETTRK     Function 10: Set track
BIOS+0x21  JP SETSEC     Function 11: Set sector
BIOS+0x24  JP SETDMA     Function 12: Set DMA address
BIOS+0x27  JP READ       Function 13: Read sector
BIOS+0x2A  JP WRITE      Function 14: Write sector
BIOS+0x2D  JP LISTST     Function 15: List status
BIOS+0x30  JP SECTRAN    Function 16: Sector translate
```

### 6.3 PPAP Implementation

The trap handler intercepts `CALL` instructions targeting the BIOS
entry area. Most BIOS functions map trivially:

| BIOS fn | Name | PPAP Translation |
|---|---|---|
| 0 | BOOT | `sys_exit(0)` |
| 1 | WBOOT | `sys_exit(0)` (same as BOOT) |
| 2 | CONST | Non-blocking stdin check → A=FF or 0 |
| 3 | CONIN | `sys_read(0, &ch, 1)` → A=char |
| 4 | CONOUT | `sys_write(1, &C, 1)` (C register = char) |
| 5 | LIST | `sys_write(1, &C, 1)` (map to stdout) |
| 6 | PUNCH | No-op |
| 7 | READER | Return 0x1A (EOF) |
| 8–14 | Disk I/O | Return error (not supported) |
| 15 | LISTST | Return 0xFF (always ready) |
| 16 | SECTRAN | Return HL=BC (1:1 translation) |

Direct disk I/O (BIOS functions 8–14) is not supported. Programs
that bypass BDOS for raw sector access will not work. This excludes
some disk utilities and copy-protection schemes, but the vast
majority of CP/M software uses BDOS exclusively.

### 6.4 BIOS Jump Table Setup

```c
/* Write BIOS jump table into emulated memory */
static void cpm_setup_bios_table(uint8_t *mem) {
    for (int i = 0; i < 17; i++) {
        uint16_t addr = CPM_BIOS_ENTRY + i * 3;
        mem[addr] = 0xC9;      /* RET (trap intercepts the CALL) */
        mem[addr + 1] = 0x00;  /* padding */
        mem[addr + 2] = 0x00;
    }
}
```

The trap handler detects CALL targets within the BIOS range
(`CPM_BIOS_ENTRY` to `CPM_BIOS_ENTRY + 0x33`) and dispatches
by offset:

```c
if (addr >= CPM_BIOS_ENTRY && addr < CPM_BIOS_ENTRY + 0x33) {
    int bios_fn = (addr - CPM_BIOS_ENTRY) / 3;
    return cpm_bios_dispatch(cpu, cpm, bios_fn);
}
```

---

## 7. File System Mapping

### 7.1 Drive Letter Translation

CP/M uses drive letters A:–P:. The bridge maps them to PPAP paths:

```
A:HELLO.COM     →  /a/HELLO.COM
B:DATA.TXT      →  /b/DATA.TXT
(no drive)      →  /<current_drive>/filename
```

Drive letters are lowercased and become directories under root.

### 7.2 FCB-to-Path Translation

```c
static void cpm_fcb_to_path(cpm_state_t *cpm, const uint8_t *fcb,
                            char *path, int path_size) {
    /* Determine drive */
    uint8_t drive = fcb[0];
    if (drive == 0) drive = cpm->current_drive + 1;  /* default → current */
    char drive_char = 'a' + drive - 1;

    /* Extract filename (strip trailing spaces) */
    char name[9], ext[4];
    int nlen = 8, elen = 3;
    memcpy(name, &fcb[1], 8); name[8] = 0;
    memcpy(ext, &fcb[9], 3);  ext[3] = 0;
    while (nlen > 0 && name[nlen - 1] == ' ') nlen--;
    while (elen > 0 && ext[elen - 1] == ' ') elen--;
    name[nlen] = 0;
    ext[elen] = 0;

    /* Build path: /drive/USER_AREA/NAME.EXT */
    if (cpm->current_user == 0) {
        if (elen > 0)
            snprintf(path, path_size, "/%c/%s.%s", drive_char, name, ext);
        else
            snprintf(path, path_size, "/%c/%s", drive_char, name);
    } else {
        snprintf(path, path_size, "/%c/user%d/%s.%s",
                 drive_char, cpm->current_user, name, ext);
    }
}
```

### 7.3 Case-Insensitive Matching

CP/M filenames are case-insensitive. PPAP (romfs/UFS) is
case-sensitive. The bridge performs case-insensitive lookup:

1. Try the path as-is (uppercase, since FCB names are uppercase)
2. If not found, try lowercase conversion
3. If not found, scan the directory for a case-insensitive match

This mirrors the approach used by the Human68k subsystem (see
`docs/subsystems/human68k.md` §8.3).

### 7.4 User Area Mapping

CP/M user areas (0–15) map to subdirectories:

```
User 0 (default):  /a/FILENAME.EXT        (no subdirectory)
User 1:            /a/user1/FILENAME.EXT
User 2:            /a/user2/FILENAME.EXT
...
User 15:           /a/user15/FILENAME.EXT
```

User area 0 maps directly to the drive root (no `user0/`
subdirectory) since most CP/M programs use only user 0.

### 7.5 Wildcard Matching

CP/M wildcards (`?` matches any single character, `*` matches
remaining characters in a field) are used in Search First/Next
(BDOS 17/18) and Delete File (BDOS 19).

The bridge converts CP/M wildcard patterns to a simple matching
function applied to directory entries during `readdir()`:

```c
static int cpm_match_fcb(const uint8_t *pattern_fcb, const char *filename) {
    /* Parse filename into 8+3 fields, compare with pattern FCB */
    /* '?' in pattern matches any character */
    /* Returns 1 if match, 0 if not */
}
```

### 7.6 Directory Search (Functions 17/18)

Search First/Next populate the DMA buffer with a directory entry
in FCB format (32 bytes at offset determined by the return value):

```
DMA + (return_value * 32):
  [0]     User number
  [1-8]   Filename
  [9-11]  Extension
  [12-15] Extent info
  [16-31] Allocation map (zeroed by PPAP)
```

The return value (0–3) indicates which of the four 32-byte slots
in the 128-byte DMA buffer contains the result. Return 0xFF means
no more matches.

---

## 8. Terminal Handling

### 8.1 Terminal Expectations

CP/M programs assume various terminal types:
- **ADM-3A** — most common on early CP/M systems (WordStar default)
- **VT52** — common on later systems
- **Kaypro** — ADM-3A superset with attribute escape sequences
- **VT100** — some later programs
- **Raw** — programs that only use printable characters + CR/LF

### 8.2 PPAP Terminal Strategy

PPAP's framebuffer console (`fbcon`) implements VT100/ANSI CSI
escape sequences natively. A translator in the CP/M console
output path (`cpm_bridge.c`) converts ADM-3A, VT52, and Kaypro
sequences to VT100 before they reach fbcon.

**Architecture:**

```
CP/M program → ADM-3A/VT52/Kaypro → translator → VT100 → fbcon (LCD)
                                                        → UART (serial)
```

### 8.3 ADM-3A Translation

ADM-3A control characters and escape sequences are always
translated:

| ADM-3A | VT100 equivalent | Description |
|---|---|---|
| `^Z` (0x1A) | `ESC[2J ESC[H` | Clear screen + cursor home |
| `^K` (0x0B) | `ESC[A` | Cursor up |
| `^L` (0x0C) | `ESC[C` | Cursor right |
| `^^` (0x1E) | `ESC[H` | Cursor home |
| `ESC = row col` | `ESC[row;colH` | Cursor address (+0x20 encoding) |
| `ESC *` | `ESC[2J` | Clear screen |
| `ESC +` | `ESC[J` | Clear to end of screen |
| `ESC T` / `ESC E` | `ESC[K` | Clear to end of line |

WordStar control characters (`^A`, `^B`, `^S`, etc.) are silently
dropped — they are attribute toggles with no VT100 equivalent.

### 8.4 VT52 Translation

VT52 escape sequences are translated to their VT100 equivalents:

| VT52 | VT100 equivalent | Description |
|---|---|---|
| `ESC A` | `ESC[A` | Cursor up |
| `ESC B` | `ESC[B` | Cursor down |
| `ESC C` | `ESC[C` | Cursor right |
| `ESC D` | `ESC[D` | Cursor left |
| `ESC H` | `ESC[H` | Cursor home |
| `ESC I` | `ESC M` | Reverse line feed |
| `ESC J` | `ESC[J` | Erase to end of screen |
| `ESC K` | `ESC[K` | Erase to end of line |
| `ESC Y row col` | `ESC[row;colH` | Cursor address (+0x20 encoding) |

### 8.5 Kaypro Attribute Translation

Kaypro systems extend ADM-3A with attribute escape sequences:

| Kaypro | VT100 equivalent | Description |
|---|---|---|
| `ESC G <n>` | `ESC[0;...m` | Set attributes (bitmask) |
| `ESC B <n>` | `ESC[Nm` | Start attribute |
| `ESC C <n>` | `ESC[Nm` | End attribute |

**ESC G bitmask:** bit 0 = underline (SGR 4), bit 1 = blink (SGR 5),
bit 2 = bold (SGR 1), bit 3 = reverse video (SGR 7).

**ESC B/C parameter:** `'0'` = dim, `'1'` = underline, `'2'` = blink,
`'3'` = underline (alias), `'4'` = bold.

### 8.6 VT52 / Kaypro Dialect Auto-Detection

`ESC B` and `ESC C` are ambiguous — VT52 uses them for cursor
down/right (standalone, no parameter byte), while Kaypro uses
them for attribute on/off (followed by a digit parameter).

The translator auto-detects the dialect by observing which
escape sequences the program emits:

| Sequence observed | Latches dialect |
|---|---|
| `ESC A`, `ESC D`, `ESC H`, `ESC I`, `ESC J`, `ESC K`, `ESC Y` | **VT52** |
| `ESC G` | **Kaypro** |

Once latched, `ESC B` / `ESC C` follow the detected dialect:

- **VT52 or unknown** (default) → cursor down / cursor right
- **Kaypro** → attribute on / attribute off (consume next byte)

The default is VT52, since it is far more common in CP/M
software. Kaypro programs typically emit `ESC G` early (e.g. to
reset attributes at startup), which latches Kaypro mode before
any ambiguous `ESC B` / `ESC C` appears.

ADM-3A sequences (`ESC =`, `ESC *`, `ESC +`, `ESC T`, `ESC E`,
and control characters) are unambiguous and always handled
regardless of dialect.

### 8.7 VT100 Pass-Through

If a CP/M program emits native VT100 CSI sequences (`ESC [`),
the translator passes them through unchanged to fbcon.

### 8.8 fbcon VT100 Support

The fbcon driver (`src/drivers/fbcon.c`) implements the following
VT100/ANSI CSI sequences used by the translator:

| Sequence | Description |
|---|---|
| `ESC[nA` / `ESC[nB` / `ESC[nC` / `ESC[nD` | Cursor movement |
| `ESC[row;colH` / `ESC[row;colf` | Cursor position |
| `ESC[nJ` (0/1/2) | Erase display |
| `ESC[nK` (0/1/2) | Erase line |
| `ESC[nL` | Insert lines |
| `ESC[nM` | Delete lines |
| `ESC[n;...m` | SGR (colors, bold, reverse video) |
| `ESC[top;botr` | Set scroll region |
| `ESC M` | Reverse index (scroll down at top) |
| `ESC 7` / `ESC 8` | Save / restore cursor + attributes |
| `ESC c` | Full reset |

SGR supports: reset (0), bold (1), reverse video (7),
bold off (22), reverse off (27), foreground 30-37/39/90-97,
background 40-47/49/100-107.

---

## 9. Implementation Plan

All phases (1–8) complete. See `docs/archive/cpm.md` for the detailed
implementation history.

---

## 10. Testing Strategy

### 10.1 Test Binaries

Hand-assembled CP/M `.COM` test programs:

| Test | BDOS Calls | Validates |
|---|---|---|
| `HELLO.COM` | fn 9 (print string) | Loader, BDOS entry, string output |
| `ECHO.COM` | fn 1, 2 (console I/O) | Character input/output |
| `EXIT0.COM` | fn 0 (system reset) | Clean exit |
| `TYPE.COM` | fn 15, 20, 16 (open/read/close) | Sequential file read |
| `SAVE.COM` | fn 22, 21, 16 (make/write/close) | Sequential file write |
| `DIR.COM` | fn 17, 18, 9 (search/print) | Directory listing |
| `RAND.COM` | fn 15, 33, 34, 16 (random access) | Random file I/O |
| `USER.COM` | fn 32 (set/get user) | User area switching |

### 10.2 Building Test Binaries

Use `z80asm` or `zmac` cross-assembler to produce `.COM` files:

```asm
; hello.asm — CP/M Hello World
        ORG     0100h

        LD      C, 9        ; BDOS function 9: Print String
        LD      DE, msg     ; pointer to message
        CALL    5            ; call BDOS

        LD      C, 0        ; BDOS function 0: System Reset
        CALL    5

msg:    DB      'Hello, World!', 0Dh, 0Ah, '$'
```

Assemble with: `z80asm -o HELLO.COM hello.asm`

### 10.3 Userland Tests ✅

On-target tests (`tests/user/test_cpm.c`) with hand-assembled Z80 .COM
binaries. Each test writes a .COM to `/tmp`, executes via `vfork`+`execve`,
and captures output through pipes. 13 tests covering BDOS functions
0/2/6/9/12/14/15/16/20/21/22/24/25/32, warm boot, HALT exit, and `.com`
extension validation.

This is now the primary regression coverage for the CP/M bridge path. It
tests the real kernel exec and subsystem dispatch path instead of keeping a
separate host-only harness in sync with the runtime behavior.

### 10.4 Integration Tests

Run on QEMU (m68k or ARM target):
1. Place `.COM` files in romfs under `/subsys/cpm/`
2. `exec("/subsys/cpm/HELLO.COM")` from the PPAP shell
3. Verify output on UART

### 10.5 Reference Software Tests

After basic functionality is working:

1. **ZEXALL.COM** — exhaustive Z80 instruction test (CP/M binary).
   Validates the ecpu-z80 emulator via BDOS console output.
   Expected: pass all tests except undocumented flag bits.

2. **MBASIC.COM** — Microsoft BASIC interpreter. Tests console I/O,
   random file access, and overall system stability.

3. **Turbo Pascal** — compile and run a simple program. Tests
   sequential file I/O and console interaction.

---

## 11. Open Questions

1. ~~**Non-blocking console I/O:**~~ **Resolved.** `cpm_char_ready()`
   uses `sys_poll()` with `POLLIN` and zero timeout for non-blocking
   character readiness checking. BDOS function 6 (E=0xFF) and function
   11 work correctly.

2. **Record-oriented vs stream I/O:** CP/M reads/writes exactly 128
   bytes per record. PPAP's `read()`/`write()` are byte-stream.
   The bridge must handle:
   - Short reads (file not a multiple of 128 → pad with 0x1A)
   - Sequential position tracking per FCB
   - Random access by record number → `lseek(record * 128)`

3. **Multiple FCBs pointing to same file:** CP/M allows opening the
   same file with multiple FCBs. The bridge must handle this —
   either by opening multiple PPAP fds (one per FCB) or by sharing
   an fd with independent seek positions.

4. **CP/M 0x1A (^Z) EOF convention:** CP/M text files use 0x1A as
   end-of-file marker within the last 128-byte record. The bridge
   should optionally strip 0x1A padding when transferring files
   to/from PPAP, or leave it to the application.

5. **Submit files (.SUB):** CP/M's batch processing via SUBMIT
   creates `$$$.SUB` files. Supporting this requires the CCP
   (Console Command Processor), which is out of initial scope.

6. **RP2040 memory budget:** 64 KB emulated memory + `z80_state_t`
   (~80 bytes) + `cpm_state_t` (~200 bytes) + ecpu-z80 code
   (~12 KB) + bridge code (~4 KB) = ~80 KB total. On RP2040 with
   264 KB SRAM, this leaves ~184 KB for kernel + page pool +
   other processes. Feasible but tight.

7. **Ctrl-C handling:** CP/M checks for Ctrl-C during console I/O
   functions and performs a warm boot. The bridge should intercept
   SIGINT and translate it to a warm boot (exit) or allow the
   program to handle it via BDOS function 11 polling.
