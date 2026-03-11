# eCPU-m68k — Motorola 68000 Emulator Core Design

Interpretive Motorola 68000 emulator for PPAP, providing the CPU
execution engine for cross-architecture execution of PPAP m68k ELF
binaries on ARM hosts. Also serves as the foundation for the
Human68k subsystem personality on non-m68k hosts (future).

---

## Table of Contents

1. [Goals and Scope](#1-goals-and-scope)
2. [Background: 68000 Architecture](#2-background-68000-architecture)
3. [eCPU Common Interface](#3-ecpu-common-interface)
4. [Emulator Architecture](#4-emulator-architecture)
5. [Instruction Decoding](#5-instruction-decoding)
6. [Register Model](#6-register-model)
7. [Memory Model](#7-memory-model)
8. [Condition Code Computation](#8-condition-code-computation)
9. [Effective Address Decoder](#9-effective-address-decoder)
10. [Trap and Hook Mechanism](#10-trap-and-hook-mechanism)
11. [Scheduler Integration](#11-scheduler-integration)
12. [Performance Considerations](#12-performance-considerations)
13. [Implementation Plan](#13-implementation-plan)
14. [Testing Strategy](#14-testing-strategy)
15. [Open Questions](#15-open-questions)

---

## 1. Goals and Scope

### 1.1 Primary Goal

Provide a correct, compact Motorola 68000 interpreter that can run
PPAP m68k ELF binaries on ARM PPAP hosts (cross-architecture
execution). The emulator handles pure CPU emulation; OS-level
semantics are provided by the PPAP cross-arch personality layer
(TRAP #0 register ABI remapping). The same core also serves as the
foundation for Human68k .X execution (see `subsystem-human68k.md`)
on non-m68k hosts as a future extension.

### 1.2 Design Priorities

1. **Correctness** — full 68000 user-mode instruction set. All
   documented instructions and addressing modes. Supervisor-mode
   instructions implemented to the extent needed by target software
   (MOVE SR, TRAP, RTE)
2. **Code size** — target ~4000 lines of C, ~16 KB binary. The
   68000 ISA is larger than Z80 but has regular structure that
   allows compact decoding
3. **Simplicity** — interpretive loop with centralized effective
   address decoder. No JIT, no dynamic dispatch tables
4. **Portability** — pure C with no host-architecture dependencies.
   Runs on ARM (RP2040), m68k (natively), or any future PPAP target
5. **Common interface** — implements the eCPU common interface
   (`ecpu_core_ops_t`) shared by all eCPU cores

### 1.3 Scope

**In scope:**
- Full 68000 user-mode instruction set
- All 12 effective addressing modes
- TRAP #n instructions (fires `ECPU_TRAP_SWI`)
- F-line exception ($Fxxx opcodes) for Human68k DOS calls
  (fires `ECPU_TRAP_ILLEGAL` with the opcode word)
- A-line exception ($Axxx opcodes) (fires `ECPU_TRAP_ILLEGAL`)
- Supervisor/user mode distinction (SR bit 13)
- USP/SSP dual stack pointers
- Address error detection (odd-address word/long access)
- Integration with PPAP scheduler (preemptible interpreter loop)

**Out of scope:**
- Cycle-accurate timing
- 68010/68020/68030 extended instructions (MOVEC, MOVEP long, etc.)
- Hardware interrupt delivery (personality layers handle this)
- Bus error emulation
- Coprocessor (FPU) instructions
- Trace mode (single-step exception)

### 1.4 Relationship to Subsystems

The m68k emulator is used by two personality types:

1. **PPAP cross-arch personality** (primary target) — intercepts
   TRAP #0 and remaps d0-d5/a0 register ABI to native ARM syscall
   ABI. This enables running PPAP m68k ELF binaries on ARM PPAP.
2. **Human68k personality** (future) — intercepts F-line exceptions
   ($FFxx opcodes) and TRAP #15 (IOCS), translating them to PPAP
   syscalls

```
PPAP m68k ELF binary
    │
    │ trap #0 (PPAP syscall)
    │
    ▼
ecpu-m68k (this document): TRAP #0 detected
    │ fires ECPU_TRAP_SWI with param=0
    ▼
PPAP cross-arch personality
    │ reads d0 (syscall#), d1-d5/a0 (args)
    │ calls native syscall
    │ writes d0 (return value)
    ▼
PPAP kernel syscall (sys_read, sys_write, sys_open, ...)
```

---

## 2. Background: 68000 Architecture

### 2.1 Overview

The Motorola 68000 (1979) is a 32-bit CISC processor with a 16-bit
data bus and 24-bit address bus (16 MB address space). Key features:

- 8 data registers (D0–D7, 32-bit)
- 8 address registers (A0–A6 general, A7 = stack pointer)
- Dual stack pointers (USP for user mode, SSP for supervisor mode)
- 16-bit status register (CCR in low byte, system byte in high byte)
- Variable-length instructions (2–10 bytes, always word-aligned)
- Big-endian byte ordering
- Rich addressing modes (12 modes)

### 2.2 Registers

```
Data registers:
  D0  D1  D2  D3  D4  D5  D6  D7    (32-bit each)

Address registers:
  A0  A1  A2  A3  A4  A5  A6  A7/SP  (32-bit each)

Special:
  PC   (program counter, 32-bit, 24-bit effective)
  SR   (status register, 16-bit)
  USP  (user stack pointer, 32-bit)
  SSP  (supervisor stack pointer, 32-bit)
```

A7 is the active stack pointer (USP in user mode, SSP in supervisor
mode). The inactive stack pointer is accessible via `MOVE USP,An`
(supervisor-only instruction).

### 2.3 Status Register

```
Bit 15:   T  — Trace mode (single-step exception)
Bit 14:   0  — Reserved
Bit 13:   S  — Supervisor mode (1 = supervisor, 0 = user)
Bit 12–11: 0  — Reserved
Bit 10–8: I2 I1 I0  — Interrupt mask level (0–7)
Bit 7–5:  0  — Reserved
Bit 4:    X  — Extend (same as carry for multi-precision)
Bit 3:    N  — Negative (sign of result)
Bit 2:    Z  — Zero
Bit 1:    V  — Overflow
Bit 0:    C  — Carry
```

The low byte (bits 0–4) is the Condition Code Register (CCR) and
is accessible from user mode. The high byte (bits 8–15) is the
system byte and is supervisor-only.

### 2.4 Instruction Encoding

68000 instructions are 1–5 words (2–10 bytes), always word-aligned.
The first word encodes the operation and addressing mode(s):

```
Bits 15–12:  Operation group (0–F)
Bits 11–0:   Group-specific encoding
```

Major groups:

| Bits 15–12 | Group | Instructions |
|---|---|---|
| 0000 | Immediate/bit | ORI, ANDI, SUBI, ADDI, EORI, CMPI, BTST/BCHG/BCLR/BSET |
| 0001–0011 | MOVE | MOVE.B, MOVE.L, MOVE.W |
| 0100 | Miscellaneous | LEA, PEA, CHK, TRAP, LINK, UNLK, MOVEM, CLR, NEG, NOT, etc. |
| 0101 | ADDQ/SUBQ/Scc/DBcc | Quick add/sub, set/decrement-branch on condition |
| 0110 | Bcc/BSR/BRA | Branch on condition, subroutine, always |
| 0111 | MOVEQ | Move quick (sign-extended 8-bit immediate to Dn) |
| 1000 | OR/DIV/SBCD | OR, DIVU, DIVS, SBCD |
| 1001 | SUB/SUBX | SUB, SUBA, SUBX |
| 1010 | (A-line) | Unassigned — trapped for emulation |
| 1011 | CMP/EOR | CMP, CMPA, CMPM, EOR |
| 1100 | AND/MUL/ABCD/EXG | AND, MULU, MULS, ABCD, EXG |
| 1101 | ADD/ADDX | ADD, ADDA, ADDX |
| 1110 | Shifts/rotates | ASL/ASR, LSL/LSR, ROL/ROR, ROXL/ROXR |
| 1111 | (F-line) | Unassigned — trapped for coprocessor/emulation |

### 2.5 Addressing Modes

The 68000 supports 12 addressing modes, encoded in a 6-bit
mode/register field (3 bits mode + 3 bits register):

| Mode | Reg | Syntax | Description |
|---|---|---|---|
| 000 | Dn | Dn | Data register direct |
| 001 | An | An | Address register direct |
| 010 | An | (An) | Address register indirect |
| 011 | An | (An)+ | Post-increment indirect |
| 100 | An | -(An) | Pre-decrement indirect |
| 101 | An | d16(An) | Displacement indirect |
| 110 | An | d8(An,Xn) | Index indirect |
| 111 | 000 | abs.W | Absolute short (sign-extended) |
| 111 | 001 | abs.L | Absolute long |
| 111 | 010 | d16(PC) | PC-relative displacement |
| 111 | 011 | d8(PC,Xn) | PC-relative index |
| 111 | 100 | #imm | Immediate |

### 2.6 Exception Processing

The 68000 exception mechanism is used for traps, errors, and
interrupts. The vector table is at address 0:

| Vector | Address | Exception |
|---|---|---|
| 2 | 0x008 | Bus error |
| 3 | 0x00C | Address error |
| 4 | 0x010 | Illegal instruction |
| 5 | 0x014 | Zero divide |
| 6 | 0x018 | CHK instruction |
| 7 | 0x01C | TRAPV instruction |
| 8 | 0x020 | Privilege violation |
| 10 | 0x028 | Line 1010 emulator (A-line) |
| 11 | 0x02C | Line 1111 emulator (F-line) |
| 32–47 | 0x080–0x0BC | TRAP #0–#15 |

In the eCPU emulator, most exceptions are converted to trap
callbacks rather than executing the vector table (the personality
layer decides what to do).

### 2.7 Condition Codes

The 68000 has 16 condition codes used by Bcc, DBcc, and Scc:

| Code | Mnemonic | Condition |
|---|---|---|
| 0000 | T (true) | Always |
| 0001 | F (false) | Never |
| 0010 | HI | !C & !Z |
| 0011 | LS | C \| Z |
| 0100 | CC (HS) | !C |
| 0101 | CS (LO) | C |
| 0110 | NE | !Z |
| 0111 | EQ | Z |
| 1000 | VC | !V |
| 1001 | VS | V |
| 1010 | PL | !N |
| 1011 | MI | N |
| 1100 | GE | (N & V) \| (!N & !V) |
| 1101 | LT | (N & !V) \| (!N & V) |
| 1110 | GT | (N & V & !Z) \| (!N & !V & !Z) |
| 1111 | LE | Z \| (N & !V) \| (!N & V) |

---

## 3. eCPU Common Interface

The m68k emulator implements the same `ecpu_core_ops_t` interface
as the Z80 core (see `ecpu-z80.md` §3 for the full interface
design). Key m68k-specific points:

### 3.1 Register IDs

```c
/* m68k register IDs for get_reg/set_reg */
#define M68K_REG_D0    0
#define M68K_REG_D1    1
#define M68K_REG_D2    2
#define M68K_REG_D3    3
#define M68K_REG_D4    4
#define M68K_REG_D5    5
#define M68K_REG_D6    6
#define M68K_REG_D7    7
#define M68K_REG_A0    8
#define M68K_REG_A1    9
#define M68K_REG_A2    10
#define M68K_REG_A3    11
#define M68K_REG_A4    12
#define M68K_REG_A5    13
#define M68K_REG_A6    14
#define M68K_REG_A7    15   /* active SP (USP or SSP) */
#define M68K_REG_PC    16
#define M68K_REG_SR    17
#define M68K_REG_USP   18
#define M68K_REG_SSP   19
```

### 3.2 Trap Mapping

| Common trap type | 68000 trigger | `param` value |
|---|---|---|
| `ECPU_TRAP_SWI` | TRAP #n instruction | trap number (0–15) |
| `ECPU_TRAP_ILLEGAL` | A-line or F-line opcode | opcode word |
| `ECPU_TRAP_HALT` | STOP instruction | status word operand |

The Human68k personality handles:
- `ECPU_TRAP_ILLEGAL` with opcode in $FF00–$FFFF → DOS call
- `ECPU_TRAP_SWI` with param=15 → IOCS call

The PPAP cross-arch personality handles:
- `ECPU_TRAP_SWI` with param=0 → PPAP syscall

### 3.3 Memory Access

The m68k is big-endian. The `read16`/`write16` methods return
big-endian word values:

```c
static inline uint16_t m68k_read16(m68k_state_t *cpu, uint32_t addr) {
    return ((uint16_t)cpu->memory[addr] << 8)
         | cpu->memory[addr + 1];
}
```

### 3.4 File Organization

```
src/kernel/ecpu/
├── ecpu.h              Common interface (shared with Z80)
├── ecpu_m68k.h         m68k-specific state, register IDs, helpers
├── ecpu_m68k.c         Common interface impl + main decode loop
│                       + effective address decoder
└── ecpu_m68k_alu.c     ALU/CCR helpers, shifts, BCD operations
```

---

## 4. Emulator Architecture

### 4.1 Interpretive Core

The emulator is a fetch-decode-execute loop. The first instruction
word is fetched and the top 4 bits select the major operation
group:

```c
int ecpu_m68k_run(m68k_state_t *cpu) {
    for (;;) {
        uint16_t opcode = m68k_fetch16(cpu);

        switch (opcode >> 12) {
            case 0x0: m68k_group0(cpu, opcode); break;  /* imm/bit */
            case 0x1: m68k_move_b(cpu, opcode); break;
            case 0x2: m68k_move_l(cpu, opcode); break;
            case 0x3: m68k_move_w(cpu, opcode); break;
            case 0x4: m68k_group4(cpu, opcode); break;  /* misc */
            case 0x5: m68k_group5(cpu, opcode); break;  /* addq/subq/scc/dbcc */
            case 0x6: m68k_group6(cpu, opcode); break;  /* bcc/bsr/bra */
            case 0x7: m68k_moveq(cpu, opcode);  break;
            case 0x8: m68k_group8(cpu, opcode); break;  /* or/div/sbcd */
            case 0x9: m68k_group9(cpu, opcode); break;  /* sub */
            case 0xA: m68k_aline(cpu, opcode);  break;
            case 0xB: m68k_groupB(cpu, opcode); break;  /* cmp/eor */
            case 0xC: m68k_groupC(cpu, opcode); break;  /* and/mul/exg */
            case 0xD: m68k_groupD(cpu, opcode); break;  /* add */
            case 0xE: m68k_groupE(cpu, opcode); break;  /* shifts */
            case 0xF: m68k_fline(cpu, opcode);  break;
        }

        if (--cpu->slice_counter <= 0) {
            cpu->slice_counter = M68K_SLICE_SIZE;
            sched_yield();
        }
    }
}
```

### 4.2 Effective Address Decoder

The most critical shared component. Many 68000 instructions use the
same 6-bit mode/register encoding for source and/or destination
operands. A central EA decoder avoids duplicating this logic:

```c
/* Compute effective address and return it.
 * For register-direct modes, returns a special sentinel. */
typedef struct {
    uint32_t addr;     /* memory address (or register index) */
    uint8_t  type;     /* EA_DATA_REG, EA_ADDR_REG, EA_MEMORY */
} ea_result_t;

ea_result_t m68k_decode_ea(m68k_state_t *cpu, uint8_t mode,
                           uint8_t reg, uint8_t size);
```

For memory modes, the decoder computes the address and fetches any
extension words (displacement, index). For register modes, it
returns a register identifier.

Read/write helpers use the EA result:

```c
uint32_t m68k_read_ea(m68k_state_t *cpu, ea_result_t *ea, uint8_t size);
void m68k_write_ea(m68k_state_t *cpu, ea_result_t *ea, uint8_t size,
                   uint32_t val);
```

### 4.3 Operation Sizes

The 68000 operates on three sizes: byte (8-bit), word (16-bit), and
long (32-bit). Most instructions encode the size in bits 7–6 of the
opcode word:

```c
#define SIZE_BYTE  0
#define SIZE_WORD  1
#define SIZE_LONG  2
```

The effective address decoder, ALU, and memory access helpers are
all parameterized by size.

### 4.4 Conditional Compilation

```cmake
if(ENABLE_SUBSYS_HUMAN68K OR ENABLE_ECPU_M68K)
    target_sources(ppap PRIVATE
        src/kernel/ecpu/ecpu_m68k.c
        src/kernel/ecpu/ecpu_m68k_alu.c
    )
    target_compile_definitions(ppap PRIVATE ENABLE_ECPU_M68K=1)
endif()
```

---

## 5. Instruction Decoding

### 5.1 MOVE Instruction

MOVE is the most common instruction and has its own dedicated
opcode groups (0001, 0010, 0011 for byte, long, word respectively).
The encoding packs both source and destination EA fields:

```
15  14  13  12  11  10  9   8   7   6   5   4   3   2   1   0
 0   0  size        dst_reg dst_mode    src_mode    src_reg
```

Note: the destination field has register and mode in reversed
order compared to the source field.

### 5.2 Group 0 (Immediate + Bit Operations)

Bits 11–8 select the sub-operation:

| Bits 11–8 | Instruction |
|---|---|
| 0000 | ORI |
| 0010 | ANDI |
| 0100 | SUBI |
| 0110 | ADDI |
| 1010 | EORI |
| 1100 | CMPI |
| 1000 | BTST/BCHG/BCLR/BSET (static bit number) |
| other | BTST/BCHG/BCLR/BSET (dynamic bit number from Dn) |

### 5.3 Group 4 (Miscellaneous)

The largest and most irregular group. Sub-decoding uses multiple
bit fields:

- CLR, NEG, NEGX, NOT, TST (bits 11–8 select op, bits 7–6 = size)
- MOVEM (bits 11–7 = 01001 for reg-to-mem, 01100 for mem-to-reg)
- LEA, PEA (bits 11–8)
- TRAP, LINK, UNLK, MOVE USP, RTE, RTS, NOP, STOP, RESET
- EXT (sign extend)
- SWAP
- JSR, JMP
- CHK
- MOVE to/from SR, CCR

### 5.4 Instruction Count Estimate

| Group | Approx. operations | Description |
|---|---|---|
| MOVE (groups 1–3) | ~3 | 3 sizes × all EA combinations |
| Arithmetic (ADD/SUB/CMP/NEG) | ~20 | + immediate, quick, extended |
| Logic (AND/OR/EOR/NOT) | ~10 | + immediate, to-CCR/SR |
| Shifts/rotates | ~8 | 4 ops × 2 directions |
| Bit operations | ~4 | BTST/BCHG/BCLR/BSET |
| Branch/jump | ~20 | Bcc×16, BRA, BSR, JMP, JSR, DBcc |
| Misc data movement | ~15 | MOVEQ, MOVEM, MOVEP, LEA, PEA, EXG, SWAP, EXT |
| Multiply/divide | ~4 | MULU, MULS, DIVU, DIVS |
| BCD | ~3 | ABCD, SBCD, NBCD |
| Control | ~15 | TRAP, RTE, RTS, LINK, UNLK, NOP, STOP, RESET, etc. |
| **Total** | **~100 base operations** | With EA decoder handling addressing complexity |

The EA decoder multiplies this: each operation works with all
applicable addressing modes, but the decoder is shared code.

---

## 6. Register Model

### 6.1 State Structure

```c
typedef struct m68k_state {
    /* Data registers (32-bit) */
    uint32_t d[8];

    /* Address registers (32-bit) — a[7] is the active SP */
    uint32_t a[8];

    /* Program counter */
    uint32_t pc;

    /* Status register */
    uint16_t sr;    /* bits 0–4: CCR (X,N,Z,V,C)
                       bit 13: S (supervisor)
                       bits 8–10: interrupt mask */

    /* Alternate stack pointers */
    uint32_t usp;   /* user stack pointer (when in supervisor mode) */
    uint32_t ssp;   /* supervisor stack pointer (when in user mode) */

    /* Emulator state */
    uint8_t stopped;          /* 1 if STOP executed */
    int32_t slice_counter;    /* instructions until yield */

    /* Memory */
    uint8_t *memory;
    uint32_t mem_size;

    /* Trap hook */
    ecpu_trap_handler_t trap_handler;
    void *trap_ctx;
} m68k_state_t;
```

### 6.2 Stack Pointer Management

The active stack pointer is `a[7]`. When switching between user
and supervisor mode:

```c
static inline void m68k_set_supervisor(m68k_state_t *cpu, int sup) {
    int was_sup = (cpu->sr >> 13) & 1;
    if (was_sup && !sup) {
        /* Leaving supervisor: save SSP, restore USP */
        cpu->ssp = cpu->a[7];
        cpu->a[7] = cpu->usp;
    } else if (!was_sup && sup) {
        /* Entering supervisor: save USP, restore SSP */
        cpu->usp = cpu->a[7];
        cpu->a[7] = cpu->ssp;
    }
    if (sup)
        cpu->sr |= 0x2000;
    else
        cpu->sr &= ~0x2000;
}
```

### 6.3 Byte/Word Access to Data Registers

Data registers support byte, word, and long operations. Only the
affected portion is modified:

```c
static inline void m68k_write_d(m68k_state_t *cpu, int reg,
                                uint32_t val, uint8_t size) {
    switch (size) {
        case SIZE_BYTE: cpu->d[reg] = (cpu->d[reg] & 0xFFFFFF00) | (val & 0xFF); break;
        case SIZE_WORD: cpu->d[reg] = (cpu->d[reg] & 0xFFFF0000) | (val & 0xFFFF); break;
        case SIZE_LONG: cpu->d[reg] = val; break;
    }
}
```

Address registers always operate on the full 32 bits. Word-size
operations sign-extend to 32 bits.

---

## 7. Memory Model

### 7.1 Address Space

The 68000 has a 24-bit address bus (16 MB). The emulator allocates
a contiguous memory region. Addresses are masked to `mem_size - 1`.

For Human68k programs, typical memory layout:

```
0x000000–0x0003FF   Exception vector table (1 KB)
0x000400–0x000FFF   System area
0x001000–...        Program load area
...–0x0BFFFF        End of main RAM (768 KB typical)
```

For the emulator, a configurable memory size (e.g., 1 MB default)
is allocated.

### 7.2 Memory Access Functions

Big-endian byte ordering:

```c
static inline uint8_t m68k_read8(m68k_state_t *cpu, uint32_t addr) {
    return cpu->memory[addr & (cpu->mem_size - 1)];
}

static inline uint16_t m68k_read16(m68k_state_t *cpu, uint32_t addr) {
    addr &= cpu->mem_size - 1;
    return ((uint16_t)cpu->memory[addr] << 8)
         | cpu->memory[addr + 1];
}

static inline uint32_t m68k_read32(m68k_state_t *cpu, uint32_t addr) {
    addr &= cpu->mem_size - 1;
    return ((uint32_t)cpu->memory[addr] << 24)
         | ((uint32_t)cpu->memory[addr + 1] << 16)
         | ((uint32_t)cpu->memory[addr + 2] << 8)
         | cpu->memory[addr + 3];
}
```

### 7.3 Address Error

The 68000 generates an address error exception when accessing a
word or long at an odd address. The emulator should detect this
and fire a trap:

```c
static inline int m68k_check_align(m68k_state_t *cpu, uint32_t addr,
                                   uint8_t size) {
    if (size != SIZE_BYTE && (addr & 1))
        return -1;  /* address error */
    return 0;
}
```

### 7.4 Instruction Fetch

Instructions are always word-aligned:

```c
static inline uint16_t m68k_fetch16(m68k_state_t *cpu) {
    uint16_t val = m68k_read16(cpu, cpu->pc);
    cpu->pc += 2;
    return val;
}

static inline uint32_t m68k_fetch32(m68k_state_t *cpu) {
    uint32_t val = m68k_read32(cpu, cpu->pc);
    cpu->pc += 4;
    return val;
}
```

### 7.5 Stack Operations

Stack grows downward. Pre-decrement for push, post-increment for
pop:

```c
static inline void m68k_push16(m68k_state_t *cpu, uint16_t val) {
    cpu->a[7] -= 2;
    m68k_write16(cpu, cpu->a[7], val);
}

static inline void m68k_push32(m68k_state_t *cpu, uint32_t val) {
    cpu->a[7] -= 4;
    m68k_write32(cpu, cpu->a[7], val);
}

static inline uint16_t m68k_pop16(m68k_state_t *cpu) {
    uint16_t val = m68k_read16(cpu, cpu->a[7]);
    cpu->a[7] += 2;
    return val;
}

static inline uint32_t m68k_pop32(m68k_state_t *cpu) {
    uint32_t val = m68k_read32(cpu, cpu->a[7]);
    cpu->a[7] += 4;
    return val;
}
```

---

## 8. Condition Code Computation

### 8.1 CCR Flags

```c
#define M68K_FLAG_C   0x01   /* Bit 0: Carry */
#define M68K_FLAG_V   0x02   /* Bit 1: Overflow */
#define M68K_FLAG_Z   0x04   /* Bit 2: Zero */
#define M68K_FLAG_N   0x08   /* Bit 3: Negative */
#define M68K_FLAG_X   0x10   /* Bit 4: Extend */
```

### 8.2 Flag Computation Patterns

**ADD (8/16/32-bit):**
- X, C: set if carry out of MSB
- V: set if sign overflow (operands same sign, result different)
- Z: set if result is zero
- N: set if result is negative (MSB set)

```c
void m68k_add(m68k_state_t *cpu, uint32_t src, uint32_t dst,
              uint32_t *result, uint8_t size) {
    uint32_t mask = size_mask(size);
    uint32_t msb = size_msb(size);
    uint64_t res = (dst & mask) + (src & mask);
    *result = res & mask;

    uint16_t ccr = 0;
    if (res & (mask + 1))               ccr |= M68K_FLAG_C | M68K_FLAG_X;
    if (*result == 0)                    ccr |= M68K_FLAG_Z;
    if (*result & msb)                   ccr |= M68K_FLAG_N;
    if ((~(src ^ dst) & (src ^ res)) & msb) ccr |= M68K_FLAG_V;
    cpu->sr = (cpu->sr & 0xFF00) | ccr;
}
```

**MOVE:**
- C, V: cleared
- Z: set if result is zero
- N: set if result is negative
- X: not affected

**CMP (same as SUB but result discarded):**
- All flags set from subtraction
- X: not affected

**Logical operations (AND, OR, EOR):**
- C, V: cleared
- Z: set if zero
- N: set if negative
- X: not affected

### 8.3 Size Helpers

```c
static inline uint32_t size_mask(uint8_t size) {
    static const uint32_t masks[] = { 0xFF, 0xFFFF, 0xFFFFFFFF };
    return masks[size];
}
static inline uint32_t size_msb(uint8_t size) {
    static const uint32_t msbs[] = { 0x80, 0x8000, 0x80000000 };
    return msbs[size];
}
```

### 8.4 Condition Code Evaluation

```c
static int m68k_test_cc(m68k_state_t *cpu, uint8_t cc) {
    uint16_t sr = cpu->sr;
    int c = sr & M68K_FLAG_C;
    int v = sr & M68K_FLAG_V;
    int z = sr & M68K_FLAG_Z;
    int n = sr & M68K_FLAG_N;
    switch (cc) {
        case  0: return 1;                       /* T */
        case  1: return 0;                       /* F */
        case  2: return !c && !z;                /* HI */
        case  3: return c || z;                  /* LS */
        case  4: return !c;                      /* CC */
        case  5: return !!c;                     /* CS */
        case  6: return !z;                      /* NE */
        case  7: return !!z;                     /* EQ */
        case  8: return !v;                      /* VC */
        case  9: return !!v;                     /* VS */
        case 10: return !n;                      /* PL */
        case 11: return !!n;                     /* MI */
        case 12: return (!!n == !!v);            /* GE */
        case 13: return (!!n != !!v);            /* LT */
        case 14: return !z && (!!n == !!v);      /* GT */
        case 15: return z || (!!n != !!v);       /* LE */
        default: return 0;
    }
}
```

---

## 9. Effective Address Decoder

### 9.1 Design

The EA decoder is the central component that makes the m68k
emulator manageable. Most instructions share the same EA encoding,
so a single decoder handles all addressing modes.

The decoder returns a result that can be used to read or write
the operand:

```c
#define EA_TYPE_DATA_REG  0
#define EA_TYPE_ADDR_REG  1
#define EA_TYPE_MEMORY    2
#define EA_TYPE_IMMEDIATE 3

typedef struct {
    uint32_t addr;   /* memory address, or register index */
    uint32_t value;  /* for immediate mode */
    uint8_t  type;
} ea_result_t;
```

### 9.2 Decode Logic

```c
ea_result_t m68k_decode_ea(m68k_state_t *cpu, uint8_t mode,
                           uint8_t reg, uint8_t size) {
    ea_result_t ea;
    switch (mode) {
        case 0: /* Dn */
            ea.type = EA_TYPE_DATA_REG;
            ea.addr = reg;
            break;
        case 1: /* An */
            ea.type = EA_TYPE_ADDR_REG;
            ea.addr = reg;
            break;
        case 2: /* (An) */
            ea.type = EA_TYPE_MEMORY;
            ea.addr = cpu->a[reg];
            break;
        case 3: /* (An)+ */
            ea.type = EA_TYPE_MEMORY;
            ea.addr = cpu->a[reg];
            cpu->a[reg] += size_bytes(size);
            /* Special: byte ops on A7 use 2-byte increment */
            if (size == SIZE_BYTE && reg == 7)
                cpu->a[reg]++;
            break;
        case 4: /* -(An) */
            if (size == SIZE_BYTE && reg == 7)
                cpu->a[reg]--;
            cpu->a[reg] -= size_bytes(size);
            ea.type = EA_TYPE_MEMORY;
            ea.addr = cpu->a[reg];
            break;
        case 5: /* d16(An) */
            ea.type = EA_TYPE_MEMORY;
            ea.addr = cpu->a[reg] + (int16_t)m68k_fetch16(cpu);
            break;
        case 6: /* d8(An,Xn) */
            ea.type = EA_TYPE_MEMORY;
            ea.addr = m68k_decode_brief_ext(cpu, cpu->a[reg]);
            break;
        case 7:
            switch (reg) {
                case 0: /* abs.W */
                    ea.type = EA_TYPE_MEMORY;
                    ea.addr = (int16_t)m68k_fetch16(cpu);
                    break;
                case 1: /* abs.L */
                    ea.type = EA_TYPE_MEMORY;
                    ea.addr = m68k_fetch32(cpu);
                    break;
                case 2: /* d16(PC) */
                    ea.type = EA_TYPE_MEMORY;
                    { uint32_t base = cpu->pc;
                      ea.addr = base + (int16_t)m68k_fetch16(cpu); }
                    break;
                case 3: /* d8(PC,Xn) */
                    ea.type = EA_TYPE_MEMORY;
                    ea.addr = m68k_decode_brief_ext(cpu, cpu->pc);
                    break;
                case 4: /* #imm */
                    ea.type = EA_TYPE_IMMEDIATE;
                    if (size == SIZE_LONG)
                        ea.value = m68k_fetch32(cpu);
                    else
                        ea.value = m68k_fetch16(cpu);
                    if (size == SIZE_BYTE)
                        ea.value &= 0xFF;
                    break;
            }
            break;
    }
    return ea;
}
```

### 9.3 Brief Extension Word (Index)

Modes 6 and 7/3 use a brief extension word for indexed addressing:

```
Bits 15:    D/A (0 = Dn, 1 = An)
Bits 14–12: Register number
Bit 11:     W/L (0 = sign-extended word, 1 = long)
Bits 7–0:   Signed 8-bit displacement
```

```c
static uint32_t m68k_decode_brief_ext(m68k_state_t *cpu, uint32_t base) {
    uint16_t ext = m68k_fetch16(cpu);
    int8_t disp = ext & 0xFF;
    int xreg = (ext >> 12) & 7;
    int32_t xval;
    if (ext & 0x8000)
        xval = (int32_t)cpu->a[xreg];
    else
        xval = (int32_t)cpu->d[xreg];
    if (!(ext & 0x0800))
        xval = (int16_t)(xval & 0xFFFF);  /* word index, sign-extend */
    return base + disp + xval;
}
```

### 9.4 A7 Byte Adjustment

The 68000 keeps the stack pointer word-aligned. When A7 is used
with byte-size post-increment or pre-decrement, the increment is
2 instead of 1. This special case must be handled in modes 3 and 4.

---

## 10. Trap and Hook Mechanism

### 10.1 TRAP Instruction

TRAP #n (n = 0–15) fires `ECPU_TRAP_SWI` with param = n:

```c
/* TRAP #n */
int n = opcode & 0x0F;
int rc = m68k_fire_trap(cpu, ECPU_TRAP_SWI, n);
if (rc == ECPU_TRAP_EXIT) return 0;
if (rc == ECPU_TRAP_HANDLED) break;
/* Unhandled: push SR+PC, load vector */
m68k_push32(cpu, cpu->pc);
m68k_push16(cpu, cpu->sr);
cpu->sr |= 0x2000;  /* enter supervisor */
cpu->pc = m68k_read32(cpu, (32 + n) * 4);
```

### 10.2 F-line Exception (Human68k DOS Calls)

F-line opcodes ($Fxxx) fire `ECPU_TRAP_ILLEGAL` with param =
the opcode word. The Human68k personality checks if the opcode is
in the $FF00–$FFFF range (DOS call):

```c
/* F-line handler in personality */
int human68k_trap(ecpu_state_t *cpu, int trap_type,
                  uint32_t param, void *ctx) {
    if (trap_type != ECPU_TRAP_ILLEGAL) return ECPU_TRAP_UNHANDLED;
    if ((param & 0xFF00) != 0xFF00)     return ECPU_TRAP_UNHANDLED;

    uint8_t dos_fn = param & 0xFF;
    return human68k_dos_dispatch(cpu, dos_fn, ctx);
}
```

### 10.3 A-line Exception

A-line opcodes ($Axxx) also fire `ECPU_TRAP_ILLEGAL`. Currently
no personality uses A-line, but the mechanism is available for
Classic Mac emulation in the future.

### 10.4 STOP Instruction

STOP loads the immediate operand into SR and halts. Fires
`ECPU_TRAP_HALT`:

```c
/* STOP #imm */
uint16_t imm = m68k_fetch16(cpu);
cpu->sr = imm;
cpu->stopped = 1;
m68k_fire_trap(cpu, ECPU_TRAP_HALT, imm);
```

---

## 11. Scheduler Integration

Same approach as the Z80 core (see `ecpu-z80.md` §10). A slice
counter yields after N instructions:

```c
#define M68K_SLICE_SIZE  500  /* fewer than Z80: m68k instructions
                                 are heavier (multi-cycle) */
```

The m68k state (`m68k_state_t`) persists in heap and survives
context switches naturally.

---

## 12. Performance Considerations

### 12.1 Expected Performance

On RP2040 (133 MHz ARM Cortex-M0+):
- Estimated ~20–50 host instructions per emulated 68000 instruction
  (larger than Z80 due to wider data paths and EA computation)
- At 133 MHz: ~2.5–6.5 million 68000 instructions/second
- Equivalent 68000 clock: **~2.5–6.5 MHz**
- Original X68000 ran at 10 MHz
- **Result: somewhat slower than original hardware** on RP2040

On ARM with more performance (Pi Zero, 1 GHz):
- ~20–50 million 68000 instructions/second
- **Far faster than original hardware**

### 12.2 Code Size Budget

Target: ~16 KB binary.

| Component | Est. lines | Est. binary |
|---|---|---|
| Common interface impl | ~200 | ~0.5 KB |
| Main decode loop | ~800 | ~3 KB |
| EA decoder | ~300 | ~1.5 KB |
| MOVE instructions | ~200 | ~1 KB |
| Arithmetic (ADD/SUB/CMP/NEG) | ~400 | ~2 KB |
| Logic (AND/OR/EOR/NOT) | ~200 | ~1 KB |
| Shifts/rotates | ~300 | ~1.5 KB |
| Branch/jump/TRAP | ~300 | ~1.5 KB |
| Misc (MOVEM/LEA/EXT/SWAP/etc.) | ~400 | ~2 KB |
| ALU/CCR helpers | ~300 | ~1 KB |
| MUL/DIV/BCD | ~200 | ~1 KB |
| **Total** | **~3600** | **~16 KB** |

---

## 13. Implementation Plan

### Step 1 — Skeleton + MOVE + basic EA

**Goal:** define m68k_state_t, common interface, main decode loop,
EA decoder for simple modes, MOVE instruction.

- `ecpu_m68k.h`: state struct, register IDs, flag constants, inline
  memory helpers (big-endian)
- `ecpu_m68k.c`: common interface (init/reset/run/get_reg/set_reg),
  EA decoder (modes 0–4: Dn, An, (An), (An)+, -(An))
- MOVE.B/W/L with the basic EA modes
- MOVEQ (quick 8-bit immediate to Dn)
- LEA (load effective address)
- CLR (clear operand)
- NOP, STOP (with trap), ILLEGAL
- F-line and A-line exception → ECPU_TRAP_ILLEGAL
- **Test:** register-to-register moves, memory loads/stores,
  post-increment/pre-decrement patterns

### Step 2 — Integer arithmetic

**Goal:** ADD/SUB/CMP and variants with full CCR computation.

- `ecpu_m68k_alu.c`: CCR computation helpers (add/sub/cmp flags)
- ADD, ADDA, ADDI, ADDQ (all sizes)
- SUB, SUBA, SUBI, SUBQ (all sizes)
- CMP, CMPA, CMPI, CMPM
- NEG, NEGX
- TST
- EXT (sign extend B→W, W→L)
- MULU, MULS (16×16→32)
- DIVU, DIVS (32÷16→16q:16r, with divide-by-zero trap)
- **Test:** arithmetic operations with flag verification

### Step 3 — Branches, jumps, subroutines

**Goal:** control flow.

- Bcc (all 16 conditions), BRA, BSR (8-bit and 16-bit displacements)
- JMP, JSR, RTS
- TRAP #n → ECPU_TRAP_SWI
- RTE (return from exception)
- DBcc (decrement and branch on condition)
- Scc (set byte on condition)
- LINK, UNLK (frame pointer management)
- **Test:** loops, subroutine calls, condition branching

### Step 4 — Logic, shifts, bit operations

**Goal:** complete logical and bit-manipulation instructions.

- AND, ANDI, OR, ORI, EOR, EORI (all sizes)
- NOT
- ASL/ASR, LSL/LSR (register and memory, all sizes)
- ROL/ROR, ROXL/ROXR
- BTST, BCHG, BCLR, BSET (static and dynamic bit number)
- SWAP, EXG
- ANDI/ORI/EORI to CCR and SR
- **Test:** bit manipulation, shift patterns

### Step 5 — Remaining EA modes + MOVEM

**Goal:** complete all addressing modes and multi-register transfer.

- EA modes 5–6: d16(An), d8(An,Xn)
- EA modes 7/0–7/3: abs.W, abs.L, d16(PC), d8(PC,Xn)
- MOVEM (register list save/restore)
- MOVEP (peripheral byte access)
- PEA (push effective address)
- MOVE USP (supervisor mode)
- MOVE to/from SR, CCR
- **Test:** all addressing modes, MOVEM save/restore patterns

### Step 6 — BCD, miscellaneous, privileged

**Goal:** complete remaining instructions.

- ABCD, SBCD, NBCD (BCD arithmetic)
- TAS (test-and-set)
- CHK (range check with exception)
- STOP, RESET (privileged)
- ADDX, SUBX (multi-precision)
- Address error detection (odd word/long access)
- **Test:** BCD operations, privilege checks

### Step 7 — Integration test + edge cases

**Goal:** comprehensive testing.

- Run larger test programs (sorting, string processing)
- Edge cases: address wrapping, SP alignment, SR protection
- Verify all 16 condition codes with Bcc
- Verify MOVEM with all register combinations
- DIVU/DIVS edge cases (overflow, divide by zero)
- **Test:** multi-instruction programs, regression suite

### Step 8 — PPAP cross-arch personality wiring

**Goal:** run a real PPAP m68k ELF binary on ARM PPAP.

- Wire TRAP #0 to PPAP cross-arch personality (d0=syscall#,
  d1-d5/a0=args, return in d0)
- Load m68k ELF binary into emulated memory (reuse ELF loader)
- Set up user-mode stack, argc/argv, a5 (GOT base for PIC)
- Run simple PPAP m68k "hello" program (write syscall)
- **Test:** hello.m68k prints "Hello" via sys_write

**Future Step 9 — Human68k personality (out of scope for now):**
- Wire F-line ($FFxx) trap to Human68k DOS bridge
- Load X-format binary, run _DOS_PRINT test

---

## 14. Testing Strategy

### 14.1 Unit Test Approach

Same approach as ecpu-z80: host-side tests using hand-assembled
68000 byte sequences. Each test loads a short program into emulated
memory, runs the interpreter, and checks final register/memory
state.

```c
void test_move_dn(void) {
    m68k_state_t cpu;
    uint8_t mem[1048576] = {0};

    ecpu_m68k_ops.init((ecpu_state_t *)&cpu, mem, sizeof(mem));
    cpu.pc = 0x1000;
    /* MOVE.L #$12345678,D0 → 203C 1234 5678 */
    m68k_write16(&cpu, 0x1000, 0x203C);
    m68k_write32(&cpu, 0x1002, 0x12345678);
    /* NOP → 4E71 */
    m68k_write16(&cpu, 0x1006, 0x4E71);
    /* STOP #$2700 → 4E72 2700 */
    m68k_write16(&cpu, 0x1008, 0x4E72);
    m68k_write16(&cpu, 0x100A, 0x2700);

    ecpu_m68k_ops.run((ecpu_state_t *)&cpu);
    assert(cpu.d[0] == 0x12345678);
}
```

### 14.2 Test Matrix

| Category | Tests | Validates |
|---|---|---|
| Data movement | MOVE.B/W/L, MOVEQ, LEA, CLR | EA decoder, register access |
| Arithmetic | ADD/SUB/CMP/NEG + flag checks | ALU + CCR |
| Logic | AND/OR/EOR/NOT | CCR for logic ops |
| Shifts | ASL/ASR/LSL/LSR/ROL/ROR | Shift + carry flag |
| Bit ops | BTST/BCHG/BCLR/BSET | Z flag, bit manipulation |
| Branches | Bcc (all 16), DBcc, BRA, BSR | Condition codes |
| Subroutines | JSR/RTS, LINK/UNLK, TRAP | Stack, trap mechanism |
| Multi-register | MOVEM save/restore | Register list decode |
| Addressing modes | All 12 modes | EA decoder completeness |
| Multiply/divide | MULU/MULS/DIVU/DIVS | CCR, divide-by-zero |
| BCD | ABCD/SBCD/NBCD | X flag, BCD correction |
| Programs | Sort, string copy, search | End-to-end validation |

### 14.3 Reference Test Suites

- **68000 instruction exerciser programs** — analogous to ZEXALL
  for Z80; various community-created test ROMs
- **Musashi test suite** — the well-known Musashi 68000 emulator
  includes test vectors
- **Human68k test binaries** — simple .X programs compiled with
  XC (Sharp's C compiler) or GCC

---

## 15. Open Questions

1. **Memory size default:** 1 MB is typical for X68000 main RAM,
   but Human68k programs rarely use more than 256 KB. A smaller
   default (e.g., 512 KB) saves memory on RP2040. Configurable
   via the personality layer.

2. **Address error handling:** should the emulator fire a trap
   callback on address error, or silently mask the low bit? Real
   68000 generates a bus-cycle exception that is notoriously
   difficult to recover from. The emulator could be more lenient.

3. **STOP instruction:** on real hardware, STOP waits for an
   interrupt. The emulator should probably treat it like Z80 HALT
   — fire ECPU_TRAP_HALT and let the personality decide.

4. **Supervisor mode:** Human68k programs run in supervisor mode
   (no memory protection on X68000). PPAP m68k ELF binaries run
   in user mode. The emulator should start in the mode the
   personality requests.

5. **MOVEP instruction:** used for 6800-peripheral byte access
   (alternating even/odd bytes). Rare in software — may be
   deferred until needed.

6. **Endianness on ARM host:** the emulator stores memory in
   big-endian (m68k native) byte order. read16/read32 helpers
   assemble bytes explicitly, which is correct regardless of host
   endianness. This is the same approach used by the Z80 core
   (which uses little-endian memory on a little-endian host).

7. **32-bit address masking:** the 68000 only uses 24 address
   bits (A0–A23). Should addresses be masked to 24 bits, or
   should the full 32-bit range be available? The 68000 hardware
   ignores A24–A31, but some software (incorrectly) uses the
   upper bits for flags. Mask to 24 bits for correctness.
