# i8086 eCPU Emulator Proposal

> **Note**: File paths in this document may be outdated after the source tree
> reorganization.  See [Source Tree Structure](../getting_started/source_tree.md)
> for the current layout.

A software Intel 8086 emulator core for PPAP, following the same
`ecpu_core_ops_t` pattern as the existing Z80 (`ecpu_z80.c`) and m68k
(`ecpu_m68k.c`) eCPU cores.  Enables running DOS .COM and .EXE programs
on any PPAP host architecture (ARM, m68k) without native x86 hardware.

On a V30/8086 native target, this eCPU is not needed — DOS programs run
directly on the CPU.  The software emulator exists for cross-architecture
execution, the same way eCPU Z80 runs CP/M programs on ARM.

---

## 1. Goals and Scope

### 1.1 Primary Goal

Implement `ecpu_i8086_ops` conforming to the `ecpu_core_ops_t` interface:

- Full 8086 real-mode instruction set (no 80186/80286 extensions).
- Segmented memory model: 20-bit addressing (1 MB) via segment:offset.
- `ECPU_TRAP_SWI` on `INT n` instructions (for INT 21h, INT 10h, etc.).
- `ECPU_TRAP_HALT` on `HLT`.
- `ECPU_TRAP_IO_IN` / `ECPU_TRAP_IO_OUT` on `IN` / `OUT` instructions.
- Single-step mode for ptrace `PTRACE_SINGLESTEP`.
- Integration with PPAP's ptrace debug surface and register reporting.

### 1.2 Extended Goals

- 80186 instruction extensions (ENTER/LEAVE, PUSHA/POPA, PUSH imm,
  IMUL imm, INS/OUTS, BOUND, shifts by immediate).
- V30/V20 extensions (`BRKEM`/`RETEM` for hardware 8080 mode emulation,
  bit manipulation instructions).
- 8087 FPU emulation (trap on ESC opcodes, software float library).
- Self-modifying code support (mark pages dirty, flush decode cache if
  added later).

### 1.3 Out of Scope

- Protected mode (80286+).
- 32-bit extensions (80386+).
- Paging or virtual memory.
- Cycle-accurate timing (instruction counting is sufficient for PPAP's
  scheduler timeslice model).

---

## 2. Architecture

### 2.1 Core Registration

```c
/* ecpu.h — already has ECPU_ARCH_8086 = 3 */

/* ecpu_i8086.h */
extern const ecpu_core_ops_t ecpu_i8086_ops;
```

The eCPU core is selected at process load time when `exec` detects a DOS
binary and the host architecture is not x86:

```c
/* In dos_loader.c */
ecpu_state_t *cpu = page_alloc_typed(ecpu_i8086_ops.state_size);
ecpu_i8086_ops.init(cpu, memory, 0x100000);  /* 1 MB */
ecpu_i8086_ops.set_trap_handler(cpu, dos_trap_handler, dos_state);
```

### 2.2 State Structure

```c
typedef struct i8086_state {
    /* General-purpose registers (16-bit) */
    union {
        uint16_t w;
        struct { uint8_t l, h; } b;  /* little-endian: AL=b.l, AH=b.h */
    } ax, bx, cx, dx;

    /* Index and pointer registers (16-bit) */
    uint16_t si, di, bp, sp;

    /* Segment registers (16-bit) */
    uint16_t cs, ds, es, ss;

    /* Instruction pointer */
    uint16_t ip;

    /* FLAGS register */
    uint16_t flags;

    /* Segment override state (per-instruction, reset after each) */
    int      seg_override;     /* -1 = none, 0=ES, 1=CS, 2=SS, 3=DS */

    /* REP prefix state */
    uint8_t  rep_prefix;       /* 0=none, 0xF2=REPNE, 0xF3=REP/REPE */

    /* Emulator control */
    uint8_t  halted;
    uint8_t  step_budget;      /* >0: return after N instructions */
    uint8_t  step_trap_exit;

    /* Memory (1 MB flat, addressed via seg*16+offset) */
    uint8_t *memory;
    uint32_t mem_size;         /* 0x100000 = 1 MB */

    /* Trap hook */
    ecpu_trap_handler_t trap_handler;
    void *trap_ctx;
} i8086_state_t;
```

### 2.3 FLAGS Register Layout

```
Bit  Name  Description
───  ────  ───────────────────
 0   CF    Carry flag
 2   PF    Parity flag
 4   AF    Auxiliary carry flag
 6   ZF    Zero flag
 7   SF    Sign flag
 8   TF    Trap flag (single-step)
 9   IF    Interrupt enable flag
10   DF    Direction flag
11   OF    Overflow flag
```

```c
#define I8086_FLAG_CF  (1u << 0)
#define I8086_FLAG_PF  (1u << 2)
#define I8086_FLAG_AF  (1u << 4)
#define I8086_FLAG_ZF  (1u << 6)
#define I8086_FLAG_SF  (1u << 7)
#define I8086_FLAG_TF  (1u << 8)
#define I8086_FLAG_IF  (1u << 9)
#define I8086_FLAG_DF  (1u << 10)
#define I8086_FLAG_OF  (1u << 11)
```

---

## 3. Instruction Set

### 3.1 Instruction Encoding Overview

8086 instructions use a variable-length encoding (1–6 bytes):

```
[prefix(es)] [opcode] [modr/m] [displacement] [immediate]
```

- **Prefixes** (0–4 bytes): segment override (26/2E/36/3E), REP (F2/F3),
  LOCK (F0)
- **Opcode** (1 byte): the operation
- **ModR/M** (0–1 byte): addressing mode, register selection
- **Displacement** (0–2 bytes): memory offset for ModR/M modes
- **Immediate** (0–2 bytes): constant operand

### 3.2 ModR/M Byte Decoding

```
Bits 7–6: mod   (00=no disp, 01=disp8, 10=disp16, 11=register)
Bits 5–3: reg   (register operand or opcode extension)
Bits 2–0: r/m   (register/memory operand)
```

Memory addressing modes (mod != 11):

| r/m | Effective address |
|-----|------------------|
| 000 | [BX+SI+disp] |
| 001 | [BX+DI+disp] |
| 010 | [BP+SI+disp] (SS) |
| 011 | [BP+DI+disp] (SS) |
| 100 | [SI+disp] |
| 101 | [DI+disp] |
| 110 | [BP+disp] (SS) — or [disp16] if mod=00 |
| 111 | [BX+disp] |

Default segment is DS, except BP-based modes use SS.

### 3.3 Instruction Categories

#### Data Transfer (~25 opcodes)

| Opcode | Mnemonic | Description |
|--------|----------|-------------|
| 88–8B | MOV r/m, r/r/m | Register/memory move |
| A0–A3 | MOV AL/AX, moffs | Direct memory access |
| B0–BF | MOV r, imm | Load immediate |
| C6–C7 | MOV r/m, imm | Store immediate |
| 8C, 8E | MOV r/m, sreg | Segment register move |
| 86–87 | XCHG | Exchange |
| 50–5F | PUSH/POP r | Stack operations |
| 06,0E,16,1E | PUSH seg | Push segment register |
| 07,17,1F | POP seg | Pop segment register |
| 8D | LEA | Load effective address |
| C4, C5 | LES, LDS | Load far pointer |
| 9C, 9D | PUSHF, POPF | Push/pop flags |
| D7 | XLAT | Table lookup |

#### Arithmetic (~30 opcodes)

| Opcode range | Mnemonic | Description |
|-------------|----------|-------------|
| 00–05 | ADD | Addition |
| 10–15 | ADC | Add with carry |
| 28–2D | SUB | Subtraction |
| 18–1D | SBB | Subtract with borrow |
| 38–3D | CMP | Compare |
| 40–47 | INC r | Increment register |
| 48–4F | DEC r | Decrement register |
| FE/FF | INC/DEC r/m | Increment/decrement memory |
| F6/F7 | MUL, IMUL, DIV, IDIV, NEG, NOT | Unary operations |
| 27, 2F | DAA, DAS | Decimal adjust |
| 37, 3F | AAA, AAS | ASCII adjust |
| D4, D5 | AAM, AAD | ASCII adjust multiply/divide |
| 98, 99 | CBW, CWD | Sign extend |

#### Logic and Shift (~15 opcodes)

| Opcode range | Mnemonic |
|-------------|----------|
| 20–25 | AND |
| 08–0D | OR |
| 30–35 | XOR |
| 84–85 | TEST |
| D0–D3 | ROL, ROR, RCL, RCR, SHL, SHR, SAR (shift by 1 or CL) |

#### Control Flow (~20 opcodes)

| Opcode | Mnemonic | Description |
|--------|----------|-------------|
| 70–7F | Jcc | Conditional jump (short) |
| E8 | CALL | Near call |
| 9A | CALL far | Far call |
| C3 | RET | Near return |
| CB | RETF | Far return |
| E9 | JMP | Near jump |
| EA | JMP far | Far jump |
| EB | JMP short | Short jump |
| E0–E3 | LOOP/LOOPE/LOOPNE/JCXZ | Loop control |
| CD | INT n | Software interrupt → **ECPU_TRAP_SWI** |
| CF | IRET | Interrupt return |
| CC | INT 3 | Breakpoint → **ECPU_TRAP_SWI** |
| CE | INTO | Overflow interrupt |

#### String Operations (~10 opcodes)

| Opcode | Mnemonic | Description |
|--------|----------|-------------|
| A4–A5 | MOVSB/MOVSW | Move string (with REP) |
| A6–A7 | CMPSB/CMPSW | Compare string (with REPE/REPNE) |
| AA–AB | STOSB/STOSW | Store string (with REP) |
| AC–AD | LODSB/LODSW | Load string |
| AE–AF | SCASB/SCASW | Scan string (with REPE/REPNE) |

String operations use DS:SI as source, ES:DI as destination, CX as count
(with REP), and honor the DF (direction flag).

#### I/O (~4 opcodes)

| Opcode | Mnemonic | Trap |
|--------|----------|------|
| E4–E5 | IN AL/AX, imm8 | **ECPU_TRAP_IO_IN** |
| EC–ED | IN AL/AX, DX | **ECPU_TRAP_IO_IN** |
| E6–E7 | OUT imm8, AL/AX | **ECPU_TRAP_IO_OUT** |
| EE–EF | OUT DX, AL/AX | **ECPU_TRAP_IO_OUT** |

#### Miscellaneous

| Opcode | Mnemonic | Notes |
|--------|----------|-------|
| F4 | HLT | **ECPU_TRAP_HALT** |
| F0 | LOCK | Ignored (single-threaded emulator) |
| 90 | NOP | No operation |
| F8–FD | CLC/STC/CLI/STI/CLD/STD | Flag manipulation |

### 3.4 Instruction Count Estimate

Total: approximately **130 opcodes** (including ModR/M extensions and
prefix combinations).  This is comparable to the Z80 core (~700 opcode
variants including CB/DD/ED/FD prefix families) and significantly smaller
than the m68k core.

The main complexity comes from the ModR/M byte decoding and segment
override handling, which affect almost every instruction.  These should
be implemented as shared helper functions.

---

## 4. Memory Model

### 4.1 Address Calculation

```c
static inline uint32_t i8086_addr(i8086_state_t *cpu,
                                   uint16_t seg, uint16_t off)
{
    return ((uint32_t)seg << 4) + off;
}
```

All memory accesses go through this function.  The result is masked to
20 bits (& 0xFFFFF) to emulate the 8086's address bus wrap-around at
1 MB.

### 4.2 Segment Selection

Each memory access has a default segment register, which can be overridden
by a segment prefix:

| Access type | Default segment | Override allowed |
|------------|----------------|-----------------|
| Code fetch | CS | No |
| Stack (PUSH/POP/CALL/RET) | SS | No |
| String source (SI) | DS | Yes (any) |
| String destination (DI) | ES | No |
| BP-based addressing | SS | Yes (any) |
| Other data access | DS | Yes (any) |

```c
static inline uint16_t i8086_data_seg(i8086_state_t *cpu, int bp_based)
{
    if (cpu->seg_override >= 0) {
        static const uint16_t *segs[] = { &cpu->es, &cpu->cs,
                                           &cpu->ss, &cpu->ds };
        return *segs[cpu->seg_override];
    }
    return bp_based ? cpu->ss : cpu->ds;
}
```

### 4.3 Memory Access Helpers

```c
static inline uint8_t i8086_read8(i8086_state_t *cpu, uint32_t addr) {
    return cpu->memory[addr & 0xFFFFF];
}

static inline void i8086_write8(i8086_state_t *cpu, uint32_t addr,
                                 uint8_t val) {
    cpu->memory[addr & 0xFFFFF] = val;
}

static inline uint16_t i8086_read16(i8086_state_t *cpu, uint32_t addr) {
    addr &= 0xFFFFF;
    return cpu->memory[addr] | ((uint16_t)cpu->memory[(addr + 1) & 0xFFFFF] << 8);
}

static inline void i8086_write16(i8086_state_t *cpu, uint32_t addr,
                                  uint16_t val) {
    addr &= 0xFFFFF;
    cpu->memory[addr] = val & 0xFF;
    cpu->memory[(addr + 1) & 0xFFFFF] = val >> 8;
}
```

Note: little-endian byte order throughout (unlike the m68k eCPU which is
big-endian).

### 4.4 Pointer Translation for Bridge

The DOS bridge needs to resolve guest `segment:offset` pairs to host
pointers for reading strings, buffers, etc.:

```c
static inline void *i8086_translate(i8086_state_t *cpu,
                                     uint16_t seg, uint16_t off)
{
    uint32_t addr = i8086_addr(cpu, seg, off);
    return &cpu->memory[addr & 0xFFFFF];
}
```

---

## 5. Emulator Loop

### 5.1 Main Loop Structure

```c
int i8086_run(ecpu_state_t *state)
{
    i8086_state_t *cpu = (i8086_state_t *)state;

    while (!cpu->halted && !cpu->step_trap_exit) {
        /* Reset per-instruction state */
        cpu->seg_override = -1;
        cpu->rep_prefix = 0;

        /* Handle prefixes */
        uint8_t opcode;
        for (;;) {
            opcode = i8086_fetch8(cpu);
            if (opcode == 0x26) { cpu->seg_override = 0; continue; } /* ES: */
            if (opcode == 0x2E) { cpu->seg_override = 1; continue; } /* CS: */
            if (opcode == 0x36) { cpu->seg_override = 2; continue; } /* SS: */
            if (opcode == 0x3E) { cpu->seg_override = 3; continue; } /* DS: */
            if (opcode == 0xF2) { cpu->rep_prefix = 0xF2; continue; } /* REPNE */
            if (opcode == 0xF3) { cpu->rep_prefix = 0xF3; continue; } /* REP */
            if (opcode == 0xF0) { continue; }  /* LOCK — ignored */
            break;
        }

        /* Decode and execute */
        if (i8086_exec_opcode(cpu, opcode) < 0)
            return -1;

        /* Step budget (for single-step ptrace) */
        if (cpu->step_budget > 0) {
            if (--cpu->step_budget == 0)
                return 0;
        }
    }

    return cpu->step_trap_exit ? -1 : 0;
}
```

### 5.2 Instruction Fetch

```c
static inline uint8_t i8086_fetch8(i8086_state_t *cpu) {
    uint32_t addr = i8086_addr(cpu, cpu->cs, cpu->ip);
    cpu->ip++;
    return cpu->memory[addr & 0xFFFFF];
}

static inline uint16_t i8086_fetch16(i8086_state_t *cpu) {
    uint8_t lo = i8086_fetch8(cpu);
    uint8_t hi = i8086_fetch8(cpu);
    return ((uint16_t)hi << 8) | lo;
}
```

### 5.3 Trap Firing

Software interrupts trigger `ECPU_TRAP_SWI`:

```c
static int i8086_do_int(i8086_state_t *cpu, uint8_t vector)
{
    if (cpu->trap_handler) {
        int rc = cpu->trap_handler((ecpu_state_t *)cpu,
                                    ECPU_TRAP_SWI, vector,
                                    cpu->trap_ctx);
        if (rc == ECPU_TRAP_EXIT) {
            cpu->step_trap_exit = 1;
            return -1;
        }
        if (rc == ECPU_TRAP_HANDLED)
            return 0;
    }

    /* Unhandled: execute real interrupt via IVT */
    i8086_push16(cpu, cpu->flags);
    i8086_push16(cpu, cpu->cs);
    i8086_push16(cpu, cpu->ip);
    cpu->flags &= ~(I8086_FLAG_IF | I8086_FLAG_TF);
    uint32_t ivt_addr = (uint32_t)vector * 4;
    cpu->ip = i8086_read16(cpu, ivt_addr);
    cpu->cs = i8086_read16(cpu, ivt_addr + 2);
    return 0;
}
```

I/O instructions trigger `ECPU_TRAP_IO_IN` / `ECPU_TRAP_IO_OUT`:

```c
static uint8_t i8086_do_in(i8086_state_t *cpu, uint16_t port)
{
    if (cpu->trap_handler) {
        int rc = cpu->trap_handler((ecpu_state_t *)cpu,
                                    ECPU_TRAP_IO_IN, port,
                                    cpu->trap_ctx);
        if (rc == ECPU_TRAP_EXIT) {
            cpu->step_trap_exit = 1;
            return 0xFF;
        }
    }
    return 0xFF;  /* Default: no device */
}
```

---

## 6. ModR/M Decoder

The ModR/M byte is the most complex part of the 8086 instruction decoder.
It should be implemented as a shared function used by all instructions
that take register/memory operands.

### 6.1 EA Result

```c
typedef struct {
    uint32_t addr;      /* linear address (for memory) */
    uint16_t seg;       /* segment register value used */
    uint16_t off;       /* offset within segment */
    uint8_t  type;      /* I8086_EA_REG or I8086_EA_MEM */
    uint8_t  reg_idx;   /* register index (if type == EA_REG) */
} i8086_ea_t;

#define I8086_EA_REG  0
#define I8086_EA_MEM  1
```

### 6.2 Decode Function

```c
i8086_ea_t i8086_decode_modrm(i8086_state_t *cpu, uint8_t modrm,
                               int wide)
{
    uint8_t mod = (modrm >> 6) & 3;
    uint8_t rm  = modrm & 7;
    i8086_ea_t ea;

    if (mod == 3) {
        /* Register direct */
        ea.type = I8086_EA_REG;
        ea.reg_idx = rm;
        return ea;
    }

    ea.type = I8086_EA_MEM;
    int bp_based = 0;
    uint16_t off = 0;

    switch (rm) {
    case 0: off = cpu->bx.w + cpu->si; break;
    case 1: off = cpu->bx.w + cpu->di; break;
    case 2: off = cpu->bp + cpu->si; bp_based = 1; break;
    case 3: off = cpu->bp + cpu->di; bp_based = 1; break;
    case 4: off = cpu->si; break;
    case 5: off = cpu->di; break;
    case 6:
        if (mod == 0) {
            off = i8086_fetch16(cpu);  /* Direct address */
        } else {
            off = cpu->bp; bp_based = 1;
        }
        break;
    case 7: off = cpu->bx.w; break;
    }

    if (mod == 1) off += (int16_t)(int8_t)i8086_fetch8(cpu);
    if (mod == 2) off += i8086_fetch16(cpu);

    ea.seg = i8086_data_seg(cpu, bp_based);
    ea.off = off;
    ea.addr = i8086_addr(cpu, ea.seg, off);
    return ea;
}
```

---

## 7. ALU Operations

### 7.1 Flag Computation

The 8086's flag computation is more complex than Z80 or m68k due to PF
(parity flag) and AF (auxiliary carry).  A shared ALU module handles this:

```c
/* ecpu_i8086_alu.c */

/* Parity lookup table (256 entries) — shared with Z80 core */
extern const uint8_t parity_table[256];

void i8086_alu_add8(i8086_state_t *cpu, uint8_t *dst, uint8_t src);
void i8086_alu_add16(i8086_state_t *cpu, uint16_t *dst, uint16_t src);
void i8086_alu_sub8(i8086_state_t *cpu, uint8_t *dst, uint8_t src);
void i8086_alu_sub16(i8086_state_t *cpu, uint16_t *dst, uint16_t src);
void i8086_alu_cmp8(i8086_state_t *cpu, uint8_t a, uint8_t b);
void i8086_alu_cmp16(i8086_state_t *cpu, uint16_t a, uint16_t b);
void i8086_alu_and8(i8086_state_t *cpu, uint8_t *dst, uint8_t src);
void i8086_alu_and16(i8086_state_t *cpu, uint16_t *dst, uint16_t src);
void i8086_alu_or8(i8086_state_t *cpu, uint8_t *dst, uint8_t src);
void i8086_alu_xor8(i8086_state_t *cpu, uint8_t *dst, uint8_t src);
void i8086_alu_test8(i8086_state_t *cpu, uint8_t a, uint8_t b);
void i8086_alu_inc8(i8086_state_t *cpu, uint8_t *dst);
void i8086_alu_dec8(i8086_state_t *cpu, uint8_t *dst);
void i8086_alu_neg8(i8086_state_t *cpu, uint8_t *dst);

/* Multiply/divide */
void i8086_alu_mul8(i8086_state_t *cpu, uint8_t src);   /* AX = AL * src */
void i8086_alu_mul16(i8086_state_t *cpu, uint16_t src);  /* DX:AX = AX * src */
void i8086_alu_imul8(i8086_state_t *cpu, uint8_t src);
void i8086_alu_imul16(i8086_state_t *cpu, uint16_t src);
int  i8086_alu_div8(i8086_state_t *cpu, uint8_t src);    /* Returns -1 on #DE */
int  i8086_alu_div16(i8086_state_t *cpu, uint16_t src);
int  i8086_alu_idiv8(i8086_state_t *cpu, uint8_t src);
int  i8086_alu_idiv16(i8086_state_t *cpu, uint16_t src);

/* Shift/rotate */
void i8086_alu_shl8(i8086_state_t *cpu, uint8_t *dst, uint8_t count);
void i8086_alu_shr8(i8086_state_t *cpu, uint8_t *dst, uint8_t count);
void i8086_alu_sar8(i8086_state_t *cpu, uint8_t *dst, uint8_t count);
void i8086_alu_rol8(i8086_state_t *cpu, uint8_t *dst, uint8_t count);
void i8086_alu_ror8(i8086_state_t *cpu, uint8_t *dst, uint8_t count);
void i8086_alu_rcl8(i8086_state_t *cpu, uint8_t *dst, uint8_t count);
void i8086_alu_rcr8(i8086_state_t *cpu, uint8_t *dst, uint8_t count);
/* 16-bit variants of all shift/rotate operations */

/* BCD */
void i8086_alu_daa(i8086_state_t *cpu);
void i8086_alu_das(i8086_state_t *cpu);
void i8086_alu_aaa(i8086_state_t *cpu);
void i8086_alu_aas(i8086_state_t *cpu);
void i8086_alu_aam(i8086_state_t *cpu, uint8_t base);
void i8086_alu_aad(i8086_state_t *cpu, uint8_t base);
```

### 7.2 Flag Setting Pattern

```c
static inline void i8086_set_flags_zsp8(i8086_state_t *cpu, uint8_t val)
{
    cpu->flags &= ~(I8086_FLAG_ZF | I8086_FLAG_SF | I8086_FLAG_PF);
    if (val == 0) cpu->flags |= I8086_FLAG_ZF;
    if (val & 0x80) cpu->flags |= I8086_FLAG_SF;
    if (parity_table[val]) cpu->flags |= I8086_FLAG_PF;
}
```

---

## 8. Stack Operations

```c
static inline void i8086_push16(i8086_state_t *cpu, uint16_t val)
{
    cpu->sp -= 2;
    uint32_t addr = i8086_addr(cpu, cpu->ss, cpu->sp);
    i8086_write16(cpu, addr, val);
}

static inline uint16_t i8086_pop16(i8086_state_t *cpu)
{
    uint32_t addr = i8086_addr(cpu, cpu->ss, cpu->sp);
    uint16_t val = i8086_read16(cpu, addr);
    cpu->sp += 2;
    return val;
}
```

---

## 9. ptrace Integration

### 9.1 Register Set

```c
/* In ptrace.h */
#define PPAP_TRACE_REGSET_8086  4

/* Register order in ppap_ptrace_regs.regs[] */
/* [0]=AX, [1]=BX, [2]=CX, [3]=DX, [4]=SI, [5]=DI, [6]=BP, [7]=SP,
   [8]=CS, [9]=DS, [10]=ES, [11]=SS, [12]=IP, [13]=FLAGS
   words=14 */
```

### 9.2 Capabilities

```c
/* For i8086 eCPU processes */
struct ppap_ptrace_caps caps = {
    .regset   = PPAP_TRACE_REGSET_8086,
    .abi      = PPAP_TRACE_ABI_DOS_INT21,  /* or PPAP_TRACE_ABI_PPAP */
    .surface  = PPAP_TRACE_SURFACE_ECPU,
    .surfaces = PPAP_PTRACE_SURFACE_MASK_REAL | PPAP_PTRACE_SURFACE_MASK_ECPU,
    .caps     = PPAP_PTRACE_CAP_GETREGS | PPAP_PTRACE_CAP_SETREGS
              | PPAP_PTRACE_CAP_PEEKPOKE | PPAP_PTRACE_CAP_SINGLESTEP
              | PPAP_PTRACE_CAP_SW_BP,
    .max_bps  = 8,
};
```

### 9.3 Software Breakpoints

Software breakpoints use `INT 3` (opcode 0xCC, single byte).  The eCPU
saves the original byte and patches 0xCC at the breakpoint address.  When
INT 3 fires, the trap handler reports `PPAP_DEBUG_STOP_SW_BP`.

This is the standard x86 debug mechanism and works cleanly because all
eCPU memory is writable.

### 9.4 Single-Step

Setting `step_budget = 1` causes the emulator to return after one
instruction.  The kernel reports `PPAP_DEBUG_STOP_STEP`.

### 9.5 pdb Register Display

```
(pdb) regs
AX=0000  BX=0000  CX=0000  DX=0000
SI=0000  DI=0000  BP=0000  SP=FFFE
CS=0800  DS=0800  ES=0800  SS=0800
IP=0100  FLAGS=0202 [IF]
```

### 9.6 pdb Disassembly

The pdb `disas` command gains an 8086 disassembler.  This is a
straightforward opcode table + ModR/M decoder, producing AT&T or Intel
syntax output.

---

## 10. New Files

```
src/kernel/core/cpu/
  ecpu_i8086.c        — Main emulator loop, instruction decode/dispatch
  ecpu_i8086.h        — i8086_state_t, register IDs, inline helpers
  ecpu_i8086_alu.c    — ALU operations (add, sub, and, or, shifts, mul, div)

src/user/
  pdb_disas_i8086.c   — 8086 disassembler for pdb (optional, can be later)
```

Changes to existing files:

| File | Change |
|------|--------|
| `src/kernel/core/cpu/ecpu.h` | `ECPU_ARCH_8086 = 3` already defined |
| `src/common/ptrace.h` | Add `PPAP_TRACE_REGSET_8086 = 4` |
| `src/user/pdb_regs.c` | Add 8086 register name table |
| `src/user/pdb_trace_util.c` | Add 8086 regset name |
| `config.h` | Add `PPAP_ENABLE_ECPU_I8086` build flag |

---

## 11. Build Configuration

```cmake
option(PPAP_ENABLE_ECPU_I8086 "Enable i8086 eCPU emulator" OFF)

if(PPAP_ENABLE_ECPU_I8086)
    target_sources(ppap PRIVATE
        src/kernel/core/cpu/ecpu_i8086.c
        src/kernel/core/cpu/ecpu_i8086_alu.c
    )
    target_compile_definitions(ppap PRIVATE PPAP_ENABLE_ECPU_I8086=1)
endif()
```

---

## 12. Implementation Phases

### Phase E-1: Core Decoder and Data Transfer

**Goal**: MOV, PUSH, POP, XCHG, LEA work correctly.

1. Implement `i8086_state_t` and init/reset.
2. Implement ModR/M decoder.
3. Implement segment override handling.
4. Implement all MOV variants, PUSH/POP, XCHG, LEA.
5. Implement instruction fetch loop with prefix handling.

**Verification**: A .COM binary that moves data between registers and
memory executes correctly (verified by inspecting final register state).

### Phase E-2: Arithmetic and Logic

**Goal**: ADD, SUB, CMP, AND, OR, XOR, shifts, INC, DEC work.

1. Implement `ecpu_i8086_alu.c` with all flag computation.
2. Implement arithmetic opcodes (00–3D, 40–4F, FE/FF).
3. Implement logic opcodes (20–35, 84–85).
4. Implement shift/rotate (D0–D3).
5. Implement MUL, IMUL, DIV, IDIV, NEG, NOT.

**Verification**: A test program with known arithmetic results produces
correct output.

### Phase E-3: Control Flow

**Goal**: JMP, Jcc, CALL, RET, LOOP, INT work correctly.

1. Implement all jump and call variants.
2. Implement conditional jumps (70–7F).
3. Implement LOOP variants (E0–E3).
4. Implement INT n → ECPU_TRAP_SWI dispatch.
5. Implement IRET.

**Verification**: A .COM program with function calls, loops, and an
INT 21h AH=4Ch exit terminates cleanly.

### Phase E-4: String Operations

**Goal**: MOVSB/W, CMPSB/W, STOSB/W, LODSB/W, SCASB/W with REP work.

1. Implement all string opcodes.
2. Implement REP/REPE/REPNE prefix handling.
3. Implement DF (direction flag) support.

**Verification**: A program using `REP MOVSB` to copy a block of memory
produces correct results.

### Phase E-5: I/O and Remaining Instructions

**Goal**: Complete 8086 instruction set.

1. Implement IN/OUT → ECPU_TRAP_IO_IN/OUT.
2. Implement HLT → ECPU_TRAP_HALT.
3. Implement XLAT, LES, LDS, CBW, CWD.
4. Implement BCD operations (DAA, DAS, AAA, AAS, AAM, AAD).
5. Implement flag manipulation (CLC/STC/CLI/STI/CLD/STD).

**Verification**: All 8086 instructions pass a per-opcode test suite.

### Phase E-6: Integration and ptrace

**Goal**: eCPU works end-to-end with DOS subsystem and ptrace.

1. Register `ecpu_i8086_ops` in the core table.
2. Implement get_reg/set_reg for ptrace GETREGS/SETREGS.
3. Implement software breakpoints (INT 3 patching).
4. Implement single-step mode.
5. Add 8086 register names to pdb.

**Verification**: `pdb /subsys/dos/hello.com` can step, set breakpoints,
and inspect registers.

---

## 13. Size Estimate

| Component | Estimated lines | Notes |
|-----------|----------------|-------|
| `ecpu_i8086.c` (decoder + loop) | ~2000 | Comparable to `ecpu_z80.c` (~1400) |
| `ecpu_i8086_alu.c` (ALU) | ~600 | Flag computation is the bulk |
| `ecpu_i8086.h` (state + helpers) | ~200 | Inline memory/fetch/stack ops |
| `pdb_disas_i8086.c` (disassembler) | ~500 | Optional, can be added later |
| **Total** | **~3300** | |

The 8086's variable-length encoding and ModR/M complexity makes the decoder
somewhat larger than the Z80's, but the instruction set is more regular
(fewer prefix families, no undocumented opcodes to worry about).

---

## 14. Risks and Open Questions

### 14.1 Code Size on Constrained Targets

The i8086 eCPU adds ~8–12 KB of code to the kernel.  On RP2040 (264 KB
SRAM, code in flash), this is not a problem.  On X68000 with 1 MB RAM and
tight floppy budget, it may be worth making the eCPU a loadable module or
keeping it disabled.

### 14.2 Performance

A software 8086 interpreter on a 133 MHz ARM Cortex-M0+ will be slower
than native execution.  Rough estimate: ~1–2 MIPS equivalent, compared
to the real 8086's ~0.33 MIPS at 5 MHz.  This is adequate for text-mode
DOS programs.  Compute-heavy programs (compilers, spreadsheets) will be
noticeably slow.

On native m68k at 10 MHz (X68000), performance will be lower: ~0.3–0.5
MIPS equivalent.  Still usable for simple programs.

### 14.3 Segment Wrap-around

The 8086 wraps addresses at 1 MB (0xFFFFF → 0x00000).  Later CPUs
(80286+) disabled this wrap-around and added the A20 gate.  The eCPU
should emulate 8086 behavior (wrap at 1 MB) for accuracy.

### 14.4 Undefined Behavior

The 8086 has several officially undefined behaviors (e.g., PUSH SP pushes
the decremented value on 8086 but the original on 80286+).  The emulator
should follow 8086 behavior for compatibility with programs that depend
on it.

### 14.5 Interrupt Handling

The emulator does not implement hardware interrupts (timer, keyboard).
All interrupt handling goes through `ECPU_TRAP_SWI` (software INT
instructions).  The host kernel's timer interrupt is invisible to the
emulated program — timeslicing happens when the emulator loop yields at
trap points, matching the Z80 and m68k eCPU behavior.

---

## 15. Dependency Graph

```
E-1 (data transfer + ModR/M)
  └─→ E-2 (arithmetic + logic + ALU)
        └─→ E-3 (control flow + INT trap)
              └─→ E-4 (string ops)
              └─→ E-5 (I/O + remaining)
                    └─→ E-6 (integration + ptrace)
                          └─→ DOS subsystem (docs/proposals/msdos_subsystem.md)
```

E-4 and E-5 can be developed in parallel.  E-6 (integration) requires
E-3 (control flow with INT support) at minimum.

---

## 16. Related Documentation

- [docs/proposals/msdos_subsystem.md](msdos_subsystem.md) — MS-DOS subsystem (uses this eCPU)
- [docs/targets/ia16.md](../targets/ia16.md) — IBM PC native port (V30, no eCPU needed)
- [docs/user/trace.md](../user/trace.md) — Trace and debug subsystem
- `src/kernel/core/cpu/ecpu.h` — Common eCPU interface
- `src/kernel/core/cpu/ecpu_z80.h` — Z80 eCPU (reference implementation)
- `src/kernel/core/cpu/ecpu_m68k.h` — m68k eCPU (reference for complex CPU emulation)
