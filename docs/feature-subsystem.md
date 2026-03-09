# Subsystems — Binary Loaders and OS Personality Layers

Every foreign binary execution in PPAP is a **subsystem**: an eCPU
emulator core (`feature-eCPU.md`) paired with a binary loader and an
OS personality layer that gives meaning to the emulated traps/syscalls.

---

## 1. Overview

eCPU provides the raw CPU emulation — fetch, decode, execute. When the
emulated program hits a trap or syscall instruction, eCPU fires a
callback. The **subsystem personality** handles that callback: it
decides what the trap means and translates it into PPAP syscalls.

This applies to **all** foreign binary execution, including running
PPAP binaries cross-architecture. Each subsystem combines three parts:

1. **Binary loader** — recognises the executable format and loads it
   into emulated memory (ELF loader, .COM loader, X-format loader, etc.)
2. **CPU emulator** — eCPU core that interprets the foreign ISA
3. **OS personality** — intercepts traps and translates them to PPAP
   syscalls

```
+----------------------------------------------------------+
|  Foreign binary                                          |
|  (PPAP ELF, CP/M .COM, DOS .EXE, Human68k .X, etc.)     |
+---------------------------+------------------------------+
                            |
+---------------------------v------------------------------+
|  Subsystem personality layer                             |
|  (PPAP ABI remap / CP/M BDOS / DOS INT 21h / etc.)      |
|  Intercepts traps → translates → PPAP syscalls           |
+---------------------------+------------------------------+
                            |
+---------------------------v------------------------------+
|  eCPU emulator core (feature-eCPU.md)                    |
|  Interprets foreign ISA instructions                     |
+---------------------------+------------------------------+
                            |
+---------------------------v------------------------------+
|  PPAP kernel (native)                                    |
|  Handles syscalls normally                               |
+----------------------------------------------------------+
```

### 1.1 Subsystem Spectrum

All subsystems use the same architecture. They differ only in the
complexity of the personality layer:

| Subsystem | Personality complexity | What it translates |
|---|---|---|
| PPAP cross-arch | Trivial — register remap only | Same syscall numbers, different register ABI |
| CP/M | Simple — ~40 BDOS functions | BDOS calls → open/read/write/exit |
| DOS | Moderate — INT 21h + BIOS stubs | INT 21h + INT 10h/16h → PPAP syscalls |
| Human68k | Moderate — F-line + IOCS | F-line exceptions → PPAP syscalls |
| Win32 | Complex — DLL function stubs | Win32 API → PPAP syscalls (stretch) |

**Analogy:** This is PPAP's equivalent of WSL (Windows Subsystem for
Linux), Wine, or FreeBSD's Linux binary compatibility — but much
simpler, targeting retro OS binaries on microcontrollers.

---

## 2. Architecture

### 2.1 Kernel Integration

The `exec()` path tries native ELF first, then walks a chain of
registered subsystem detectors. Foreign-arch ELF is handled as
a subsystem (the PPAP cross-arch personality), not as a special case:

```c
int do_execve(pcb_t *p, const char *path, const char *const *argv) {
    const uint8_t *file = romfs_lookup(path);

    /* Try native ELF (matching host e_machine) */
    if (is_native_elf(file))
        return exec_elf_native(p, file, argv);

    /* Try registered subsystems — including foreign-arch ELF */
    for (int i = 0; i < n_subsystems; i++) {
        if (subsystems[i].detect(file, file_size))
            return exec_subsystem(p, &subsystems[i], file, argv);
    }

    return -ENOEXEC;
}
```

Foreign-arch ELF detection: valid ELF magic but `e_machine` doesn't
match the host → dispatches to the PPAP cross-arch subsystem.

### 2.2 Subsystem Registration

Each subsystem registers a descriptor:

```c
typedef struct {
    const char *name;           /* "ppap-arm", "cpm", "dos", etc. */
    int (*detect)(const uint8_t *file, uint32_t size);
    const char *emulator_path;  /* "/subsys/ppap-arm" */
} subsystem_t;
```

Detection heuristics vary by format — see per-subsystem sections below.

### 2.3 Execution Model

All subsystem emulators run as **user-space PPAP processes**.
The kernel replaces the exec with:

```c
const char *new_argv[] = { subsys->emulator_path, path, ... };
return do_execve(p, subsys->emulator_path, new_argv);
```

The emulator binary (a native PPAP ELF) contains both the eCPU core
and the personality layer, statically linked as a single binary.

---

## 3. PPAP Cross-Architecture Subsystem

### 3.1 Overview

The simplest subsystem: run PPAP ELF binaries built for a different
architecture. Same OS, same syscall numbers — only the register ABI
differs.

### 3.2 Binary Format

Standard ELF with valid magic but non-native `e_machine`:
- On ARM host: detects `EM_68K` (4) → dispatches to `ppap-m68k`
- On m68k host: detects `EM_ARM` (40) → dispatches to `ppap-arm`

### 3.3 Personality Layer

The thinnest possible personality — mechanical register remapping:

```c
/* PPAP uses the same syscall numbers on all architectures.
 * Only the register positions differ. */

/* ARM registers → native syscall */
void ppap_arm_personality(ecpu_state_t *cpu) {
    long ret = syscall6(
        cpu->regs[7],   /* r7  = syscall number */
        cpu->regs[0],   /* r0  = arg1 */
        cpu->regs[1],   /* r1  = arg2 */
        cpu->regs[2],   /* r2  = arg3 */
        cpu->regs[3],   /* r3  = arg4 */
        cpu->regs[4],   /* r4  = arg5 */
        cpu->regs[5]    /* r5  = arg6 */
    );
    cpu->regs[0] = ret;
}

/* m68k registers → native syscall */
void ppap_m68k_personality(ecpu_state_t *cpu) {
    long ret = syscall6(
        cpu->dregs[0],  /* d0  = syscall number */
        cpu->dregs[1],  /* d1  = arg1 */
        cpu->dregs[2],  /* d2  = arg2 */
        cpu->dregs[3],  /* d3  = arg3 */
        cpu->dregs[4],  /* d4  = arg4 */
        cpu->dregs[5],  /* d5  = arg5 */
        0               /* m68k ABI has 5 args in d1-d5 (a0 for 6th) */
    );
    cpu->dregs[0] = ret;
}
```

No file format translation, no path mapping, no API bridging.
Pointer translation (guest→host address) is still needed for
syscalls that take pointer arguments — this is handled by the
eCPU memory model (see `feature-eCPU.md` §3.3).

### 3.4 Variants

| Emulator binary | eCPU core | Host → Guest |
|---|---|---|
| `/subsys/ppap-arm` | ecpu-arm | m68k host runs ARM ELF |
| `/subsys/ppap-m68k` | ecpu-m68k | ARM host runs m68k ELF |
| `/subsys/ppap-armv6` | ecpu-armv6 | Any host runs ARMv6 ELF |

---

## 4. CP/M Subsystem

### 4.1 Target

Run CP/M 2.2 `.COM` files — the simplest and most historically
significant target. Thousands of CP/M programs exist: MBASIC, Turbo
Pascal, WordStar, dBASE II, etc.

### 4.2 Binary Format

CP/M `.COM` is the simplest executable format:
- Raw binary, no header
- Loaded at address 0x0100
- Execution starts at 0x0100
- Max size: 0xFE00 bytes (to 0xFF00)

Detection: any file with `.com` extension and size <= 0xFE00 is treated
as CP/M .COM. (Ambiguity with DOS .COM is resolved by user
configuration or directory convention — see §8.)

### 4.3 Memory Map (Emulated)

```
0x0000  BIOS warm-boot entry (JP to warm boot handler)
0x0005  BDOS entry point (JP to BDOS dispatcher)
0x005C  Default FCB 1 (parsed from command line)
0x006C  Default FCB 2
0x0080  Default DMA buffer / command-line tail
0x0100  Program load area (.COM binary)
  ...
0xFE00  End of TPA (Transient Program Area)
```

### 4.4 CPU Emulator: ecpu-z80

Zilog Z80 interpreter (~150 instructions + CB/DD/ED/FD prefix groups).

```c
typedef struct {
    uint8_t a, f, b, c, d, e, h, l;  /* main registers */
    uint8_t a2, f2, b2, c2, d2, e2, h2, l2; /* shadow set */
    uint16_t ix, iy, sp, pc;
    uint8_t iff1, iff2, im;  /* interrupt state */
    uint8_t memory[65536];   /* 64 KB flat address space */
} z80_state_t;
```

Size estimate: ~3000 lines of C, ~12 KB binary.

The Z80 is a superset of Intel 8080. Programs written for 8080/CP/M
work unmodified.

### 4.5 OS Personality: CP/M BDOS Bridge

CP/M programs invoke BDOS via `CALL 5` with C=function number, DE=parameter.

The emulator intercepts execution at address 0x0005 and translates:

| BDOS fn | Name | PPAP translation |
|---|---|---|
| 0 | System Reset | `_exit(0)` |
| 1 | Console Input | `read(0, &ch, 1)` |
| 2 | Console Output | `write(1, &ch, 1)` |
| 6 | Direct Console I/O | `read`/`write` with non-blocking flag |
| 9 | Print String | `write(1, buf, len)` (scan for `$` terminator) |
| 10 | Read Console Buffer | `read(0, buf, max)` with line editing |
| 15 | Open File | FCB → pathname, `open()` |
| 16 | Close File | `close()` |
| 19 | Delete File | `unlink()` |
| 20 | Read Sequential | `read()` 128 bytes at current position |
| 21 | Write Sequential | `write()` 128 bytes |
| 22 | Make File | `open(O_CREAT)` |
| 26 | Set DMA Address | Update internal DMA pointer |
| 33 | Read Random | `lseek()` + `read()` |
| 34 | Write Random | `lseek()` + `write()` |
| 35 | Compute File Size | `fstat()` or `lseek(SEEK_END)` |
| 36 | Set Random Record | Compute from FCB sequential position |

**FCB (File Control Block) translation:** CP/M uses FCBs with 8.3
filenames in a flat directory. The bridge maps FCB drive/name to PPAP
paths:

```
FCB { drive=A, name="HELLO   ", ext="COM" }
  → "/a/HELLO.COM"    (or configurable mount point)
```

### 4.6 Implementation Phases

1. **Phase 1:** Z80 interpreter core + BDOS 0/1/2/9 (console I/O)
   - Test: run a "Hello World" CP/M .COM
2. **Phase 2:** FCB file operations (BDOS 15-22, 33-36)
   - Test: run MBASIC, load/save files
3. **Phase 3:** Full BDOS compatibility, user area support
   - Test: run Turbo Pascal, WordStar

---

## 5. DOS Subsystem

### 5.1 Target

Run MS-DOS `.COM` and `.EXE` files. Focus on simple DOS programs
(text utilities, games, educational software).

### 5.2 Binary Formats

**DOS .COM:**
- Raw binary, loaded at offset 0x0100 within a segment
- CS=DS=ES=SS=PSP segment
- Execution starts at CS:0100
- Max size: ~64 KB
- Detection: `.com` extension (disambiguated from CP/M by configuration)

**DOS .EXE (MZ format):**
- Header starts with `MZ` (0x4D5A)
- Relocation table for segment fixups
- Separate code/data/stack segments
- Detection: first two bytes = `MZ`

### 5.3 CPU Emulator: ecpu-8086

Intel 8086/8088 real-mode interpreter.

```c
typedef struct {
    uint16_t ax, bx, cx, dx;      /* general registers */
    uint16_t si, di, bp, sp;      /* index/pointer registers */
    uint16_t cs, ds, es, ss;      /* segment registers */
    uint16_t ip;                  /* instruction pointer */
    uint16_t flags;               /* FLAGS register */
    uint8_t memory[1048576];      /* 1 MB address space */
} x86_state_t;
```

Real-mode addressing: physical = segment × 16 + offset.

Size estimate: ~5000 lines of C, ~20 KB binary. The 8086 ISA has
variable-length instructions with complex ModR/M addressing — more
effort than Z80 but well-documented.

### 5.4 OS Personality: DOS INT 21h Bridge

DOS programs invoke services via `INT 21h` with AH=function number.

The emulator installs a handler in the emulated IVT at vector 0x21:

| INT 21h AH | Name | PPAP translation |
|---|---|---|
| 01h | Char Input with Echo | `read(0, &ch, 1)` + `write(1, &ch, 1)` |
| 02h | Char Output | `write(1, &ch, 1)` |
| 09h | Print String | `write(1, buf, len)` (scan for `$`) |
| 0Ah | Buffered Input | `read(0, buf, max)` |
| 3Ch | Create File | `open(O_CREAT | O_TRUNC)` |
| 3Dh | Open File | `open()` |
| 3Eh | Close File | `close()` |
| 3Fh | Read File | `read()` |
| 40h | Write File | `write()` |
| 41h | Delete File | `unlink()` |
| 42h | Seek (LSEEK) | `lseek()` |
| 4Ch | Terminate | `_exit()` |
| 4Eh | Find First | `opendir()` + `readdir()` |
| 4Fh | Find Next | `readdir()` |

Additional INT handlers needed:
- **INT 20h** — Program terminate (older .COM exit method)
- **INT 10h** — BIOS video (stub: text-mode output via UART)
- **INT 16h** — BIOS keyboard (stub: input from UART)

### 5.5 Implementation Phases

1. **Phase 1:** 8086 interpreter + INT 21h 01/02/09/4Ch (console I/O)
   - Test: "Hello World" DOS .COM
2. **Phase 2:** File handle operations (3Ch-42h) + MZ .EXE loader
   - Test: simple DOS utilities
3. **Phase 3:** INT 10h/16h stubs, Find First/Next, more complete DOS

---

## 6. Human68k Subsystem

### 6.1 Target

Run X68000 Human68k binaries on PPAP-m68k (and via eCPU on other
architectures). See `target-68000.md` §7 for X68000 hardware details.

### 6.2 Binary Format

Human68k X-format executable:
- Header: magic `HU` (0x4855), then base/text/data/bss/reloc sizes
- Detection: first two bytes = `HU`

Human68k R-format (relocatable .R):
- For device drivers and TSRs
- Not prioritised for initial implementation

### 6.3 CPU Emulator

On PPAP-m68k: runs natively (same CPU), no emulator needed — only
the personality layer is used (F-line exception handler).
On PPAP-ARM: uses ecpu-m68k from eCPU.

### 6.4 OS Personality: Human68k DOS Call Bridge

Human68k uses the F-line exception (opcodes $FFxx) for DOS calls.
The emulator/trap handler intercepts these and translates:

| DOS call | Number | PPAP translation |
|---|---|---|
| `_EXIT` | $FF00 | `_exit()` |
| `_GETCHAR` | $FF01 | `read(0, &ch, 1)` |
| `_PUTCHAR` | $FF02 | `write(1, &ch, 1)` |
| `_PRINT` | $FF09 | `write(1, str, len)` |
| `_CREATE` | $FF39 | `open(O_CREAT)` |
| `_OPEN` | $FF3D | `open()` |
| `_CLOSE` | $FF3E | `close()` |
| `_READ` | $FF3F | `read()` |
| `_WRITE` | $FF40 | `write()` |
| `_SEEK` | $FF42 | `lseek()` |

See `target-68000.md` §7 for X68000 hardware details including IOCS
(TRAP #15) passthrough.

---

## 7. Windows PE Subsystem (Stretch Goal)

### 7.1 Target

Run simple Win32 console applications (.EXE) on PPAP. This is
aspirational — a minimal proof-of-concept rather than full
compatibility.

### 7.2 Binary Format

PE (Portable Executable):
- DOS stub + `PE\0\0` signature
- COFF header + optional header + section table
- Detection: MZ header with PE offset at e_lfanew

### 7.3 CPU Emulator: ecpu-x86

32-bit x86 protected-mode interpreter. Significantly more complex than
8086 real mode — 32-bit operands, paging concepts (ignored in
emulation), many more instructions.

Size estimate: ~8000+ lines of C. This is the most complex emulator.

### 7.4 OS Personality: Win32 API Bridge

Instead of syscall-level translation, Win32 programs call DLL functions.
The emulator provides stub DLLs:

- **kernel32.dll** — `CreateFileA`, `ReadFile`, `WriteFile`,
  `CloseHandle`, `ExitProcess`, `GetStdHandle`,
  `GetCommandLineA`, `HeapAlloc`, `HeapFree`
- **msvcrt.dll** — `printf`, `fopen`, `fread`, `fwrite`, `malloc`,
  `free` (C runtime mapped to PPAP libc)

This is conceptually similar to Wine's approach but dramatically
smaller in scope — only enough to run trivial console programs.

### 7.5 Implementation

Deferred until CP/M, DOS, and Human68k subsystems are proven. The PE
loader and x86 emulator are the most complex components in this feature.

---

## 8. Subsystem Selection

### 8.1 Detection Chain

When `exec()` encounters a non-native binary:

```
1. ELF magic (0x7f "ELF"):
   - e_machine matches host  → native exec (no subsystem)
   - e_machine is foreign    → PPAP cross-arch subsystem (§3)

2. Other magic bytes:
   - "MZ"     → check for PE ("PE\0\0" at e_lfanew)
                   yes → Windows PE subsystem (§7)
                   no  → DOS .EXE subsystem (§5)
   - "HU"     → Human68k X-format subsystem (§6)
   - No magic → raw binary (step 3)

3. Raw binary — check file extension:
   - .com     → CP/M or DOS .COM (see 8.2)
   - .x       → Human68k (alternate extension)
   - .r       → Human68k R-format
   - other    → -ENOEXEC

4. If still ambiguous → -ENOEXEC
```

### 8.2 CP/M vs DOS .COM Disambiguation

Both CP/M and DOS use `.com` for raw binaries. Resolution:

- **Directory convention:** files under `/cpm/` use CP/M subsystem,
  files under `/dos/` use DOS subsystem
- **Configuration:** `/etc/subsys.conf` maps paths to subsystems
- **Default:** CP/M (simpler, more likely to work on constrained targets)

---

## 9. Shared Infrastructure

### 9.1 Emulator Core Reuse

Every subsystem uses an eCPU core. The same core serves multiple
personalities:

| eCPU core | Subsystem personalities |
|---|---|
| ecpu-arm | PPAP cross-arch (ARM ELF on m68k) |
| ecpu-m68k | PPAP cross-arch (m68k ELF on ARM), Human68k |
| ecpu-z80 | CP/M |
| ecpu-8086 | DOS |
| ecpu-armv6 | PPAP cross-arch (ARMv6 ELF on other hosts) |
| ecpu-x86 | Windows PE (stretch) |

### 9.2 Common Emulator Framework

All eCPU cores expose a common interface with a trap callback hook.
The subsystem personality registers its handler at init time:

```c
/* Common interface — see feature-eCPU.md §3.2 */
typedef void (*ecpu_trap_handler_t)(ecpu_state_t *cpu, uint32_t trap_id);

/* Each subsystem binary does: */
ecpu_state_t cpu;
ecpu_init(&cpu, arch_z80);
ecpu_set_trap_handler(&cpu, cpm_bdos_handler);  /* personality */
cpm_load_com(&cpu, binary_path);                /* loader */
ecpu_run(&cpu);                                 /* execute */
```

### 9.3 File System Mapping

All subsystems need to map foreign filenames to PPAP paths:

| Foreign OS | Filename style | Mapping |
|---|---|---|
| CP/M | `A:HELLO.COM` | `/a/HELLO.COM` |
| DOS | `C:\DIR\FILE.TXT` | `/c/DIR/FILE.TXT` |
| Human68k | `A:\DIR\FILE.X` | `/a/DIR/FILE.X` |
| Win32 | `C:\Users\file.txt` | `/c/Users/file.txt` |

Drive letters map to directories under root. Path separators (`\`) are
converted to `/`. Case handling follows the foreign OS convention
(CP/M and DOS: case-insensitive matching).

---

## 10. Implementation Priority

| Priority | Subsystem | Rationale |
|---|---|---|
| 1 | PPAP cross-arch | Simplest personality. Proves the eCPU+subsystem framework. |
| 2 | CP/M (Z80) | Simplest foreign OS. Small CPU + small API. |
| 3 | Human68k (m68k) | Natural fit for X68000 target. Shares ecpu-m68k. |
| 4 | DOS (8086) | Larger software library. More complex emulator. |
| 5 | Windows PE (x86) | Stretch goal. Very complex. Low priority. |

### 10.1 Overall Implementation Plan

**Phase A — Framework + PPAP cross-arch**
1. eCPU common interface and trap hook API
2. Subsystem detection chain in `exec()` (ELF + non-ELF)
3. Subsystem registration and dispatch
4. First eCPU core (ecpu-arm) + PPAP personality
5. Test: run ARM `hello` ELF on m68k PPAP

**Phase B — CP/M Subsystem**
1. Z80 interpreter
2. CP/M memory map setup + .COM loader
3. BDOS console I/O bridge (functions 0-9)
4. BDOS file operations (functions 15-36)
5. Test with MBASIC, Turbo Pascal

**Phase C — Human68k Subsystem**
1. X-format loader
2. F-line exception handler
3. DOS call bridge (console + file I/O)
4. Test with X68000 utilities

**Phase D — DOS Subsystem**
1. 8086 real-mode interpreter
2. .COM loader + PSP setup
3. INT 21h console I/O bridge
4. MZ .EXE loader with relocation
5. INT 21h file operations
6. Test with simple DOS programs

**Phase E — Win32 Subsystem (if pursued)**
1. x86 protected-mode interpreter extensions
2. PE loader
3. kernel32.dll / msvcrt.dll stubs
4. Test with trivial console .EXE

---

## 11. Performance

Subsystem overhead has two components:

1. **CPU emulation** — same as eCPU (10-100x slower than native)
2. **OS call translation** — negligible (a few comparisons and
   register copies per call)

Programs that spend most time in I/O (which is most CP/M and DOS
software) will run at near-native I/O speed since syscalls execute
natively after translation.

On RP2040 (133 MHz), running a CP/M program through Z80 emulation:
- Z80 effective speed: ~5-10 MHz equivalent
- Original CP/M machines ran Z80 at 2-4 MHz
- Result: **faster than original hardware**

---

## 12. Open Questions

1. **Memory limits:** Z80 (64 KB) and 8086 real mode (1 MB) fit
   easily in RP2040's 264 KB SRAM. x86 protected mode programs may
   expect more. Limit TPA/conventional memory to what fits.

2. **Terminal emulation:** CP/M programs expect VT52/VT100 or
   ADM-3A terminals. DOS programs expect ANSI.SYS. The PPAP terminal
   should handle these escape sequences (or the personality layer
   translates them).

3. **Block I/O vs stream I/O:** CP/M uses 128-byte records, DOS uses
   byte-level I/O. The BDOS bridge must handle record-oriented
   read/write mapped to PPAP's byte-stream `read()`/`write()`.

4. **Multiple subsystems simultaneously:** can a CP/M program pipe
   output to a DOS program? In principle yes — the kernel sees both
   as regular PPAP processes with normal file descriptors.

5. **Self-contained binaries:** each subsystem emulator
   (e.g., `/subsys/cpm`) includes both the CPU emulator and
   the OS bridge, statically linked as a single PPAP binary. This
   avoids needing shared libraries in the emulator itself.
