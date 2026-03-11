/*
 * ecpu_z80_alu.c — Z80 ALU operations with full flag computation
 *
 * All 8 flag bits (including undocumented F3/F5) are computed for every
 * operation. See docs/ecpu-z80.md §8 for the flag computation design.
 */

#include "ecpu_z80.h"

/* ── Parity lookup table ─────────────────────────────────────────────────── */
/* parity_table[v] = FLAG_PV if v has even number of set bits, else 0 */

const uint8_t z80_parity_table[256] = {
    /* Generated: for each byte value, FLAG_PV (0x04) if even parity */
    0x04,0x00,0x00,0x04,0x00,0x04,0x04,0x00, /* 0x00-0x07 */
    0x00,0x04,0x04,0x00,0x04,0x00,0x00,0x04, /* 0x08-0x0F */
    0x00,0x04,0x04,0x00,0x04,0x00,0x00,0x04, /* 0x10-0x17 */
    0x04,0x00,0x00,0x04,0x00,0x04,0x04,0x00, /* 0x18-0x1F */
    0x00,0x04,0x04,0x00,0x04,0x00,0x00,0x04, /* 0x20-0x27 */
    0x04,0x00,0x00,0x04,0x00,0x04,0x04,0x00, /* 0x28-0x2F */
    0x04,0x00,0x00,0x04,0x00,0x04,0x04,0x00, /* 0x30-0x37 */
    0x00,0x04,0x04,0x00,0x04,0x00,0x00,0x04, /* 0x38-0x3F */
    0x00,0x04,0x04,0x00,0x04,0x00,0x00,0x04, /* 0x40-0x47 */
    0x04,0x00,0x00,0x04,0x00,0x04,0x04,0x00, /* 0x48-0x4F */
    0x04,0x00,0x00,0x04,0x00,0x04,0x04,0x00, /* 0x50-0x57 */
    0x00,0x04,0x04,0x00,0x04,0x00,0x00,0x04, /* 0x58-0x5F */
    0x04,0x00,0x00,0x04,0x00,0x04,0x04,0x00, /* 0x60-0x67 */
    0x00,0x04,0x04,0x00,0x04,0x00,0x00,0x04, /* 0x68-0x6F */
    0x00,0x04,0x04,0x00,0x04,0x00,0x00,0x04, /* 0x70-0x77 */
    0x04,0x00,0x00,0x04,0x00,0x04,0x04,0x00, /* 0x78-0x7F */
    0x00,0x04,0x04,0x00,0x04,0x00,0x00,0x04, /* 0x80-0x87 */
    0x04,0x00,0x00,0x04,0x00,0x04,0x04,0x00, /* 0x88-0x8F */
    0x04,0x00,0x00,0x04,0x00,0x04,0x04,0x00, /* 0x90-0x97 */
    0x00,0x04,0x04,0x00,0x04,0x00,0x00,0x04, /* 0x98-0x9F */
    0x04,0x00,0x00,0x04,0x00,0x04,0x04,0x00, /* 0xA0-0xA7 */
    0x00,0x04,0x04,0x00,0x04,0x00,0x00,0x04, /* 0xA8-0xAF */
    0x00,0x04,0x04,0x00,0x04,0x00,0x00,0x04, /* 0xB0-0xB7 */
    0x04,0x00,0x00,0x04,0x00,0x04,0x04,0x00, /* 0xB8-0xBF */
    0x04,0x00,0x00,0x04,0x00,0x04,0x04,0x00, /* 0xC0-0xC7 */
    0x00,0x04,0x04,0x00,0x04,0x00,0x00,0x04, /* 0xC8-0xCF */
    0x00,0x04,0x04,0x00,0x04,0x00,0x00,0x04, /* 0xD0-0xD7 */
    0x04,0x00,0x00,0x04,0x00,0x04,0x04,0x00, /* 0xD8-0xDF */
    0x00,0x04,0x04,0x00,0x04,0x00,0x00,0x04, /* 0xE0-0xE7 */
    0x04,0x00,0x00,0x04,0x00,0x04,0x04,0x00, /* 0xE8-0xEF */
    0x04,0x00,0x00,0x04,0x00,0x04,0x04,0x00, /* 0xF0-0xF7 */
    0x00,0x04,0x04,0x00,0x04,0x00,0x00,0x04, /* 0xF8-0xFF */
};

/* ── 8-bit ALU operations ────────────────────────────────────────────────── */

void z80_add_a(z80_state_t *cpu, uint8_t val)
{
    uint16_t result = cpu->a + val;
    uint8_t res8 = result & 0xFF;

    cpu->f = res8 & FLAG_35;                              /* F3, F5 */
    if (res8 == 0)            cpu->f |= FLAG_Z;
    if (res8 & 0x80)          cpu->f |= FLAG_S;
    if (result > 0xFF)        cpu->f |= FLAG_C;
    if (((cpu->a ^ val ^ res8) & 0x10))
        cpu->f |= FLAG_H;
    if ((~(cpu->a ^ val) & (cpu->a ^ res8)) & 0x80)
        cpu->f |= FLAG_PV;

    cpu->a = res8;
}

void z80_adc_a(z80_state_t *cpu, uint8_t val)
{
    uint8_t carry = cpu->f & FLAG_C;
    uint16_t result = cpu->a + val + carry;
    uint8_t res8 = result & 0xFF;

    cpu->f = res8 & FLAG_35;
    if (res8 == 0)            cpu->f |= FLAG_Z;
    if (res8 & 0x80)          cpu->f |= FLAG_S;
    if (result > 0xFF)        cpu->f |= FLAG_C;
    if (((cpu->a ^ val ^ res8) & 0x10))
        cpu->f |= FLAG_H;
    if ((~(cpu->a ^ val) & (cpu->a ^ res8)) & 0x80)
        cpu->f |= FLAG_PV;

    cpu->a = res8;
}

void z80_sub_a(z80_state_t *cpu, uint8_t val)
{
    uint16_t result = cpu->a - val;
    uint8_t res8 = result & 0xFF;

    cpu->f = (res8 & FLAG_35) | FLAG_N;
    if (res8 == 0)            cpu->f |= FLAG_Z;
    if (res8 & 0x80)          cpu->f |= FLAG_S;
    if (result > 0xFF)        cpu->f |= FLAG_C;
    if (((cpu->a ^ val ^ res8) & 0x10))
        cpu->f |= FLAG_H;
    if (((cpu->a ^ val) & (cpu->a ^ res8)) & 0x80)
        cpu->f |= FLAG_PV;

    cpu->a = res8;
}

void z80_sbc_a(z80_state_t *cpu, uint8_t val)
{
    uint8_t carry = cpu->f & FLAG_C;
    uint16_t result = cpu->a - val - carry;
    uint8_t res8 = result & 0xFF;

    cpu->f = (res8 & FLAG_35) | FLAG_N;
    if (res8 == 0)            cpu->f |= FLAG_Z;
    if (res8 & 0x80)          cpu->f |= FLAG_S;
    if (result > 0xFF)        cpu->f |= FLAG_C;
    if (((cpu->a ^ val ^ res8) & 0x10))
        cpu->f |= FLAG_H;
    if (((cpu->a ^ val) & (cpu->a ^ res8)) & 0x80)
        cpu->f |= FLAG_PV;

    cpu->a = res8;
}

void z80_and_a(z80_state_t *cpu, uint8_t val)
{
    cpu->a &= val;
    cpu->f = (cpu->a & FLAG_35) | FLAG_H;
    if (cpu->a == 0) cpu->f |= FLAG_Z;
    if (cpu->a & 0x80) cpu->f |= FLAG_S;
    cpu->f |= z80_parity_table[cpu->a];
}

void z80_xor_a(z80_state_t *cpu, uint8_t val)
{
    cpu->a ^= val;
    cpu->f = cpu->a & FLAG_35;
    if (cpu->a == 0) cpu->f |= FLAG_Z;
    if (cpu->a & 0x80) cpu->f |= FLAG_S;
    cpu->f |= z80_parity_table[cpu->a];
}

void z80_or_a(z80_state_t *cpu, uint8_t val)
{
    cpu->a |= val;
    cpu->f = cpu->a & FLAG_35;
    if (cpu->a == 0) cpu->f |= FLAG_Z;
    if (cpu->a & 0x80) cpu->f |= FLAG_S;
    cpu->f |= z80_parity_table[cpu->a];
}

void z80_cp_a(z80_state_t *cpu, uint8_t val)
{
    uint16_t result = cpu->a - val;
    uint8_t res8 = result & 0xFF;

    cpu->f = (val & FLAG_35) | FLAG_N;   /* F3, F5 from operand! */
    if (res8 == 0)            cpu->f |= FLAG_Z;
    if (res8 & 0x80)          cpu->f |= FLAG_S;
    if (result > 0xFF)        cpu->f |= FLAG_C;
    if (((cpu->a ^ val ^ res8) & 0x10))
        cpu->f |= FLAG_H;
    if (((cpu->a ^ val) & (cpu->a ^ res8)) & 0x80)
        cpu->f |= FLAG_PV;
    /* A is NOT modified */
}

/* ── ALU dispatch ────────────────────────────────────────────────────────── */

void z80_alu_op(z80_state_t *cpu, uint8_t op, uint8_t val)
{
    switch (op) {
        case 0: z80_add_a(cpu, val); break;
        case 1: z80_adc_a(cpu, val); break;
        case 2: z80_sub_a(cpu, val); break;
        case 3: z80_sbc_a(cpu, val); break;
        case 4: z80_and_a(cpu, val); break;
        case 5: z80_xor_a(cpu, val); break;
        case 6: z80_or_a(cpu, val);  break;
        case 7: z80_cp_a(cpu, val);  break;
    }
}

/* ── INC/DEC 8-bit ───────────────────────────────────────────────────────── */

uint8_t z80_inc8(z80_state_t *cpu, uint8_t val)
{
    uint8_t res = val + 1;
    cpu->f = (cpu->f & FLAG_C)         /* preserve carry */
           | (res & FLAG_35)           /* F3, F5 from result */
           | ((res == 0) ? FLAG_Z : 0)
           | (res & 0x80)             /* S */
           | (((val & 0x0F) == 0x0F) ? FLAG_H : 0)
           | ((val == 0x7F) ? FLAG_PV : 0);
    /* N = 0 (addition) */
    return res;
}

uint8_t z80_dec8(z80_state_t *cpu, uint8_t val)
{
    uint8_t res = val - 1;
    cpu->f = (cpu->f & FLAG_C)         /* preserve carry */
           | (res & FLAG_35)           /* F3, F5 from result */
           | FLAG_N
           | ((res == 0) ? FLAG_Z : 0)
           | (res & 0x80)             /* S */
           | (((val & 0x0F) == 0x00) ? FLAG_H : 0)
           | ((val == 0x80) ? FLAG_PV : 0);
    return res;
}

/* ── ADD HL, rr (16-bit add) ─────────────────────────────────────────────── */

void z80_add_hl(z80_state_t *cpu, uint16_t val)
{
    uint16_t hl = z80_hl(cpu);
    uint32_t result = hl + val;
    uint16_t res16 = result & 0xFFFF;

    cpu->f = (cpu->f & (FLAG_S | FLAG_Z | FLAG_PV))  /* preserve S, Z, PV */
           | ((res16 >> 8) & FLAG_35)                 /* F3, F5 from high byte */
           | ((result > 0xFFFF) ? FLAG_C : 0)
           | ((((hl ^ val ^ res16) >> 8) & 0x10) ? FLAG_H : 0);
    /* N = 0 */

    z80_set_hl(cpu, res16);
    cpu->wz = hl + 1;  /* WZ = old HL + 1 */
}

/* ── ADC HL,rr / SBC HL,rr (16-bit with flags) ─────────────────────────── */

void z80_adc_hl(z80_state_t *cpu, uint16_t val)
{
    uint16_t hl = z80_hl(cpu);
    uint8_t carry = cpu->f & FLAG_C;
    uint32_t result = hl + val + carry;
    uint16_t res16 = result & 0xFFFF;

    cpu->f = ((res16 >> 8) & (FLAG_S | FLAG_35))
           | ((res16 == 0) ? FLAG_Z : 0)
           | ((result > 0xFFFF) ? FLAG_C : 0)
           | ((((hl ^ val ^ res16) >> 8) & 0x10) ? FLAG_H : 0)
           | (((~(hl ^ val) & (hl ^ res16)) >> 8) & 0x80 ? FLAG_PV : 0);
    /* N = 0 */

    z80_set_hl(cpu, res16);
    cpu->wz = hl + 1;
}

void z80_sbc_hl(z80_state_t *cpu, uint16_t val)
{
    uint16_t hl = z80_hl(cpu);
    uint8_t carry = cpu->f & FLAG_C;
    uint32_t result = hl - val - carry;
    uint16_t res16 = result & 0xFFFF;

    cpu->f = ((res16 >> 8) & (FLAG_S | FLAG_35))
           | FLAG_N
           | ((res16 == 0) ? FLAG_Z : 0)
           | ((result > 0xFFFF) ? FLAG_C : 0)
           | ((((hl ^ val ^ res16) >> 8) & 0x10) ? FLAG_H : 0)
           | ((((hl ^ val) & (hl ^ res16)) >> 8) & 0x80 ? FLAG_PV : 0);

    z80_set_hl(cpu, res16);
    cpu->wz = hl + 1;
}

/* ── Rotate accumulator ─────────────────────────────────────────────────── */

void z80_rlca(z80_state_t *cpu)
{
    uint8_t carry = (cpu->a >> 7) & 1;
    cpu->a = (cpu->a << 1) | carry;
    cpu->f = (cpu->f & (FLAG_S | FLAG_Z | FLAG_PV))
           | (cpu->a & FLAG_35)
           | carry;
}

void z80_rrca(z80_state_t *cpu)
{
    uint8_t carry = cpu->a & 1;
    cpu->a = (cpu->a >> 1) | (carry << 7);
    cpu->f = (cpu->f & (FLAG_S | FLAG_Z | FLAG_PV))
           | (cpu->a & FLAG_35)
           | carry;
}

void z80_rla(z80_state_t *cpu)
{
    uint8_t old_carry = cpu->f & FLAG_C;
    uint8_t new_carry = (cpu->a >> 7) & 1;
    cpu->a = (cpu->a << 1) | old_carry;
    cpu->f = (cpu->f & (FLAG_S | FLAG_Z | FLAG_PV))
           | (cpu->a & FLAG_35)
           | new_carry;
}

void z80_rra(z80_state_t *cpu)
{
    uint8_t old_carry = cpu->f & FLAG_C;
    uint8_t new_carry = cpu->a & 1;
    cpu->a = (cpu->a >> 1) | (old_carry << 7);
    cpu->f = (cpu->f & (FLAG_S | FLAG_Z | FLAG_PV))
           | (cpu->a & FLAG_35)
           | new_carry;
}

/* ── DAA ─────────────────────────────────────────────────────────────────── */

void z80_daa(z80_state_t *cpu)
{
    uint8_t a = cpu->a;
    uint8_t correction = 0;
    uint8_t carry = cpu->f & FLAG_C;

    if ((cpu->f & FLAG_H) || (a & 0x0F) > 9)
        correction |= 0x06;
    if (carry || a > 0x99) {
        correction |= 0x60;
        carry = FLAG_C;
    }

    if (cpu->f & FLAG_N)
        cpu->a -= correction;
    else
        cpu->a += correction;

    cpu->f = (cpu->f & FLAG_N) | carry;
    cpu->f |= cpu->a & FLAG_35;
    if (cpu->a == 0) cpu->f |= FLAG_Z;
    if (cpu->a & 0x80) cpu->f |= FLAG_S;
    cpu->f |= z80_parity_table[cpu->a];
    /* H flag after DAA */
    if (cpu->f & FLAG_N)
        cpu->f |= ((a & 0x0F) < (correction & 0x0F)) ? FLAG_H : 0;
    else
        cpu->f |= (((a & 0x0F) + (correction & 0x0F)) > 0x0F) ? FLAG_H : 0;
}

/* ── CPL ─────────────────────────────────────────────────────────────────── */

void z80_cpl(z80_state_t *cpu)
{
    cpu->a = ~cpu->a;
    cpu->f = (cpu->f & (FLAG_S | FLAG_Z | FLAG_PV | FLAG_C))
           | (cpu->a & FLAG_35)
           | FLAG_H | FLAG_N;
}

/* ── SCF / CCF ───────────────────────────────────────────────────────────── */

void z80_scf(z80_state_t *cpu)
{
    /* F3, F5 from A OR F (undocumented) */
    uint8_t f35 = (cpu->a | cpu->f) & FLAG_35;
    cpu->f = (cpu->f & (FLAG_S | FLAG_Z | FLAG_PV))
           | f35 | FLAG_C;
    /* H = 0, N = 0 */
}

void z80_ccf(z80_state_t *cpu)
{
    /* F3, F5 from A OR F (undocumented) */
    uint8_t f35 = (cpu->a | cpu->f) & FLAG_35;
    uint8_t old_c = cpu->f & FLAG_C;
    cpu->f = (cpu->f & (FLAG_S | FLAG_Z | FLAG_PV))
           | f35
           | (old_c ? FLAG_H : 0)   /* H = old carry */
           | (old_c ? 0 : FLAG_C);  /* C = ~old carry */
    /* N = 0 */
}

/* ── ADD IX/IY,rr (16-bit add targeting index register) ─────────────── */

void z80_add_ix(z80_state_t *cpu, uint16_t *idx, uint16_t val)
{
    uint16_t ix = *idx;
    uint32_t result = ix + val;
    uint16_t res16 = result & 0xFFFF;

    cpu->f = (cpu->f & (FLAG_S | FLAG_Z | FLAG_PV))  /* preserve S, Z, PV */
           | ((res16 >> 8) & FLAG_35)                 /* F3, F5 from high byte */
           | ((result > 0xFFFF) ? FLAG_C : 0)
           | ((((ix ^ val ^ res16) >> 8) & 0x10) ? FLAG_H : 0);
    /* N = 0 */

    *idx = res16;
    cpu->wz = ix + 1;  /* WZ = old IX + 1 */
}

/* ── CB prefix shift/rotate operations ───────────────────────────────────
 * All set S, Z, PV=parity, F3/F5 from result.  H=0, N=0.
 * Return the result byte; caller writes it back to the register. */

static uint8_t cb_flags(z80_state_t *cpu, uint8_t res, uint8_t carry)
{
    cpu->f = (res & (FLAG_S | FLAG_35))
           | z80_parity_table[res]
           | ((res == 0) ? FLAG_Z : 0)
           | carry;
    return res;
}

uint8_t z80_rlc(z80_state_t *cpu, uint8_t val)
{
    uint8_t carry = (val >> 7) & 1;
    return cb_flags(cpu, (val << 1) | carry, carry);
}

uint8_t z80_rrc(z80_state_t *cpu, uint8_t val)
{
    uint8_t carry = val & 1;
    return cb_flags(cpu, (val >> 1) | (carry << 7), carry);
}

uint8_t z80_rl(z80_state_t *cpu, uint8_t val)
{
    uint8_t old_c = cpu->f & FLAG_C;
    uint8_t carry = (val >> 7) & 1;
    return cb_flags(cpu, (val << 1) | old_c, carry);
}

uint8_t z80_rr(z80_state_t *cpu, uint8_t val)
{
    uint8_t old_c = cpu->f & FLAG_C;
    uint8_t carry = val & 1;
    return cb_flags(cpu, (val >> 1) | (old_c << 7), carry);
}

uint8_t z80_sla(z80_state_t *cpu, uint8_t val)
{
    uint8_t carry = (val >> 7) & 1;
    return cb_flags(cpu, val << 1, carry);
}

uint8_t z80_sra(z80_state_t *cpu, uint8_t val)
{
    uint8_t carry = val & 1;
    return cb_flags(cpu, (val >> 1) | (val & 0x80), carry);
}

uint8_t z80_sll(z80_state_t *cpu, uint8_t val)
{
    /* Undocumented: shift left, set bit 0 to 1 */
    uint8_t carry = (val >> 7) & 1;
    return cb_flags(cpu, (val << 1) | 1, carry);
}

uint8_t z80_srl(z80_state_t *cpu, uint8_t val)
{
    uint8_t carry = val & 1;
    return cb_flags(cpu, val >> 1, carry);
}

/* ── CB prefix BIT operation ─────────────────────────────────────────────
 * BIT n,r: test bit n of register, set Z if bit is 0.
 * S is set if n=7 and bit is set.  PV = Z (same as parity of single bit).
 * F3/F5 from result for register operands, from WZ high byte for (HL). */

void z80_bit(z80_state_t *cpu, uint8_t bit, uint8_t val)
{
    uint8_t result = val & (1 << bit);
    cpu->f = (cpu->f & FLAG_C)
           | FLAG_H
           | (result ? (result & FLAG_S) : (FLAG_Z | FLAG_PV))
           | (val & FLAG_35);
}
