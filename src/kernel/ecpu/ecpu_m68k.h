/*
 * ecpu_m68k.h — Motorola 68000 emulator core state and register IDs
 *
 * Defines m68k_state_t (the concrete type behind ecpu_state_t for the m68k
 * core), register ID constants for the common interface, flag constants,
 * and inline helpers for memory access (big-endian).
 *
 * See docs/ecpu/m68k.md for the full design.
 */

#ifndef PPAP_ECPU_M68K_H
#define PPAP_ECPU_M68K_H

#include <stdint.h>
#include "ecpu.h"

/* ── m68k register IDs (for ecpu_core_ops_t get_reg/set_reg) ────────────── */
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

/* ── Flag constants (CCR bits in SR low byte) ───────────────────────────── */
#define M68K_FLAG_C    0x01   /* Bit 0: Carry */
#define M68K_FLAG_V    0x02   /* Bit 1: Overflow */
#define M68K_FLAG_Z    0x04   /* Bit 2: Zero */
#define M68K_FLAG_N    0x08   /* Bit 3: Negative */
#define M68K_FLAG_X    0x10   /* Bit 4: Extend */

/* ── SR system bits ─────────────────────────────────────────────────────── */
#define M68K_SR_S      0x2000  /* Supervisor mode */
#define M68K_SR_T      0x8000  /* Trace mode */
#define M68K_SR_IPM    0x0700  /* Interrupt priority mask */

/* ── Operation sizes ────────────────────────────────────────────────────── */
#define M68K_SIZE_BYTE  0
#define M68K_SIZE_WORD  1
#define M68K_SIZE_LONG  2

/* ── EA result types ────────────────────────────────────────────────────── */
#define EA_TYPE_DATA_REG   0
#define EA_TYPE_ADDR_REG   1
#define EA_TYPE_MEMORY     2
#define EA_TYPE_IMMEDIATE  3

/* ── EA result structure ────────────────────────────────────────────────── */
typedef struct {
    uint32_t addr;    /* memory address, or register index */
    uint32_t value;   /* for immediate mode */
    uint8_t  type;
} ea_result_t;

/* ── m68k state structure ───────────────────────────────────────────────── */
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
    uint8_t step_budget;      /* >0: run() returns after N instructions */
    uint8_t step_trap_exit;   /* set when trap handler requests exit */

    /* Memory */
    uint8_t *memory;
    uint32_t mem_size;

    /* Trap hook */
    ecpu_trap_handler_t trap_handler;
    void *trap_ctx;
} m68k_state_t;

/* ── Size helpers ───────────────────────────────────────────────────────── */

static inline uint32_t m68k_size_bytes(uint8_t size) {
    static const uint32_t bytes[] = { 1, 2, 4 };
    return bytes[size];
}

static inline uint32_t m68k_size_mask(uint8_t size) {
    static const uint32_t masks[] = { 0xFF, 0xFFFF, 0xFFFFFFFF };
    return masks[size];
}

static inline uint32_t m68k_size_msb(uint8_t size) {
    static const uint32_t msbs[] = { 0x80, 0x8000, 0x80000000 };
    return msbs[size];
}

/* ── Memory access (big-endian) ─────────────────────────────────────────── */

static inline uint8_t m68k_read8(m68k_state_t *cpu, uint32_t addr) {
    return cpu->memory[addr & (cpu->mem_size - 1)];
}

static inline void m68k_write8(m68k_state_t *cpu, uint32_t addr, uint8_t val) {
    cpu->memory[addr & (cpu->mem_size - 1)] = val;
}

static inline uint16_t m68k_read16(m68k_state_t *cpu, uint32_t addr) {
    addr &= cpu->mem_size - 1;
    return ((uint16_t)cpu->memory[addr] << 8)
         | cpu->memory[addr + 1];
}

static inline void m68k_write16(m68k_state_t *cpu, uint32_t addr, uint16_t val) {
    addr &= cpu->mem_size - 1;
    cpu->memory[addr] = val >> 8;
    cpu->memory[addr + 1] = val & 0xFF;
}

static inline uint32_t m68k_read32(m68k_state_t *cpu, uint32_t addr) {
    addr &= cpu->mem_size - 1;
    return ((uint32_t)cpu->memory[addr] << 24)
         | ((uint32_t)cpu->memory[addr + 1] << 16)
         | ((uint32_t)cpu->memory[addr + 2] << 8)
         | cpu->memory[addr + 3];
}

static inline void m68k_write32(m68k_state_t *cpu, uint32_t addr, uint32_t val) {
    addr &= cpu->mem_size - 1;
    cpu->memory[addr]     = val >> 24;
    cpu->memory[addr + 1] = (val >> 16) & 0xFF;
    cpu->memory[addr + 2] = (val >> 8) & 0xFF;
    cpu->memory[addr + 3] = val & 0xFF;
}

/* ── Instruction fetch ──────────────────────────────────────────────────── */

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

/* ── Stack operations ───────────────────────────────────────────────────── */

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

/* ── Sized memory read/write ────────────────────────────────────────────── */

static inline uint32_t m68k_read_sz(m68k_state_t *cpu, uint32_t addr,
                                     uint8_t size) {
    switch (size) {
        case M68K_SIZE_BYTE: return m68k_read8(cpu, addr);
        case M68K_SIZE_WORD: return m68k_read16(cpu, addr);
        case M68K_SIZE_LONG: return m68k_read32(cpu, addr);
        default: return 0;
    }
}

static inline void m68k_write_sz(m68k_state_t *cpu, uint32_t addr,
                                  uint32_t val, uint8_t size) {
    switch (size) {
        case M68K_SIZE_BYTE: m68k_write8(cpu, addr, val); break;
        case M68K_SIZE_WORD: m68k_write16(cpu, addr, val); break;
        case M68K_SIZE_LONG: m68k_write32(cpu, addr, val); break;
    }
}

/* ── Data register partial write ────────────────────────────────────────── */

static inline void m68k_write_d(m68k_state_t *cpu, int reg,
                                 uint32_t val, uint8_t size) {
    switch (size) {
        case M68K_SIZE_BYTE:
            cpu->d[reg] = (cpu->d[reg] & 0xFFFFFF00) | (val & 0xFF);
            break;
        case M68K_SIZE_WORD:
            cpu->d[reg] = (cpu->d[reg] & 0xFFFF0000) | (val & 0xFFFF);
            break;
        case M68K_SIZE_LONG:
            cpu->d[reg] = val;
            break;
    }
}

/* ── ALU operations (ecpu_m68k_alu.c) ───────────────────────────────────── */

uint32_t m68k_alu_add(m68k_state_t *cpu, uint32_t src, uint32_t dst,
                       uint8_t size);
uint32_t m68k_alu_sub(m68k_state_t *cpu, uint32_t src, uint32_t dst,
                       uint8_t size);
void     m68k_alu_cmp(m68k_state_t *cpu, uint32_t src, uint32_t dst,
                       uint8_t size);
uint32_t m68k_alu_neg(m68k_state_t *cpu, uint32_t dst, uint8_t size);
uint32_t m68k_alu_negx(m68k_state_t *cpu, uint32_t dst, uint8_t size);
void     m68k_alu_tst(m68k_state_t *cpu, uint32_t val, uint8_t size);
uint32_t m68k_alu_mulu(m68k_state_t *cpu, uint16_t src, uint16_t dst);
uint32_t m68k_alu_muls(m68k_state_t *cpu, int16_t src, int16_t dst);
uint32_t m68k_alu_addx(m68k_state_t *cpu, uint32_t src, uint32_t dst,
                        uint8_t size);
uint32_t m68k_alu_subx(m68k_state_t *cpu, uint32_t src, uint32_t dst,
                        uint8_t size);
uint32_t m68k_alu_abcd(m68k_state_t *cpu, uint8_t src, uint8_t dst);
uint32_t m68k_alu_sbcd(m68k_state_t *cpu, uint8_t src, uint8_t dst);
uint32_t m68k_alu_nbcd(m68k_state_t *cpu, uint8_t dst);
int      m68k_alu_divu(m68k_state_t *cpu, uint32_t dst, uint16_t src,
                        uint32_t *result);
int      m68k_alu_divs(m68k_state_t *cpu, int32_t dst, int16_t src,
                        uint32_t *result);

/* ── EA decoder + read/write (implemented in ecpu_m68k.c) ───────────────── */

ea_result_t m68k_decode_ea(m68k_state_t *cpu, uint8_t mode,
                            uint8_t reg, uint8_t size);
uint32_t m68k_read_ea(m68k_state_t *cpu, ea_result_t *ea, uint8_t size);
void m68k_write_ea(m68k_state_t *cpu, ea_result_t *ea, uint8_t size,
                    uint32_t val);

/* ── Public API ─────────────────────────────────────────────────────────── */

extern const ecpu_core_ops_t ecpu_m68k_ops;

#endif /* PPAP_ECPU_M68K_H */
