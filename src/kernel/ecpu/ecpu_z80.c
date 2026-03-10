/*
 * ecpu_z80.c — Z80 emulator core
 *
 * Implements the eCPU common interface for the Z80 core.
 * Step 1: NOP, HALT, LD r,r', LD r,n, LD rr,nn, JP nn, JR e, CALL nn, RET.
 * Step 2: ALU ops, INC/DEC, ADD HL,rr, rotates, DAA, CPL, SCF, CCF.
 * Step 3: JP/JR/CALL/RET cc, DJNZ, RST, PUSH/POP, EX/EXX.
 * Step 4: Memory indirect loads, IN/OUT port trapping.
 *
 * See docs/ecpu-z80.md for the full design.
 */

#include "ecpu_z80.h"
#include <string.h>  /* memset */

/* ── Common interface implementations ────────────────────────────────────── */

static int ecpu_z80_init(ecpu_state_t *state, uint8_t *memory,
                         uint32_t mem_size)
{
    z80_state_t *cpu = (z80_state_t *)state;
    memset(cpu, 0, sizeof(*cpu));
    cpu->memory = memory;
    cpu->mem_size = mem_size;
    cpu->slice_counter = Z80_SLICE_SIZE;
    return 0;
}

static void ecpu_z80_reset(ecpu_state_t *state)
{
    z80_state_t *cpu = (z80_state_t *)state;
    ecpu_trap_handler_t handler = cpu->trap_handler;
    void *ctx = cpu->trap_ctx;
    uint8_t *mem = cpu->memory;
    uint32_t msz = cpu->mem_size;

    memset(cpu, 0, sizeof(*cpu));
    cpu->memory = mem;
    cpu->mem_size = msz;
    cpu->trap_handler = handler;
    cpu->trap_ctx = ctx;
    cpu->slice_counter = Z80_SLICE_SIZE;
}

static void ecpu_z80_set_trap_handler(ecpu_state_t *state,
                                      ecpu_trap_handler_t handler, void *ctx)
{
    z80_state_t *cpu = (z80_state_t *)state;
    cpu->trap_handler = handler;
    cpu->trap_ctx = ctx;
}

static uint32_t ecpu_z80_get_reg(ecpu_state_t *state, int reg_id)
{
    z80_state_t *cpu = (z80_state_t *)state;
    switch (reg_id) {
        case Z80_REG_A:    return cpu->a;
        case Z80_REG_F:    return cpu->f;
        case Z80_REG_B:    return cpu->b;
        case Z80_REG_C:    return cpu->c;
        case Z80_REG_D:    return cpu->d;
        case Z80_REG_E:    return cpu->e;
        case Z80_REG_H:    return cpu->h;
        case Z80_REG_L:    return cpu->l;
        case Z80_REG_AF:   return z80_af(cpu);
        case Z80_REG_BC:   return z80_bc(cpu);
        case Z80_REG_DE:   return z80_de(cpu);
        case Z80_REG_HL:   return z80_hl(cpu);
        case Z80_REG_IX:   return cpu->ix;
        case Z80_REG_IY:   return cpu->iy;
        case Z80_REG_SP:   return cpu->sp;
        case Z80_REG_PC:   return cpu->pc;
        case Z80_REG_I:    return cpu->i;
        case Z80_REG_R:    return cpu->r;
        case Z80_REG_AF2:  return ((uint16_t)cpu->a2 << 8) | cpu->f2;
        case Z80_REG_BC2:  return ((uint16_t)cpu->b2 << 8) | cpu->c2;
        case Z80_REG_DE2:  return ((uint16_t)cpu->d2 << 8) | cpu->e2;
        case Z80_REG_HL2:  return ((uint16_t)cpu->h2 << 8) | cpu->l2;
        case Z80_REG_IFF1: return cpu->iff1;
        case Z80_REG_IFF2: return cpu->iff2;
        case Z80_REG_IM:   return cpu->im;
        case Z80_REG_WZ:   return cpu->wz;
        default:           return 0;
    }
}

static void ecpu_z80_set_reg(ecpu_state_t *state, int reg_id, uint32_t val)
{
    z80_state_t *cpu = (z80_state_t *)state;
    switch (reg_id) {
        case Z80_REG_A:    cpu->a = val; break;
        case Z80_REG_F:    cpu->f = val; break;
        case Z80_REG_B:    cpu->b = val; break;
        case Z80_REG_C:    cpu->c = val; break;
        case Z80_REG_D:    cpu->d = val; break;
        case Z80_REG_E:    cpu->e = val; break;
        case Z80_REG_H:    cpu->h = val; break;
        case Z80_REG_L:    cpu->l = val; break;
        case Z80_REG_AF:   z80_set_af(cpu, val); break;
        case Z80_REG_BC:   z80_set_bc(cpu, val); break;
        case Z80_REG_DE:   z80_set_de(cpu, val); break;
        case Z80_REG_HL:   z80_set_hl(cpu, val); break;
        case Z80_REG_IX:   cpu->ix = val; break;
        case Z80_REG_IY:   cpu->iy = val; break;
        case Z80_REG_SP:   cpu->sp = val; break;
        case Z80_REG_PC:   cpu->pc = val; break;
        case Z80_REG_I:    cpu->i  = val; break;
        case Z80_REG_R:    cpu->r  = val; break;
        case Z80_REG_AF2:  cpu->a2 = val >> 8; cpu->f2 = val & 0xFF; break;
        case Z80_REG_BC2:  cpu->b2 = val >> 8; cpu->c2 = val & 0xFF; break;
        case Z80_REG_DE2:  cpu->d2 = val >> 8; cpu->e2 = val & 0xFF; break;
        case Z80_REG_HL2:  cpu->h2 = val >> 8; cpu->l2 = val & 0xFF; break;
        case Z80_REG_IFF1: cpu->iff1 = val; break;
        case Z80_REG_IFF2: cpu->iff2 = val; break;
        case Z80_REG_IM:   cpu->im   = val; break;
        case Z80_REG_WZ:   cpu->wz   = val; break;
    }
}

static void *ecpu_z80_translate_ptr(ecpu_state_t *state, uint32_t guest_addr,
                                    uint32_t size)
{
    z80_state_t *cpu = (z80_state_t *)state;
    if (guest_addr + size > cpu->mem_size)
        return (void *)0;
    return cpu->memory + guest_addr;
}

static uint8_t ecpu_z80_read8_op(ecpu_state_t *state, uint32_t addr)
{
    z80_state_t *cpu = (z80_state_t *)state;
    return cpu->memory[(uint16_t)addr];
}

static void ecpu_z80_write8_op(ecpu_state_t *state, uint32_t addr,
                               uint8_t val)
{
    z80_state_t *cpu = (z80_state_t *)state;
    cpu->memory[(uint16_t)addr] = val;
}

static uint16_t ecpu_z80_read16_op(ecpu_state_t *state, uint32_t addr)
{
    z80_state_t *cpu = (z80_state_t *)state;
    return z80_read16(cpu, (uint16_t)addr);
}

static void ecpu_z80_write16_op(ecpu_state_t *state, uint32_t addr,
                                uint16_t val)
{
    z80_state_t *cpu = (z80_state_t *)state;
    z80_write16(cpu, (uint16_t)addr, val);
}

/* ── Helper: fire trap handler ───────────────────────────────────────────── */

static int z80_fire_trap(z80_state_t *cpu, int trap_type, uint32_t param)
{
    if (cpu->trap_handler)
        return cpu->trap_handler((ecpu_state_t *)cpu, trap_type, param,
                                 cpu->trap_ctx);
    return ECPU_TRAP_UNHANDLED;
}

/* ── 16-bit register pair read/write by 2-bit code (pp field) ────────────── */

/* Used in Step 2+ (ADD HL,rr etc.) */
uint16_t z80_read_rr(z80_state_t *cpu, uint8_t pp)
{
    switch (pp) {
        case 0: return z80_bc(cpu);
        case 1: return z80_de(cpu);
        case 2: return z80_hl(cpu);
        case 3: return cpu->sp;
        default: return 0;
    }
}

void z80_write_rr(z80_state_t *cpu, uint8_t pp, uint16_t val)
{
    switch (pp) {
        case 0: z80_set_bc(cpu, val); break;
        case 1: z80_set_de(cpu, val); break;
        case 2: z80_set_hl(cpu, val); break;
        case 3: cpu->sp = val; break;
    }
}

/* ── Condition code evaluation ────────────────────────────────────────────
 * 3-bit condition codes used in JP cc, JR cc, CALL cc, RET cc:
 *   0=NZ, 1=Z, 2=NC, 3=C, 4=PO, 5=PE, 6=P(plus), 7=M(minus) */

static int z80_condition(z80_state_t *cpu, uint8_t cc)
{
    switch (cc) {
        case 0: return !(cpu->f & FLAG_Z);   /* NZ */
        case 1: return !!(cpu->f & FLAG_Z);  /* Z  */
        case 2: return !(cpu->f & FLAG_C);   /* NC */
        case 3: return !!(cpu->f & FLAG_C);  /* C  */
        case 4: return !(cpu->f & FLAG_PV);  /* PO */
        case 5: return !!(cpu->f & FLAG_PV); /* PE */
        case 6: return !(cpu->f & FLAG_S);   /* P  */
        case 7: return !!(cpu->f & FLAG_S);  /* M  */
        default: return 0;
    }
}

/* ── PUSH/POP register pair by 2-bit code (qq field) ────────────────────
 * qq: 0=BC, 1=DE, 2=HL, 3=AF (differs from pp which has SP) */

static uint16_t z80_read_qq(z80_state_t *cpu, uint8_t qq)
{
    switch (qq) {
        case 0: return z80_bc(cpu);
        case 1: return z80_de(cpu);
        case 2: return z80_hl(cpu);
        case 3: return z80_af(cpu);
        default: return 0;
    }
}

static void z80_write_qq(z80_state_t *cpu, uint8_t qq, uint16_t val)
{
    switch (qq) {
        case 0: z80_set_bc(cpu, val); break;
        case 1: z80_set_de(cpu, val); break;
        case 2: z80_set_hl(cpu, val); break;
        case 3: z80_set_af(cpu, val); break;
    }
}

/* ── Main interpreter loop ───────────────────────────────────────────────── */

static int ecpu_z80_run(ecpu_state_t *state)
{
    z80_state_t *cpu = (z80_state_t *)state;

    for (;;) {
        if (cpu->halted) {
            /* Fire HALT trap — personality may restart or exit */
            int rc = z80_fire_trap(cpu, ECPU_TRAP_HALT, 0);
            if (rc == ECPU_TRAP_EXIT)
                return 0;
            if (!cpu->halted)
                continue;  /* trap handler cleared halted */
            return 0;      /* no handler or unhandled — stop */
        }

        uint8_t opcode = z80_fetch8(cpu);

        /* Increment R register (low 7 bits) per instruction fetch */
        cpu->r = (cpu->r & 0x80) | ((cpu->r + 1) & 0x7F);

        /* Decode using xx/yyy/zzz bit fields */
        uint8_t xx  = opcode >> 6;
        uint8_t yyy = (opcode >> 3) & 7;
        uint8_t zzz = opcode & 7;

        switch (xx) {
        case 1:
            /* xx=01: LD r, r' — except 0x76 = HALT */
            if (opcode == 0x76) {
                cpu->halted = 1;
                continue;  /* loop back to halted check */
            }
            z80_write_r8(cpu, yyy, z80_read_r8(cpu, zzz));
            break;

        case 0:
            /* xx=00: miscellaneous */
            switch (zzz) {
            case 0:
                /* NOP, EX AF,AF', DJNZ, JR, JR cc */
                if (yyy == 0) {
                    /* NOP */
                } else if (yyy == 1) {
                    /* EX AF, AF' (0x08) */
                    uint8_t t;
                    t = cpu->a; cpu->a = cpu->a2; cpu->a2 = t;
                    t = cpu->f; cpu->f = cpu->f2; cpu->f2 = t;
                } else if (yyy == 2) {
                    /* DJNZ e (0x10) */
                    int8_t offset = (int8_t)z80_fetch8(cpu);
                    cpu->b--;
                    if (cpu->b != 0)
                        cpu->pc += offset;
                } else if (yyy == 3) {
                    /* JR e (0x18) */
                    int8_t offset = (int8_t)z80_fetch8(cpu);
                    cpu->pc += offset;
                } else {
                    /* JR cc, e (yyy=4..7 → cc=0..3: NZ,Z,NC,C) */
                    int8_t offset = (int8_t)z80_fetch8(cpu);
                    if (z80_condition(cpu, yyy - 4))
                        cpu->pc += offset;
                }
                break;

            case 1:
                if (yyy & 1) {
                    /* ADD HL, rr */
                    z80_add_hl(cpu, z80_read_rr(cpu, yyy >> 1));
                } else {
                    /* LD rr, nn */
                    uint16_t nn = z80_fetch16(cpu);
                    z80_write_rr(cpu, yyy >> 1, nn);
                }
                break;

            case 2:
                /* Memory indirect loads/stores */
                switch (yyy) {
                case 0: /* LD (BC), A */
                    z80_write8(cpu, z80_bc(cpu), cpu->a);
                    cpu->wz = (z80_bc(cpu) + 1) & 0xFFFF;
                    break;
                case 1: /* LD A, (BC) */
                    cpu->a = z80_read8(cpu, z80_bc(cpu));
                    cpu->wz = (z80_bc(cpu) + 1) & 0xFFFF;
                    break;
                case 2: /* LD (DE), A */
                    z80_write8(cpu, z80_de(cpu), cpu->a);
                    cpu->wz = (z80_de(cpu) + 1) & 0xFFFF;
                    break;
                case 3: /* LD A, (DE) */
                    cpu->a = z80_read8(cpu, z80_de(cpu));
                    cpu->wz = (z80_de(cpu) + 1) & 0xFFFF;
                    break;
                case 4: /* LD (nn), HL */
                    {
                        uint16_t nn = z80_fetch16(cpu);
                        z80_write16(cpu, nn, z80_hl(cpu));
                        cpu->wz = (nn + 1) & 0xFFFF;
                    }
                    break;
                case 5: /* LD HL, (nn) */
                    {
                        uint16_t nn = z80_fetch16(cpu);
                        z80_set_hl(cpu, z80_read16(cpu, nn));
                        cpu->wz = (nn + 1) & 0xFFFF;
                    }
                    break;
                case 6: /* LD (nn), A */
                    {
                        uint16_t nn = z80_fetch16(cpu);
                        z80_write8(cpu, nn, cpu->a);
                        cpu->wz = (nn + 1) & 0xFFFF;
                    }
                    break;
                case 7: /* LD A, (nn) */
                    {
                        uint16_t nn = z80_fetch16(cpu);
                        cpu->a = z80_read8(cpu, nn);
                        cpu->wz = (nn + 1) & 0xFFFF;
                    }
                    break;
                }
                break;

            case 3:
                /* INC/DEC rr (16-bit) — no flags affected */
                {
                    uint8_t pp = yyy >> 1;
                    uint16_t val = z80_read_rr(cpu, pp);
                    if (yyy & 1)
                        z80_write_rr(cpu, pp, val - 1);
                    else
                        z80_write_rr(cpu, pp, val + 1);
                }
                break;

            case 4:
                /* INC r (8-bit) */
                z80_write_r8(cpu, yyy,
                             z80_inc8(cpu, z80_read_r8(cpu, yyy)));
                break;

            case 5:
                /* DEC r (8-bit) */
                z80_write_r8(cpu, yyy,
                             z80_dec8(cpu, z80_read_r8(cpu, yyy)));
                break;

            case 6:
                /* LD r, n (8-bit immediate) */
                {
                    uint8_t n = z80_fetch8(cpu);
                    z80_write_r8(cpu, yyy, n);
                }
                break;

            case 7:
                /* Accumulator/flag ops: RLCA,RRCA,RLA,RRA,DAA,CPL,SCF,CCF */
                switch (yyy) {
                case 0: z80_rlca(cpu); break;
                case 1: z80_rrca(cpu); break;
                case 2: z80_rla(cpu);  break;
                case 3: z80_rra(cpu);  break;
                case 4: z80_daa(cpu);  break;
                case 5: z80_cpl(cpu);  break;
                case 6: z80_scf(cpu);  break;
                case 7: z80_ccf(cpu);  break;
                }
                break;
            }
            break;

        case 2:
            /* xx=10: ALU A, r */
            z80_alu_op(cpu, yyy, z80_read_r8(cpu, zzz));
            break;

        case 3:
            /* xx=11: miscellaneous */
            switch (zzz) {
            case 0:
                /* RET cc */
                if (z80_condition(cpu, yyy)) {
                    cpu->pc = z80_pop16(cpu);
                    cpu->wz = cpu->pc;
                }
                break;

            case 1:
                if (yyy & 1) {
                    /* Odd yyy: RET, EXX, JP (HL), LD SP,HL */
                    switch (yyy) {
                    case 1: /* RET (0xC9) */
                        cpu->pc = z80_pop16(cpu);
                        cpu->wz = cpu->pc;
                        break;
                    case 3: /* EXX (0xD9) */
                        {
                            uint8_t t;
                            t = cpu->b; cpu->b = cpu->b2; cpu->b2 = t;
                            t = cpu->c; cpu->c = cpu->c2; cpu->c2 = t;
                            t = cpu->d; cpu->d = cpu->d2; cpu->d2 = t;
                            t = cpu->e; cpu->e = cpu->e2; cpu->e2 = t;
                            t = cpu->h; cpu->h = cpu->h2; cpu->h2 = t;
                            t = cpu->l; cpu->l = cpu->l2; cpu->l2 = t;
                        }
                        break;
                    case 5: /* JP (HL) (0xE9) */
                        cpu->pc = z80_hl(cpu);
                        break;
                    case 7: /* LD SP, HL (0xF9) */
                        cpu->sp = z80_hl(cpu);
                        break;
                    }
                } else {
                    /* POP qq */
                    z80_write_qq(cpu, yyy >> 1, z80_pop16(cpu));
                }
                break;

            case 2:
                /* JP cc, nn */
                {
                    uint16_t nn = z80_fetch16(cpu);
                    cpu->wz = nn;
                    if (z80_condition(cpu, yyy))
                        cpu->pc = nn;
                }
                break;

            case 3:
                /* Miscellaneous: JP nn, CB prefix, OUT, IN, EX, DI, EI */
                switch (yyy) {
                case 0: /* JP nn (0xC3) */
                    {
                        uint16_t nn = z80_fetch16(cpu);
                        cpu->wz = nn;
                        cpu->pc = nn;
                    }
                    break;
                case 1: /* CB prefix — Step 5 */
                    break;
                case 2: /* OUT (n), A (0xD3) */
                    {
                        uint8_t port = z80_fetch8(cpu);
                        uint16_t addr = ((uint16_t)cpu->a << 8) | port;
                        cpu->wz = ((uint16_t)cpu->a << 8) | ((port + 1) & 0xFF);
                        int rc = z80_fire_trap(cpu, ECPU_TRAP_IO_OUT, addr);
                        if (rc == ECPU_TRAP_EXIT)
                            return 0;
                    }
                    break;
                case 3: /* IN A, (n) (0xDB) */
                    {
                        uint8_t port = z80_fetch8(cpu);
                        uint16_t addr = ((uint16_t)cpu->a << 8) | port;
                        int rc = z80_fire_trap(cpu, ECPU_TRAP_IO_IN, addr);
                        if (rc == ECPU_TRAP_EXIT)
                            return 0;
                        cpu->wz = ((uint16_t)cpu->a << 8) | ((port + 1) & 0xFF);
                    }
                    break;
                case 4: /* EX (SP), HL (0xE3) */
                    {
                        uint16_t val = z80_read16(cpu, cpu->sp);
                        z80_write16(cpu, cpu->sp, z80_hl(cpu));
                        z80_set_hl(cpu, val);
                        cpu->wz = val;
                    }
                    break;
                case 5: /* EX DE, HL (0xEB) */
                    {
                        uint16_t de = z80_de(cpu);
                        z80_set_de(cpu, z80_hl(cpu));
                        z80_set_hl(cpu, de);
                    }
                    break;
                case 6: /* DI (0xF3) */
                    cpu->iff1 = 0;
                    cpu->iff2 = 0;
                    break;
                case 7: /* EI (0xFB) */
                    cpu->iff1 = 1;
                    cpu->iff2 = 1;
                    break;
                }
                break;

            case 4:
                /* CALL cc, nn */
                {
                    uint16_t nn = z80_fetch16(cpu);
                    cpu->wz = nn;
                    if (z80_condition(cpu, yyy)) {
                        z80_push16(cpu, cpu->pc);
                        cpu->pc = nn;
                    }
                }
                break;

            case 5:
                if (yyy & 1) {
                    /* Odd yyy: CALL nn, DD/ED/FD prefixes */
                    if (yyy == 1) {
                        /* CALL nn (0xCD) */
                        uint16_t nn = z80_fetch16(cpu);
                        cpu->wz = nn;
                        int rc = z80_fire_trap(cpu, ECPU_TRAP_CALL, nn);
                        if (rc == ECPU_TRAP_EXIT)
                            return 0;
                        if (rc == ECPU_TRAP_HANDLED)
                            break;
                        z80_push16(cpu, cpu->pc);
                        cpu->pc = nn;
                    }
                    /* yyy=3: DD prefix, yyy=5: ED prefix,
                     * yyy=7: FD prefix — Step 5/6/7 */
                } else {
                    /* PUSH qq */
                    z80_push16(cpu, z80_read_qq(cpu, yyy >> 1));
                }
                break;

            case 6:
                /* ALU A, n (immediate) */
                {
                    uint8_t n = z80_fetch8(cpu);
                    z80_alu_op(cpu, yyy, n);
                }
                break;

            case 7:
                /* RST p — restart to address p*8 */
                {
                    uint16_t addr = yyy * 8;
                    int rc = z80_fire_trap(cpu, ECPU_TRAP_CALL, addr);
                    if (rc == ECPU_TRAP_EXIT)
                        return 0;
                    if (rc == ECPU_TRAP_HANDLED)
                        break;
                    z80_push16(cpu, cpu->pc);
                    cpu->pc = addr;
                    cpu->wz = addr;
                }
                break;
            }
            break;
        }

        /* Preemption check */
        if (--cpu->slice_counter <= 0) {
            cpu->slice_counter = Z80_SLICE_SIZE;
            /* In kernel context, yield to let other processes run.
             * For host tests, this is a no-op (sched_yield not linked). */
#ifdef PPAP_KERNEL
            extern void sched_yield(void);
            sched_yield();
#endif
        }
    }
}

/* ── Operations table ────────────────────────────────────────────────────── */

const ecpu_core_ops_t ecpu_z80_ops = {
    .name           = "z80",
    .arch_id        = ECPU_ARCH_Z80,
    .init           = ecpu_z80_init,
    .reset          = ecpu_z80_reset,
    .run            = ecpu_z80_run,
    .set_trap_handler = ecpu_z80_set_trap_handler,
    .get_reg        = ecpu_z80_get_reg,
    .set_reg        = ecpu_z80_set_reg,
    .translate_ptr  = ecpu_z80_translate_ptr,
    .read8          = ecpu_z80_read8_op,
    .write8         = ecpu_z80_write8_op,
    .read16         = ecpu_z80_read16_op,
    .write16        = ecpu_z80_write16_op,
    .state_size     = sizeof(z80_state_t),
};
