# V30 8080 Emulation Mode Support

> V30/V20-specific design for running CP/M-80 programs via hardware 8080
> emulation.  See [ia16.md](../targets/ia16.md) for the base PC/XT port.

---

## 1. V30 CPU Overview

### 1.1 8086 Compatibility

The NEC V30 (μPD70116) is a pin-compatible and instruction-compatible
replacement for the Intel 8086.  It runs all 8086 code unchanged, with
slightly improved cycle counts on many instructions.

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

### 1.2 Hardware 8080 Emulation Mode

The V30 includes dedicated microcode for executing Intel 8080 instructions
natively.  This is distinct from Z80 — the 8080 is a strict subset.

**Entering 8080 mode: `BRKEM imm8`**

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

**Exiting 8080 mode: `RETEM`**

```nasm
RETEM           ; 8080 opcode: ED FD
```

`RETEM` is a two-byte 8080 instruction (using an undefined 8080 opcode
sequence `ED FD`).  When executed:

1. The V30 restores the saved x86 state from the stack.
2. The instruction decoder switches back to x86 mode.
3. Execution continues at the x86 instruction following the original `BRKEM`.

### 1.3 8080 vs Z80 — What Works, What Doesn't

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

## 2. V30 8080 Mode as eCPU

Instead of the software Z80 interpreter (`ecpu_z80.c`, ~1400 lines), the
V30's hardware 8080 mode provides native instruction execution for
8080-clean CP/M programs.

### 2.1 Architecture Mapping

| PPAP eCPU Concept | Software Z80 (current) | V30 8080 Mode |
|-------------------|----------------------|---------------|
| Emulator loop | `ecpu_z80_run()` in C | `BRKEM imm8` — CPU runs 8080 natively |
| Trap on CALL 0x0005 | Check PC after decode | 8080 `CALL 0x0005` → hits `RETEM` at address 0x0005 |
| Trap on I/O IN/OUT | Decoded in emulator loop | 8080 `IN`/`OUT` → real x86 I/O port access (see §2.3) |
| Register access | `cpu->regs[]` struct | x86 registers (AL=A, CH=B, CL=C, etc.) |
| Memory access | `cpu->mem[]` array | Direct memory in the 64 KB DS segment |
| Context switch cost | Zero (it's just C data) | `BRKEM`/`RETEM` save/restore (~30 clocks) |

### 2.2 BDOS/BIOS Trap Mechanism

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

### 2.3 I/O Port Handling

The 8080 `IN` and `OUT` instructions in V30 8080 mode execute as real x86
I/O port accesses.

**Option A — Unused port range (recommended)**:
Map CP/M virtual I/O ports to a range of x86 I/O ports that are unused on
a standard PC (e.g., 0x300–0x30F).  Since these ports have no hardware
behind them, reads return 0xFF and writes are silently ignored — safe
but useless.  Most CP/M programs do not use direct I/O; they go through
BDOS.

**Option B — NMI on port access**:
Complex and hardware-dependent; not recommended for the initial port.

**Option C — Pre/post-process in bridge**:
For the few CP/M programs that use direct I/O (mainly terminal control),
patch I/O addresses in the loaded binary.  Fragile but workable for
specific programs.

Initial approach: Option A.  Add Option C selectively if needed.

### 2.4 Memory Layout for 8080 Mode

Each CP/M process gets one 64 KB segment:

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

### 2.5 Dual eCPU Strategy

The V30 port supports both:

1. **Hardware 8080 mode** — for 8080-clean CP/M programs (fast, native)
2. **Software Z80 emulator** — for Z80 CP/M programs (slower, full compat)

Auto-detection scans for Z80-only opcodes (CB/DD/ED/FD prefixes).
Explicit selection via `run8080`, `runz80`, or `cpm` (auto-detect default).

---

## 3. Design Notes

**V30 Availability** — V30 is required only for hardware 8080 mode.  On a
standard 8086/8088 PC, the kernel falls back to the software 8080/Z80
emulator.  Runtime detection, not build-time switch.

**8080 Mode I/O Port Conflict** — 8080 `IN`/`OUT` access real x86 I/O
ports.  Mitigation: CP/M BIOS should not expose I/O ports directly.

**Interrupt Handling During 8080 Mode** — Hardware interrupts use 8080
semantics.  Recommended approach: disable interrupts (`DI`) before
entering 8080 mode; rely on periodic `RETEM` exits (BDOS calls) for
timeslicing — matches existing eCPU design.
