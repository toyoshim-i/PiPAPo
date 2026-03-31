# IBM PC Target Port Plan (V30 with 8080 Mode)

Porting PPAP to the IBM PC platform using the NEC V30 (μPD70116) CPU.
The V30 is 8086-compatible and includes a hardware 8080 emulation mode,
which maps naturally onto PPAP's eCPU architecture for running CP/M-80
programs without a software interpreter.

---

## 1. Goals and Scope

### 1.1 Primary Goal

Produce a bootable PPAP system on an IBM PC/XT-class machine with V30 that:

- Boots from a floppy disk (1.44 MB) with a two-stage bootstrap.
- Provides a console on the CGA/MDA text display via BIOS INT 10h, mirrored
  to COM1 serial (same pattern as pico1's USB + UART mirror and x68k's
  TVRAM + serial mirror).
- Mounts the boot floppy as a UFS volume and executes PPAP userland from it.
- Runs the PPAP userland test suite (`runtests`) and passes it.
- Uses the V30's hardware 8080 emulation mode (`BRKEM`/`RETEM`) as an eCPU
  for running 8080 CP/M-80 programs at native speed.

### 1.2 Extended Goals

- DOS subsystem: load and run MS-DOS .COM and .EXE (MZ) binaries with an
  INT 21h bridge (analogous to the Human68k bridge on X68000).
- Boot from a hard disk via BIOS INT 13h (Phase P-5b); direct ATA/IDE
  PIO driver as an extended feature.
- Direct VGA text-mode driver replacing BIOS calls.
- Keyboard input via INT 16h / direct 8042 controller access.
- Full Z80 CP/M support via software eCPU (for programs using Z80 extensions
  beyond the 8080 instruction set).

### 1.3 Out of Scope

- Graphics modes (VGA/EGA/CGA graphics, VESA).
- Sound (PC speaker melody, Sound Blaster, AdLib).
- 80286/80386 protected mode — the kernel runs entirely in real mode.
- Network drivers (NE2000, etc.).
- GPT partitioning — MBR only (matches PC/XT scope).

---

## 2. V30 CPU Overview

### 2.1 8086 Compatibility

The NEC V30 (μPD70116) is a pin-compatible and instruction-compatible
replacement for the Intel 8086.  It runs all 8086 code unchanged, with
slightly improved cycle counts on many instructions.

Key properties relevant to PPAP:

| Property | Value |
|----------|-------|
| CPU | NEC V30 (μPD70116), 8086-compatible |
| Clock | 8 MHz (PC/XT) or 10 MHz |
| Address bus | 20-bit (1 MB address space) |
| Data bus | 16-bit external |
| Registers | AX, BX, CX, DX, SI, DI, BP, SP, CS, DS, ES, SS, IP, FLAGS |
| Memory model | Segmented: physical = segment × 16 + offset |
| Privilege | No protection rings in real mode |

The V20 (μPD70108) is the 8088-compatible variant (8-bit external bus).
Both V20 and V30 include the 8080 emulation mode described below.

### 2.2 Hardware 8080 Emulation Mode

The V30 includes dedicated microcode for executing Intel 8080 instructions
natively.  This is distinct from Z80 — the 8080 is a strict subset.

#### Entering 8080 mode: `BRKEM imm8`

```nasm
BRKEM imm8      ; opcode: 0F E0 imm8
```

When `BRKEM` executes:

1. The V30 saves the current x86 state (all segment registers, GP registers,
   FLAGS, IP) onto the current stack (SS:SP).
2. The CPU switches its instruction decoder to 8080 mode.
3. The 8080 register file is mapped onto x86 registers:

   | 8080 Register | V30 Register | Notes |
   |---------------|-------------|-------|
   | A (accumulator) | AL | |
   | Flags | AH | 8080 flag layout, not x86 |
   | B | CH | |
   | C | CL | |
   | D | DH | |
   | E | DL | |
   | H | BH | |
   | L | BL | |
   | SP | SP | Within the current 64 KB segment |
   | PC | IP | Within the current CS segment |

4. The `imm8` operand selects an entry in the 8080 interrupt vector table
   (at address `imm8 × 4` within the segment).  The 8080 PC is loaded from
   this vector — execution begins at that address in 8080 mode.

The 8080 code sees a flat 64 KB address space corresponding to the x86
segment that was active when `BRKEM` was issued.  All 8080 memory accesses
use DS as the implicit segment base.

#### Exiting 8080 mode: `RETEM`

```nasm
RETEM           ; 8080 opcode: ED FD
```

`RETEM` is a two-byte 8080 instruction (using an undefined 8080 opcode
sequence `ED FD`).  When executed:

1. The V30 restores the saved x86 state from the stack.
2. The instruction decoder switches back to x86 mode.
3. Execution continues at the x86 instruction following the original `BRKEM`.

This provides a clean, symmetric context switch between the two ISAs.

### 2.3 8080 vs Z80 — What Works, What Doesn't

| Feature | 8080 | Z80 | V30 8080 mode |
|---------|------|-----|---------------|
| Basic arithmetic (ADD, SUB, AND, OR, XOR) | Yes | Yes | Yes |
| Conditional jumps/calls/returns | Yes | Yes | Yes |
| PUSH/POP register pairs | Yes | Yes | Yes |
| RST (restart vectors) | Yes | Yes | Yes |
| IN/OUT (I/O ports) | Yes | Yes | Yes |
| IX, IY index registers | No | Yes | **No** |
| Alternate register set (AF', BC', DE', HL') | No | Yes | **No** |
| Relative jumps (JR, DJNZ) | No | Yes | **No** |
| Bit manipulation (BIT, SET, RES) | No | Yes | **No** |
| Block transfer (LDIR, LDDR) | No | Yes | **No** |
| Block I/O (INIR, OTIR) | No | Yes | **No** |
| Extended ED-prefix instructions | No | Yes | **No** |

**Implication**: Only 8080-clean CP/M programs can run on V30 8080 mode.
This includes many early CP/M applications (MBASIC, WordStar early versions,
ED, PIP, ASM, DDT, STAT) which were written before Z80 extensions became
common.  Programs written specifically for Z80 (Turbo Pascal, many games,
most post-1980 CP/M software) will not run.

---

## 3. Segmented Memory — Impact on PPAP

This is the most significant architectural difference from ARM and m68k.

### 3.1 The Segment Model

Physical address = `segment × 16 + offset`.  All pointers are implicitly
relative to a segment register (CS for code, DS for data, SS for stack,
ES for extra).  A "far pointer" is a 32-bit `segment:offset` pair that
can address any byte in the 1 MB space.

### 3.2 Kernel Memory Model

All kernel modules share **SS=0** for data access.  Each module's code
lives in its own code segment (separate CS).  Cross-module calls
use `lcall`/`lret` stubs — SS stays unchanged, so pointer arguments
work without serialization.

**ia16-elf-gcc segment register behaviour (important):**

The small-model compiler uses **SS for all data access** (`%ss:`
prefix on every memory operand).  DS is used as a **scratch
register** — the compiler freely loads arbitrary values into DS
for temporary storage.  DS is NOT used for actual memory reads/writes.

This means:
- SS must always point to the shared data segment (SS=0)
- DS can contain any value — it does not affect data access
- Assembly code must use `%ss:` for data access, not rely on DS

```
Segment     Contents
────────    ─────────────────────────────
CS=0x0060   Core code: main, klog, mm, proc, sched, syscall, blkdev
CS=0x1000   VFS code: vfs, namei, romfs, tmpfs, devfs, procfs, ufs
CS=????     Exec code: exec, loaders (future)
SS=0        Shared data: all modules' .data, .bss, stack, page pool
```

Each module's code must fit in 64 KB.  Total data+BSS from all
modules must fit in 64 KB (shared SS=0; currently ~6 KB).

A **segment manager** (`src/kernel/common/seg.h`/`seg.c`) in the core
module tracks each module's code segment base at runtime.  Stage2 loads
module binaries and records their addresses in a `mod_info_t` block at
0x0500; the core reads this at boot.

### 3.3 User Process Memory

Each user process is assigned one or more 64 KB segments:

| Format | Segments | Layout |
|--------|----------|--------|
| .COM (CP/M 8080) | 1 segment (64 KB max) | Loaded at offset 0x0100 in one segment; CS=DS=ES=SS |
| .COM (DOS) | 1 segment (64 KB max) | Loaded at offset 0x0100; PSP at 0x0000 |
| .EXE (DOS MZ) | N segments | Code segment(s) + data segment + stack segment |
| PPAP native | 1–2 segments | Flat binary or PIE-like within segments |

### 3.4 Page Allocator Adaptation

The page allocator uses `page_id_t` (16-bit index) to identify pages,
not `void *` pointers.  This is essential on i16 where `void *` is
16-bit and cannot represent page-pool addresses above 64 KB.

- 4 KB page = 256 paragraphs (0x100 segment units)
- 1 MB address space / 4 KB = 256 possible pages
- Minus kernel, BIOS, video memory → ~200 usable pages
- Comparable to RP2040 (51 pages) and X68000 1 MB (250 pages)

`mm_page_linear(id)` returns the 32-bit linear address of a page.
Data on pages is accessed via `mm_page_read()`/`mm_page_write()`
which handle segment setup internally.

The broader memory management refactoring (converting `user_pages[]`,
`mmap_regions[]`, and `proc_image_t` from `void *` to `page_id_t`) is
described in [`memory_management.md`](../kernel/memory_management.md).

### 3.5 Pointer Translation at the Syscall Boundary

When a user process passes a pointer to the kernel (e.g., buffer for
`read()`), the kernel must resolve the segment:offset pair to a linear
address before accessing it:

```c
static inline uint8_t *resolve_user_ptr(uint16_t seg, uint16_t off) {
    return (uint8_t *)((uint32_t)seg << 4) + off;
    /* Or use far pointer: MK_FP(seg, off) */
}
```

The cleaner approach is to **copy user data into kernel buffers** at the
syscall boundary (as PPAP already does for `sys_read`/`sys_write`), making
the segment resolution local to the syscall handler.

---

## 4. IBM PC Hardware

### 4.1 Base Machine (PC/XT Class)

| Item | Address/IRQ | PPAP Use |
|------|-------------|----------|
| RAM | 0x00000–0x9FFFF (640 KB conventional) | Kernel + user pages |
| Video RAM | 0xB0000–0xB7FFF (MDA) or 0xB8000–0xBFFFF (CGA) | Text display |
| BIOS ROM | 0xF0000–0xFFFFF (64 KB) | INT 10h, INT 13h, INT 16h |
| 8259A PIC | 0x20–0x21 | Interrupt controller |
| 8253/8254 PIT | 0x40–0x43 | **Timer (100 Hz tick)** |
| 8250 UART | 0x3F8–0x3FF (COM1), IRQ 4 | **Serial console** |
| 8042 KBC | 0x60, 0x64 | Keyboard |
| FDC (µPD765A) | 0x3F0–0x3F7, IRQ 6 | **Floppy block driver** |
| DMA (8237A) | 0x00–0x0F | FDC DMA channel 2 |

### 4.2 BIOS Availability

Like the X68000 IPL ROM, the PC BIOS remains mapped at 0xF0000 throughout
execution.  BIOS interrupt handlers are always callable via software
interrupts:

| BIOS Call | INT | Purpose |
|-----------|-----|---------|
| Video output | INT 10h (AH=0Eh) | Teletype character output |
| Disk I/O | INT 13h (AH=02h) | Read floppy sectors (CHS) |
| Keyboard | INT 16h (AH=00h/01h) | Read key / check status |
| Equipment | INT 11h | Detect installed hardware |
| Memory size | INT 12h | Conventional memory size in KB |

The initial port uses BIOS calls for all I/O, replacing them with direct
hardware drivers later (same phased approach as X68000).

### 4.3 Timer (8253/8254 PIT)

Channel 0 drives IRQ 0 for the system timer.

```
PIT clock input:  1,193,182 Hz
Divisor for 100 Hz:  11932  (0x2E9C)
    1,193,182 / 11932 ≈ 100.006 Hz
```

The BIOS initialises Channel 0 to ~18.2 Hz (divisor 65536).  PPAP
reprograms it to 100 Hz during `target_late_init()`:

```c
outb(0x43, 0x36);       /* Channel 0, lobyte/hibyte, mode 3 (square wave) */
outb(0x40, 0x9C);       /* Divisor low byte */
outb(0x40, 0x2E);       /* Divisor high byte */
```

IRQ 0 (INT 08h) fires at 100 Hz.  The ISR increments the tick counter
and triggers context switch, same as SysTick on ARM and MFP Timer-C on
X68000.

**Note**: Timer init is deferred to `target_post_mount` (after
`target_late_init`) because INT 08h replacement breaks BIOS floppy motor
control.  The floppy block device uses INT 13h, which depends on the
original BIOS timer handler for motor timeout.

### 4.4 Interrupt Controller (8259A PIC)

The PC/XT has one 8259A with 8 IRQ lines (IRQ 0–7) mapped to INT 08h–0Fh:

| IRQ | INT | Source |
|-----|-----|--------|
| 0 | 08h | PIT timer (Channel 0) |
| 1 | 09h | Keyboard |
| 2 | 0Ah | Cascade (XT: unused) |
| 3 | 0Bh | COM2 |
| 4 | 0Ch | COM1 |
| 5 | 0Dh | LPT2 / hard disk |
| 6 | 0Eh | FDC |
| 7 | 0Fh | LPT1 |

PPAP needs IRQ 0 (timer) and optionally IRQ 4 (COM1 receive) and
IRQ 6 (FDC completion).  All others can be masked.

EOI (End of Interrupt) must be sent to port 0x20 after each ISR:

```c
outb(0x20, 0x20);       /* Non-specific EOI */
```

---

## 5. V30 8080 Mode as eCPU

This is the key differentiator of the V30 port.  Instead of the software
Z80 interpreter (`ecpu_z80.c`, ~1400 lines), the V30's hardware 8080 mode
provides native instruction execution for 8080-clean CP/M programs.

### 5.1 Architecture Mapping

| PPAP eCPU Concept | Software Z80 (current) | V30 8080 Mode |
|-------------------|----------------------|---------------|
| Emulator loop | `ecpu_z80_run()` in C | `BRKEM imm8` — CPU runs 8080 natively |
| Trap on CALL 0x0005 | Check PC after decode | 8080 `CALL 0x0005` → hits `RETEM` at address 0x0005 |
| Trap on I/O IN/OUT | Decoded in emulator loop | 8080 `IN`/`OUT` → real x86 I/O port access (see §5.3) |
| Register access | `cpu->regs[]` struct | x86 registers (AL=A, CH=B, CL=C, etc.) |
| Memory access | `cpu->mem[]` array | Direct memory in the 64 KB DS segment |
| Context switch cost | Zero (it's just C data) | `BRKEM`/`RETEM` save/restore (~30 clocks) |

### 5.2 BDOS/BIOS Trap Mechanism

CP/M programs call BDOS at address 0x0005 and BIOS via the jump table at
the top of the TPA (typically 0xFE00+).  The trap mechanism:

1. Before entering 8080 mode, the kernel writes a `RETEM` instruction
   (`ED FD`) at address 0x0005 within the 8080's 64 KB memory segment.
2. The kernel also writes `RETEM` at each BIOS entry point.
3. `BRKEM` enters 8080 mode.  The CP/M program runs at full speed.
4. When the program executes `CALL 0x0005`, the CPU reaches the `RETEM`
   instruction, exits 8080 mode, and returns to x86 code.
5. The x86 trap handler reads the 8080 register state (C register = BDOS
   function number, DE = argument) from the x86 registers (CL, DX).
6. The kernel's CP/M bridge (`cpm_bridge.c`) processes the call.
7. The kernel writes the result back into the x86 registers and re-enters
   8080 mode with another `BRKEM`.

```
User: CP/M program (8080 code)
  │
  │ CALL 0x0005 (BDOS)
  ▼
[0x0005]: RETEM instruction
  │
  │ V30 exits 8080 mode, restores x86 state
  ▼
Kernel: cpm_bridge.c
  │ Reads CL (= C register = BDOS function number)
  │ Reads DX (= DE register = argument)
  │ Translates to PPAP VFS/process syscalls
  │ Writes result back to x86 regs
  │
  │ BRKEM
  ▼
User: CP/M program continues in 8080 mode
```

This is functionally identical to the software Z80's `ECPU_TRAP_RST`
mechanism, but the "emulator loop" is the V30 hardware itself.

### 5.3 I/O Port Handling

The 8080 `IN` and `OUT` instructions in V30 8080 mode execute as real x86
I/O port accesses.  This is different from the software emulator, which
traps every I/O instruction.

Options:

**Option A — Unused port range (recommended)**:
Map CP/M virtual I/O ports to a range of x86 I/O ports that are unused on
the PC (e.g., 0x300–0x30F).  The CP/M BIOS routines set up the port
addresses accordingly.  Since these ports have no hardware behind them on
a standard PC, reads return 0xFF and writes are silently ignored — safe
but useless.  Most CP/M programs do not use direct I/O; they go through
BDOS.

**Option B — NMI on port access**:
Some I/O addresses can be configured to generate NMI or other traps.
Complex and hardware-dependent; not recommended for the initial port.

**Option C — Pre/post-process in bridge**:
For the few CP/M programs that use direct I/O (mainly terminal control),
the CP/M bridge can patch the I/O addresses in the loaded binary to
redirect to known trap addresses.  Fragile but workable for specific
programs.

The recommended initial approach is Option A: let I/O fall through to
unused ports.  Add Option C selectively if specific CP/M programs need it.

### 5.4 Memory Layout for 8080 Mode

Each CP/M process gets one 64 KB segment.  The kernel sets DS to point at
this segment before issuing `BRKEM`:

```
8080 address space (64 KB segment at DS:0000)
┌──────────────────────────────┐ 0xFFFF
│  BIOS jump table + RETEM     │ 0xFE00
├──────────────────────────────┤
│  BDOS (emulated — just RETEM │
│  at entry + a few bytes)     │ 0xE400
├──────────────────────────────┤
│                              │
│  TPA (Transient Program Area)│
│  CP/M program loaded here    │
│                              │ 0x0100
├──────────────────────────────┤
│  Zero page (BIOS/BDOS info)  │ 0x0000
│  [0x0005] = RETEM (ED FD)    │
│  [0x0000] = RETEM (warm boot)│
└──────────────────────────────┘
```

This matches the standard CP/M memory map exactly.  The `cpm_loader.c`
already builds this layout for the software Z80; the same logic applies
here, just writing into a real memory segment instead of `cpu->mem[]`.

### 5.5 Dual eCPU Strategy

The V30 port should support both:

1. **Hardware 8080 mode** — for 8080-clean CP/M programs (fast, native)
2. **Software Z80 emulator** — for Z80 CP/M programs (slower, full compat)

The subsystem loader can auto-detect which to use:

```c
int cpm_load(const char *path, ...) {
    /* Scan binary for Z80-only opcodes (CB/DD/ED/FD prefixes) */
    if (binary_uses_z80_extensions(code, size)) {
        /* Use software Z80 emulator */
        return cpm_load_ecpu_z80(path, ...);
    }
    /* Use V30 hardware 8080 mode */
    return cpm_load_v30_8080(path, ...);
}
```

Or the user explicitly selects:

```sh
# Force hardware 8080 mode (fails if binary uses Z80 instructions)
run8080 foo.com

# Force software Z80 emulator (always works)
runz80 foo.com

# Auto-detect (default)
cpm foo.com
```

---

## 6. DOS Subsystem

The extended goal: run MS-DOS .COM and .EXE programs with a DOS
personality bridge, analogous to the Human68k bridge on X68000.

### 6.1 Program Formats

#### .COM Files

Identical in concept to CP/M .COM — flat binary loaded at offset 0x0100
within a single segment.  PSP (Program Segment Prefix) occupies
0x0000–0x00FF.

```
DOS .COM layout (single segment, CS=DS=ES=SS)
┌──────────────────────┐ 0xFFFF
│  Stack (grows down)  │
├──────────────────────┤
│  Program code + data │ 0x0100
├──────────────────────┤
│  PSP (256 bytes)     │ 0x0000
│  [0x00] INT 20h      │
│  [0x2C] env segment  │
│  [0x80] command tail │
└──────────────────────┘
```

#### .EXE Files (MZ Format)

Multi-segment programs with a relocation table in the MZ header:

```c
struct mz_header {
    uint16_t magic;         /* 'MZ' or 'ZM' */
    uint16_t last_page_size;
    uint16_t page_count;    /* 512-byte pages */
    uint16_t reloc_count;
    uint16_t header_size;   /* in paragraphs (16B units) */
    uint16_t min_alloc;     /* min extra paragraphs */
    uint16_t max_alloc;     /* max extra paragraphs */
    uint16_t init_ss;       /* initial SS (relative to load segment) */
    uint16_t init_sp;
    uint16_t checksum;
    uint16_t init_ip;
    uint16_t init_cs;       /* initial CS (relative to load segment) */
    uint16_t reloc_offset;
    uint16_t overlay;
};
```

Loading:

1. Parse MZ header, compute load size.
2. Allocate segments for code + data + stack + extra.
3. Load program image after the header.
4. Apply segment relocations: each entry is a `(offset, segment)` pair
   within the loaded image where a 16-bit segment value must be adjusted
   by adding the actual load segment.
5. Build PSP in the first paragraph.
6. Set CS:IP and SS:SP from the header (adjusted by load segment).

Segment relocation is simpler than ELF relocation or Human68k .x
relocation — it's a flat list of fixup addresses containing segment values.

### 6.2 INT 21h Bridge

The core of the DOS subsystem.  Function number in AH:

```c
/* dos_bridge.c — analogous to human68k_bridge.c */
static int dos_dispatch(struct dos_regs *r) {
    switch (r->ah) {
    /* Character I/O */
    case 0x01: return dos_getchar_echo(r);    /* Read char with echo */
    case 0x02: return dos_putchar(r);         /* Write char to stdout */
    case 0x06: return dos_direct_io(r);       /* Direct console I/O */
    case 0x09: return dos_print_string(r);    /* Print $-terminated string */
    case 0x0A: return dos_buffered_input(r);  /* Buffered keyboard input */

    /* File operations — map to PPAP VFS */
    case 0x3C: return dos_create(r);          /* Create file: DS:DX=path, CX=attr */
    case 0x3D: return dos_open(r);            /* Open file: DS:DX=path, AL=mode */
    case 0x3E: return dos_close(r);           /* Close: BX=handle */
    case 0x3F: return dos_read(r);            /* Read: BX=handle, CX=count, DS:DX=buf */
    case 0x40: return dos_write(r);           /* Write: BX=handle, CX=count, DS:DX=buf */
    case 0x41: return dos_unlink(r);          /* Delete: DS:DX=path */
    case 0x43: return dos_chmod(r);           /* Get/set attributes */
    case 0x56: return dos_rename(r);          /* Rename: DS:DX=old, ES:DI=new */

    /* Directory */
    case 0x39: return dos_mkdir(r);           /* Create directory */
    case 0x3A: return dos_rmdir(r);           /* Remove directory */
    case 0x3B: return dos_chdir(r);           /* Change directory */
    case 0x47: return dos_getcwd(r);          /* Get current directory */

    /* File position */
    case 0x42: return dos_lseek(r);           /* Seek: BX=handle, CX:DX=offset, AL=whence */

    /* Memory management */
    case 0x48: return dos_alloc(r);           /* Alloc: BX=paragraphs → AX=segment */
    case 0x49: return dos_free(r);            /* Free: ES=segment */
    case 0x4A: return dos_realloc(r);         /* Resize: ES=segment, BX=paragraphs */

    /* Process control */
    case 0x4B: return dos_exec(r);            /* EXEC: AL=subfunc, DS:DX=path */
    case 0x4C: return dos_exit(r);            /* EXIT: AL=return code */
    case 0x4D: return dos_wait(r);            /* Get child return code */

    /* Misc */
    case 0x25: return dos_set_vector(r);      /* Set interrupt vector */
    case 0x30: return dos_get_version(r);     /* Report DOS version (e.g. 3.30) */
    case 0x35: return dos_get_vector(r);      /* Get interrupt vector */
    case 0x44: return dos_ioctl(r);           /* IOCTL (subset) */
    }
    return -1; /* Unhandled */
}
```

Most functions map directly to existing PPAP VFS operations (`sys_open`,
`sys_read`, `sys_write`, `sys_close`, `sys_unlink`, `sys_rename`, etc.).
The bridge translates DOS-style error codes (carry flag + AX) and
path separators (`\` → `/`).

### 6.3 Drive Letter Mapping

Same pattern as Human68k:

```
A: → /a/   (floppy drive 0)
B: → /b/   (floppy drive 1)
C: → /c/   (hard disk — if present)
```

Current working directory is tracked per-drive in the bridge, matching
DOS semantics.

### 6.4 PSP Construction

The DOS bridge builds a 256-byte PSP for each loaded program:

| Offset | Size | Content |
|--------|------|---------|
| 0x00 | 2 | `INT 20h` instruction (CD 20) |
| 0x02 | 2 | Top of memory (segment) |
| 0x05 | 5 | Far call to DOS (INT 21h wrapper) |
| 0x0A | 4 | Saved INT 22h (terminate) |
| 0x0E | 4 | Saved INT 23h (Ctrl-C) |
| 0x12 | 4 | Saved INT 24h (critical error) |
| 0x16 | 2 | Parent PSP segment |
| 0x18 | 20 | Job File Table (20 handles) |
| 0x2C | 2 | Environment segment |
| 0x50 | 3 | `INT 21h` + `RETF` (alternate DOS entry) |
| 0x5C | 16 | FCB 1 (parsed from command line) |
| 0x6C | 16 | FCB 2 (parsed from command line) |
| 0x80 | 1 | Command tail length |
| 0x81 | 127 | Command tail string |

---

## 7. Completed Phases (P-1 through P-4b)

Phases P-1 through P-4b are complete.  The kernel boots on QEMU,
mounts a UFS floppy, and enters the scheduler.

### Summary

| Phase | Scope | Key Result |
|-------|-------|------------|
| P-1 | Target skeleton + BIOS console | `src/arch/i16/`, `src/target/ibmpc/`, Docker toolchain, BIOS INT 10h + COM1 serial |
| P-2 | Timer + context switch | PIT 100 Hz, 8259A PIC, 24-byte frame, flag-driven switch |
| P-3a | Two-stage bootstrap | stage1 (512 B) + stage2 (UFS reader, indirect blocks), mkpcimg.sh |
| P-3b | Full kernel integration | 46 KB binary, ia16-elf-gcc, `uintptr_t` address fields, boots to idle |
| P-4a | Module system | `MOD_DECLARE`/`MOD_DEFINE`, mod_vfs (12 fn), mod_core (16 fn), boundary enforcement |
| P-4b | Segment split + floppy mount | Core (CS=0x0060) + VFS (CS=0x1000) + shared SS=0, far-call stubs, UFS root mounted |

### Current File Layout

```
src/arch/i16/
  arch.h              — IRQ save/restore (CLI/STI), preemption, hints
  cpu.h               — inb/outb, PIC/PIT/UART register definitions
  boot.S              — DS=ES=SS=0, BSS zero, stack, → kmain()
  switch.S            — INT 08h timer ISR, 24-byte frame, context switch
  trap.S              — INT 30h syscall handler (AX=nr, BX-DI=args)
  i16_common.c        — arch_build_initial_frame(), syscall ABI adapter

src/target/ibmpc/
  CMakeLists.txt      — Builds stage1, stage2, core, VFS, hello_com
  ibmpc_kernel.ld     — Core linker (0x0600, reserves 0xA000-0xBFFF for VFS data)
  ibmpc_vfs.ld        — VFS linker (.text at 0, .data at DS:0xA000)
  target_ibmpc.c      — early/late/post_mount hooks, seg_register, far-call patching
  boot/
    stage1.S          — 512 B boot sector + BPB, loads stage2 to 0xC000
    stage2_entry.S    — Stack at 0x7C00, entry stub for stage2.c
    stage2.c          — UFS reader, loads core+VFS+VFS data, mod_info at 0x0500
    stage2.ld         — Linked at 0xC000
  drivers/
    uart_com.c        — 8250 UART, COM1 9600 baud, polled I/O
    bios_con.c        — BIOS INT 10h teletype output
    timer_pit.c       — PIT 100 Hz + PIC unmask + ISR/syscall install
    floppy_blk.c      — BIOS INT 13h floppy, LBA→CHS, read-only, "fd0"
  stubs/
    vfs_stubs.S       — Core→VFS far-call caller stubs
    vfs_entries.S     — VFS-side entry point targets
    vfs_header.S      — VFS module header (magic 0x5646, 12 entries)
    core_stubs.S      — VFS→Core far-call caller stubs
    core_entries.S    — Core-side entry point targets

src/kernel/common/
  seg.h / seg.c       — Segment manager (i16 only), mod_id → segment lookup
  mod/
    module.h          — MOD_DECLARE / MOD_DEFINE macros
    mod_core.h        — Core exports (16 functions: klog, kmem, mm_page, blkdev)
    mod_vfs.h         — VFS exports (12 functions: init, mount, lookup, etc.)

scripts/
  mkpcimg.sh          — Assembles 1.44 MB floppy (stage1 + stage2 + UFS)
```

### Boot Sequence

1. BIOS loads sector 0 → 0x7C00 (stage1).  Prints "Pi".
2. Stage1 loads 8 sectors → 0xC000 (stage2).  Prints "PA".
3. Stage2 reads UFS: loads `/boot/kernel` → 0x0600,
   `/boot/kernel_vfs` → 0x1000:0000, `/boot/kernel_vfs_data` → DS:0xA000.
   Writes `mod_info_t` at 0x0500.  Jumps to 0x0000:0x0600.
4. Kernel prints "Po booting... [ibmpc]" — completing "PiPAPo booting...".
5. `target_early_init()`: UART + BIOS console, reads mod_info, registers
   segments, patches far-call tables (vfs_fptrs[], core_fptrs[]).
6. `mm_init()` → `proc_init()` → `vfs_init()` (via far call to VFS).
7. `target_late_init()`: registers floppy block device, mounts UFS root.
8. `target_post_mount()`: installs PIT timer (deferred — BIOS INT 13h
   floppy motor control needs original INT 08h handler).
9. `sched_start()`: enables interrupts, enters idle loop.

### Memory Layout (Kernel Running)

```
0x00000-0x003FF  IVT (256 vectors × 4 bytes)
0x00400-0x004FF  BIOS Data Area
0x00500-0x005FF  mod_info_t (module load addresses, written by stage2)
0x00600          Core .text.entry (boot.S: _start)
  ...            Core .text (~28 KB), .rodata, .data (~0.5 KB)
  ...            Core .bss (~5.8 KB, includes proc_table ~4.3 KB)
  ...            Stack (2 KB)
0x0A000-0x0BFFF  VFS .data + .bss (8 KB reserved in core linker)
0x0C000-0x0FFFF  Page pool (within near-pointer range, 4 pages max)
0x10000-0x1????  VFS .text (code segment, CS=0x1000)
  ...  -0x9FBFF  Extended page pool (far-pointer access only)
0x9FC00-0x9FFFF  EBDA (1 KB, preserved)
0xA0000-0xBFFFF  Video RAM
0xC0000-0xFFFFF  ROM / BIOS
```

### Far-Call Mechanism

Cross-module calls use assembly stubs to bridge code segments:

```
Core segment (CS=0x0060):        VFS segment (CS=0x1000):
──────────────────────────       ──────────────────────────
caller()                         vfs_init_entry:
  call mod_vfs.init()              call vfs_init   ; near
  ; near call to ──►              vfs_init:
vfs_init_caller_stub:                ...  (SS=0, shared data)
  pop ret_ip                       ret            ; near
  push CS; push ret_ip            lret  ◄── far return
  ljmp *[vfs_fptrs + 0] ── far ──►
──────────────────────────       ──────────────────────────
```

The VFS header at offset 0 in the VFS segment contains a magic word
(0x5646 = "VF"), entry count (12), and a table of entry offsets.
`target_early_init()` reads this header and patches both the core's
`vfs_fptrs[]` and the VFS's `core_fptrs[]` with far pointer pairs
(offset, segment).

### Key Bugs Found and Fixed

| Bug | Root Cause | Fix |
|-----|-----------|-----|
| INT 13h "not ready" | PIT timer replaced BIOS INT 08h, breaking floppy motor timeout | Defer `timer_init` to `target_post_mount` |
| VFS→core crash | DS clobbered by compiler (scratch register) | Entry stubs restore DS=0 before call; all data via SS |
| Near fptr crosses segments | `blkdev_t.read = floppy_read` invalid in VFS CS | `mod_core.blkdev_read` far-call wrapper |
| Far-call BP shift | `lcall; ret` left extra return addresses on stack | Rewrite stubs: pop/push CS:IP, ljmp; entry pops far frame |
| klogf garbled output | `%x`/`%u` read `uint32_t` (4B) but i16 `unsigned int` is 2B | `%x`/`%u` read `unsigned int`; `%lx`/`%lu` for 32-bit |
| klogf string corruption | BIOS INT 10h teletype no-op on QEMU; QEMU floppy DMA byte corruption | Not BIOS — QEMU-specific DMA quirk; correct on DOSBox-X |
| DOSBox-X INT 0/6 crash | `subsys_init` → `procfs_register_subsys` unresolved → call 0 | Guard with `PPAP_HAS_PROCFS`; add INT 0/6 debug handler |
| vfs_fd_stdio_init call 0 | VFS entry stub calls `vfs_fd_stdio_init` but actual function is `fd_stdio_init` | `.set vfs_fd_stdio_init, fd_stdio_init` alias in vfs_entries.S |
| core_fptrs overflow | 12 slots but 16 functions; boot-time patching overwrote mod_core | Add mm_page_* slots to core_fptrs (16 entries) |
| Far-call return value lost | Entry stubs did `xor %ax,%ax; mov %ax,%ds` after call, destroying AX | Use BX instead of AX for DS restore |
| exec ops->read hangs | `vn->mount->ops->read` is a near pointer valid only in VFS CS | `mod_vfs.vnode_read()` wrapper executes read in VFS CS |
| `fd_close_all` crash in sys_exit | Unresolved VFS function called from core sys_exit path | **Blocker**: requires completing module boundary migration |

### Memory Model

**Page-indexed free stack**: `free_stack[]` stores `page_id_t` indices
(16-bit), not `void *` pointers.  This allows pages beyond the 64 KB
near-pointer range on i16.  `page_linear[]` maps each index to its
32-bit linear address.  BIOS INT 12h detects conventional RAM size
at boot (typically 640 KB = 147 pages).

### Size Constraint

Core binary (text + rodata + data) plus BSS and stack must fit between
0x0600 and 0xFFFF (~63 KB) due to 16-bit near pointer limit.

| Component | Size |
|-----------|------|
| Core .text | ~30 KB |
| VFS .text | ~26 KB (separate segment) |
| .data | ~0.5 KB |
| .bss | ~5.8 KB |
| stack | 2 KB |
| VFS data | 8 KB (reserved at 0xA000) |
| Page pool | 588 KB (147 pages, far access via page_id_t) |

The segment split resolved the original 64 KB code limit.  Future
modules (exec) can use additional code segments.

---

## 8. Phase P-5: User-Space Exec and Tests

**Status**: In progress.  Module system complete (P-4a/P-4b done).
ELF loading and exit work.  Memory management needs refactoring
(see [`memory_management.md`](../kernel/memory_management.md)).

### What Works

- `elf16_loader.c` loads ELF32 (EM_386) via page-indexed API
  (`mm_page_alloc_contiguous` + `mm_page_write`)
- Process gets its own segment: CS=DS=ES = `base >> 4`
- Segment-aware initial frame: 24-byte ISR frame with correct CS/IP
- Module boundary fully resolved: `--unresolved-symbols=ignore-all`
  removed; two-pass link with `core_exports.ld`
- PIT 100 Hz timer and preemptive scheduler run
- Process exits cleanly (exit syscall works)

### Current Blocker

`user_pages[]` uses `void *` (16-bit on i16).  Page-pool addresses
above 64 KB are truncated, causing double-free on exit.  The
`elf16_loader` works around this by using `mm_page_*` directly and
not setting `PROC_IMAGE_SEG_OWNED`, but `proc_track_page()` still
stores truncated pointers.  Fixed by the page-index refactoring
(see [`memory_management.md`](../kernel/memory_management.md) §4).

### What's Fixed So Far

- `user_pages[]` converted to `page_id_t` — page tracking works.
- Timer ISR race fixed — `sched_start()` moved after direct IRET
  removed; idle thread (PID 0) gets proper context save.
- Process exits cleanly via `exit()` syscall.
- tty.c uses `mod_core.uart_*` wrappers on all targets (not direct
  function pointers) — fixes cross-segment function pointer issue.

### Steps (revised)

1. **User-space memory access abstraction** — implement
   `copy_from_user` / `copy_to_user` / `strncpy_from_user` as a
   uniform API on all targets.  See §8.1 below.

2. **Convert syscalls to use uaccess** — sys_write, sys_read, and
   path-taking syscalls use kernel bounce buffers instead of raw
   user pointers.

3. **Signal delivery for i16** — implement `sys_sigreturn` / signal
   trampoline.

4. **Fork / waitpid** — process segment duplication via
   `mem_region_page_read/write` for cross-segment copy.

5. **`--test ibmpc` in run.sh** — integrate with existing test harness.

### 8.1 User-Space Memory Access (`user_to_page`)

Syscall handlers currently receive user pointers as raw `void *` and
dereference them directly.  This is broken on i16 (user memory is in
a different segment) and will not scale to MMU or 64-bit targets.

An audit found **35+ syscalls with 60+ unresolved pointer arguments**.

#### Design

All user-space pointer arguments are resolved to a **page_id + offset
pair** before access.  No new files, no new cross-module APIs — the
existing `mem_region_page_read/write` (already in the `mod_core`
vtable) is the only mechanism used.

A single inline helper converts a user-space offset to a page
reference:

```c
typedef struct { page_id_t page; uint16_t off; } user_page_ref_t;

static inline user_page_ref_t user_to_page(const pcb_t *p,
                                           uint32_t user_off) {
  page_id_t base = proc_page_backed_base(p);
  uint32_t page_idx = user_off / PAGE_SIZE;
  uint16_t page_off = (uint16_t)(user_off % PAGE_SIZE);
  return (user_page_ref_t){ base + (page_id_t)page_idx, page_off };
}
```

`user_off` is a process-relative byte offset from the start of the
process's contiguous page allocation:

- **i16**: the raw 16-bit register value.  The user process runs with
  DS pointing at its page allocation base, so the register value *is*
  the offset from base.  No DS lookup or storage needed — the process
  segment is derivable from `proc_page_backed_base(current)`.
- **32-bit flat (no MMU)**: `user_off = (uint32_t)arg - base_linear`,
  where `base_linear = mem_region_page_linear(proc_page_backed_base(p))`.
- **Future MMU targets**: page-table walk from virtual address.

The resolution produces a `(page_id_t, uint16_t offset)` pair that
works with the existing `mem_region_page_read/write` on all targets.

#### I/O syscall changes

`fd_read` and `fd_write` in the VFS API change from
`(desc, char *buf, size_t n)` to
`(desc, page_id_t page, uint16_t off, size_t n)`.

VFS implementations (tty, pipe, tmpfs, romfs, etc.) use
`mem_region_page_read/write` to access the user buffer.  No bounce
copies.  The same code path works on all architectures:

```c
/* sys_write: resolve user buf, pass page ref to VFS */
user_page_ref_t ref = user_to_page(current, user_buf_off);
return mod_vfs.fd_write(desc, ref.page, ref.off, n);

/* Inside tty_write (VFS module): */
char ch;
mem_region_page_read(page, off + i, &ch, 1);
uart_putc(ch);
```

Internal callers (subsystem bridges, ktest) that pass kernel-space
buffers use `mem_region_ptr_to_page()` to get a page_id, or allocate
a kernel page for the buffer.

#### Path syscalls

Path arguments are copied into a kernel stack buffer before calling
VFS (the copy is unavoidable — VFS needs a NUL-terminated `const
char *`).  The copy uses `mem_region_page_read` directly:

```c
/* sys_open: read path from user page into kernel stack */
char kpath[VFS_PATH_MAX];
user_page_ref_t ref = user_to_page(current, user_path_off);
/* read across page boundaries in chunks */
mem_region_page_read(ref.page, ref.off, kpath, n);
int desc = mod_vfs.fd_open(kpath, flags, mode);
```

#### Struct-output syscalls

Syscalls that write structured results (stat, getcwd, pipe fds,
uname, timespec, etc.) fill a kernel-stack struct, then write it to
the user page via `mem_region_page_write`:

```c
/* sys_getcwd: fill kernel buf, write to user page */
char kbuf[64];
memcpy(kbuf, current->cwd, len + 1);
user_page_ref_t ref = user_to_page(current, user_buf_off);
mem_region_page_write(ref.page, ref.off, kbuf, len + 1);
```

#### `i16_syscall_dispatch` cleanup

The per-syscall pointer-resolution switch is **removed entirely**.
`i16_syscall_dispatch` passes raw 16-bit register values as
`frame[0..3]`.  Each syscall handler resolves its own pointer
arguments via `user_to_page`.

#### Affected syscalls

| Category | Syscalls | Pointer args | Access |
|----------|----------|-------------|--------|
| I/O | read, write, readv, writev | buf, iovec | page_read/write via VFS |
| FS paths | open, chdir, access, mkdir, unlink, rmdir, rename, readlink, mount, execve | path strings | page_read → kernel stack |
| FS data | stat, fstat, getdents, getcwd, llseek | output structs | page_write from kernel stack |
| Process | pipe, wait4, set_tid_address | fds[], status | page_write from kernel stack |
| Signals | rt_sigaction, rt_sigprocmask | sigaction structs | page_read + page_write |
| Time | gettimeofday, clock_gettime, clock_nanosleep | timespec structs | page_read + page_write |
| Terminal | ioctl | arg struct | page_read/write |
| Info | uname | utsname struct | page_write from kernel stack |

#### Migration order

1. Add `user_page_ref_t` and `user_to_page()` inline helper
   (in `proc.h`, no new files).
2. Change `fd_read`/`fd_write` VFS API to accept `page_id_t` +
   `uint16_t offset` instead of `char *`.  Update tty, pipe, tmpfs,
   romfs, devfs, ufs implementations.
3. Convert `sys_write` / `sys_read` to use `user_to_page`.
4. Convert path syscalls (page_read into kernel stack buffer).
5. Convert struct-output syscalls (page_write from kernel stack).
6. Remove `read_user_byte` from arch.h.
7. Remove per-syscall pointer switch from `i16_syscall_dispatch`.
8. Remove `(void *)(uintptr_t)` casts from `syscall_dispatch`.

---

## 9. Phase P-5b: Hard Disk (IDE/ATA) Boot Support

**Status**: Not started.

**Goal**: PPAP boots from an HDD image and mounts a UFS root volume,
using BIOS INT 13h for all disk I/O (no direct ATA/IDE driver).

1. **HDD block device driver** (`hdd_blk.c`) — clone of `floppy_blk.c`
   with dynamic CHS geometry queried via INT 13h AH=08h and drive
   number 0x80.  Register as "hd0" in the blkdev table.
2. **MBR partition table parser** (`mbr.c`) — read sector 0, parse the
   four 16-byte entries at offset 0x1BE.  Use a dedicated partition
   type byte (e.g. 0xA9) to identify the PPAP UFS partition.  The
   partition's start LBA becomes the base offset for the block device,
   same pattern as `UFS_FLOPPY_BASE`.
3. **Stage1 for HDD** (`stage1_hdd.S`) — MBR boot sector (446 bytes of
   code + 64-byte partition table + 0xAA55 signature).  Locates the
   active partition, loads stage2 from it, passes drive number 0x80.
4. **Stage2 dynamic geometry** — replace hardcoded 18 spt / 2 heads
   with values from INT 13h AH=08h so the same stage2 works for both
   floppy and HDD.
5. **target_ibmpc.c probe** — detect HDD at boot via INT 13h AH=08h
   (DL=0x80); if present, register "hd0" and prefer it over "fd0" for
   root mount.
6. **HDD image builder** (`mkhddimg.sh`) — create a raw disk image with
   MBR + single PPAP partition + UFS filesystem.

**Constraints**: CHS addressing only (no INT 13h extensions / LBA),
max ~504 MB (1024 cyl × 16 heads × 63 spt × 512 B).  This matches
the PC/XT target scope.

**Verification**: `qemu-system-i386 -hda ppap.img` boots to shell,
mounts UFS root from the HDD partition, runs runtests.

---

## 10. Phase P-6: V30 8080 Mode eCPU

**Status**: Not started.

**Goal**: CP/M-80 programs run on V30 hardware 8080 mode.

1. Implement `ecpu_8080_v30.c` — BRKEM/RETEM wrappers.
2. CP/M memory layout in a dedicated segment.
3. Test with 8080-clean CP/M programs (ED, PIP, STAT, DDT, MBASIC).

**Verification**: MBASIC runs, prints output, exits cleanly.

---

## 11. Phase P-7: DOS Subsystem

**Status**: Not started.

**Goal**: MS-DOS .COM programs run with INT 21h bridge.

1. Implement `dos_loader.c` — .COM and .EXE loading.
2. Implement `dos_bridge.c` — core INT 21h functions (~20 initially).
3. PSP construction, drive letter mapping.

**Verification**: "Hello, world" DOS .COM runs and prints output.

---

## 12. Phase P-8: Real Hardware

**Status**: Not started.

**Goal**: PPAP boots on physical V30 hardware (PC/XT or compatible).

| Issue | Mitigation |
|-------|-----------|
| V30 detection | Check for V30-specific instructions at startup |
| 8080 mode on non-V30 | Fall back to software Z80/8080 emulator |
| FDC timing | Use BIOS INT 13h initially; direct FDC driver later |
| CGA snow | Retrace-sync writes (only needed for direct video) |

**Verification**: PPAP boots and runs tests on physical hardware.

---

## 13. Phase P-9: Extended Features

- Software Z80 eCPU (fallback for Z80 CP/M programs on V30)
- Direct video driver (replacing BIOS INT 10h)
- Direct keyboard driver (replacing BIOS INT 16h)
- Direct ATA/IDE PIO driver (replacing BIOS INT 13h for HDD)
- DOS .EXE (MZ) multi-segment loading

---

## 14. Dependency Graph

```
P-1 through P-4b (complete — kernel boots, mounts UFS, scheduler runs)
  └─→ P-5 (user-space exec + tests)
        ├─→ P-5b (HDD boot via BIOS INT 13h)
        ├─→ P-6 (V30 8080 eCPU)
        └─→ P-7 (DOS subsystem)
              └─→ P-8 (real hardware)
                    └─→ P-9 (extended features)
```

P-5b, P-6, and P-7 can be developed in parallel after P-5.  P-8 requires
at least P-5 (tests passing on emulator) before attempting real hardware.

---

## 15. Risks and Open Questions

### 15.1 V30 Availability

V30 is required only for hardware 8080 mode.  On a standard 8086/8088 PC,
the same kernel works but falls back to the software 8080/Z80 emulator.
This should be a runtime detection, not a build-time switch.

### 15.2 Toolchain

`ia16-elf-gcc` (GCC port for 16-bit x86, actively maintained) is used
for all C and assembly code.  The Docker image `ppap/ia16` bundles the
toolchain and QEMU.  Build via `./scripts/run.sh --build ibmpc`.

### 15.3 Emulator for Development

- **QEMU** — `qemu-system-i386` via Docker.  Primary development
  emulator.  Does not emulate V30 8080 mode.
- **86Box** — cycle-accurate PC/XT emulation, supports V30 CPU selection,
  serial port passthrough, floppy images.  Recommended for V30/8080 testing.
- **PCem** — similar capabilities, slightly different UI.
- **MartyPC** — Rust-based cycle-accurate IBM PC 5150 emulator.

### 15.4 8080 Mode I/O Port Conflict

8080 `IN`/`OUT` instructions access real x86 I/O ports.  If a CP/M program
does `OUT 0x20, A` it would send an EOI to the PIC.  Mitigation: the CP/M
BIOS should not expose I/O ports directly; programs that bypass BIOS for
I/O are inherently non-portable and unlikely to work on any non-native
platform.

### 15.5 Interrupt Handling During 8080 Mode

When the V30 is executing in 8080 mode, hardware interrupts (timer, UART)
are still delivered using 8080 interrupt handling semantics.  The timer ISR
needs to work correctly in both x86 and 8080 contexts.

Options:
- Disable interrupts (`DI` in 8080) before entering 8080 mode; rely on
  periodic `RETEM` exits (via BDOS calls) for timeslicing.
- Keep interrupts enabled and ensure the timer ISR can handle being invoked
  in 8080 mode (execute `RETEM` to return to x86, then handle the interrupt).

Option 1 is simpler and matches PPAP's existing eCPU design where the
emulator loop runs with interrupts disabled and checks for pending signals
at trap points.

---

## 16. Related Documentation

- [docs/kernel/overview.md](../kernel/overview.md) — PPAP kernel architecture
- [docs/kernel/trace.md](../kernel/trace.md) — Trace and debug subsystem
- [docs/kernel/syscall.md](../kernel/syscall.md) — System call reference
- [docs/proposals/x68k_port.md](x68k_port.md) — X68000 target port (analogous m68k effort)
- [docs/kernel/memory_management.md](../kernel/memory_management.md) — Memory management architecture
- [docs/archive/history/target-68000-plan.md](../archive/history/target-68000-plan.md) — Original m68k planning
