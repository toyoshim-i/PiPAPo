# MS-DOS Subsystem Proposal

> **Note**: File paths in this document may be outdated after the source tree
> reorganization.  See [Source Tree Structure](../getting_started/source_tree.md)
> for the current layout.

An MS-DOS personality layer for PPAP, following the same pattern as the
Human68k subsystem (`human68k_bridge.c`) and CP/M subsystem
(`cpm_bridge.c`).  Translates DOS INT 21h calls and related interrupts
into PPAP syscalls, allowing DOS .COM and .EXE (MZ) binaries to run on
any PPAP host architecture through the eCPU i8086 emulator or natively
on a V30/8086 target.

---

## 1. Goals and Scope

### 1.1 Primary Goal

Run MS-DOS 2.x–3.x compatible .COM and .EXE programs on PPAP:

- Load DOS .COM flat binaries (single segment, max 64 KB).
- Load DOS .EXE (MZ format) multi-segment binaries with relocation.
- Translate INT 21h DOS API calls to PPAP VFS, process, and TTY operations.
- Translate INT 10h video calls to PPAP TTY output (text mode only).
- Provide a PSP (Program Segment Prefix) compatible with DOS 2.x+.
- Support drive letter mapping (A:, B:, C:) to PPAP mount points.
- Integrate with the ptrace/trace subsystem (new ABI tag for DOS calls).

### 1.2 Extended Goals

- FCB-based file operations (INT 21h AH=0Fh–24h) for DOS 1.x compat.
- INT 10h direct video buffer access (CGA-compatible 80×25 text).
- INT 16h keyboard input with scan codes.
-AZOMBIE TSR (Terminate and Stay Resident) support.
- INT 2Fh multiplex interrupt (limited, for PRINT and ASSIGN).
- Batch file execution (COMMAND.COM-style `.BAT` interpreter).
- Environment variable expansion.

### 1.3 Out of Scope

- EMS/XMS extended memory.
- DOS 4.x+ features (large partition support, INT 25h/26h absolute disk).
- DPMI (DOS Protected Mode Interface) or DOS extenders.
- Graphics modes (INT 10h AH=00h mode changes, direct VRAM pixel access).
- Sound (INT 61h PC speaker, Sound Blaster).
- Networking (INT 60h–6Fh redirector interface).
- Full COMMAND.COM shell (PPAP's own shell is used for launching DOS programs).

---

## 2. Architecture

### 2.1 Subsystem Registration

Following the existing pattern in `subsys.h`:

```c
/* src/kernel/proc/proc.h */
#define SUBSYS_PPAP      0
#define SUBSYS_HUMAN68K  1
#define SUBSYS_CPM       2
#define SUBSYS_MSDOS     3    /* new */

/* SUBSYS_MAX must be >= 4 (already 4 in current code) */
```

The DOS subsystem registers its `subsys_ops_t` in `subsys_ops_table[3]`:

```c
const subsys_ops_t msdos_subsys_ops = {
    .on_crash  = msdos_on_crash,
    .on_signal = msdos_on_signal,
    .on_init   = msdos_on_init,
};
```

### 2.2 Execution Model

DOS programs run on the i8086 eCPU emulator (see `docs/proposals/i8086_ecpu.md`)
or natively on a V30/8086 target.  The execution flow:

```
User: /bin/sh
  │
  │ execve("/subsys/dos/hello.com")
  ▼
Kernel: exec detects .COM or MZ signature
  │
  │ subsys = SUBSYS_MSDOS
  ▼
dos_loader.c
  │ Allocates segment(s), builds PSP, loads binary
  │ Sets up i8086 eCPU state (or V30 segments for native)
  ▼
dos_run_process()
  │
  │ ecpu_i8086_ops.run(cpu)  ──→  emulator loop
  │                                    │
  │                                    │ INT 21h → ECPU_TRAP_SWI
  │                                    ▼
  │                              dos_bridge.c
  │                                    │ Translates to PPAP syscalls
  │                                    │ Returns result in AX
  │                                    ▼
  │                              emulator continues
  │
  │ INT 21h AH=4Ch (EXIT) → ECPU_TRAP_EXIT
  ▼
Kernel: process exits, parent wakes up
```

On a native V30/8086 target, the i8086 eCPU is not needed — the DOS program
runs directly on the CPU.  INT 21h is hooked to the kernel trap handler,
which calls the same `dos_bridge.c` functions.

### 2.3 Relationship to Existing Subsystems

| Aspect | CP/M | Human68k | MS-DOS |
|--------|------|----------|--------|
| Host CPU | Z80 (eCPU) | m68k (native or eCPU) | i8086 (eCPU or native V30) |
| API entry | CALL 0x0005 | F-line ($FFxx) | INT 21h |
| Binary format | .COM (flat @ 0x0100) | .x/.r (relocatable) | .COM (flat @ 0x0100) + .EXE (MZ) |
| Memory model | Flat 64 KB | Flat 24-bit | Segmented 20-bit (1 MB) |
| Bridge file | `cpm_bridge.c` | `human68k_bridge.c` | `dos_bridge.c` |
| Loader file | `cpm_loader.c` | `human68k_loader.c` | `dos_loader.c` |
| Per-process state | `cpm_state_t` | `h68k_proc_t` | `dos_proc_t` |
| Drive mapping | A: → `/a/` | A: → `/a/` | A: → `/a/` |

---

## 3. DOS Memory Model (Emulated)

The i8086 eCPU provides 1 MB of flat memory internally.  Within that space,
the DOS bridge manages segment allocation using a simplified MCB (Memory
Control Block) scheme.

### 3.1 Memory Map

```
i8086 eCPU address space (1 MB)
┌──────────────────────────────────┐ 0xFFFFF
│  Reserved (BIOS data area sim)   │ 0xF0000
├──────────────────────────────────┤
│  Unused / available              │
│                                  │
├──────────────────────────────────┤
│  Program segment(s)             │
│  ┌───────────────────────┐      │
│  │ PSP (256 bytes)       │ seg:0000
│  │ Program code/data     │ seg:0100
│  │ Stack (grows down)    │ seg:FFFF ↓
│  └───────────────────────┘      │
├──────────────────────────────────┤
│  DOS data area (simulated)      │
│  Environment segment             │ 0x00800
├──────────────────────────────────┤
│  Interrupt vector table (IVT)    │ 0x00000  (256 × 4B = 1 KB)
│  BIOS data area (simulated)     │ 0x00400
└──────────────────────────────────┘
```

### 3.2 Segment Allocation

The DOS bridge maintains a simple free-list of paragraph-aligned blocks:

```c
#define DOS_PARA_SIZE       16      /* bytes per paragraph */
#define DOS_MCB_SIGNATURE_M 0x4D    /* 'M' — more blocks follow */
#define DOS_MCB_SIGNATURE_Z 0x5A    /* 'Z' — last block */

typedef struct dos_mcb {
    uint8_t  signature;     /* 'M' or 'Z' */
    uint16_t owner_psp;     /* PSP segment of owner (0 = free) */
    uint16_t size;          /* size in paragraphs (excluding header) */
} dos_mcb_t;
```

INT 21h memory functions map to MCB operations:

| AH | Function | Operation |
|----|----------|-----------|
| 48h | Allocate memory | Find free MCB, split, return segment |
| 49h | Free memory | Mark MCB as free, merge adjacent |
| 4Ah | Resize memory | Grow or shrink the MCB block |

Internally, MCB operations translate to PPAP page allocations as backing
storage.

### 3.3 .COM Loading

```c
int dos_load_com(i8086_state_t *cpu, dos_proc_t *dos,
                 const char *path)
{
    /* 1. Allocate one 64 KB segment */
    uint16_t seg = dos_alloc_segment(dos, 0x1000);  /* 64 KB = 0x1000 paragraphs */

    /* 2. Build PSP at seg:0000 */
    dos_build_psp(cpu, seg, dos);

    /* 3. Load binary at seg:0100 */
    int fd = sys_open(path, O_RDONLY, 0);
    int size = sys_read(fd, i8086_translate(cpu, seg, 0x0100), 0xFEFF);
    sys_close(fd);

    /* 4. Set registers */
    cpu->cs = seg;
    cpu->ds = seg;
    cpu->es = seg;
    cpu->ss = seg;
    cpu->sp = 0xFFFE;
    cpu->ip = 0x0100;

    return 0;
}
```

### 3.4 .EXE (MZ) Loading

```c
int dos_load_exe(i8086_state_t *cpu, dos_proc_t *dos,
                 const char *path)
{
    struct mz_header hdr;
    /* 1. Read MZ header */
    /* 2. Calculate load size from header */
    uint32_t code_size = (hdr.page_count * 512)
                       - (hdr.header_size * 16)
                       - (hdr.last_page_size ? 512 - hdr.last_page_size : 0);
    uint32_t total_para = (code_size + 15) / 16 + hdr.min_alloc;

    /* 3. Allocate segments: PSP (0x10 para) + program */
    uint16_t psp_seg = dos_alloc_segment(dos, 0x10 + total_para);
    uint16_t load_seg = psp_seg + 0x10;

    /* 4. Build PSP */
    dos_build_psp(cpu, psp_seg, dos);

    /* 5. Load program image after MZ header */
    sys_lseek(fd, hdr.header_size * 16, SEEK_SET);
    sys_read(fd, i8086_translate(cpu, load_seg, 0), code_size);

    /* 6. Apply segment relocations */
    for (int i = 0; i < hdr.reloc_count; i++) {
        uint16_t off, seg;
        /* Read relocation entry */
        /* Add load_seg to the word at seg:off in the loaded image */
        uint32_t addr = ((uint32_t)(seg + load_seg) << 4) + off;
        uint16_t val = i8086_read16(cpu, addr);
        i8086_write16(cpu, addr, val + load_seg);
    }

    /* 7. Set registers from header */
    cpu->cs = hdr.init_cs + load_seg;
    cpu->ip = hdr.init_ip;
    cpu->ss = hdr.init_ss + load_seg;
    cpu->sp = hdr.init_sp;
    cpu->ds = psp_seg;
    cpu->es = psp_seg;

    return 0;
}
```

---

## 4. INT 21h Bridge

### 4.1 Dispatch

The eCPU traps INT 21h as `ECPU_TRAP_SWI` with `param = 0x21`.  The trap
handler dispatches based on AH:

```c
int dos_trap_handler(ecpu_state_t *cpu, int trap_type,
                     uint32_t param, void *ctx)
{
    dos_proc_t *dos = (dos_proc_t *)ctx;
    i8086_state_t *i86 = (i8086_state_t *)cpu;

    if (trap_type == ECPU_TRAP_SWI) {
        switch (param) {
        case 0x20: return dos_exit_int20(i86, dos);
        case 0x21: return dos_int21h(i86, dos);
        case 0x10: return dos_int10h(i86, dos);  /* Video */
        case 0x16: return dos_int16h(i86, dos);  /* Keyboard */
        default:   return ECPU_TRAP_HANDLED;     /* Ignore unknown */
        }
    }
    if (trap_type == ECPU_TRAP_HALT)
        return ECPU_TRAP_EXIT;

    return ECPU_TRAP_UNHANDLED;
}
```

### 4.2 Function Coverage

The INT 21h bridge is implemented in phases, starting with the most
commonly used functions.

#### Phase 1 — Core Functions (~20 functions)

These cover enough to run simple DOS .COM utilities:

| AH | Name | PPAP mapping |
|----|------|-------------|
| 01h | Read char with echo | `sys_read(0)` + `sys_write(1)` |
| 02h | Write character | `sys_write(1)` |
| 06h | Direct console I/O | `sys_read(0)` / `sys_write(1)` (non-blocking) |
| 08h | Read char no echo | `sys_read(0)` |
| 09h | Print string ($-term) | `sys_write(1)` until '$' |
| 0Ah | Buffered input | Line-edit read into DS:DX buffer |
| 0Bh | Check input status | `sys_poll(0)` or `sys_ioctl(FIONREAD)` |
| 19h | Get current drive | Return `dos->current_drive` |
| 25h | Set interrupt vector | Store in emulated IVT |
| 2Ah | Get date | `sys_gettimeofday()` → year/month/day |
| 2Ch | Get time | `sys_gettimeofday()` → hour/min/sec |
| 30h | Get DOS version | Return 3.30 (static) |
| 35h | Get interrupt vector | Read from emulated IVT |
| 3Ch | Create file | `sys_open(path, O_CREAT\|O_TRUNC\|O_WRONLY)` |
| 3Dh | Open file | `sys_open(path, mode)` |
| 3Eh | Close file | `sys_close(handle)` |
| 3Fh | Read file | `sys_read(handle, buf, count)` |
| 40h | Write file | `sys_write(handle, buf, count)` |
| 41h | Delete file | `sys_unlink(path)` |
| 42h | Seek (LSEEK) | `sys_lseek(handle, offset, whence)` |
| 43h | Get/set file attributes | Stub (return 0x20 archive) |
| 47h | Get current directory | Return `dos->cwd[drive]` |
| 4Ch | Terminate with code | `sys_exit(code)` |

#### Phase 2 — File System Extensions (~15 functions)

| AH | Name | PPAP mapping |
|----|------|-------------|
| 39h | Create directory | `sys_mkdir(path)` |
| 3Ah | Remove directory | `sys_rmdir(path)` |
| 3Bh | Change directory | Update `dos->cwd[drive]` |
| 44h | IOCTL | Subset: AX=4400h (get device info) |
| 45h | Duplicate handle | `sys_dup(handle)` |
| 46h | Force duplicate | `sys_dup2(old, new)` |
| 48h | Allocate memory | MCB allocation |
| 49h | Free memory | MCB free |
| 4Ah | Resize memory | MCB resize |
| 4Bh | EXEC | `sys_vfork()` + `sys_execve()` |
| 4Dh | Get child return code | `sys_waitpid()` |
| 56h | Rename | `sys_rename(old, new)` |
| 57h | Get/set file date | Stub |
| 5Ah | Create temporary file | `sys_open()` with generated name |
| 5Bh | Create new file | `sys_open(O_CREAT\|O_EXCL)` |

#### Phase 3 — FCB Operations (~10 functions)

For DOS 1.x compatibility.  FCB (File Control Block) operations translate
to PPAP handle-based file I/O internally:

| AH | Name |
|----|------|
| 0Fh | Open file (FCB) |
| 10h | Close file (FCB) |
| 11h | Search first (FCB) |
| 12h | Search next (FCB) |
| 14h | Sequential read (FCB) |
| 15h | Sequential write (FCB) |
| 16h | Create file (FCB) |
| 21h | Random read (FCB) |
| 22h | Random write (FCB) |
| 27h | Random block read (FCB) |
| 28h | Random block write (FCB) |

FCB-to-handle translation follows the same pattern as CP/M's FCB table
in `cpm_bridge.c`.

### 4.3 Handle Translation

DOS uses handle numbers 0–19 (JFT, Job File Table, 20 entries in the PSP).
The bridge maps these to PPAP file descriptors:

```c
#define DOS_MAX_HANDLES  20

typedef struct dos_proc {
    /* Handle table: dos_handle → ppap_fd (-1 = closed) */
    int handle_to_fd[DOS_MAX_HANDLES];

    /* Standard handles pre-opened at load time:
     *   0 = stdin  → PPAP fd 0
     *   1 = stdout → PPAP fd 1
     *   2 = stderr → PPAP fd 2
     *   3 = stdaux → PPAP fd 2 (serial, mapped to stderr)
     *   4 = stdprn → PPAP fd 2 (printer, mapped to stderr)
     */

    uint8_t  current_drive;          /* 0=A, 1=B, ... */
    char     cwd[26][DOS_PATH_MAX];  /* per-drive CWD */

    /* Environment block */
    uint16_t env_seg;                /* segment of environment strings */

    /* PSP segment of this process */
    uint16_t psp_seg;

    /* MCB chain head (for memory management) */
    uint16_t mcb_head_seg;

    /* FCB-to-handle mapping (for FCB operations) */
    struct {
        uint16_t fcb_seg;    /* segment:offset of FCB (0 = free) */
        uint16_t fcb_off;
        int      fd;         /* PPAP fd */
        uint32_t file_pos;   /* current position */
    } fcb_table[8];

    /* Directory search state (for FindFirst/FindNext) */
    char     search_pattern[DOS_PATH_MAX];
    void    *search_dir;     /* opaque DIR handle */
    uint8_t  search_attr;    /* attribute filter */
} dos_proc_t;
```

### 4.4 Path Translation

DOS paths use backslash separators and drive letters.  The bridge converts
them to PPAP paths:

```
C:\GAMES\DOOM.EXE  →  /c/GAMES/DOOM.EXE
A:HELLO.COM        →  /a/<cwd_A>/HELLO.COM
HELLO.COM          →  /a/<cwd_current>/HELLO.COM (relative to current drive)
```

Rules:

1. Drive letter `X:` maps to PPAP mount point `/x/` (lowercase).
2. Backslashes become forward slashes.
3. Relative paths are resolved against the per-drive CWD.
4. DOS filenames are case-insensitive; the bridge does case-folding
   lookup against the PPAP filesystem (try exact match first, then
   case-insensitive scan).

```c
int dos_resolve_path(dos_proc_t *dos, const char *dos_path,
                     char *ppap_path, int ppap_path_size);
```

### 4.5 Error Mapping

DOS returns errors via the carry flag (CF) and AX.  The bridge maps
PPAP errno values to DOS error codes:

| PPAP errno | DOS error (AX) | DOS meaning |
|-----------|---------------|------------|
| ENOENT | 2 | File not found |
| ENOTDIR | 3 | Path not found |
| EMFILE | 4 | Too many open files |
| EACCES | 5 | Access denied |
| EBADF | 6 | Invalid handle |
| ENOMEM | 8 | Insufficient memory |
| EEXIST | 80 | File exists |

On error, the bridge sets CF=1 and AX=error code.  On success, CF=0.

---

## 5. PSP (Program Segment Prefix)

The bridge constructs a 256-byte PSP at the base of each program's segment:

```c
void dos_build_psp(i8086_state_t *cpu, uint16_t psp_seg,
                   dos_proc_t *dos)
{
    uint32_t base = (uint32_t)psp_seg << 4;

    /* 00h: INT 20h (CD 20) — terminate program */
    i8086_write16(cpu, base + 0x00, 0x20CD);

    /* 02h: Memory size (top of allocated segment) */
    i8086_write16(cpu, base + 0x02, psp_seg + /* allocated paragraphs */);

    /* 05h: Far call to DOS dispatcher (INT 21h + RETF) */
    i8086_write8(cpu, base + 0x05, 0xEA);    /* JMP FAR */
    /* ... address of INT 21h handler ... */

    /* 0Ah–15h: Saved interrupt vectors (INT 22h, 23h, 24h) */
    /* Read current values from IVT and save */

    /* 2Ch: Environment segment */
    i8086_write16(cpu, base + 0x2C, dos->env_seg);

    /* 50h: INT 21h + RETF (alternate DOS entry) */
    i8086_write8(cpu, base + 0x50, 0xCD);
    i8086_write8(cpu, base + 0x51, 0x21);
    i8086_write8(cpu, base + 0x52, 0xCB);     /* RETF */

    /* 5Ch: FCB 1 (parsed from command line) */
    /* 6Ch: FCB 2 (parsed from command line) */

    /* 80h: Command tail */
    /* Copy from argv, DOS format: length byte + space-separated args + CR */
}
```

---

## 6. Video and Keyboard (INT 10h / INT 16h)

### 6.1 INT 10h — Video Services

Only text-mode services are supported.  All output maps to PPAP TTY:

| AH | Function | Implementation |
|----|----------|---------------|
| 00h | Set video mode | Stub (always report mode 3 = 80×25 color) |
| 02h | Set cursor position | ANSI escape: `\033[row;colH` |
| 03h | Get cursor position | Return stored cursor pos |
| 06h | Scroll up | ANSI escape: `\033[nS` |
| 07h | Scroll down | ANSI escape: `\033[nT` |
| 09h | Write char+attr | `sys_write(1)` (ignore attribute) |
| 0Eh | Teletype output | `sys_write(1)` (simplest path) |
| 0Fh | Get video mode | Return mode 3, 80 columns, page 0 |

### 6.2 INT 16h — Keyboard Services

| AH | Function | Implementation |
|----|----------|---------------|
| 00h | Read key | `sys_read(0)` → AH=scan code, AL=ASCII |
| 01h | Check key | `sys_poll(0)` → ZF=0 if key available |
| 02h | Get shift flags | Return 0 (no shift key tracking) |

Scan code generation is approximate — the bridge maps ASCII codes to
PC-compatible scan codes for common keys.

---

## 7. Trace Integration

### 7.1 ABI Tag

```c
#define PPAP_TRACE_ABI_DOS_INT21  5   /* in ptrace.h */
```

### 7.2 Event Generation

The bridge generates trace events at INT 21h entry and exit:

```c
/* At INT 21h entry */
trace_subsys_event(PPAP_TRACE_EVENT_SUBSYS_ENTER,
                   PPAP_TRACE_ABI_DOS_INT21,
                   regs->ah,           /* function number */
                   args);              /* AX, BX, CX, DX, DS, SI */

/* At INT 21h exit */
trace_subsys_event(PPAP_TRACE_EVENT_SUBSYS_EXIT,
                   PPAP_TRACE_ABI_DOS_INT21,
                   regs->ah,
                   &ret);              /* AX return value */
```

### 7.3 Trace Output

```
$ trace --subsys hello.com
[subsys:dos] INT21 AH=09 WRITE_STRING DS:DX=0x0180
[subsys:dos] INT21 AH=3D OPEN DS:DX=0x0150 "DATA.TXT" AL=00 → handle=5
[subsys:dos] INT21 AH=3F READ BX=5 CX=128 DS:DX=0x0200 → 128
[subsys:dos] INT21 AH=3E CLOSE BX=5 → OK
[subsys:dos] INT21 AH=4C EXIT AL=00
```

### 7.4 pdb Debugger

The pdb debugger gains DOS awareness through the existing multi-ABI
infrastructure:

- `show abi` reports `dos/int21`
- Register display uses 8086 register set (AX, BX, CX, DX, SI, DI, BP,
  SP, CS, DS, ES, SS, IP, FLAGS)
- `disas` uses 8086 disassembly
- Breakpoints work through the eCPU step/trap mechanism

---

## 8. New Files

```
src/kernel/subsys/
  dos_bridge.c        — INT 21h dispatch and function implementations
  dos_bridge.h        — dos_proc_t, constants, public API
  dos_loader.c        — .COM and .EXE (MZ) binary loading
  dos_loader.h        — Loader API, MZ header struct

src/kernel/exec/
  exec_dos.c          — exec path for .COM and .EXE detection and dispatch
```

Changes to existing files:

| File | Change |
|------|--------|
| `src/kernel/proc/proc.h` | Add `SUBSYS_MSDOS = 3` |
| `src/kernel/subsys/subsys.c` | Register `msdos_subsys_ops` in slot 3 |
| `src/kernel/exec/exec.c` | Add MZ/.COM detection in exec path |
| `src/common/ptrace.h` | Add `PPAP_TRACE_ABI_DOS_INT21`, `PPAP_TRACE_REGSET_8086` |
| `src/user/trace.c` | Add DOS INT 21h function name decoder |
| `src/user/pdb_regs.c` | Add 8086 register names |
| `src/user/pdb_trace_util.c` | Add DOS ABI name formatting |
| `config.h` | Add `PPAP_ENABLE_MSDOS` build flag |

---

## 9. Build Configuration

```cmake
option(PPAP_ENABLE_MSDOS "Enable MS-DOS subsystem" OFF)

if(PPAP_ENABLE_MSDOS)
    target_sources(ppap PRIVATE
        src/kernel/subsys/dos_bridge.c
        src/kernel/subsys/dos_loader.c
        src/kernel/exec/exec_dos.c
    )
    target_compile_definitions(ppap PRIVATE PPAP_ENABLE_MSDOS=1)
endif()
```

The DOS subsystem requires the i8086 eCPU (`PPAP_ENABLE_ECPU_I8086`)
unless building for a native V30/8086 target.

---

## 10. Implementation Phases

### Phase D-1: .COM Loader and Minimal INT 21h

**Goal**: "Hello, world" DOS .COM program runs and prints output.

1. Implement `dos_loader.c` — .COM loading only.
2. Implement `dos_bridge.c` — INT 21h AH=02h (putchar), AH=09h (print
   string), AH=4Ch (exit).
3. Build PSP with minimal fields.
4. Test with a hand-assembled 3-line .COM program.

**Verification**: "Hello, world!" appears on PPAP console.

### Phase D-2: File I/O

**Goal**: DOS programs can read and write files.

1. INT 21h AH=3Ch–42h (create, open, close, read, write, seek).
2. Path translation (drive letters, backslash conversion).
3. Handle table management.
4. Error code mapping.

**Verification**: A DOS .COM program that reads a file and prints its
contents works correctly.

### Phase D-3: .EXE Loading

**Goal**: Multi-segment DOS .EXE programs run.

1. MZ header parsing and validation.
2. Segment allocation and relocation.
3. Multi-segment CS/SS setup.

**Verification**: A simple .EXE program (compiled with OpenWatcom or
ia16-elf-gcc) runs correctly.

### Phase D-4: Memory and Process Management

**Goal**: DOS programs can allocate memory and spawn child processes.

1. INT 21h AH=48h/49h/4Ah (memory allocation via MCB).
2. INT 21h AH=4Bh (EXEC — load and execute child program).
3. INT 21h AH=4Dh (get child return code).

**Verification**: A parent .COM launches a child .COM and retrieves its
exit code.

### Phase D-5: Console and Keyboard

**Goal**: Interactive DOS programs work.

1. INT 21h AH=01h/06h/08h/0Ah (character input functions).
2. INT 10h AH=02h/0Eh (cursor positioning, teletype output).
3. INT 16h AH=00h/01h (keyboard read/check).

**Verification**: A simple DOS text editor or menu-driven program works
interactively.

### Phase D-6: FCB Operations and Compatibility

**Goal**: DOS 1.x programs using FCBs work.

1. INT 21h AH=0Fh–28h (FCB file operations).
2. Directory search (FindFirst/FindNext via AH=4Eh/4Fh).
3. Remaining misc functions (AH=19h, 25h, 2Ah, 2Ch, 30h, 35h, etc.).

**Verification**: Classic DOS utilities (EDLIN, DEBUG) run.

### Phase D-7: Trace Integration

**Goal**: `trace --subsys` and `pdb` work with DOS programs.

1. Add `PPAP_TRACE_ABI_DOS_INT21` events.
2. INT 21h function name decoder in trace tool.
3. 8086 register display in pdb.

**Verification**: `trace --subsys hello.com` shows decoded INT 21h calls.

---

## 11. Testing

### 11.1 Test Programs

Ship a set of minimal DOS .COM test programs in `tests/dos/`:

```
tests/dos/
  hello.com           — AH=09h print + AH=4Ch exit
  fileio.com          — Create, write, read, close, delete
  args.com            — Print command tail from PSP
  memory.com          — AH=48h alloc + AH=49h free
  exec.com            — AH=4Bh launch child + AH=4Dh wait
```

These are assembled from source using NASM and included in the test
suite as binary blobs (similar to test_x68k and test_cpm patterns).

### 11.2 Integration Test

A `test_dos` test case exercises the bridge via the standard PPAP test
runner.  It is similar in structure to `test_cpm`:

```c
static int test_dos(void) {
    /* Spawn hello.com via execve, verify output */
    /* Spawn fileio.com, verify file created and read back */
    /* Spawn exec.com, verify child return code */
    return 0;
}
```

---

## 12. Risks and Open Questions

### 12.1 Case Sensitivity

PPAP filesystems (tmpfs, UFS) are case-sensitive.  DOS is case-insensitive.
The bridge must do case-folding directory lookups, which adds overhead.
The recommended approach: DOS programs see uppercase filenames; the bridge
uppercases all filenames during directory creation and does case-insensitive
comparison during lookup.

### 12.2 i8086 eCPU Performance

The i8086 eCPU is a software interpreter running on ARM or m68k.  Complex
DOS programs with heavy computation may be slow.  For the initial target
(simple utilities, text adventures, BASIC interpreters), this should be
acceptable.

### 12.3 INT 21h Coverage

MS-DOS has ~100 INT 21h functions, many with sub-functions.  Full
compatibility is impractical.  The phased approach covers the most-used
subset.  Programs using exotic INT 21h functions will fail gracefully
(return "invalid function" error code).

### 12.4 TSR Programs

Terminate-and-Stay-Resident programs (INT 21h AH=31h, INT 27h) are
complex to support correctly.  They require keeping the program's memory
allocated while the parent process resumes.  This is deferred to extended
goals.

### 12.5 COMMAND.COM

The DOS subsystem does not include a COMMAND.COM equivalent.  Programs
are launched from the PPAP shell.  A minimal COMMAND.COM with `.BAT`
file support could be added as an extended goal.

---

## 13. Dependency Graph

```
i8086 eCPU (docs/proposals/i8086_ecpu.md)
  └─→ D-1 (.COM + minimal INT 21h)
        └─→ D-2 (file I/O)
              └─→ D-3 (.EXE loading)
              └─→ D-4 (memory + process)
              └─→ D-5 (console + keyboard)
                    └─→ D-6 (FCB + compat)
                          └─→ D-7 (trace integration)
```

The i8086 eCPU must be functional before any DOS subsystem work begins
(unless targeting native V30, where the eCPU is optional).

---

## 14. Related Documentation

- [docs/proposals/i8086_ecpu.md](i8086_ecpu.md) — i8086 eCPU emulator
- [docs/targets/ia16.md](../targets/ia16.md) — IBM PC target port (ia16)
- [docs/kernel/trace.md](../kernel/trace.md) — Trace and debug subsystem
- [docs/kernel/syscall.md](../kernel/syscall.md) — System call reference
