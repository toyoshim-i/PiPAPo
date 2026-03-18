# S-OS SWORD Subsystem

Load and execute S-OS "SWORD" programs on PPAP, bridging S-OS monitor
calls to PPAP's native syscall interface via the ecpu-z80 emulator core.

---

## 1. Background

S-OS "SWORD" (Sugano's Working Operating System for Restricted Devices)
was a portable Z80 operating system designed by Isao Sugano and published
through *Oh!MZ* (later *Oh!X*) magazine in the mid-1980s. Its purpose was
to provide a common software platform across the fragmented Japanese 8-bit
computer market:

- **Sharp MZ-80K/C/1200/2000/2200/2500/700** series
- **Sharp X1** series
- **NEC PC-8001/8801** series
- **Fujitsu FM-7/FM-8** series
- **MSX** computers

S-OS achieved portability by defining a fixed API (approximately 30
system calls) for console I/O, cassette/disk file operations, and memory
management. Each host machine required a machine-specific S-OS "monitor"
(similar to a BIOS) that implemented these calls using the hardware's
native I/O routines. User programs called only the S-OS API and were
binary-compatible across all supported machines.

The API is intentionally minimal — simpler than CP/M's BDOS — making it
an excellent fit for PPAP's lightweight subsystem model.

---

## 2. Binary Format

S-OS `.obj` files use a text-based 18-byte header followed by raw Z80
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
 +8      4    Load address in hex ASCII (e.g. "8000")
+12      1    Space (0x20)
+13      4    Execution address in hex ASCII (e.g. "8000")
+17      1    Terminator: LF (0x0A)
──────  ────  ───────────────────────────────────
+18      N    Payload (raw Z80 machine code for mode 01)
```

Example header (hex dump):
```
5F 53 4F 53 20 30 31 20 38 30 30 30 20 38 30 30 30 0A
 _  S  O  S     0  1     8  0  0  0     8  0  0  0  \n
```

Detection: `_SOS` magic at offset 0, valid hex fields, byte 17 = 0x0A.

For mode "04" (ASCII), the payload is text data (e.g., GAME language
source). The loader stores it at the load address but does not execute
it directly — the GAME interpreter loads it via #FLOAD.

---

## 3. Memory Map (Emulated)

```
Z80 eCPU address space (64 KB)
┌──────────────────────────────────┐ 0xFFFF
│  (unused / stack grows down)     │
├──────────────────────────────────┤
│  User program area               │ load addr – end
│  (load address from header)      │ (typically 8000h+)
├──────────────────────────────────┤
│  Extended API JP table           │ 0x2000 – 0x2036
├──────────────────────────────────┤
│  Monitor JP table                │ 0x1F80 – 0x1FFD
├──────────────────────────────────┤
│  S-OS Work Area                  │ 0x1F40 – 0x1F7F
├──────────────────────────────────┤
│  Stack (grows down from 0800h)   │
├──────────────────────────────────┤
│  RST stub area (RST 0; RET ×59) │ 0x0100 – 0x0175
├──────────────────────────────────┤
│  RST vectors (00h, 18h)          │ 0x0000 – 0x001A
└──────────────────────────────────┘
```

Programs are loaded at the header-specified load address and execution
begins at the specified exec address.

### Work Area (1F80h–1FFFh)

| Address | Name | Purpose |
|---------|------|---------|
| 1F80h | FNAM | Filename buffer (16 bytes) |
| 1F90h | FTYPE | File type byte |
| 1F91h | DTADR | Data (load) address |
| 1F93h | EDADR | End address |
| 1F95h | EXADR | Execution address |
| 1F97h | SESSION | Current file session (drive) |
| 1F98h | DESSION | Default session |
| 1F99h | MAXLIN | Screen max lines |
| 1F9Bh | MAXCOL | Screen max columns |
| 1FFBh | ENTRY | JP xxxx (API alternate entry) |
| 1FFEh | COLD | Cold start entry |

---

## 4. Execution Model

S-OS programs run on the same ecpu-z80 emulator used by the CP/M
subsystem — only the personality layer (trap handler) differs:

```
User: /bin/sh
  │
  │ execve("/subsys/sos/game.obj")
  ▼
Kernel: exec detects "_SOS" magic at offset 0
  │
  ▼
exec_sos.c
  │ Parses 18-byte header (load addr, exec addr, mode)
  │ Allocates 64 KB Z80 memory
  │ Sets up S-OS work area (1F40h–1F7Fh)
  │ Installs monitor JP table (0x1F80–0x1FFD → stub area)
  │ Installs RST stub area at 0x0100 (RST 0 + RET per fn)
  │ Loads payload at header's load address
  ▼
sos_run_process()
  │
  │ ecpu_z80_ops.run(cpu)  ──→  emulator loop
  │                                  │
  │                                  │ CALL/JP to monitor entry
  │                                  │   → JP to stub area
  │                                  │   → RST 0 → ECPU_TRAP_RST
  │                                  ▼
  │                            sos_bridge.c
  │                                  │ Computes fn from RST address
  │                                  │ Translates to PPAP syscalls
  │                                  │ Returns result in registers/flags
  │                                  ▼
  │                            emulator continues
  │
  │ #COLD → exit
  ▼
Kernel: process exits, parent wakes up
```

### Relationship to CP/M Subsystem

| Aspect | CP/M | S-OS |
|--------|------|------|
| API entry | `CALL 0005h` (BDOS) | `CALL`/`JP` to monitor table (0x1F80–0x1FFD) |
| Function dispatch | C register | Function number derived from entry address |
| File model | FCB (8.3 names, 128B records) | Session + filename (atomic open-do-close) |
| Memory map | TPA 0100h–FE00h | User area variable, work area 1F80h–1FFFh |
| Binary format | .COM (headerless, load at 0100h) | .obj (`_SOS` header, load/exec addr in header) |
| Error signaling | A register | Z80 carry flag (CF=1 = error) |

---

## 5. Monitor API Bridge

S-OS programs call monitor functions via `CALL` or `JP` to entry points
in the monitor table (addresses 0x1F80–0x1FFD, counting downward by 3
from 0x1FFD).

**Trap mechanism:** Each monitor entry contains a `JP` instruction that
redirects to an internal RST stub area at 0x0100.  Each stub is two
bytes: `RST 0; RET`.  When `RST 0` executes, the ecpu-z80 emulator
fires `ECPU_TRAP_RST` with param 0x0000.  The trap handler computes
the function index from the RST instruction's address:

```
fn = (cpu->pc - 1 - 0x0100) / 2
```

This design means only `RST` instructions trigger the trap — regular
`CALL` instructions in user code pass through without overhead.  Both
`CALL` and `JP` to a monitor entry work identically: the CPU executes
the `JP` at the entry, reaches the stub, and `RST 0` fires the trap.

Programs may also use `RST 18h` with the function number in the A
register (an alternative calling convention).

### Console Output

| Fn | Name | Description | PPAP translation |
|---|---|---|---|
| 03 | #GETL | Line input (HL=buf, B=max) | `read(0, buf, max)` |
| 06 | #1CHR | Read single char | `read(0, &ch, 1)` |
| 09 | #MSG | Print string (DE=addr, '$'-terminated) | `write(1, str, len)` |
| 0C | #MSX | Print string (DE=addr, 0x0D-terminated) | `write(1, str, len)` |
| 0F | #MPRINT | Print inline string after CALL | `write(1, str, len)` |
| 12 | #TAB | Move cursor to column (A=col) | VT100 cursor movement |
| 18 | #LTNL | Print newline + line number | `write(1, "\r\n", 2)` |
| 1B | #NL | Print newline | `write(1, "\r\n", 2)` |
| 1E | #PRINT | Print character (A=char) | `write(1, &ch, 1)` |
| 21 | #PRINTS | Print character (raw) | `write(1, &ch, 1)` |

### Console Input

| Fn | Name | Description | PPAP translation |
|---|---|---|---|
| 24 | #GETKY | Wait for keypress | `read(0, &ch, 1)` |
| 27 | #BRKEY | Check break key | `poll(stdin, POLLIN, 0)` |
| 2A | #INKEY | Non-blocking key check | `poll()` + `read()` |
| 2D | #PAUSE | Wait for keypress (like GETKY) | `read(0, &ch, 1)` |
| 30 | #BELL | Ring bell | `write(1, "\a", 1)` |

### Numeric I/O

| Fn | Name | Description |
|---|---|---|
| 33 | #PRTHX | Print A register as 1-digit hex |
| 36 | #PRTHL | Print HL register as 4-digit hex |
| 39 | #ASC | Convert ASCII digit to binary |
| 3C | #HEX | Convert hex digit to binary |
| 3F | #2HEX | Parse 2-digit hex string to byte |
| 42 | #HLHEX | Parse 4-digit hex string to word |

### File Operations

| Fn | Name | Description | PPAP translation |
|---|---|---|---|
| 45 | #WOPEN | Open file for writing (DE=filename) | `open(path, O_WRONLY|O_CREAT)` |
| 48 | #WRD | Write data (HL=addr, BC=len) | `write(fd, buf, len)` |
| 4B | #ROPEN | Open file for reading | `open(path, O_RDONLY)` |
| 4E | #RDD | Read data (HL=addr, BC=len) | `read(fd, buf, len)` |
| 51 | #FCB | File control block operations | — |
| 54 | #FILE | File name query | — |
| 57 | #FSAME | File attribute query | — |
| 5A | #FPRNT | Print file information | — |

### Screen and System

| Fn | Name | Description | PPAP translation |
|---|---|---|---|
| 56 | #WIDCH | Screen mode change | No-op (preserves user's mode) |
| 59 | #SCRN | Read screen char at (H,L) | Read from in-memory screen buffer |
| 5C | #LOC | Set cursor position (H=col,L=row) | VT100 `ESC[row;colH` |
| 5F | #CSR | Query cursor position | Return current row/col |
| 00 | #COLD | Cold start / exit | `_exit(0)` |

### Extended API (0x2000+)

| Fn | Name | Description | PPAP translation |
|---|---|---|---|
| ext 00 | #NAME | Rename file | `rename()` |
| ext 03 | #KILL | Delete file | `unlink()` |
| ext 06 | #DIR | List directory | `opendir()` + `readdir()` |
| ext 09 | #MON | Enter monitor (exit) | `_exit(0)` |

### I/O Port Stubs

| Fn | Name | Description | PPAP translation |
|---|---|---|---|
| 4D | #POKE | Write byte to address | Direct memory write |
| 50 | #POKEA | Write block to address | Direct memory write |
| 53 | #PEEK | Read byte from address | Direct memory read |
| 56 | #PEEKA | Read block from address | Direct memory read |
| — | #LPRINT | Print to line printer | Redirected to stdout |

### Error Handling

S-OS uses the Z80 carry flag (CF) to signal errors:
- **CF=0**: success
- **CF=1**: error (A register may contain an error code)

---

## 6. Screen Buffer

S-OS programs can read screen contents via `#SCRN`. The bridge maintains
an 80×25 in-memory screen buffer that tracks all character output and
cursor movements. This buffer is updated by console output functions
and read by `#SCRN`.

## 7. TTY Configuration

S-OS programs run with the TTY in raw mode (no line buffering, no echo)
to match the S-OS console model where programs handle input character by
character. The terminal screen is cleared on program startup.

The `#WIDCH` (screen width change) API is intentionally a no-op so that
the user's current screen mode (40×20, 80×40, or 40×40 on PicoCalc) is
preserved across S-OS program execution.

## 8. Trace Integration

S-OS API calls are visible via `trace --subsys`:

```
$ trace --subsys game.obj
[subsys:sos] #VER
[subsys:sos] #MSG DE=2050 "HELLO WORLD"
[subsys:sos] #GETKY → A=41 'A'
[subsys:sos] #COLD → exit(0)
```

The trace ABI tag is `PPAP_TRACE_ABI_SOS` (defined in `ptrace.h`).

## 9. Source Files

| File | Purpose |
|------|---------|
| `src/kernel/subsys/sos_bridge.c` | Monitor call bridge (personality layer) |
| `src/kernel/subsys/sos_bridge.h` | Header, SOS header parser, constants |
| `src/kernel/subsys/exec_sos.c` | Binary loader + execution setup |

## 10. Usage

S-OS programs are placed under `/subsys/sos/` in romfs. The directory
is included in the default PATH, so programs can be run by name:

```sh
$ game.obj        # runs /subsys/sos/game.obj
```

The subsystem is enabled per-target via `ENABLE_SUBSYS_SOS` in CMake.
The S-OS subsystem requires `ENABLE_ECPU_Z80` (shared with CP/M).
If CP/M is already enabled, the Z80 emulator is already compiled in
and incurs no additional cost.

## 11. Related Documentation

- [ecpu/z80.md](../ecpu/z80.md) — Z80 eCPU emulator core design
- [subsystems/cpm.md](cpm.md) — CP/M subsystem (shares ecpu-z80)
- [subsystems/overview.md](overview.md) — Subsystem architecture
