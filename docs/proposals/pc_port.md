# IBM PC Target Port Plan (V30 with 8080 Mode)

Porting PPAP to the IBM PC platform using the NEC V30 (μPD70116) CPU.
The V30 is 8086-compatible and includes a hardware 8080 emulation mode,
which maps naturally onto PPAP's eCPU architecture for running CP/M-80
programs without a software interpreter.

---

## 1. Goals and Scope

### 1.1 Primary Goal

Produce a bootable PPAP system on an IBM PC/XT-class machine with V30 that:

- Boots from a floppy disk (360 KB or 1.44 MB) with a two-stage bootstrap.
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
- Boot from a hard disk (IDE/ATA) or CF card.
- Direct VGA text-mode driver replacing BIOS calls.
- Keyboard input via INT 16h / direct 8042 controller access.
- Full Z80 CP/M support via software eCPU (for programs using Z80 extensions
  beyond the 8080 instruction set).

### 1.3 Out of Scope

- Graphics modes (VGA/EGA/CGA graphics, VESA).
- Sound (PC speaker melody, Sound Blaster, AdLib).
- 80286/80386 protected mode — the kernel runs entirely in real mode.
- Network drivers (NE2000, etc.).
- Hard disk partitioning (MBR/GPT) — initial target is floppy-only.

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

All kernel modules share **DS=0** for data.  Each module's code
lives in its own code segment (separate CS).  Cross-module calls
use `lcall`/`lret` stubs — DS stays unchanged, so pointer arguments
work without serialization.

```
Segment     Contents
────────    ─────────────────────────────────
CS=0x0060   Core code: main, klog, mm, proc, sched, syscall, blkdev
CS=0x1000   VFS code: vfs, namei, romfs, tmpfs, devfs, procfs, ufs
CS=????     Exec code: exec, loaders (future)
DS=SS=0     Shared data: all modules' .data, .bss, stack, page pool
```

Each module's code must fit in 64 KB.  Total data+BSS from all
modules must fit in 64 KB (shared DS=0; currently ~6 KB).

This is **not** medium or compact model — those are broken in
ia16-elf-ld 2.39 (R_386_16 overflow, linker segfault).  And it is
**not** isolated DS per module — that would require argument
serialization at every cross-module call (the C compiler cannot
access data via ES instead of DS).

A **segment manager** in the core module tracks each module's code
segment base at runtime.  Stage2 loads module binaries and records
their addresses; the core reads this at boot.

### 3.3 User Process Memory

Each user process is assigned one or more 64 KB segments:

| Format | Segments | Layout |
|--------|----------|--------|
| .COM (CP/M 8080) | 1 segment (64 KB max) | Loaded at offset 0x0100 in one segment; CS=DS=ES=SS |
| .COM (DOS) | 1 segment (64 KB max) | Loaded at offset 0x0100; PSP at 0x0000 |
| .EXE (DOS MZ) | N segments | Code segment(s) + data segment + stack segment |
| PPAP native | 1–2 segments | Flat binary or PIE-like within segments |

### 3.4 Page Allocator Adaptation

The page allocator currently returns flat 32-bit addresses.  On x86 real
mode, it must return paragraph-aligned addresses that can be expressed as
`segment:0000`.

- 4 KB page = 256 paragraphs (0x100 segment units)
- 1 MB address space / 4 KB = 256 possible pages
- Minus kernel, BIOS, video memory → ~200 usable pages
- Comparable to RP2040 (51 pages) and X68000 1 MB (250 pages)

Page addresses are stored as segment values internally.  Conversion to
linear address: `page_seg << 4`.  This is purely arithmetic.

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
reprograms it to 100 Hz during `target_early_init()`:

```c
outb(0x43, 0x36);       /* Channel 0, lobyte/hibyte, mode 3 (square wave) */
outb(0x40, 0x9C);       /* Divisor low byte */
outb(0x40, 0x2E);       /* Divisor high byte */
```

IRQ 0 (INT 08h) fires at 100 Hz.  The ISR increments the tick counter
and triggers context switch, same as SysTick on ARM and MFP Timer-C on
X68000.

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

## 5. Architecture Layer: 8086

A new architecture directory `src/arch/i8086/` provides the 8086-specific
low-level code, analogous to `src/arch/arm_m/` and `src/arch/m68k/`.

### 5.1 New Files

```
src/arch/i8086/
  boot.S            — Reset vector, GDT (none in real mode), IVT setup
  switch.S          — Context switch: save/restore AX-DI, segment regs, FLAGS
  trap.S            — INT 30h syscall handler (or chosen software interrupt)
  arch_i8086.h      — Register frame struct, inline I/O (inb/outb)
  cpu_i8086.h       — CLI/STI wrappers, HLT for idle
  probe_mem.S       — Detect conventional memory size (INT 12h or probe)
```

### 5.2 Syscall Convention

Use a dedicated software interrupt for PPAP syscalls:

```nasm
; User-space syscall stub
; AX = syscall number, BX/CX/DX/SI/DI = arguments
; Return value in AX
INT 30h
```

INT 30h is unused by BIOS and DOS, making it a clean choice.  The trap
handler in `trap.S` saves the user register frame, switches to the kernel
stack (SS:SP), dispatches via `syscall_dispatch()`, and returns via `IRET`.

### 5.3 Context Switch

```nasm
; switch.S — context switch on x86 real mode
; Save callee-saved registers + segment registers to PCB
;
; PCB layout:
;   +0   SP
;   +2   SS
;   +4   BX
;   +6   SI
;   +8   DI
;   +10  BP
;   +12  DS
;   +14  ES
;   +16  FLAGS (saved via PUSHF)
;
; Total: 18 bytes per PCB save area

ppap_switch:
    pushf
    push es
    push ds
    push bp
    push di
    push si
    push bx
    ; Save SP and SS to current PCB
    mov  [current_pcb + 0], sp
    mov  [current_pcb + 2], ss
    ; Load next PCB
    mov  sp, [next_pcb + 0]
    mov  ss, [next_pcb + 2]
    pop  bx
    pop  si
    pop  di
    pop  bp
    pop  ds
    pop  es
    popf
    ret
```

### 5.4 Memory Map (640 KB Conventional)

| Address | Size | Use |
|---------|------|-----|
| 0x00000–0x003FF | 1 KB | Interrupt Vector Table (256 × 4B far pointers) |
| 0x00400–0x004FF | 256 B | BIOS Data Area (BDA) — must not clobber |
| 0x00500–0x07BFF | ~30 KB | Free (stage1/stage2 load area, then freed) |
| 0x07C00–0x07DFF | 512 B | Stage1 boot sector (BIOS loads here) |
| 0x07E00–0x0FFFF | ~33 KB | Stage2 + scratch; freed after boot |
| 0x10000–0x1FFFF | 64 KB | Kernel code segment (CS = 0x1000) |
| 0x20000–0x2FFFF | 64 KB | Kernel data segment (DS = SS = 0x2000) |
| 0x30000–0x9FBFF | ~448 KB | Page pool (~112 pages × 4 KB) |
| 0x9FC00–0x9FFFF | 1 KB | Extended BIOS Data Area (EBDA) — preserve |
| 0xA0000–0xBFFFF | 128 KB | Video RAM (CGA/MDA text at 0xB8000) |
| 0xC0000–0xEFFFF | 192 KB | ROM expansion area |
| 0xF0000–0xFFFFF | 64 KB | BIOS ROM |

~112 user pages is comparable to RP2040 (51) and X68000 1 MB (250).

---

## 6. V30 8080 Mode as eCPU

This is the key differentiator of the V30 port.  Instead of the software
Z80 interpreter (`ecpu_z80.c`, ~1400 lines), the V30's hardware 8080 mode
provides native instruction execution for 8080-clean CP/M programs.

### 6.1 Architecture Mapping

| PPAP eCPU Concept | Software Z80 (current) | V30 8080 Mode |
|-------------------|----------------------|---------------|
| Emulator loop | `ecpu_z80_run()` in C | `BRKEM imm8` — CPU runs 8080 natively |
| Trap on CALL 0x0005 | Check PC after decode | 8080 `CALL 0x0005` → hits `RETEM` at address 0x0005 |
| Trap on I/O IN/OUT | Decoded in emulator loop | 8080 `IN`/`OUT` → real x86 I/O port access (see §6.3) |
| Register access | `cpu->regs[]` struct | x86 registers (AL=A, CH=B, CL=C, etc.) |
| Memory access | `cpu->mem[]` array | Direct memory in the 64 KB DS segment |
| Context switch cost | Zero (it's just C data) | `BRKEM`/`RETEM` save/restore (~30 clocks) |

### 6.2 BDOS/BIOS Trap Mechanism

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

### 6.3 I/O Port Handling

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

### 6.4 Memory Layout for 8080 Mode

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

### 6.5 Dual eCPU Strategy

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

The ptrace debug surface concept already supports this:

```
surface=real  → V30 x86 registers
surface=ecpu  → 8080 or Z80 registers (depending on which eCPU is active)
```

---

## 7. DOS Subsystem

The extended goal: run MS-DOS .COM and .EXE programs with a DOS
personality bridge, analogous to the Human68k bridge on X68000.

### 7.1 Program Formats

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

### 7.2 INT 21h Bridge

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

### 7.3 Drive Letter Mapping

Same pattern as Human68k:

```
A: → /a/   (floppy drive 0)
B: → /b/   (floppy drive 1)
C: → /c/   (hard disk — if present)
```

Current working directory is tracked per-drive in the bridge, matching
DOS semantics.

### 7.4 PSP Construction

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

### 7.5 Trace Integration

```c
#define PPAP_TRACE_ABI_DOS_INT21  5   /* New ABI tag */
#define PPAP_TRACE_REGSET_8086    4   /* AX,BX,CX,DX,SI,DI,BP,SP,CS,DS,ES,SS,IP,FLAGS */
```

The trace tool and pdb gain DOS awareness:

```
$ trace --subsys hello.com
[subsys:dos] INT 21h AH=09h PRINT_STRING DS:DX=0x0180
[subsys:dos] INT 21h AH=4Ch EXIT code=0
```

---

## 8. New Files and Directory Layout

```
src/arch/i8086/
  boot.S              — IVT setup, initial stack, jump to kmain
  switch.S            — Context switch (save/restore regs + segments)
  trap.S              — INT 30h syscall handler
  arch_i8086.h        — Register frame, inb/outb, CLI/STI
  cpu_i8086.h         — CPU control macros
  probe_mem.S         — Conventional memory detection
  v30_8080.S          — BRKEM/RETEM wrappers for eCPU 8080 mode

src/target/ibmpc/
  CMakeLists.txt      — Build rules, memory layout, feature flags
  ibmpc_kernel.ld     — Core module linker script (DS=0, reserves VFS data)
  ibmpc_vfs.ld        — VFS module linker script (.text at 0, .data at DS=0)
  target_ibmpc.c      — target hooks: early_init, console_init, late_init
  drivers/
    uart_8250.c       — 8250 UART driver (COM1 serial console)
    timer_pit.c       — 8253/8254 PIT driver (100 Hz tick)
    pic_8259.c        — 8259A PIC driver (IRQ mask, EOI)
    fdc_pc.c          — µPD765A floppy block driver (via BIOS INT 13h initially)
    kbd_pc.c          — Keyboard (via BIOS INT 16h initially)
    video_pc.c        — Text output (via BIOS INT 10h initially; direct VGA later)

src/kernel/subsys/
  dos_bridge.c        — INT 21h → PPAP syscall bridge
  dos_bridge.h
  dos_loader.c        — .COM and .EXE (MZ) binary loader
  dos_loader.h

src/kernel/ecpu/
  ecpu_8080_v30.c     — V30 hardware 8080 mode eCPU wrapper
  ecpu_8080_v30.h     — Register mapping, BRKEM/RETEM interface

tools/
  mkpcfloppy/
    mkpcfloppy.c      — Produces bootable .IMG floppy image:
                        boot sector + stage2 + UFS partition
```

Changes to existing files:

| File | Change |
|------|--------|
| `src/common/ptrace.h` | Add `PPAP_TRACE_REGSET_8086`, `PPAP_TRACE_ABI_DOS_INT21` |
| `src/kernel/ecpu/ecpu.h` | Add 8080 eCPU type |
| `src/kernel/subsys/subsys.c` | Register DOS subsystem |
| `scripts/build.sh` | Add `ibmpc` as valid target |
| `scripts/run.sh` | Add `ibmpc` target (runs under 86Box or PCem) |

---

## 9. Boot Sequence

### 9.1 BIOS Boot

1. BIOS POST completes.
2. BIOS reads boot sector (512 bytes from floppy sector 0) into 0x7C00.
3. BIOS jumps to 0x7C00 (Stage1).

### 9.2 Stage1 (512 B, boot sector at 0x7C00)

Implemented in `src/target/ibmpc/boot/stage1.S`.

1. BIOS loads sector 0 to 0x7C00, jumps here.
2. Sets up segments (DS=ES=SS=0), stack at 0x7C00.
3. Prints "Pi" via BIOS INT 10h (progressive banner).
4. Reads 8 sectors (stage2) from floppy to 0xC000 via INT 13h.
5. Passes boot drive in DL, jumps to 0x0000:0xC000.

Includes BPB (BIOS Parameter Block) at offset 3–61 for 1.44 MB format.

### 9.3 Stage2 (up to 4 KB at 0xC000)

Implemented in `src/target/ibmpc/boot/stage2.c` + `stage2_entry.S`.

1. Prints "PA" (banner continues).
2. Reads UFS superblock from floppy sector 9 (start of UFS partition).
3. Walks directory: root → `/boot/` → `/boot/kernel`.
4. Loads kernel directly to 0x0600 (linked address) via INT 13h.
   Supports both direct blocks (10 × 4 KB = 40 KB) and indirect
   blocks (for kernels >40 KB).
5. Far jumps to kernel at 0x0000:0x0600.

**Floppy layout** (1.44 MB, 2880 × 512 B sectors):

| Sectors | Contents |
|---------|----------|
| 0 | Stage1 boot sector (512 B) |
| 1–8 | Stage2 UFS loader (4 KB) |
| 9+ | UFS partition with `/boot/kernel` |

Assembled by `scripts/mkpcimg.sh` using `tools/mkufs/mkufs`.

### 9.4 Kernel Entry (kmain)

Implemented in `src/arch/i16/boot.S` → `src/kernel/main.c`.

boot.S sets DS=ES=SS=0, zeroes BSS, sets SP to `__stack_top`, calls
kmain(). The kernel follows the standard PPAP boot sequence:

```c
void kmain(void) {
    target_early_init();    /* UART + BIOS console, prints "Po booting..." */
    mm_init();              /* Page pool from __page_pool_start to 640 KB */
    proc_init();            /* Process table (8 slots) */
    vfs_init();             /* VFS + file pool */
    target_late_init();     /* PIT 100 Hz timer */
    sched_start();          /* Enable interrupts, enter idle loop */
}
```

---

## 9.5 Memory Layout by Boot Stage

### Stage1 running (loaded by BIOS at 0x7C00)

```
0x00000-0x003FF  IVT (256 vectors × 4 bytes, BIOS-owned)
0x00400-0x004FF  BIOS Data Area
0x00500-0x07BFF  Free conventional memory
0x07C00-0x07DFF  Stage1 code (512 B, loaded by BIOS)
0x07C00          Stack (grows down from 0x7C00, below stage1)
```

Stage1 prints "Pi" (progressive banner), loads 8 sectors (stage2) from
floppy to 0xC000 via INT 13h, and jumps there.

### Stage2 running (loaded by stage1 at 0xC000)

```
0x00000-0x004FF  IVT + BIOS Data Area (preserved)
0x00500-0x005FF  Free
0x00600-0x0BFFF  Kernel load area (loaded directly to linked address)
0x0C000          Stack (grows down from 0xC000, below stage2)
0x0C000-0x0CFFF  Stage2 code + data (up to 4 KB)
0x0D000-0x0DFFF  BUF — UFS metadata scratch buffer (4 KB)
0x0E000-0x0EFFF  IBUF — indirect block scratch buffer (4 KB)
```

Stage2 prints "PA", reads the UFS filesystem on the floppy (sector 9+),
finds `/boot/kernel`, and loads it directly to 0x0600 — no relocation
needed since stage2 is above the kernel at 0xC000.  Supports indirect
blocks (kernels >40 KB).  Then jumps to 0x0000:0x0600.

**Key constraint**: stage2 + buffers must be above the kernel end
(currently 0x0600 + 46 KB = 0xBC2E).  Stage2 at 0xC000 satisfies this.

### Kernel running (linked at 0x0600)

```
0x00000-0x003FF  IVT (kernel installs INT 08h timer)
0x00400-0x005FF  BIOS Data Area + free gap
0x00600          Kernel .text.entry (boot.S: _start)
  ...            .text (~42 KB), .rodata, .data (~0.5 KB)
  ...            .bss (~5.8 KB, includes proc_table ~4.3 KB)
  ...            Stack (2 KB)
0x?????          __page_pool_start (4 KB-aligned, after stack)
  ...  -0x9FBFF  Page pool (conventional RAM ceiling - EBDA)
0x9FC00-0x9FFFF  EBDA (1 KB, preserved)
0xA0000-0xBFFFF  Video RAM
0xC0000-0xFFFFF  ROM / BIOS
```

Kernel prints "Po booting... [ibmpc]" — completing the progressive
banner "PiPAPo booting..." across all three stages.

### Size Constraint

The kernel binary (text + rodata + data) plus BSS and stack must fit
between 0x0600 and 0xFFFF (~63 KB) due to 16-bit near pointer limit.
Current P-3b core kernel:

| Component | Size |
|-----------|------|
| .text     | ~42 KB |
| .data     | ~0.5 KB |
| .bss      | ~5.8 KB (proc_table is ~4.3 KB alone) |
| stack     | 2 KB |
| **Total** | **~50 KB** |

This leaves ~13 KB headroom.  Adding blkdev + floppy driver pushes
past 64 KB, requiring the segment split.

### Segment Split: Shared DS, Separate CS

All modules share DS=0 for data.  Each module's code is in its own
segment (separate CS).  Cross-module calls use `lcall`/`lret` —
DS stays unchanged, so pointer arguments work without serialization.

```
Core code segment (CS=0x0060):    VFS code segment (CS=0x1000):
──────────────────────────────    ──────────────────────────────
caller()                          vfs_init_entry:
  call mod_vfs.init()               call vfs_init   ; near
  ; near call to ──►                vfs_init:
vfs_init_caller_stub:                  ...  (DS=0, shared data)
  lcall *[vfs_fptrs + 0] ─ far ─►    ret            ; near
  ret  ◄─────────────────── far ── lret
──────────────────────────────    ──────────────────────────────
```

**Caller-side stub** (in core segment):
```nasm
vfs_init_caller_stub:
    lcall *vfs_fptrs + 0    ; indirect far call [offset:segment]
    ret
```

**Target-side stub** (in VFS segment):
```nasm
vfs_init_entry:
    call vfs_init            ; near call to real function
    lret                     ; far return to caller
```

No DS manipulation — both sides use DS=0 for all data access.
The real function uses normal `ret`.  Only the assembly stubs
know about segments.

**Why shared DS, not isolated DS per module?**
- Isolated DS requires argument serialization (copy-in/copy-out)
  at every cross-module call — the C compiler cannot access data
  via ES instead of DS
- Shared DS: pointer arguments just work, zero overhead

**Why not medium model?**
- ia16-elf-ld 2.39 is broken (R_386_16 overflow, linker segfault)

**What goes where:**

| Binary | Segment | Contents | Code budget |
|--------|---------|----------|-------------|
| Core | CS=0x0060 | main, klog, mm, proc, sched, syscall, blkdev, stubs | ≤64 KB code |
| VFS | CS=0x1000 | vfs, namei, romfs, tmpfs, devfs, procfs, ufs, stubs | ≤64 KB code |

**Memory layout:**

```
0x00000-0x005FF  IVT + BIOS Data Area
0x00600-0x?????  Core .text (code only, ~26 KB)
  .rodata, .data, .bss  (shared DS=0, after core .text)
  stack (2 KB)
  __page_pool_start (4 KB aligned)
0x10000-0x?????  VFS .text (code only, ~27 KB, CS=0x1000)
  VFS .data, .bss linked into DS=0 at reserved addresses
...    -0x9FBFF  Page pool (conventional RAM ceiling - EBDA)
0x9FC00-0x9FFFF  EBDA (1 KB)
0xA0000-0xFFFFF  Video RAM + ROM
```

**VFS data in shared DS=0 — concrete mechanism:**

VFS data/BSS must be at known addresses in DS=0.  The VFS linker
script links .text at offset 0 (VFS CS), but .data/.bss at a fixed
DS=0 address (e.g. 0xA000):

```ld
/* ibmpc_vfs.ld */
SECTIONS {
  . = 0x0000;
  .text   : { *(.text*) }      /* VFS code segment, offset 0 */
  .rodata : { *(.rodata*) }    /* constants in code segment */

  . = 0xA000;                  /* switch to DS=0 address space */
  .data   : { *(.data*) }      /* VFS globals at DS:0xA000+ */
  .bss    : { *(.bss*) }       /* VFS BSS at DS:0xA0xx+ */
}
```

The compiler generates `mov ax, ds:[0xA0xx]` for VFS globals.
Since DS=0 at runtime, this correctly accesses linear 0xA0xx.

**Build steps:**
1. Link VFS ELF with the above script (.text at 0, .data at 0xA000)
2. Extract .text only: `objcopy -j .text -j .rodata -O binary` → VFS code binary
3. Extract .data only: `objcopy -j .data -O binary` → VFS data blob
4. Stage2 loads VFS code to 0x1000:0000 (CS=0x1000)
5. Stage2 (or core init) copies VFS data blob to DS:0xA000
6. Core init zeroes VFS BSS range (0xA000+data_size to 0xA000+data+bss)

The core linker script reserves 0xA000-0xBFFF for VFS data (8 KB):

```ld
/* ibmpc_kernel.ld */
  __page_pool_start = .;
  . = 0xA000;
  __vfs_data_reserved_start = .;
  . += 8192;   /* 8 KB reserved for VFS .data + .bss */
  __vfs_data_reserved_end = .;
```

Stage2 loads both `/boot/kernel` and `/boot/kernel_vfs` from the
UFS floppy.  A `mod_info_t` block at 0x0500 tells the kernel where
each module was loaded.

---

## 10. Implementation Phases

### Phase P-1: Target Skeleton and BIOS Console ✓

**Status**: Complete.

- `src/arch/i16/` — arch dispatch headers, boot.S, cpu.h (inb/outb)
- `src/target/ibmpc/` — target hooks, linker script, BIOS + UART drivers
- Boot sector at 0x7C00, COM1 serial + BIOS INT 10h console
- Docker toolchain: `ppap/ia16` image with ia16-elf-gcc + QEMU i386
- `./scripts/run.sh ibmpc` runs on QEMU via Docker

### Phase P-2: Timer and Context Switch ✓

**Status**: Complete.

- PIT Channel 0 at 100 Hz (mode 3, divisor 11932)
- 8259A PIC: BIOS-initialized, unmask IRQ 0 only
- switch.S: timer ISR saves 9 regs + FLAGS/CS/IP (24-byte frame),
  flag-driven context switch (m68k/RISC-V pattern)
- Standalone scheduler with two test processes printing A/B

### Phase P-3a: Two-Stage Bootstrap ✓

**Status**: Complete.

- stage1.S: 512 B boot sector with BPB, loads stage2 to 0xC000
- stage2.c: UFS reader (direct + indirect blocks), loads kernel
  directly to 0x0600, supports kernels >40 KB
- mkpcimg.sh: assembles 1.44 MB floppy (stage1 + stage2 + UFS)
- Progressive banner: stage1 "Pi" + stage2 "PA" + kernel "Po booting..."

### Phase P-3b: Full Kernel Integration ✓

**Status**: Complete (kernel boots to idle).

- Full PPAP kernel compiles with ia16-elf-gcc (46 KB binary)
- Shared kernel changes: `uint32_t` → `uintptr_t` for address fields
  in page.h/c, proc.h/c, sched.c, sys_mem.c (all arches benefit)
- `__ia16__` arch guards in spinlock.h, cpu.h, cpu_native.c, sched.c,
  proc.h, page.h, loader.c, sys_proc.c
- Minimal string.c (memset/memcpy/strlen/strcmp etc. — no libc)
- Kernel boots: MM init, proc table, VFS, CPU, PIT timer, scheduler
- klog MM output shows wrong values (32-bit format on 16-bit — cosmetic)

**Not yet working**: user-space exec (needs module system for code >64 KB),
signal delivery (stub), ELF loader (disabled for i16).

### Phase P-4a: Kernel Module System ✓

**Status**: Complete.

Platform-agnostic module system with explicit API surfaces:

- `MOD_DECLARE`/`MOD_DEFINE` macros in `kernel/common/mod/module.h`
- mod_vfs (11 functions), mod_exec (1), mod_core (6)
- VFS callers migrated to `mod_vfs.init()` syntax on all platforms
- Boundary enforcement script (`check_module_boundaries.sh`)
- `kernel/common/` directory for shared headers (mod/, errno.h, spinlock.h)

### Phase P-4b: i16 Segment Split + Floppy Mount (in progress)

**Goal**: Split kernel into separate code segments, mount UFS root.

**Architecture**: Shared DS=0 for all data, separate CS per module.
Pointer arguments work without serialization.  See §9.5 for details.

**Done:**

| Item | Details |
|------|---------|
| Segment manager | `seg.h`/`seg.c` — runtime table of module code segments |
| Separate CMake targets | ppap_ibmpc (core 26 KB) + ppap_ibmpc_vfs (VFS 27 KB) |
| Two-level stubs | vfs_stubs.S (caller), vfs_entries.S (target), core_stubs.S, core_entries.S |
| VFS module header | Magic + 11 entry offsets at offset 0, read by core at boot |
| Stage2 multi-load | `load_file_far()` loads VFS to 0x1000:0000 via INT 13h |
| mod_info protocol | Stage2 writes module table at 0x0500, core reads at boot |
| mkpcimg.sh | Packages both binaries into UFS floppy |
| Floppy block device | `floppy_blk.c` — INT 13h driver, registers as "fd0" |
| Core boot verified | Core boots, prints "SEG: VFS module loaded", MM/PROC/PIT init |

**Remaining:**

1. **Remove DS switching from stubs** — stubs still have push/pop ds
   from the earlier isolated-DS design.  Remove since DS=0 is shared.
2. **VFS data in DS=0** — VFS linker script must place .data/.bss at
   a reserved DS=0 address (e.g. 0xA000).  Build extracts code and
   data as separate binaries (objcopy).  See §9.5 for details.
3. **Core linker reserves VFS data range** — 0xA000-0xBFFF (8 KB)
4. **Stage2 loads VFS data blob** to DS:0xA000, zeroes BSS
5. **Wire floppy mount** — target_mount_rootfs() calls mod_vfs.mount
   via far stub → VFS mounts UFS from fd0
6. **Test end-to-end** — boot to "Hello from user!"

**Verification**: Kernel mounts floppy UFS, loads /sbin/init, prints
"Hello from user!".

### Phase P-5: User-Space Exec and Tests

**Goal**: Full user-space support, `runtests` passes.

1. Signal delivery for i16
2. More syscalls (fork, waitpid, pipe)
3. `--test ibmpc` in run.sh

**Verification**: "ALL TESTS PASSED" in serial log.

### Phase P-6: V30 8080 Mode eCPU

**Goal**: CP/M-80 programs run on V30 hardware 8080 mode.

1. Implement `ecpu_8080_v30.c` — BRKEM/RETEM wrappers.
2. CP/M memory layout in a dedicated segment.
3. Test with 8080-clean CP/M programs (ED, PIP, STAT, DDT, MBASIC).

**Verification**: MBASIC runs, prints output, exits cleanly.

### Phase P-7: DOS Subsystem

**Goal**: MS-DOS .COM programs run with INT 21h bridge.

1. Implement `dos_loader.c` — .COM and .EXE loading.
2. Implement `dos_bridge.c` — core INT 21h functions (~20 initially).
3. PSP construction, drive letter mapping.

**Verification**: "Hello, world" DOS .COM runs and prints output.

### Phase P-8: Real Hardware

**Goal**: PPAP boots on physical V30 hardware (PC/XT or compatible).

| Issue | Mitigation |
|-------|-----------|
| V30 detection | Check for V30-specific instructions at startup |
| 8080 mode on non-V30 | Fall back to software Z80/8080 emulator |
| FDC timing | Use BIOS INT 13h initially; direct FDC driver later |
| CGA snow | Retrace-sync writes (only needed for direct video) |

**Verification**: PPAP boots and runs tests on physical hardware.

### Phase P-8: Extended Features

- Software Z80 eCPU (fallback for Z80 CP/M programs on V30)
- Direct video driver (replacing BIOS INT 10h)
- Direct keyboard driver (replacing BIOS INT 16h)
- Hard disk support (IDE/ATA or CF card)
- DOS .EXE (MZ) multi-segment loading

---

## 11. Risks and Open Questions

### 11.1 Kernel Size vs. 64 KB Code Segment

The PPAP kernel is ~80–150 KB on m68k with all features.  On 8086, code
may be larger (16-bit addressing needs more instructions for 32-bit
arithmetic) or smaller (simpler ABI).  If it exceeds 64 KB, options:

1. Disable optional features (eCPU Z80 software emulator, DOS subsystem)
   to stay within 64 KB.
2. Use compact model (multiple CS values, far calls between modules).
3. Split kernel into a resident core (≤64 KB) and loadable overlays.

Option 1 is recommended initially.  The V30 port can start minimal.

### 11.2 V30 Availability

V30 is required only for hardware 8080 mode.  On a standard 8086/8088 PC,
the same kernel works but falls back to the software 8080/Z80 emulator.
This should be a runtime detection, not a build-time switch.

### 11.3 Toolchain

PPAP's current build uses `arm-none-eabi-gcc` and `m68k-elf-gcc`.  For
8086 real mode, options:

- **ia16-elf-gcc** — GCC port for 16-bit x86 (actively maintained,
  available via `gcc-ia16` packages).  Recommended.
- **OpenWatcom** — Classic 16-bit x86 C compiler.  Good codegen but
  different calling convention.
- **NASM/FASM + C** — Assembly for arch layer, C via ia16-elf-gcc for
  kernel.

Recommended: `ia16-elf-gcc` for C code, NASM for assembly files in
`src/arch/i8086/`.

### 11.4 Emulator for Development

- **86Box** — cycle-accurate PC/XT emulation, supports V30 CPU selection,
  serial port passthrough, floppy images.  Recommended primary emulator.
- **PCem** — similar capabilities, slightly different UI.
- **QEMU** — `qemu-system-i386` with `-cpu 486` can test 8086 code but
  does not emulate V30 8080 mode.  Useful for non-8080 testing only.
- **MartyPC** — Rust-based cycle-accurate IBM PC 5150 emulator.

86Box is recommended because it explicitly supports V30 CPU selection
and has good debugging facilities.

### 11.5 8080 Mode I/O Port Conflict

8080 `IN`/`OUT` instructions access real x86 I/O ports.  If a CP/M program
does `OUT 0x20, A` it would send an EOI to the PIC.  Mitigation: the CP/M
BIOS should not expose I/O ports directly; programs that bypass BIOS for
I/O are inherently non-portable and unlikely to work on any non-native
platform.

### 11.6 Interrupt Handling During 8080 Mode

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

## 12. Dependency Graph

```
P-1 (console + kernel build)
  └─→ P-2 (PIT timer + context switch)
        └─→ P-3 (stage1/2 + floppy image)
              └─→ P-4 (integration tests)
                    ├─→ P-5 (V30 8080 eCPU)
                    └─→ P-6 (DOS subsystem)
                          └─→ P-7 (real hardware)
                                └─→ P-8 (extended features)
```

P-5 and P-6 can be developed in parallel after P-4.  P-7 requires at least
P-4 (basic tests passing on emulator) before attempting real hardware.

---

## 13. Related Documentation

- [docs/kernel/overview.md](../kernel/overview.md) — PPAP kernel architecture
- [docs/kernel/trace.md](../kernel/trace.md) — Trace and debug subsystem
- [docs/kernel/syscall.md](../kernel/syscall.md) — System call reference
- [docs/proposals/x68k_port.md](x68k_port.md) — X68000 target port (analogous m68k effort)
- [docs/archive/history/target-68000-plan.md](../archive/history/target-68000-plan.md) — Original m68k planning
