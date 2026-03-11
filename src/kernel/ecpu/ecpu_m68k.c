/*
 * ecpu_m68k.c — Motorola 68000 emulator core
 *
 * Implements the eCPU common interface for the m68k core.
 * Step 1: MOVE.B/W/L, MOVEQ, LEA, CLR, NOP, STOP, ILLEGAL,
 *         A-line/F-line traps, EA modes 0–4.
 *
 * See docs/ecpu-m68k.md for the full design.
 */

#include "ecpu_m68k.h"
#include <string.h>  /* memset */

/* ── Trap fire helper ───────────────────────────────────────────────────── */

static int m68k_fire_trap(m68k_state_t *cpu, int trap_type, uint32_t param)
{
    if (cpu->trap_handler)
        return cpu->trap_handler((ecpu_state_t *)cpu, trap_type, param,
                                 cpu->trap_ctx);
    return ECPU_TRAP_UNHANDLED;
}

/* ── Effective Address decoder ──────────────────────────────────────────── */

ea_result_t m68k_decode_ea(m68k_state_t *cpu, uint8_t mode,
                            uint8_t reg, uint8_t size)
{
    ea_result_t ea;
    ea.value = 0;

    switch (mode) {
        case 0: /* Dn — data register direct */
            ea.type = EA_TYPE_DATA_REG;
            ea.addr = reg;
            break;

        case 1: /* An — address register direct */
            ea.type = EA_TYPE_ADDR_REG;
            ea.addr = reg;
            break;

        case 2: /* (An) — address register indirect */
            ea.type = EA_TYPE_MEMORY;
            ea.addr = cpu->a[reg];
            break;

        case 3: /* (An)+ — post-increment */
            ea.type = EA_TYPE_MEMORY;
            ea.addr = cpu->a[reg];
            cpu->a[reg] += m68k_size_bytes(size);
            /* A7 byte ops use 2-byte increment to keep SP aligned */
            if (size == M68K_SIZE_BYTE && reg == 7)
                cpu->a[reg]++;
            break;

        case 4: /* -(An) — pre-decrement */
            if (size == M68K_SIZE_BYTE && reg == 7)
                cpu->a[reg]--;
            cpu->a[reg] -= m68k_size_bytes(size);
            ea.type = EA_TYPE_MEMORY;
            ea.addr = cpu->a[reg];
            break;

        case 5: /* d16(An) — displacement indirect */
            ea.type = EA_TYPE_MEMORY;
            ea.addr = cpu->a[reg] + (int16_t)m68k_fetch16(cpu);
            break;

        case 6: { /* d8(An,Xn) — index indirect */
            uint16_t ext = m68k_fetch16(cpu);
            int8_t disp = ext & 0xFF;
            int xreg = (ext >> 12) & 7;
            int32_t xval;
            if (ext & 0x8000)
                xval = (int32_t)cpu->a[xreg];
            else
                xval = (int32_t)cpu->d[xreg];
            if (!(ext & 0x0800))
                xval = (int16_t)(xval & 0xFFFF);
            ea.type = EA_TYPE_MEMORY;
            ea.addr = cpu->a[reg] + disp + xval;
            break;
        }

        case 7:
            switch (reg) {
                case 0: /* abs.W — absolute short (sign-extended) */
                    ea.type = EA_TYPE_MEMORY;
                    ea.addr = (uint32_t)(int16_t)m68k_fetch16(cpu);
                    break;
                case 1: /* abs.L — absolute long */
                    ea.type = EA_TYPE_MEMORY;
                    ea.addr = m68k_fetch32(cpu);
                    break;
                case 2: { /* d16(PC) — PC-relative displacement */
                    uint32_t base = cpu->pc;
                    ea.type = EA_TYPE_MEMORY;
                    ea.addr = base + (int16_t)m68k_fetch16(cpu);
                    break;
                }
                case 3: { /* d8(PC,Xn) — PC-relative index */
                    uint32_t base = cpu->pc;
                    uint16_t ext = m68k_fetch16(cpu);
                    int8_t disp = ext & 0xFF;
                    int xreg = (ext >> 12) & 7;
                    int32_t xval;
                    if (ext & 0x8000)
                        xval = (int32_t)cpu->a[xreg];
                    else
                        xval = (int32_t)cpu->d[xreg];
                    if (!(ext & 0x0800))
                        xval = (int16_t)(xval & 0xFFFF);
                    ea.type = EA_TYPE_MEMORY;
                    ea.addr = base + disp + xval;
                    break;
                }
                case 4: /* #imm — immediate */
                    ea.type = EA_TYPE_IMMEDIATE;
                    if (size == M68K_SIZE_LONG)
                        ea.value = m68k_fetch32(cpu);
                    else
                        ea.value = m68k_fetch16(cpu);
                    if (size == M68K_SIZE_BYTE)
                        ea.value &= 0xFF;
                    break;
                default:
                    /* Invalid EA mode — treat as NOP */
                    ea.type = EA_TYPE_DATA_REG;
                    ea.addr = 0;
                    break;
            }
            break;
    }
    return ea;
}

/* ── EA read/write helpers ──────────────────────────────────────────────── */

uint32_t m68k_read_ea(m68k_state_t *cpu, ea_result_t *ea, uint8_t size)
{
    switch (ea->type) {
        case EA_TYPE_DATA_REG:
            return cpu->d[ea->addr] & m68k_size_mask(size);
        case EA_TYPE_ADDR_REG:
            return cpu->a[ea->addr] & m68k_size_mask(size);
        case EA_TYPE_MEMORY:
            return m68k_read_sz(cpu, ea->addr, size);
        case EA_TYPE_IMMEDIATE:
            return ea->value;
        default:
            return 0;
    }
}

void m68k_write_ea(m68k_state_t *cpu, ea_result_t *ea, uint8_t size,
                    uint32_t val)
{
    switch (ea->type) {
        case EA_TYPE_DATA_REG:
            m68k_write_d(cpu, ea->addr, val, size);
            break;
        case EA_TYPE_ADDR_REG:
            /* Address register writes are always 32-bit.
             * Word writes sign-extend to 32 bits. */
            if (size == M68K_SIZE_WORD)
                cpu->a[ea->addr] = (uint32_t)(int16_t)(val & 0xFFFF);
            else
                cpu->a[ea->addr] = val;
            break;
        case EA_TYPE_MEMORY:
            m68k_write_sz(cpu, ea->addr, val, size);
            break;
        default:
            break;
    }
}

/* ── CCR helpers for MOVE ───────────────────────────────────────────────── */

static void m68k_set_ccr_move(m68k_state_t *cpu, uint32_t val, uint8_t size)
{
    uint32_t msb = m68k_size_msb(size);
    uint32_t mask = m68k_size_mask(size);

    /* Preserve X, clear N/Z/V/C, then set N/Z */
    uint16_t ccr = cpu->sr & M68K_FLAG_X;
    if ((val & mask) == 0)  ccr |= M68K_FLAG_Z;
    if (val & msb)          ccr |= M68K_FLAG_N;
    /* V and C cleared */
    cpu->sr = (cpu->sr & 0xFF00) | ccr;
}

/* ── MOVE.B/W/L ─────────────────────────────────────────────────────────── */

static void m68k_move(m68k_state_t *cpu, uint16_t opcode, uint8_t size)
{
    /* Source EA: bits 5–3 = mode, bits 2–0 = reg */
    uint8_t src_mode = (opcode >> 3) & 7;
    uint8_t src_reg  = opcode & 7;

    /* Destination EA: bits 11–9 = reg, bits 8–6 = mode (reversed!) */
    uint8_t dst_reg  = (opcode >> 9) & 7;
    uint8_t dst_mode = (opcode >> 6) & 7;

    ea_result_t src_ea = m68k_decode_ea(cpu, src_mode, src_reg, size);
    uint32_t val = m68k_read_ea(cpu, &src_ea, size);

    ea_result_t dst_ea = m68k_decode_ea(cpu, dst_mode, dst_reg, size);
    m68k_write_ea(cpu, &dst_ea, size, val);

    /* MOVEA (dst mode 1 = address register) does not set flags */
    if (dst_mode != 1)
        m68k_set_ccr_move(cpu, val, size);
}

/* ── MOVEQ ──────────────────────────────────────────────────────────────── */

static void m68k_moveq(m68k_state_t *cpu, uint16_t opcode)
{
    int reg = (opcode >> 9) & 7;
    int32_t val = (int8_t)(opcode & 0xFF);  /* sign-extend 8→32 */
    cpu->d[reg] = (uint32_t)val;
    m68k_set_ccr_move(cpu, (uint32_t)val, M68K_SIZE_LONG);
}

/* ── Group 4 (Miscellaneous) ────────────────────────────────────────────── */

static int m68k_group4(m68k_state_t *cpu, uint16_t opcode)
{
    uint8_t bits_11_8 = (opcode >> 8) & 0xF;
    uint8_t bits_7_6  = (opcode >> 6) & 3;
    uint8_t ea_mode   = (opcode >> 3) & 7;
    uint8_t ea_reg    = opcode & 7;

    /* NOP: 4E71 */
    if (opcode == 0x4E71)
        return 0;

    /* STOP: 4E72 */
    if (opcode == 0x4E72) {
        uint16_t imm = m68k_fetch16(cpu);
        cpu->sr = imm;
        cpu->stopped = 1;
        int rc = m68k_fire_trap(cpu, ECPU_TRAP_HALT, imm);
        if (rc == ECPU_TRAP_EXIT)
            return -1;
        return 0;
    }

    /* ILLEGAL: 4AFC */
    if (opcode == 0x4AFC) {
        int rc = m68k_fire_trap(cpu, ECPU_TRAP_ILLEGAL, opcode);
        if (rc == ECPU_TRAP_EXIT)
            return -1;
        return 0;
    }

    /* RTS: 4E75 */
    if (opcode == 0x4E75) {
        cpu->pc = m68k_pop32(cpu);
        return 0;
    }

    /* RTE: 4E73 */
    if (opcode == 0x4E73) {
        cpu->sr = m68k_pop16(cpu);
        cpu->pc = m68k_pop32(cpu);
        return 0;
    }

    /* TRAP #n: 4E40–4E4F */
    if ((opcode & 0xFFF0) == 0x4E40) {
        int n = opcode & 0x0F;
        int rc = m68k_fire_trap(cpu, ECPU_TRAP_SWI, n);
        if (rc == ECPU_TRAP_EXIT)
            return -1;
        if (rc == ECPU_TRAP_HANDLED)
            return 0;
        /* Unhandled: push SR+PC, load vector */
        m68k_push32(cpu, cpu->pc);
        m68k_push16(cpu, cpu->sr);
        cpu->sr |= M68K_SR_S;
        cpu->pc = m68k_read32(cpu, (32 + n) * 4);
        return 0;
    }

    /* LEA: 0100 rrr 111 ea  (bits 8–6 = 111) */
    if (bits_7_6 == 3 && (bits_11_8 & 1)) {
        int reg = (opcode >> 9) & 7;
        ea_result_t ea = m68k_decode_ea(cpu, ea_mode, ea_reg, M68K_SIZE_LONG);
        if (ea.type == EA_TYPE_MEMORY)
            cpu->a[reg] = ea.addr;
        return 0;
    }

    /* CLR: 0100 0010 ss ea  (bits 11–8 = 0010) */
    if (bits_11_8 == 0x2) {
        uint8_t size = bits_7_6;  /* 00=byte, 01=word, 10=long */
        if (size > 2) return 0;   /* size 11 is invalid for CLR */
        ea_result_t ea = m68k_decode_ea(cpu, ea_mode, ea_reg, size);
        m68k_write_ea(cpu, &ea, size, 0);
        /* CCR: X unchanged, N=0, Z=1, V=0, C=0 */
        cpu->sr = (cpu->sr & (0xFF00 | M68K_FLAG_X)) | M68K_FLAG_Z;
        return 0;
    }

    /* JSR: 4E80–4EBF (0100 1110 10 ea) */
    if ((opcode & 0xFFC0) == 0x4E80) {
        ea_result_t ea = m68k_decode_ea(cpu, ea_mode, ea_reg, M68K_SIZE_LONG);
        if (ea.type == EA_TYPE_MEMORY) {
            m68k_push32(cpu, cpu->pc);
            cpu->pc = ea.addr;
        }
        return 0;
    }

    /* JMP: 4EC0–4EFF (0100 1110 11 ea) */
    if ((opcode & 0xFFC0) == 0x4EC0) {
        ea_result_t ea = m68k_decode_ea(cpu, ea_mode, ea_reg, M68K_SIZE_LONG);
        if (ea.type == EA_TYPE_MEMORY)
            cpu->pc = ea.addr;
        return 0;
    }

    /* Unimplemented group 4 — treat as NOP for now */
    return 0;
}

/* ── Main decode loop ───────────────────────────────────────────────────── */

static int ecpu_m68k_run(ecpu_state_t *state)
{
    m68k_state_t *cpu = (m68k_state_t *)state;
    cpu->stopped = 0;

    for (;;) {
        if (cpu->stopped)
            return 0;

        uint16_t opcode = m68k_fetch16(cpu);

        switch (opcode >> 12) {
            case 0x0: /* Group 0: immediate/bit ops — future */
                break;

            case 0x1: /* MOVE.B */
                m68k_move(cpu, opcode, M68K_SIZE_BYTE);
                break;

            case 0x2: /* MOVE.L */
                m68k_move(cpu, opcode, M68K_SIZE_LONG);
                break;

            case 0x3: /* MOVE.W */
                m68k_move(cpu, opcode, M68K_SIZE_WORD);
                break;

            case 0x4: /* Group 4: miscellaneous */
                if (m68k_group4(cpu, opcode) < 0)
                    return 0;
                break;

            case 0x5: /* Group 5: ADDQ/SUBQ/Scc/DBcc — future */
                break;

            case 0x6: /* Group 6: Bcc/BSR/BRA — future */
                break;

            case 0x7: /* MOVEQ */
                m68k_moveq(cpu, opcode);
                break;

            case 0x8: /* Group 8: OR/DIV/SBCD — future */
                break;

            case 0x9: /* Group 9: SUB — future */
                break;

            case 0xA: { /* A-line trap */
                int rc = m68k_fire_trap(cpu, ECPU_TRAP_ILLEGAL, opcode);
                if (rc == ECPU_TRAP_EXIT)
                    return 0;
                break;
            }

            case 0xB: /* Group B: CMP/EOR — future */
                break;

            case 0xC: /* Group C: AND/MUL/EXG — future */
                break;

            case 0xD: /* Group D: ADD — future */
                break;

            case 0xE: /* Group E: shifts — future */
                break;

            case 0xF: { /* F-line trap */
                int rc = m68k_fire_trap(cpu, ECPU_TRAP_ILLEGAL, opcode);
                if (rc == ECPU_TRAP_EXIT)
                    return 0;
                break;
            }
        }

        if (--cpu->slice_counter <= 0) {
            cpu->slice_counter = M68K_SLICE_SIZE;
            /* sched_yield() — called in kernel context */
        }
    }
}

/* ── Common interface implementations ───────────────────────────────────── */

static int ecpu_m68k_init(ecpu_state_t *state, uint8_t *memory,
                           uint32_t mem_size)
{
    m68k_state_t *cpu = (m68k_state_t *)state;
    memset(cpu, 0, sizeof(*cpu));
    cpu->memory = memory;
    cpu->mem_size = mem_size;
    cpu->slice_counter = M68K_SLICE_SIZE;
    /* Start in supervisor mode */
    cpu->sr = M68K_SR_S | 0x0700;
    return 0;
}

static void ecpu_m68k_reset(ecpu_state_t *state)
{
    m68k_state_t *cpu = (m68k_state_t *)state;
    ecpu_trap_handler_t handler = cpu->trap_handler;
    void *ctx = cpu->trap_ctx;
    uint8_t *mem = cpu->memory;
    uint32_t msz = cpu->mem_size;

    memset(cpu, 0, sizeof(*cpu));
    cpu->memory = mem;
    cpu->mem_size = msz;
    cpu->trap_handler = handler;
    cpu->trap_ctx = ctx;
    cpu->slice_counter = M68K_SLICE_SIZE;
    cpu->sr = M68K_SR_S | 0x0700;
}

static void ecpu_m68k_set_trap_handler(ecpu_state_t *state,
                                        ecpu_trap_handler_t handler, void *ctx)
{
    m68k_state_t *cpu = (m68k_state_t *)state;
    cpu->trap_handler = handler;
    cpu->trap_ctx = ctx;
}

static uint32_t ecpu_m68k_get_reg(ecpu_state_t *state, int reg_id)
{
    m68k_state_t *cpu = (m68k_state_t *)state;
    if (reg_id >= M68K_REG_D0 && reg_id <= M68K_REG_D7)
        return cpu->d[reg_id - M68K_REG_D0];
    if (reg_id >= M68K_REG_A0 && reg_id <= M68K_REG_A7)
        return cpu->a[reg_id - M68K_REG_A0];
    switch (reg_id) {
        case M68K_REG_PC:  return cpu->pc;
        case M68K_REG_SR:  return cpu->sr;
        case M68K_REG_USP: return cpu->usp;
        case M68K_REG_SSP: return cpu->ssp;
        default: return 0;
    }
}

static void ecpu_m68k_set_reg(ecpu_state_t *state, int reg_id, uint32_t val)
{
    m68k_state_t *cpu = (m68k_state_t *)state;
    if (reg_id >= M68K_REG_D0 && reg_id <= M68K_REG_D7) {
        cpu->d[reg_id - M68K_REG_D0] = val;
        return;
    }
    if (reg_id >= M68K_REG_A0 && reg_id <= M68K_REG_A7) {
        cpu->a[reg_id - M68K_REG_A0] = val;
        return;
    }
    switch (reg_id) {
        case M68K_REG_PC:  cpu->pc = val; break;
        case M68K_REG_SR:  cpu->sr = val & 0xFFFF; break;
        case M68K_REG_USP: cpu->usp = val; break;
        case M68K_REG_SSP: cpu->ssp = val; break;
    }
}

static void *ecpu_m68k_translate_ptr(ecpu_state_t *state, uint32_t guest_addr,
                                      uint32_t size)
{
    m68k_state_t *cpu = (m68k_state_t *)state;
    guest_addr &= cpu->mem_size - 1;
    if (guest_addr + size > cpu->mem_size)
        return 0;
    return cpu->memory + guest_addr;
}

static uint8_t ecpu_m68k_read8(ecpu_state_t *state, uint32_t addr)
{
    return m68k_read8((m68k_state_t *)state, addr);
}

static void ecpu_m68k_write8(ecpu_state_t *state, uint32_t addr, uint8_t val)
{
    m68k_write8((m68k_state_t *)state, addr, val);
}

static uint16_t ecpu_m68k_read16(ecpu_state_t *state, uint32_t addr)
{
    return m68k_read16((m68k_state_t *)state, addr);
}

static void ecpu_m68k_write16(ecpu_state_t *state, uint32_t addr, uint16_t val)
{
    m68k_write16((m68k_state_t *)state, addr, val);
}

/* ── Core ops table ─────────────────────────────────────────────────────── */

const ecpu_core_ops_t ecpu_m68k_ops = {
    .name           = "m68k",
    .arch_id        = ECPU_ARCH_M68K,
    .init           = ecpu_m68k_init,
    .reset          = ecpu_m68k_reset,
    .run            = ecpu_m68k_run,
    .set_trap_handler = ecpu_m68k_set_trap_handler,
    .get_reg        = ecpu_m68k_get_reg,
    .set_reg        = ecpu_m68k_set_reg,
    .translate_ptr  = ecpu_m68k_translate_ptr,
    .read8          = ecpu_m68k_read8,
    .write8         = ecpu_m68k_write8,
    .read16         = ecpu_m68k_read16,
    .write16        = ecpu_m68k_write16,
    .state_size     = sizeof(m68k_state_t),
};
