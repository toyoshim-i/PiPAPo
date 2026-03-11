/*
 * ecpu_z80.h — Z80 emulator core state and register IDs
 *
 * Defines z80_state_t (the concrete type behind ecpu_state_t for the Z80
 * core), register ID constants for the common interface, flag constants,
 * and inline helpers for register pair / memory access.
 *
 * See docs/ecpu-z80.md for the full design.
 */

#ifndef PPAP_ECPU_Z80_H
#define PPAP_ECPU_Z80_H

#include <stdint.h>
#include "ecpu.h"

/* ── Z80 register IDs (for ecpu_core_ops_t get_reg/set_reg) ─────────────── */
#define Z80_REG_A    0
#define Z80_REG_F    1
#define Z80_REG_B    2
#define Z80_REG_C    3
#define Z80_REG_D    4
#define Z80_REG_E    5
#define Z80_REG_H    6
#define Z80_REG_L    7
#define Z80_REG_AF   8    /* 16-bit pair */
#define Z80_REG_BC   9
#define Z80_REG_DE   10
#define Z80_REG_HL   11
#define Z80_REG_IX   12
#define Z80_REG_IY   13
#define Z80_REG_SP   14
#define Z80_REG_PC   15
#define Z80_REG_I    16
#define Z80_REG_R    17
#define Z80_REG_AF2  18   /* shadow registers */
#define Z80_REG_BC2  19
#define Z80_REG_DE2  20
#define Z80_REG_HL2  21
#define Z80_REG_IFF1 22
#define Z80_REG_IFF2 23
#define Z80_REG_IM   24
#define Z80_REG_WZ   25   /* internal MEMPTR register */

/* ── Flag constants ──────────────────────────────────────────────────────── */
#define FLAG_C   0x01   /* Bit 0: Carry                                     */
#define FLAG_N   0x02   /* Bit 1: Subtract                                  */
#define FLAG_PV  0x04   /* Bit 2: Parity/Overflow                           */
#define FLAG_F3  0x08   /* Bit 3: Undocumented (copy of bit 3 of result)    */
#define FLAG_H   0x10   /* Bit 4: Half carry                                */
#define FLAG_F5  0x20   /* Bit 5: Undocumented (copy of bit 5 of result)    */
#define FLAG_Z   0x40   /* Bit 6: Zero                                      */
#define FLAG_S   0x80   /* Bit 7: Sign                                      */
#define FLAG_35  (FLAG_F3 | FLAG_F5)  /* Mask for undocumented bits         */

/* ── Scheduler slice size ────────────────────────────────────────────────── */
#define Z80_SLICE_SIZE  1000  /* instructions between sched_yield() calls   */

/* ── Z80 state structure ─────────────────────────────────────────────────── */
typedef struct z80_state {
    /* Main register set — individual bytes for direct access.
     * Register pairs accessed via inline helpers below. */
    uint8_t a, f;
    uint8_t b, c, d, e, h, l;

    /* Shadow register set */
    uint8_t a2, f2;
    uint8_t b2, c2, d2, e2, h2, l2;

    /* Index registers (16-bit) */
    uint16_t ix, iy;

    /* Special registers */
    uint16_t sp, pc;
    uint8_t  i;       /* interrupt vector page */
    uint8_t  r;       /* refresh counter (low 7 bits increment) */

    /* Internal register — affects undocumented F3/F5 behavior */
    uint16_t wz;      /* MEMPTR / WZ register */

    /* Interrupt state */
    uint8_t iff1, iff2;  /* interrupt flip-flops */
    uint8_t im;          /* interrupt mode (0, 1, or 2) */

    /* Emulator state (not part of real Z80) */
    uint8_t halted;           /* 1 if HALT executed */
    int32_t slice_counter;    /* instructions until yield */

    /* Memory (64 KB flat) */
    uint8_t *memory;
    uint32_t mem_size;        /* always 65536 for Z80 */

    /* Trap hook — via eCPU common interface */
    ecpu_trap_handler_t trap_handler;
    void *trap_ctx;           /* opaque context for personality */
} z80_state_t;

/* ── Register pair access ────────────────────────────────────────────────── */

static inline uint16_t z80_af(const z80_state_t *cpu) {
    return ((uint16_t)cpu->a << 8) | cpu->f;
}
static inline void z80_set_af(z80_state_t *cpu, uint16_t val) {
    cpu->a = val >> 8;
    cpu->f = val & 0xFF;
}

static inline uint16_t z80_bc(const z80_state_t *cpu) {
    return ((uint16_t)cpu->b << 8) | cpu->c;
}
static inline void z80_set_bc(z80_state_t *cpu, uint16_t val) {
    cpu->b = val >> 8;
    cpu->c = val & 0xFF;
}

static inline uint16_t z80_de(const z80_state_t *cpu) {
    return ((uint16_t)cpu->d << 8) | cpu->e;
}
static inline void z80_set_de(z80_state_t *cpu, uint16_t val) {
    cpu->d = val >> 8;
    cpu->e = val & 0xFF;
}

static inline uint16_t z80_hl(const z80_state_t *cpu) {
    return ((uint16_t)cpu->h << 8) | cpu->l;
}
static inline void z80_set_hl(z80_state_t *cpu, uint16_t val) {
    cpu->h = val >> 8;
    cpu->l = val & 0xFF;
}

/* ── Memory access ───────────────────────────────────────────────────────── */

static inline uint8_t z80_read8(z80_state_t *cpu, uint16_t addr) {
    return cpu->memory[addr];
}

static inline void z80_write8(z80_state_t *cpu, uint16_t addr, uint8_t val) {
    cpu->memory[addr] = val;
}

static inline uint16_t z80_read16(z80_state_t *cpu, uint16_t addr) {
    return cpu->memory[addr] |
           ((uint16_t)cpu->memory[(uint16_t)(addr + 1)] << 8);
}

static inline void z80_write16(z80_state_t *cpu, uint16_t addr, uint16_t val) {
    cpu->memory[addr] = val & 0xFF;
    cpu->memory[(uint16_t)(addr + 1)] = val >> 8;
}

/* ── Instruction fetch ───────────────────────────────────────────────────── */

static inline uint8_t z80_fetch8(z80_state_t *cpu) {
    return cpu->memory[cpu->pc++];
}

static inline uint16_t z80_fetch16(z80_state_t *cpu) {
    uint8_t lo = z80_fetch8(cpu);
    uint8_t hi = z80_fetch8(cpu);
    return ((uint16_t)hi << 8) | lo;
}

/* ── Stack operations ────────────────────────────────────────────────────── */

static inline void z80_push16(z80_state_t *cpu, uint16_t val) {
    cpu->sp--;
    cpu->memory[cpu->sp] = val >> 8;
    cpu->sp--;
    cpu->memory[cpu->sp] = val & 0xFF;
}

static inline uint16_t z80_pop16(z80_state_t *cpu) {
    uint16_t val = z80_read16(cpu, cpu->sp);
    cpu->sp += 2;
    return val;
}

/* ── Register decode (3-bit encoding) ────────────────────────────────────── */

static inline uint8_t z80_read_r8(z80_state_t *cpu, uint8_t code) {
    switch (code) {
        case 0: return cpu->b;
        case 1: return cpu->c;
        case 2: return cpu->d;
        case 3: return cpu->e;
        case 4: return cpu->h;
        case 5: return cpu->l;
        case 6: return z80_read8(cpu, z80_hl(cpu));
        case 7: return cpu->a;
        default: return 0;
    }
}

static inline void z80_write_r8(z80_state_t *cpu, uint8_t code, uint8_t val) {
    switch (code) {
        case 0: cpu->b = val; break;
        case 1: cpu->c = val; break;
        case 2: cpu->d = val; break;
        case 3: cpu->e = val; break;
        case 4: cpu->h = val; break;
        case 5: cpu->l = val; break;
        case 6: z80_write8(cpu, z80_hl(cpu), val); break;
        case 7: cpu->a = val; break;
    }
}

/* ── ALU operations (ecpu_z80_alu.c) ─────────────────────────────────────── */

extern const uint8_t z80_parity_table[256];

void z80_add_a(z80_state_t *cpu, uint8_t val);
void z80_adc_a(z80_state_t *cpu, uint8_t val);
void z80_sub_a(z80_state_t *cpu, uint8_t val);
void z80_sbc_a(z80_state_t *cpu, uint8_t val);
void z80_and_a(z80_state_t *cpu, uint8_t val);
void z80_xor_a(z80_state_t *cpu, uint8_t val);
void z80_or_a(z80_state_t *cpu, uint8_t val);
void z80_cp_a(z80_state_t *cpu, uint8_t val);
void z80_alu_op(z80_state_t *cpu, uint8_t op, uint8_t val);

uint8_t z80_inc8(z80_state_t *cpu, uint8_t val);
uint8_t z80_dec8(z80_state_t *cpu, uint8_t val);
void z80_add_hl(z80_state_t *cpu, uint16_t val);
void z80_adc_hl(z80_state_t *cpu, uint16_t val);
void z80_sbc_hl(z80_state_t *cpu, uint16_t val);
void z80_add_ix(z80_state_t *cpu, uint16_t *idx, uint16_t val);

void z80_rlca(z80_state_t *cpu);
void z80_rrca(z80_state_t *cpu);
void z80_rla(z80_state_t *cpu);
void z80_rra(z80_state_t *cpu);

void z80_daa(z80_state_t *cpu);
void z80_cpl(z80_state_t *cpu);
void z80_scf(z80_state_t *cpu);
void z80_ccf(z80_state_t *cpu);

/* CB prefix shift/rotate operations — return result byte */
uint8_t z80_rlc(z80_state_t *cpu, uint8_t val);
uint8_t z80_rrc(z80_state_t *cpu, uint8_t val);
uint8_t z80_rl(z80_state_t *cpu, uint8_t val);
uint8_t z80_rr(z80_state_t *cpu, uint8_t val);
uint8_t z80_sla(z80_state_t *cpu, uint8_t val);
uint8_t z80_sra(z80_state_t *cpu, uint8_t val);
uint8_t z80_sll(z80_state_t *cpu, uint8_t val);  /* undocumented */
uint8_t z80_srl(z80_state_t *cpu, uint8_t val);
void z80_bit(z80_state_t *cpu, uint8_t bit, uint8_t val);

/* 16-bit register pair access by 2-bit pp field */
uint16_t z80_read_rr(z80_state_t *cpu, uint8_t pp);
void z80_write_rr(z80_state_t *cpu, uint8_t pp, uint16_t val);

/* ── Public API ──────────────────────────────────────────────────────────── */

/* The Z80 core's ops table — registered for use by subsystems/kernel */
extern const ecpu_core_ops_t ecpu_z80_ops;

#endif /* PPAP_ECPU_Z80_H */
