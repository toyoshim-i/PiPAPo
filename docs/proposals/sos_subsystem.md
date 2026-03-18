# S-OS "SWORD" Subsystem Proposal

An S-OS "SWORD" personality layer for PPAP, following the same pattern as
the CP/M subsystem (`cpm_bridge.c`).  Translates S-OS system calls into
PPAP syscalls, allowing S-OS binaries to run on any PPAP host architecture
through the existing eCPU-Z80 emulator.

---

## 1. Goals and Scope

### 1.1 Primary Goal

Run S-OS "SWORD" programs on PPAP:

- Load S-OS `.obj` binaries (18-byte `_SOS` header + raw Z80 code).
- Translate S-OS API calls (via `RST 18h` or `CALL 1FFFh`) to PPAP VFS,
  process, and TTY operations.
- Provide the S-OS work area at fixed addresses (1F80h–1FFFh) and the
  standard memory map expected by S-OS programs.
- Reuse the existing ecpu-z80 emulator core — no new CPU emulator needed.
- Integrate with the ptrace/trace subsystem (new ABI tag for S-OS calls).

### 1.2 Background: S-OS "SWORD"

S-OS "SWORD" (Sugano's Working Operating System for Restricted Devices)
was a portable Z80 operating system designed by Isao Sugano and published
through Oh!MZ (later Oh!X) magazine in the mid-1980s.  Its purpose was to
provide a common software platform across the fragmented Japanese 8-bit
computer market:

- **Sharp MZ-80K/C/1200/2000/2200/2500/700** series
- **Sharp X1** series
- **NEC PC-8001/8801** series
- **Fujitsu FM-7/FM-8** series
- **MSX** computers

S-OS achieved portability by defining a fixed API (approximately 30
system calls) for console I/O, cassette/disk file operations, and memory
management.  Each host machine required a machine-specific S-OS "monitor"
(similar to a BIOS) that implemented these calls using the hardware's
native I/O routines.  User programs called only the S-OS API and were
binary-compatible across all supported machines.

The API is intentionally minimal — simpler than CP/M's BDOS — making it
an excellent fit for PPAP's lightweight subsystem model.

### 1.3 Extended Goals

- Run S-OS GAME language programs (a simple BASIC-like game language
  distributed with S-OS).
- Support S-OS disk extensions (beyond the original cassette-based API).
- Run the S-OS line editor and assembler.

### 1.4 Out of Scope

- Machine-specific hardware emulation (MZ CRT controller, X1 PSG, etc.).
- S-OS monitor ROM emulation — we implement the API directly, not the
  monitor.
- Graphics operations — S-OS programs that directly access video RAM are
  machine-specific and cannot be portably emulated.
- Sound/music — no PSG or FM synthesis emulation.

---

## 2. Architecture

### 2.1 Subsystem Registration

Following the existing pattern in `subsys.h`:

```c
/* src/kernel/proc/proc.h */
#define SUBSYS_PPAP      0
#define SUBSYS_HUMAN68K  1
#define SUBSYS_CPM       2
#define SUBSYS_MSDOS     3
#define SUBSYS_SOS       4    /* new */

/* SUBSYS_MAX must be >= 5 */
```

The S-OS subsystem registers its `subsys_ops_t` in `subsys_ops_table[4]`:

```c
const subsys_ops_t sos_subsys_ops = {
    .on_crash  = sos_on_crash,
    .on_signal = sos_on_signal,
    .on_init   = sos_on_init,
};
```

### 2.2 Execution Model

S-OS programs run on the same ecpu-z80 emulator used by the CP/M
subsystem — only the personality layer (trap handler) differs:

```
User: /bin/sh
  │
  │ execve("/subsys/sos/game.obj")
  ▼
Kernel: exec detects "_SOS" magic at offset 0
  │
  │ subsys = SUBSYS_SOS
  ▼
sos_loader.c
  │ Parses 18-byte header (load addr, exec addr, mode)
  │ Allocates 64 KB Z80 memory
  │ Sets up S-OS work area (1F80h–1FFFh)
  │ Installs RST 18h hook + CALL 1FFFh hook
  │ Loads payload at header's load address
  ▼
sos_run_process()
  │
  │ ecpu_z80_ops.run(cpu)  ──→  emulator loop
  │                                  │
  │                                  │ RST 18h → ECPU_TRAP_RST
  │                                  ▼
  │                            sos_bridge.c
  │                                  │ Reads function # from A register
  │                                  │ Translates to PPAP syscalls
  │                                  │ Returns result in A/flags
  │                                  ▼
  │                            emulator continues
  │
  │ #RET (return to monitor) → ECPU_TRAP_EXIT
  ▼
Kernel: process exits, parent wakes up
```

### 2.3 Relationship to CP/M Subsystem

S-OS and CP/M share the ecpu-z80 emulator core but differ in everything
else:

| Aspect | CP/M | S-OS |
|--------|------|------|
| API entry | `CALL 0005h` (BDOS) | `RST 18h` or `CALL 1FFFh` |
| Function dispatch | C register | A register |
| File model | FCB (8.3 names, 128B records) | Cassette-style (name + load/save) |
| Memory map | TPA 0100h–FE00h | User area variable, work area 1F80h–1FFFh |
| Binary format | .COM (headerless, load at 0100h) | .OBJ (`_SOS` header, load/exec addr in header) |
| Personality bridge | `cpm_bridge.c` | `sos_bridge.c` (new) |
| Loader | `cpm_loader.c` (reuse pattern) | `sos_loader.c` (new) |

---

## 3. S-OS Memory Map (Emulated)

### 3.1 Standard Layout

```
Z80 eCPU address space (64 KB)
┌──────────────────────────────────┐ 0xFFFF
│  (unused / stack grows down)     │
├──────────────────────────────────┤
│  User program area               │ load addr – end addr
│  (load address from _SOS header) │ (typically 3000h+)
├──────────────────────────────────┤
│  S-OS Work Area                  │ 0x1F00 – 0x1FFF
│  ┌───────────────────────┐      │
│  │ 1F80h: FNAM (filename) │      │
│  │ 1F90h: FTYPE, DTADR   │      │
│  │ 1FFBh: Entry (JP xxxx) │      │
│  │ 1FFEh: Cold start      │      │
│  └───────────────────────┘      │
├──────────────────────────────────┤
│  Stack area                      │ 0x0600 – 0x07FF
├──────────────────────────────────┤
│  FAT / sector buffer             │ 0x0300 – 0x05FF
├──────────────────────────────────┤
│  Keyboard input buffer           │ 0x0200 – 0x02FF
├──────────────────────────────────┤
│  S-OS input buffer               │ 0x0180 – 0x019F
│  Cursor position                 │ 0x01A0 – 0x01A1
├──────────────────────────────────┤
│  S-OS subroutine hooks           │ 0x0100 – 0x017F
├──────────────────────────────────┤
│  RST vectors / boot code         │ 0x0000 – 0x00FF
│  RST 18h at 0x0018: JP handler  │
│  RST 00h at 0x0000: warm boot   │
└──────────────────────────────────┘
```

### 3.2 Work Area (1F80h–1FFFh)

The S-OS work area contains system variables and the API jump table.
The bridge initializes this area at load time:

```c
#define SOS_WORK_BASE    0x1F80
#define SOS_FNAM         0x1F80  /* Filename buffer (16 bytes) */
#define SOS_FTYPE        0x1F90  /* File type byte */
#define SOS_DTADR        0x1F91  /* Data address (load/save target) */
#define SOS_EDADR        0x1F93  /* End address */
#define SOS_EXADR        0x1F95  /* Execution address */
#define SOS_SESSION      0x1F97  /* Current file session (A: B: etc.) */
#define SOS_DESSION      0x1F98  /* Default session */
#define SOS_MAXLIN       0x1F99  /* Screen max lines */
#define SOS_MAXCOL       0x1F9B  /* Screen max columns */
#define SOS_ENTRY        0x1FFB  /* JP xxxx (API alternate entry) */
#define SOS_COLD         0x1FFE  /* Cold start entry */
```

---

## 4. S-OS API Bridge

### 4.1 Dispatch

S-OS programs invoke system calls via `RST 18h` with A=function number.
Some calls also accept parameters in HL, DE, BC, or the work area.

The ecpu-z80 traps `RST 18h` as an `ECPU_TRAP_RST` with `param = 0x18`.
The bridge dispatches based on the A register:

```c
int sos_trap_handler(ecpu_state_t *cpu, int trap_type,
                     uint32_t param, void *ctx)
{
    sos_proc_t *sos = (sos_proc_t *)ctx;
    z80_state_t *z80 = (z80_state_t *)cpu;

    if (trap_type == ECPU_TRAP_RST && param == 0x18) {
        return sos_dispatch(z80, sos);
    }
    if (trap_type == ECPU_TRAP_CALL &&
        param == SOS_ENTRY) {
        return sos_dispatch(z80, sos);
    }
    if (trap_type == ECPU_TRAP_HALT)
        return ECPU_TRAP_EXIT;

    return ECPU_TRAP_UNHANDLED;
}

int sos_dispatch(z80_state_t *z80, sos_proc_t *sos)
{
    uint8_t fn = z80->a;
    switch (fn) {
    case 0x00: return sos_cold(z80, sos);       /* #COLD */
    case 0x01: return sos_hot(z80, sos);        /* #HOT  */
    case 0x02: return sos_ver(z80, sos);        /* #VER  */
    case 0x03: return sos_print(z80, sos);      /* #PRINT */
    case 0x04: return sos_prnmsg(z80, sos);     /* #MSG  */
    case 0x05: return sos_lprint(z80, sos);     /* #MSX  */
    case 0x06: return sos_tab(z80, sos);        /* #TAB  */
    case 0x07: return sos_cr(z80, sos);         /* #CR   */
    case 0x08: return sos_lf(z80, sos);         /* #LF   */
    case 0x09: return sos_getl(z80, sos);       /* #GETL */
    case 0x0A: return sos_getky(z80, sos);      /* #GETKY */
    case 0x0B: return sos_brkey(z80, sos);      /* #BRKEY */
    case 0x0C: return sos_inkey(z80, sos);      /* #INKEY */
    case 0x0D: return sos_pause(z80, sos);      /* #PAUSE */
    case 0x0E: return sos_bell(z80, sos);       /* #BELL */
    case 0x0F: return sos_prthx(z80, sos);      /* #PRTHX */
    case 0x10: return sos_prthl(z80, sos);      /* #PRTHL */
    case 0x11: return sos_arone(z80, sos);      /* #ASC1B */
    case 0x12: return sos_artwo(z80, sos);      /* #HEX1B */
    case 0x13: return sos_arhex(z80, sos);      /* #HEX2B */
    case 0x14: return sos_flget(z80, sos);      /* #FLGET */
    case 0x15: return sos_rdvsw(z80, sos);      /* #RDVSW */
    case 0x16: return sos_sdvsw(z80, sos);      /* #SDVSW */
    case 0x17: return sos_inp(z80, sos);        /* #INP  */
    case 0x18: return sos_out(z80, sos);        /* #OUT  */
    case 0x19: return sos_widch(z80, sos);      /* #WIDCH */
    case 0x1A: return sos_file(z80, sos);       /* #FILE */
    case 0x1B: return sos_fsave(z80, sos);      /* #FSAVE */
    case 0x1C: return sos_fload(z80, sos);      /* #FLOAD */
    case 0x1D: return sos_fvrfy(z80, sos);      /* #FVRFY */
    case 0x1E: return sos_fkill(z80, sos);      /* #FKILL */
    case 0x1F: return sos_fren(z80, sos);       /* #FREN  */
    default:
        /* Unknown function — return with carry set (error) */
        z80->f |= Z80_CF;
        return ECPU_TRAP_HANDLED;
    }
}
```

### 4.2 Function Coverage

#### Console Output

| Code | Name | S-OS Semantics | PPAP Mapping |
|------|------|----------------|--------------|
| 00h | #COLD | Cold start (return to monitor) | `sys_exit(0)` |
| 01h | #HOT | Warm start (soft reset) | `sys_exit(0)` |
| 02h | #VER | Print S-OS version | `sys_write(1, "S-OS SWORD\r\n")` |
| 03h | #PRINT | Print character in A | `sys_write(1, &a, 1)` |
| 04h | #MSG | Print NUL-terminated string at DE | `sys_write(1, buf, len)` |
| 05h | #MSX | Print to printer (line printer) | `sys_write(1, &a, 1)` (redirect to stdout) |
| 06h | #TAB | Move cursor to column in A | ANSI escape: `\033[<col>G` |
| 07h | #CR | Output carriage return | `sys_write(1, "\r", 1)` |
| 08h | #LF | Output line feed | `sys_write(1, "\n", 1)` |
| 0Eh | #BELL | Sound bell | `sys_write(1, "\a", 1)` |
| 0Fh | #PRTHX | Print A as 2-digit hex | Format + `sys_write(1)` |
| 10h | #PRTHL | Print HL as 4-digit hex | Format + `sys_write(1)` |

#### Console Input

| Code | Name | S-OS Semantics | PPAP Mapping |
|------|------|----------------|--------------|
| 09h | #GETL | Line input to buffer at DE, max B chars | `sys_read(0, buf, max)` with line editing |
| 0Ah | #GETKY | Wait for keypress, return in A | `sys_read(0, &ch, 1)` (blocking) |
| 0Bh | #BRKEY | Check for break key | `sys_poll(0)` — check for Ctrl-C |
| 0Ch | #INKEY | Non-blocking key check | `sys_read(0, &ch, 1)` non-blocking; A=0 if no key |
| 0Dh | #PAUSE | Wait for any keypress | `sys_read(0, &ch, 1)` (blocking, discard) |

#### Numeric Conversion

| Code | Name | S-OS Semantics | PPAP Mapping |
|------|------|----------------|--------------|
| 11h | #ASC1B | ASCII char → 1-byte value | Pure computation (in-emulator) |
| 12h | #HEX1B | Hex char → 1-byte value | Pure computation |
| 13h | #HEX2B | 2 hex chars → byte value | Pure computation |

#### I/O Port Access (Stubs)

| Code | Name | S-OS Semantics | PPAP Mapping |
|------|------|----------------|--------------|
| 14h | #FLGET | Get character from tape | Stub: return error (carry set) |
| 15h | #RDVSW | Read DIP switch | Return 0 |
| 16h | #SDVSW | Set DIP switch | No-op |
| 17h | #INP | Input from I/O port A | Stub: return 0 |
| 18h | #OUT | Output B to I/O port A | Stub: no-op |
| 19h | #WIDCH | Set screen width | Update `sos->maxcol` |

#### File Operations

| Code | Name | S-OS Semantics | PPAP Mapping |
|------|------|----------------|--------------|
| 1Ah | #FILE | Directory listing | `sys_opendir()` + `sys_readdir()` → formatted output |
| 1Bh | #FSAVE | Save memory to file | `sys_open(O_CREAT)` + `sys_write()` (from DTADR to EDADR) |
| 1Ch | #FLOAD | Load file to memory | `sys_open(O_RDONLY)` + `sys_read()` (to DTADR) |
| 1Dh | #FVRFY | Verify file against memory | `sys_open()` + `sys_read()` + `memcmp()` |
| 1Eh | #FKILL | Delete file | `sys_unlink()` |
| 1Fh | #FREN | Rename file | `sys_rename()` |

### 4.3 File Name Translation

S-OS filenames are simple: up to 16 characters, stored in the FNAM work
area (1F80h).  The "session" (drive letter, 1F97h) selects a directory:

```
Session A, filename "GAME1"  →  /a/GAME1
Session B, filename "DATA"   →  /b/DATA
```

Unlike CP/M's FCB model, S-OS filenames are plain strings — no 8.3 split,
no wildcards (except in #FILE directory listing).  This makes path
translation trivial:

```c
int sos_resolve_path(sos_proc_t *sos, z80_state_t *z80,
                     char *ppap_path, int ppap_path_size)
{
    /* Read filename from FNAM work area */
    char fnam[17];
    for (int i = 0; i < 16; i++) {
        fnam[i] = z80->memory[SOS_FNAM + i];
        if (fnam[i] == '\0' || fnam[i] == ' ') {
            fnam[i] = '\0';
            break;
        }
    }
    fnam[16] = '\0';

    /* Map session to directory */
    char drive = 'a' + sos->current_session;
    snprintf(ppap_path, ppap_path_size, "/%c/%s", drive, fnam);
    return 0;
}
```

### 4.4 Error Handling

S-OS uses the Z80 carry flag (CF) to signal errors:

- **CF=0**: success
- **CF=1**: error (A register may contain an error code)

The bridge maps PPAP errno to the S-OS error convention:

```c
static void sos_set_error(z80_state_t *z80, int ppap_errno)
{
    if (ppap_errno < 0) {
        z80->f |= Z80_CF;      /* set carry = error */
        z80->a = (uint8_t)(-ppap_errno);
    } else {
        z80->f &= ~Z80_CF;     /* clear carry = success */
    }
}
```

---

## 5. Binary Format and Detection

### 5.1 Binary Format

S-OS `.obj` files have an 18-byte ASCII header followed by the raw Z80
binary payload:

```
Offset  Size  Content
──────  ────  ───────────────────────────────────
 +0      4    Magic: "_SOS" (0x5F 0x53 0x4F 0x53)
 +4      1    Space (0x20)
 +5      2    File mode in hex ASCII:
              "01" = binary (machine code)
              "04" = ASCII (text / source)
 +7      1    Space (0x20)
 +8      4    Load address in hex ASCII (e.g. "3000")
+12      1    Space (0x20)
+13      4    Execution address in hex ASCII (e.g. "3000")
+17      1    Terminator: LF (0x0A)
──────  ────  ───────────────────────────────────
+18      N    Payload (raw Z80 machine code for mode 01)
```

Example header (hex dump):
```
5F 53 4F 53 20 30 31 20 33 30 30 30 20 33 30 30 30 0A
 _  S  O  S     0  1     3  0  0  0     3  0  0  0  \n
```

This means: binary file, load at 0x3000, execute at 0x3000.

The header is entirely ASCII text, making it easy to inspect with
standard tools.  The load address tells the loader where in Z80 memory
to place the payload; the execution address is the entry point (often
the same as load address).

For mode "04" (ASCII), the payload is text data (e.g., GAME language
source).  The loader stores it at the load address but does not execute
it directly — the GAME interpreter loads it via #FLOAD.

### 5.2 Header Parsing

```c
#define SOS_MAGIC  "_SOS"
#define SOS_HEADER_SIZE  18

typedef struct sos_header {
    uint16_t load_addr;     /* parsed from hex ASCII at +8 */
    uint16_t exec_addr;     /* parsed from hex ASCII at +13 */
    uint8_t  file_mode;     /* 0x01 = binary, 0x04 = ASCII */
} sos_header_t;

int sos_parse_header(const uint8_t *file, uint32_t size,
                     sos_header_t *hdr)
{
    if (size < SOS_HEADER_SIZE)
        return -1;
    if (memcmp(file, SOS_MAGIC, 4) != 0)
        return -1;
    if (file[4] != ' ' || file[7] != ' ' || file[12] != ' ')
        return -1;
    if (file[17] != 0x0A)
        return -1;

    hdr->file_mode = hex2byte(file + 5);     /* "01" or "04" */
    hdr->load_addr = hex4_to_u16(file + 8);  /* e.g. "3000" */
    hdr->exec_addr = hex4_to_u16(file + 13); /* e.g. "3000" */
    return 0;
}
```

### 5.3 Detection

S-OS binaries are detected by the `_SOS` magic at offset 0:

```
1. First 4 bytes = "_SOS" (0x5F 0x53 0x4F 0x53)
   AND byte 17 = 0x0A
   → S-OS subsystem

2. Path under /subsys/sos/ (fallback for headerless files)
   → S-OS subsystem with default load/exec at 2000h

3. Otherwise → not S-OS
```

Unlike CP/M `.COM` files (which are headerless), S-OS `.obj` files have
a clear magic signature, enabling reliable content-based detection
without relying solely on path conventions.

---

## 6. Per-Process State

```c
typedef struct sos_proc {
    /* Work area mirror (kept in sync with Z80 memory) */
    uint8_t  current_session;     /* Current file session (drive) */
    uint8_t  default_session;     /* Default session */
    uint16_t dtadr;               /* Data (load) address */
    uint16_t edadr;               /* End address */
    uint16_t exadr;               /* Execution address */
    uint8_t  maxlin;              /* Screen lines */
    uint8_t  maxcol;              /* Screen columns */

    /* File state (simple: S-OS has no file handles) */
    /* S-OS file ops are atomic: open-do-close in one call */

    /* Input line buffer for #GETL */
    char     linebuf[256];
    int      linebuf_len;
} sos_proc_t;
```

Note that S-OS has no open file handle table — file operations (#FSAVE,
#FLOAD, #FKILL) are self-contained: the bridge opens, performs the
operation, and closes in a single call.  This is much simpler than CP/M's
FCB table or DOS's handle table.

---

## 7. Trace Integration

### 7.1 ABI Tag

```c
#define PPAP_TRACE_ABI_SOS  6   /* in ptrace.h */
```

### 7.2 Event Generation

```c
/* At RST 18h entry */
trace_subsys_event(PPAP_TRACE_EVENT_SUBSYS_ENTER,
                   PPAP_TRACE_ABI_SOS,
                   z80->a,             /* function number */
                   args);             /* DE, HL, BC, work area */

/* At RST 18h exit */
trace_subsys_event(PPAP_TRACE_EVENT_SUBSYS_EXIT,
                   PPAP_TRACE_ABI_SOS,
                   z80->a,
                   &ret);             /* A, flags */
```

### 7.3 Trace Output

```
$ trace --subsys game.obj
[subsys:sos] RST18 A=02 #VER
[subsys:sos] RST18 A=04 #MSG DE=2050 "HELLO WORLD"
[subsys:sos] RST18 A=0A #GETKY → A=41 'A'
[subsys:sos] RST18 A=1C #FLOAD FNAM="DATA" DTADR=3000 EDADR=3FFF → OK
[subsys:sos] RST18 A=00 #COLD → exit(0)
```

---

## 8. New Files

```
src/kernel/subsys/
  sos_bridge.c        — RST 18h dispatch and function implementations
  sos_bridge.h        — sos_proc_t, constants, API names

src/kernel/exec/
  exec_sos.c          — exec path for S-OS binary detection and dispatch
```

Changes to existing files:

| File | Change |
|------|--------|
| `src/kernel/proc/proc.h` | Add `SUBSYS_SOS = 4` |
| `src/kernel/subsys/subsys.c` | Register `sos_subsys_ops` in slot 4 |
| `src/kernel/exec/exec.c` | Add `_SOS` magic detection in exec path |
| `src/common/ptrace.h` | Add `PPAP_TRACE_ABI_SOS` |
| `src/user/trace.c` | Add S-OS function name decoder |
| `config.h` | Add `PPAP_ENABLE_SOS` build flag |

---

## 9. Build Configuration

```cmake
option(PPAP_ENABLE_SOS "Enable S-OS SWORD subsystem" OFF)

if(PPAP_ENABLE_SOS)
    target_sources(ppap PRIVATE
        src/kernel/subsys/sos_bridge.c
        src/kernel/exec/exec_sos.c
    )
    target_compile_definitions(ppap PRIVATE PPAP_ENABLE_SOS=1)
endif()
```

The S-OS subsystem requires `PPAP_ENABLE_ECPU_Z80` (shared with CP/M).

---

## 10. Implementation Phases

### Phase S-1: Loader and Console I/O

**Goal**: "Hello, world" S-OS program runs and prints output.

1. Implement `exec_sos.c` — `_SOS` magic detection, header parsing,
   payload loading at header-specified address.
2. Implement `sos_bridge.c` — #COLD (00h), #PRINT (03h), #MSG (04h),
   #CR (07h), #LF (08h).
3. Initialize work area (1F80h–1FFFh) and RST 18h hook.
4. Test with a hand-assembled S-OS hello-world program.

**Verification**: "HELLO WORLD" appears on PPAP console.

### Phase S-2: Console Input

**Goal**: Interactive S-OS programs work.

1. #GETKY (0Ah), #INKEY (0Ch), #BRKEY (0Bh), #PAUSE (0Dh).
2. #GETL (09h) — line input with buffer.
3. #BELL (0Eh), #TAB (06h).
4. Numeric display: #PRTHX (0Fh), #PRTHL (10h).

**Verification**: S-OS monitor-like prompt reads and echoes input.

### Phase S-3: File Operations

**Goal**: S-OS programs can save and load files.

1. #FLOAD (1Ch) — load file to memory at DTADR.
2. #FSAVE (1Bh) — save memory range to file.
3. #FKILL (1Eh) — delete file.
4. #FREN (1Fh) — rename file.
5. #FILE (1Ah) — directory listing.
6. #FVRFY (1Dh) — verify file against memory.
7. Path translation (session/filename → PPAP path).

**Verification**: An S-OS program saves data, reloads it, and verifies
the contents match.

### Phase S-4: Numeric Conversion and Misc

**Goal**: Remaining API functions work.

1. #ASC1B (11h), #HEX1B (12h), #HEX2B (13h) — pure computation.
2. #VER (02h), #MSX (05h), #WIDCH (19h).
3. I/O port stubs: #FLGET (14h), #RDVSW (15h), #SDVSW (16h),
   #INP (17h), #OUT (18h).

**Verification**: All 32 S-OS functions return reasonable results.

### Phase S-5: Trace Integration

**Goal**: `trace --subsys` and `pdb` work with S-OS programs.

1. Add `PPAP_TRACE_ABI_SOS` events.
2. S-OS function name decoder in trace tool.
3. Z80 register display (shared with CP/M).

**Verification**: `trace --subsys game.obj` shows decoded S-OS calls.

---

## 11. Testing

### 11.1 Test Programs

Ship minimal S-OS test programs in `tests/sos/`:

```
tests/sos/
  hello.obj         — #MSG + #COLD (print and exit)
  echo.obj          — #GETKY + #PRINT loop
  fileio.obj        — #FSAVE + #FLOAD + #FVRFY
  hexdump.obj       — #PRTHX + #PRTHL test
```

These are assembled from Z80 source and included as binary blobs,
following the same pattern as the CP/M test suite.

### 11.2 Integration Test

A `test_sos` test case exercises the bridge via the PPAP test runner:

```c
static int test_sos(void) {
    /* Spawn hello.obj via execve, verify output */
    /* Spawn fileio.obj, verify file created and read back */
    return 0;
}
```

This follows the `test_cpm` pattern (guarded by `#if defined(PPAP_ENABLE_SOS)`).

---

## 12. Risks and Open Questions

### 12.1 Binary Format

S-OS `.obj` files have a clear `_SOS` magic signature in the header,
so content-based detection is reliable.  The 18-byte header is ASCII
text with hex-encoded addresses, which is unusual but straightforward
to parse.  The only edge case is headerless S-OS binaries (rare) —
these fall back to path-based detection under `/subsys/sos/`.

### 12.2 Address Space Conflicts with CP/M

Both CP/M and S-OS use ecpu-z80 with a 64 KB address space, but their
memory maps differ (CP/M TPA starts at 0100h; S-OS user area typically
starts at 2000h).  Each subsystem creates its own `z80_state_t` instance,
so there is no conflict — they are separate processes with separate
emulated memory.

### 12.3 S-OS Disk Extensions

The original S-OS was cassette-based.  Later extensions added disk I/O
with additional function codes beyond 1Fh (e.g., sector read/write,
disk format).  These vary between S-OS implementations and are not
standardized.  The initial implementation covers the standard 00h–1Fh
function set.  Disk extensions can be added later if needed.

### 12.4 GAME Language Runtime

S-OS included "GAME" — a simple BASIC-like language for writing games.
GAME programs are not Z80 machine code; they are interpreted by the
GAME runtime (itself a Z80 binary).  Supporting GAME requires:

1. Shipping the GAME interpreter as `/subsys/sos/GAME.obj`.
2. Ensuring #FLOAD works correctly (GAME loads source files via S-OS).
3. No additional bridge work — GAME uses only standard S-OS calls.

### 12.5 Code Size Estimate

The S-OS bridge is the simplest personality layer in PPAP:

| Component | Estimated Size |
|-----------|---------------|
| `sos_bridge.c` | ~400 lines (~1.5 KB binary) |
| `exec_sos.c` | ~80 lines (~0.3 KB binary) |
| Test programs | ~200 lines Z80 assembly |
| **Total new code** | **~480 lines C + ~200 lines asm** |

The ecpu-z80 emulator (~3500 lines, ~14 KB binary) is shared with CP/M
and incurs no additional cost if CP/M is already enabled.

---

## 13. Dependency Graph

```
ecpu-z80 (already implemented for CP/M)
  └─→ S-1 (loader + console output)
        └─→ S-2 (console input)
        └─→ S-3 (file operations)
              └─→ S-4 (misc + stubs)
                    └─→ S-5 (trace integration)
```

No new eCPU core is needed.  If `PPAP_ENABLE_CPM` is already active, the
Z80 emulator is already compiled in.  If only `PPAP_ENABLE_SOS` is set,
the Z80 emulator is pulled in by the S-OS dependency.

---

## 14. Related Documentation

- [docs/ecpu/z80.md](../ecpu/z80.md) — Z80 eCPU emulator core design
- [docs/subsystems/cpm.md](../subsystems/cpm.md) — CP/M subsystem (shares ecpu-z80)
- [docs/subsystems/overview.md](../subsystems/overview.md) — Subsystem architecture
- [docs/kernel/syscall.md](../kernel/syscall.md) — System call reference
- [docs/kernel/trace.md](../kernel/trace.md) — Trace and debug subsystem
