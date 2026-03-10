/*
 * test_ecpu_z80.c — Host-side unit tests for the Z80 eCPU core (Step 1)
 *
 * Tests the minimal instruction set: NOP, HALT, LD r,r', LD r,n,
 * LD rr,nn, JP nn, JR e, CALL nn, RET, and the common interface.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "kernel/ecpu/ecpu.h"
#include "kernel/ecpu/ecpu_z80.h"

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static z80_state_t cpu;
static uint8_t mem[65536];

static void setup(void)
{
    memset(mem, 0, sizeof(mem));
    ecpu_z80_ops.init((ecpu_state_t *)&cpu, mem, sizeof(mem));
}

/* ── Test: NOP + HALT ────────────────────────────────────────────────────── */

static void test_nop_halt(void)
{
    setup();
    mem[0] = 0x00;  /* NOP */
    mem[1] = 0x00;  /* NOP */
    mem[2] = 0x76;  /* HALT */

    ecpu_z80_ops.run((ecpu_state_t *)&cpu);

    assert(cpu.halted == 1);
    assert(cpu.pc == 3);
    printf("  PASS: nop_halt\n");
}

/* ── Test: LD r, n (immediate) ───────────────────────────────────────────── */

static void test_ld_r_n(void)
{
    setup();
    mem[0] = 0x3E;  /* LD A, 0x42 */
    mem[1] = 0x42;
    mem[2] = 0x06;  /* LD B, 0x11 */
    mem[3] = 0x11;
    mem[4] = 0x0E;  /* LD C, 0x22 */
    mem[5] = 0x22;
    mem[6] = 0x16;  /* LD D, 0x33 */
    mem[7] = 0x33;
    mem[8] = 0x1E;  /* LD E, 0x44 */
    mem[9] = 0x44;
    mem[10] = 0x26; /* LD H, 0x55 */
    mem[11] = 0x55;
    mem[12] = 0x2E; /* LD L, 0x66 */
    mem[13] = 0x66;
    mem[14] = 0x76; /* HALT */

    ecpu_z80_ops.run((ecpu_state_t *)&cpu);

    assert(cpu.a == 0x42);
    assert(cpu.b == 0x11);
    assert(cpu.c == 0x22);
    assert(cpu.d == 0x33);
    assert(cpu.e == 0x44);
    assert(cpu.h == 0x55);
    assert(cpu.l == 0x66);
    printf("  PASS: ld_r_n\n");
}

/* ── Test: LD r, r' (register to register) ───────────────────────────────── */

static void test_ld_r_r(void)
{
    setup();
    mem[0] = 0x3E;  /* LD A, 0x42 */
    mem[1] = 0x42;
    mem[2] = 0x47;  /* LD B, A */
    mem[3] = 0x48;  /* LD C, B */
    mem[4] = 0x51;  /* LD D, C */
    mem[5] = 0x76;  /* HALT */

    ecpu_z80_ops.run((ecpu_state_t *)&cpu);

    assert(cpu.a == 0x42);
    assert(cpu.b == 0x42);
    assert(cpu.c == 0x42);
    assert(cpu.d == 0x42);
    printf("  PASS: ld_r_r\n");
}

/* ── Test: LD rr, nn (16-bit immediate) ──────────────────────────────────── */

static void test_ld_rr_nn(void)
{
    setup();
    mem[0] = 0x01;  /* LD BC, 0x1234 */
    mem[1] = 0x34;  /* low byte first */
    mem[2] = 0x12;
    mem[3] = 0x11;  /* LD DE, 0x5678 */
    mem[4] = 0x78;
    mem[5] = 0x56;
    mem[6] = 0x21;  /* LD HL, 0x9ABC */
    mem[7] = 0xBC;
    mem[8] = 0x9A;
    mem[9] = 0x31;  /* LD SP, 0xFFFE */
    mem[10] = 0xFE;
    mem[11] = 0xFF;
    mem[12] = 0x76; /* HALT */

    ecpu_z80_ops.run((ecpu_state_t *)&cpu);

    assert(z80_bc(&cpu) == 0x1234);
    assert(z80_de(&cpu) == 0x5678);
    assert(z80_hl(&cpu) == 0x9ABC);
    assert(cpu.sp == 0xFFFE);
    printf("  PASS: ld_rr_nn\n");
}

/* ── Test: JP nn ─────────────────────────────────────────────────────────── */

static void test_jp_nn(void)
{
    setup();
    mem[0] = 0xC3;  /* JP 0x0100 */
    mem[1] = 0x00;
    mem[2] = 0x01;
    /* At 0x0100 */
    mem[0x100] = 0x3E;  /* LD A, 0x99 */
    mem[0x101] = 0x99;
    mem[0x102] = 0x76;  /* HALT */

    ecpu_z80_ops.run((ecpu_state_t *)&cpu);

    assert(cpu.a == 0x99);
    assert(cpu.pc == 0x0103);
    printf("  PASS: jp_nn\n");
}

/* ── Test: JR e (relative jump) ──────────────────────────────────────────── */

static void test_jr_e(void)
{
    setup();
    cpu.pc = 0x0100;
    mem[0x100] = 0x18;  /* JR +3 (skip 3 bytes forward from after JR) */
    mem[0x101] = 0x03;
    /* 0x102, 0x103, 0x104 skipped */
    mem[0x105] = 0x3E;  /* LD A, 0xAA */
    mem[0x106] = 0xAA;
    mem[0x107] = 0x76;  /* HALT */

    ecpu_z80_ops.run((ecpu_state_t *)&cpu);

    assert(cpu.a == 0xAA);
    printf("  PASS: jr_e\n");
}

/* ── Test: JR e (backward jump) ──────────────────────────────────────────── */

static void test_jr_backward(void)
{
    setup();
    /* At 0x0100: LD A, 0x00 */
    mem[0x100] = 0x3E;
    mem[0x101] = 0x00;
    /* JP to 0x0110 to start */
    mem[0x102] = 0xC3;
    mem[0x103] = 0x10;
    mem[0x104] = 0x01;
    /* At 0x0105: LD A, 0xBB; HALT — target of backward jump */
    mem[0x105] = 0x3E;
    mem[0x106] = 0xBB;
    mem[0x107] = 0x76;
    /* At 0x0110: JR -13 (0xF3) → 0x0110 + 2 - 13 = 0x0105 */
    mem[0x110] = 0x18;
    mem[0x111] = 0xF3;  /* -13 as signed byte */

    cpu.pc = 0x0110;
    ecpu_z80_ops.run((ecpu_state_t *)&cpu);

    assert(cpu.a == 0xBB);
    printf("  PASS: jr_backward\n");
}

/* ── Test: CALL nn + RET ─────────────────────────────────────────────────── */

static void test_call_ret(void)
{
    setup();
    cpu.pc = 0x0100;
    cpu.sp = 0xFFFE;

    /* At 0x0100: LD A, 0x42 */
    mem[0x100] = 0x3E;
    mem[0x101] = 0x42;
    /* At 0x0102: CALL 0x0200 */
    mem[0x102] = 0xCD;
    mem[0x103] = 0x00;
    mem[0x104] = 0x02;
    /* At 0x0105: HALT (return lands here) */
    mem[0x105] = 0x76;

    /* At 0x0200: subroutine — LD B, A; LD C, 0x11; RET */
    mem[0x200] = 0x47;  /* LD B, A */
    mem[0x201] = 0x0E;  /* LD C, 0x11 */
    mem[0x202] = 0x11;
    mem[0x203] = 0xC9;  /* RET */

    ecpu_z80_ops.run((ecpu_state_t *)&cpu);

    assert(cpu.a == 0x42);
    assert(cpu.b == 0x42);
    assert(cpu.c == 0x11);
    assert(cpu.halted == 1);
    assert(cpu.sp == 0xFFFE);  /* SP restored after RET */
    assert(cpu.pc == 0x0106);  /* HALT consumed PC */
    printf("  PASS: call_ret\n");
}

/* ── Test: CALL trap handler intercepts ──────────────────────────────────── */

static int trap_call_count;
static uint32_t trap_last_addr;

static int test_trap_handler(ecpu_state_t *state, int trap_type,
                             uint32_t param, void *ctx)
{
    (void)state;
    (void)ctx;
    if (trap_type == ECPU_TRAP_CALL && param == 0x0005) {
        trap_call_count++;
        trap_last_addr = param;
        /* Simulate BDOS: just set A = 0xFF and return handled */
        z80_state_t *cpu = (z80_state_t *)state;
        cpu->a = 0xFF;
        return ECPU_TRAP_HANDLED;
    }
    return ECPU_TRAP_UNHANDLED;
}

static void test_call_trap(void)
{
    setup();
    cpu.pc = 0x0100;
    cpu.sp = 0xFFFE;
    trap_call_count = 0;
    trap_last_addr = 0;

    ecpu_z80_ops.set_trap_handler((ecpu_state_t *)&cpu, test_trap_handler,
                                  NULL);

    /* LD A, 0x42; CALL 0x0005; HALT */
    mem[0x100] = 0x3E;  /* LD A, 0x42 */
    mem[0x101] = 0x42;
    mem[0x102] = 0xCD;  /* CALL 0x0005 */
    mem[0x103] = 0x05;
    mem[0x104] = 0x00;
    mem[0x105] = 0x76;  /* HALT */

    ecpu_z80_ops.run((ecpu_state_t *)&cpu);

    assert(trap_call_count == 1);
    assert(trap_last_addr == 0x0005);
    assert(cpu.a == 0xFF);  /* trap handler set this */
    assert(cpu.sp == 0xFFFE);  /* CALL was intercepted, no push */
    printf("  PASS: call_trap\n");
}

/* ── Test: CALL to non-hooked address goes through ───────────────────────── */

static void test_call_unhooked(void)
{
    setup();
    cpu.pc = 0x0100;
    cpu.sp = 0xFFFE;
    trap_call_count = 0;

    ecpu_z80_ops.set_trap_handler((ecpu_state_t *)&cpu, test_trap_handler,
                                  NULL);

    /* CALL 0x0200 (not 0x0005, so trap returns UNHANDLED) */
    mem[0x100] = 0xCD;  /* CALL 0x0200 */
    mem[0x101] = 0x00;
    mem[0x102] = 0x02;
    mem[0x103] = 0x76;  /* HALT (return target) */

    mem[0x200] = 0x3E;  /* LD A, 0xBB */
    mem[0x201] = 0xBB;
    mem[0x202] = 0xC9;  /* RET */

    ecpu_z80_ops.run((ecpu_state_t *)&cpu);

    assert(cpu.a == 0xBB);
    assert(cpu.sp == 0xFFFE);
    printf("  PASS: call_unhooked\n");
}

/* ── Test: CALL trap EXIT ────────────────────────────────────────────────── */

static int exit_trap_handler(ecpu_state_t *state, int trap_type,
                             uint32_t param, void *ctx)
{
    (void)state;
    (void)param;
    (void)ctx;
    if (trap_type == ECPU_TRAP_CALL)
        return ECPU_TRAP_EXIT;
    return ECPU_TRAP_UNHANDLED;
}

static void test_call_trap_exit(void)
{
    setup();
    cpu.pc = 0x0100;
    cpu.sp = 0xFFFE;

    ecpu_z80_ops.set_trap_handler((ecpu_state_t *)&cpu, exit_trap_handler,
                                  NULL);

    mem[0x100] = 0xCD;  /* CALL 0x0000 */
    mem[0x101] = 0x00;
    mem[0x102] = 0x00;
    /* Should never reach here */
    mem[0x103] = 0x3E;
    mem[0x104] = 0xAA;
    mem[0x105] = 0x76;

    ecpu_z80_ops.run((ecpu_state_t *)&cpu);

    assert(cpu.a == 0x00);  /* LD A,0xAA never executed */
    printf("  PASS: call_trap_exit\n");
}

/* ── Test: common interface get_reg / set_reg ────────────────────────────── */

static void test_common_interface(void)
{
    setup();
    const ecpu_core_ops_t *ops = &ecpu_z80_ops;
    ecpu_state_t *s = (ecpu_state_t *)&cpu;

    ops->set_reg(s, Z80_REG_A, 0x42);
    ops->set_reg(s, Z80_REG_BC, 0x1234);
    ops->set_reg(s, Z80_REG_SP, 0xFFF0);
    ops->set_reg(s, Z80_REG_PC, 0x0100);

    assert(ops->get_reg(s, Z80_REG_A) == 0x42);
    assert(ops->get_reg(s, Z80_REG_B) == 0x12);
    assert(ops->get_reg(s, Z80_REG_C) == 0x34);
    assert(ops->get_reg(s, Z80_REG_BC) == 0x1234);
    assert(ops->get_reg(s, Z80_REG_SP) == 0xFFF0);
    assert(ops->get_reg(s, Z80_REG_PC) == 0x0100);

    /* Memory access through common interface */
    ops->write8(s, 0x1000, 0xAB);
    assert(ops->read8(s, 0x1000) == 0xAB);

    ops->write16(s, 0x2000, 0xCDEF);
    assert(ops->read16(s, 0x2000) == 0xCDEF);

    /* translate_ptr */
    void *ptr = ops->translate_ptr(s, 0x1000, 1);
    assert(ptr == mem + 0x1000);
    assert(*(uint8_t *)ptr == 0xAB);

    /* Out of bounds */
    ptr = ops->translate_ptr(s, 0xFFFF, 2);
    assert(ptr == NULL);

    printf("  PASS: common_interface\n");
}

/* ── Test: R register increments ─────────────────────────────────────────── */

static void test_r_register(void)
{
    setup();
    mem[0] = 0x00;  /* NOP */
    mem[1] = 0x00;  /* NOP */
    mem[2] = 0x00;  /* NOP */
    mem[3] = 0x76;  /* HALT */

    ecpu_z80_ops.run((ecpu_state_t *)&cpu);

    /* 4 instructions fetched (3 NOP + HALT), R increments low 7 bits */
    assert((cpu.r & 0x7F) == 4);
    printf("  PASS: r_register\n");
}

/* ── Test: LD (HL), n and LD r, (HL) ────────────────────────────────────── */

static void test_ld_hl_indirect(void)
{
    setup();
    /* LD HL, 0x8000 */
    mem[0] = 0x21;
    mem[1] = 0x00;
    mem[2] = 0x80;
    /* LD (HL), 0x77 */
    mem[3] = 0x36;
    mem[4] = 0x77;
    /* LD A, (HL) — via LD r,r' with zzz=6 */
    mem[5] = 0x7E;  /* LD A, (HL) */
    mem[6] = 0x76;  /* HALT */

    ecpu_z80_ops.run((ecpu_state_t *)&cpu);

    assert(mem[0x8000] == 0x77);
    assert(cpu.a == 0x77);
    printf("  PASS: ld_hl_indirect\n");
}

/* ── Test: reset preserves memory and trap handler ───────────────────────── */

static void test_reset(void)
{
    setup();
    cpu.a = 0x42;
    cpu.pc = 0x1234;
    cpu.sp = 0xFFF0;
    cpu.trap_handler = test_trap_handler;
    cpu.trap_ctx = (void *)0xDEADBEEF;
    mem[0x1000] = 0xAB;

    ecpu_z80_ops.reset((ecpu_state_t *)&cpu);

    assert(cpu.a == 0);
    assert(cpu.pc == 0);
    assert(cpu.sp == 0);
    assert(cpu.memory == mem);
    assert(cpu.trap_handler == test_trap_handler);
    assert(cpu.trap_ctx == (void *)0xDEADBEEF);
    assert(mem[0x1000] == 0xAB);  /* memory preserved */
    printf("  PASS: reset\n");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Step 2 tests — ALU, INC/DEC, rotates, DAA, CPL, SCF, CCF
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── Test: ADD A, r and ADD A, n ─────────────────────────────────────────── */

static void test_add(void)
{
    setup();
    /* LD A, 0x30; ADD A, 0x12 → 0x42 */
    mem[0] = 0x3E; mem[1] = 0x30;  /* LD A, 0x30 */
    mem[2] = 0xC6; mem[3] = 0x12;  /* ADD A, 0x12 */
    mem[4] = 0x76;
    ecpu_z80_ops.run((ecpu_state_t *)&cpu);
    assert(cpu.a == 0x42);
    assert(!(cpu.f & FLAG_Z));
    assert(!(cpu.f & FLAG_C));
    assert(!(cpu.f & FLAG_N));
    assert(!(cpu.f & FLAG_S));

    /* ADD with carry: 0xFF + 0x01 = 0x00, C=1, Z=1 */
    setup();
    mem[0] = 0x3E; mem[1] = 0xFF;  /* LD A, 0xFF */
    mem[2] = 0xC6; mem[3] = 0x01;  /* ADD A, 0x01 */
    mem[4] = 0x76;
    ecpu_z80_ops.run((ecpu_state_t *)&cpu);
    assert(cpu.a == 0x00);
    assert(cpu.f & FLAG_Z);
    assert(cpu.f & FLAG_C);

    /* ADD with overflow: 0x7F + 0x01 = 0x80, PV=1, S=1 */
    setup();
    mem[0] = 0x3E; mem[1] = 0x7F;
    mem[2] = 0xC6; mem[3] = 0x01;
    mem[4] = 0x76;
    ecpu_z80_ops.run((ecpu_state_t *)&cpu);
    assert(cpu.a == 0x80);
    assert(cpu.f & FLAG_PV);
    assert(cpu.f & FLAG_S);
    assert(!(cpu.f & FLAG_C));

    /* ADD with half-carry: 0x0F + 0x01 = 0x10, H=1 */
    setup();
    mem[0] = 0x3E; mem[1] = 0x0F;
    mem[2] = 0xC6; mem[3] = 0x01;
    mem[4] = 0x76;
    ecpu_z80_ops.run((ecpu_state_t *)&cpu);
    assert(cpu.a == 0x10);
    assert(cpu.f & FLAG_H);

    /* ADD A, B (register) */
    setup();
    mem[0] = 0x3E; mem[1] = 0x20;  /* LD A, 0x20 */
    mem[2] = 0x06; mem[3] = 0x15;  /* LD B, 0x15 */
    mem[4] = 0x80;                  /* ADD A, B */
    mem[5] = 0x76;
    ecpu_z80_ops.run((ecpu_state_t *)&cpu);
    assert(cpu.a == 0x35);

    printf("  PASS: add\n");
}

/* ── Test: SUB ───────────────────────────────────────────────────────────── */

static void test_sub(void)
{
    setup();
    /* 0x42 - 0x12 = 0x30 */
    mem[0] = 0x3E; mem[1] = 0x42;
    mem[2] = 0xD6; mem[3] = 0x12;  /* SUB 0x12 */
    mem[4] = 0x76;
    ecpu_z80_ops.run((ecpu_state_t *)&cpu);
    assert(cpu.a == 0x30);
    assert(cpu.f & FLAG_N);
    assert(!(cpu.f & FLAG_C));

    /* SUB with borrow: 0x00 - 0x01 = 0xFF, C=1, S=1 */
    setup();
    mem[0] = 0x3E; mem[1] = 0x00;
    mem[2] = 0xD6; mem[3] = 0x01;
    mem[4] = 0x76;
    ecpu_z80_ops.run((ecpu_state_t *)&cpu);
    assert(cpu.a == 0xFF);
    assert(cpu.f & FLAG_C);
    assert(cpu.f & FLAG_S);

    /* SUB with overflow: 0x80 - 0x01 = 0x7F, PV=1 */
    setup();
    mem[0] = 0x3E; mem[1] = 0x80;
    mem[2] = 0xD6; mem[3] = 0x01;
    mem[4] = 0x76;
    ecpu_z80_ops.run((ecpu_state_t *)&cpu);
    assert(cpu.a == 0x7F);
    assert(cpu.f & FLAG_PV);

    printf("  PASS: sub\n");
}

/* ── Test: ADC / SBC ─────────────────────────────────────────────────────── */

static void test_adc_sbc(void)
{
    /* ADC: 0x42 + 0x01 + C(1) = 0x44 */
    setup();
    mem[0] = 0x3E; mem[1] = 0xFF;  /* LD A, 0xFF */
    mem[2] = 0xC6; mem[3] = 0x01;  /* ADD A, 0x01 → A=0, C=1 */
    mem[4] = 0x3E; mem[5] = 0x42;  /* LD A, 0x42 */
    mem[6] = 0xCE; mem[7] = 0x01;  /* ADC A, 0x01 → 0x42+0x01+1=0x44 */
    mem[8] = 0x76;
    ecpu_z80_ops.run((ecpu_state_t *)&cpu);
    assert(cpu.a == 0x44);

    /* SBC: 0x42 - 0x01 - C(1) = 0x40 */
    setup();
    mem[0] = 0x3E; mem[1] = 0xFF;
    mem[2] = 0xC6; mem[3] = 0x01;  /* ADD → C=1 */
    mem[4] = 0x3E; mem[5] = 0x42;
    mem[6] = 0xDE; mem[7] = 0x01;  /* SBC A, 0x01 → 0x42-0x01-1=0x40 */
    mem[8] = 0x76;
    ecpu_z80_ops.run((ecpu_state_t *)&cpu);
    assert(cpu.a == 0x40);

    printf("  PASS: adc_sbc\n");
}

/* ── Test: AND / OR / XOR ────────────────────────────────────────────────── */

static void test_logic(void)
{
    /* AND: 0xFF & 0x0F = 0x0F, H=1, parity */
    setup();
    mem[0] = 0x3E; mem[1] = 0xFF;
    mem[2] = 0xE6; mem[3] = 0x0F;  /* AND 0x0F */
    mem[4] = 0x76;
    ecpu_z80_ops.run((ecpu_state_t *)&cpu);
    assert(cpu.a == 0x0F);
    assert(cpu.f & FLAG_H);
    assert(!(cpu.f & FLAG_C));
    assert(!(cpu.f & FLAG_N));

    /* OR: 0xF0 | 0x0F = 0xFF, parity(0xFF)=even */
    setup();
    mem[0] = 0x3E; mem[1] = 0xF0;
    mem[2] = 0xF6; mem[3] = 0x0F;  /* OR 0x0F */
    mem[4] = 0x76;
    ecpu_z80_ops.run((ecpu_state_t *)&cpu);
    assert(cpu.a == 0xFF);
    assert(cpu.f & FLAG_PV);  /* even parity */
    assert(cpu.f & FLAG_S);
    assert(!(cpu.f & FLAG_H));

    /* XOR A, A = 0, Z=1, P=1 (even parity of 0) */
    setup();
    mem[0] = 0x3E; mem[1] = 0x42;
    mem[2] = 0xAF;                  /* XOR A (self) */
    mem[3] = 0x76;
    ecpu_z80_ops.run((ecpu_state_t *)&cpu);
    assert(cpu.a == 0x00);
    assert(cpu.f & FLAG_Z);
    assert(cpu.f & FLAG_PV);

    printf("  PASS: logic\n");
}

/* ── Test: CP (compare) — F3/F5 from operand ─────────────────────────────── */

static void test_cp(void)
{
    /* CP 0x42 when A=0x42 → Z=1, A unchanged */
    setup();
    mem[0] = 0x3E; mem[1] = 0x42;
    mem[2] = 0xFE; mem[3] = 0x42;  /* CP 0x42 */
    mem[4] = 0x76;
    ecpu_z80_ops.run((ecpu_state_t *)&cpu);
    assert(cpu.a == 0x42);  /* A not modified */
    assert(cpu.f & FLAG_Z);
    assert(cpu.f & FLAG_N);
    assert(!(cpu.f & FLAG_C));

    /* CP: F3/F5 come from operand (0x28), not result */
    setup();
    mem[0] = 0x3E; mem[1] = 0x50;
    mem[2] = 0xFE; mem[3] = 0x28;  /* CP 0x28 → result 0x28 */
    mem[4] = 0x76;
    ecpu_z80_ops.run((ecpu_state_t *)&cpu);
    assert(cpu.a == 0x50);
    /* F3 = bit 3 of 0x28 = 1, F5 = bit 5 of 0x28 = 1 */
    assert(cpu.f & FLAG_F3);
    assert(cpu.f & FLAG_F5);

    /* CP with borrow: A=0x10, CP 0x20 → C=1 */
    setup();
    mem[0] = 0x3E; mem[1] = 0x10;
    mem[2] = 0xFE; mem[3] = 0x20;
    mem[4] = 0x76;
    ecpu_z80_ops.run((ecpu_state_t *)&cpu);
    assert(cpu.f & FLAG_C);

    printf("  PASS: cp\n");
}

/* ── Test: INC/DEC 8-bit ─────────────────────────────────────────────────── */

static void test_inc_dec_8(void)
{
    /* INC B: 0x0F → 0x10, H=1 */
    setup();
    mem[0] = 0x06; mem[1] = 0x0F;  /* LD B, 0x0F */
    mem[2] = 0x04;                  /* INC B */
    mem[3] = 0x76;
    ecpu_z80_ops.run((ecpu_state_t *)&cpu);
    assert(cpu.b == 0x10);
    assert(cpu.f & FLAG_H);
    assert(!(cpu.f & FLAG_N));

    /* INC: 0x7F → 0x80, PV=1 (overflow) */
    setup();
    mem[0] = 0x3E; mem[1] = 0x7F;
    mem[2] = 0x3C;                  /* INC A */
    mem[3] = 0x76;
    ecpu_z80_ops.run((ecpu_state_t *)&cpu);
    assert(cpu.a == 0x80);
    assert(cpu.f & FLAG_PV);
    assert(cpu.f & FLAG_S);

    /* INC: 0xFF → 0x00, Z=1, H=1 */
    setup();
    mem[0] = 0x3E; mem[1] = 0xFF;
    mem[2] = 0x3C;
    mem[3] = 0x76;
    ecpu_z80_ops.run((ecpu_state_t *)&cpu);
    assert(cpu.a == 0x00);
    assert(cpu.f & FLAG_Z);
    assert(cpu.f & FLAG_H);

    /* DEC B: 0x01 → 0x00, Z=1 */
    setup();
    mem[0] = 0x06; mem[1] = 0x01;
    mem[2] = 0x05;                  /* DEC B */
    mem[3] = 0x76;
    ecpu_z80_ops.run((ecpu_state_t *)&cpu);
    assert(cpu.b == 0x00);
    assert(cpu.f & FLAG_Z);
    assert(cpu.f & FLAG_N);

    /* DEC: 0x80 → 0x7F, PV=1 */
    setup();
    mem[0] = 0x3E; mem[1] = 0x80;
    mem[2] = 0x3D;                  /* DEC A */
    mem[3] = 0x76;
    ecpu_z80_ops.run((ecpu_state_t *)&cpu);
    assert(cpu.a == 0x7F);
    assert(cpu.f & FLAG_PV);

    /* DEC: 0x00 → 0xFF, H=1 (borrow from bit 4) */
    setup();
    mem[0] = 0x3E; mem[1] = 0x00;
    mem[2] = 0x3D;
    mem[3] = 0x76;
    ecpu_z80_ops.run((ecpu_state_t *)&cpu);
    assert(cpu.a == 0xFF);
    assert(cpu.f & FLAG_H);

    /* INC/DEC preserve carry */
    setup();
    cpu.f = FLAG_C;  /* set carry manually */
    mem[0] = 0x3E; mem[1] = 0x10;
    mem[2] = 0x3C;  /* INC A */
    mem[3] = 0x76;
    ecpu_z80_ops.run((ecpu_state_t *)&cpu);
    assert(cpu.a == 0x11);
    assert(cpu.f & FLAG_C);  /* carry preserved */

    printf("  PASS: inc_dec_8\n");
}

/* ── Test: INC/DEC 16-bit (no flags) ─────────────────────────────────────── */

static void test_inc_dec_16(void)
{
    setup();
    mem[0] = 0x01; mem[1] = 0xFF; mem[2] = 0x00;  /* LD BC, 0x00FF */
    mem[3] = 0x03;                                   /* INC BC → 0x0100 */
    mem[4] = 0x76;
    ecpu_z80_ops.run((ecpu_state_t *)&cpu);
    assert(z80_bc(&cpu) == 0x0100);

    /* DEC BC: 0x0100 → 0x00FF */
    setup();
    mem[0] = 0x01; mem[1] = 0x00; mem[2] = 0x01;
    mem[3] = 0x0B;                                   /* DEC BC */
    mem[4] = 0x76;
    ecpu_z80_ops.run((ecpu_state_t *)&cpu);
    assert(z80_bc(&cpu) == 0x00FF);

    /* INC SP: 0xFFFE → 0xFFFF */
    setup();
    mem[0] = 0x31; mem[1] = 0xFE; mem[2] = 0xFF;  /* LD SP, 0xFFFE */
    mem[3] = 0x33;                                   /* INC SP */
    mem[4] = 0x76;
    ecpu_z80_ops.run((ecpu_state_t *)&cpu);
    assert(cpu.sp == 0xFFFF);

    /* Wrap: INC from 0xFFFF → 0x0000 */
    setup();
    mem[0] = 0x21; mem[1] = 0xFF; mem[2] = 0xFF;  /* LD HL, 0xFFFF */
    mem[3] = 0x23;                                   /* INC HL */
    mem[4] = 0x76;
    ecpu_z80_ops.run((ecpu_state_t *)&cpu);
    assert(z80_hl(&cpu) == 0x0000);

    printf("  PASS: inc_dec_16\n");
}

/* ── Test: ADD HL, rr ────────────────────────────────────────────────────── */

static void test_add_hl(void)
{
    /* HL=0x1000 + BC=0x2000 = 0x3000, no carry */
    setup();
    mem[0] = 0x21; mem[1] = 0x00; mem[2] = 0x10;  /* LD HL, 0x1000 */
    mem[3] = 0x01; mem[4] = 0x00; mem[5] = 0x20;  /* LD BC, 0x2000 */
    mem[6] = 0x09;                                   /* ADD HL, BC */
    mem[7] = 0x76;
    ecpu_z80_ops.run((ecpu_state_t *)&cpu);
    assert(z80_hl(&cpu) == 0x3000);
    assert(!(cpu.f & FLAG_C));
    assert(!(cpu.f & FLAG_N));

    /* With carry: 0xFFFF + 0x0001 = 0x0000, C=1 */
    setup();
    mem[0] = 0x21; mem[1] = 0xFF; mem[2] = 0xFF;
    mem[3] = 0x01; mem[4] = 0x01; mem[5] = 0x00;
    mem[6] = 0x09;
    mem[7] = 0x76;
    ecpu_z80_ops.run((ecpu_state_t *)&cpu);
    assert(z80_hl(&cpu) == 0x0000);
    assert(cpu.f & FLAG_C);

    /* ADD HL, HL (doubles) */
    setup();
    mem[0] = 0x21; mem[1] = 0x34; mem[2] = 0x12;  /* LD HL, 0x1234 */
    mem[3] = 0x29;                                   /* ADD HL, HL */
    mem[4] = 0x76;
    ecpu_z80_ops.run((ecpu_state_t *)&cpu);
    assert(z80_hl(&cpu) == 0x2468);

    /* ADD HL preserves S, Z, PV */
    setup();
    cpu.f = FLAG_S | FLAG_Z | FLAG_PV;
    mem[0] = 0x21; mem[1] = 0x00; mem[2] = 0x10;
    mem[3] = 0x01; mem[4] = 0x00; mem[5] = 0x20;
    mem[6] = 0x09;
    mem[7] = 0x76;
    ecpu_z80_ops.run((ecpu_state_t *)&cpu);
    assert(cpu.f & FLAG_S);
    assert(cpu.f & FLAG_Z);
    assert(cpu.f & FLAG_PV);

    printf("  PASS: add_hl\n");
}

/* ── Test: RLCA / RRCA / RLA / RRA ──────────────────────────────────────── */

static void test_rotates(void)
{
    /* RLCA: 0x80 → 0x01, C=1 (bit 7 goes to bit 0 and carry) */
    setup();
    mem[0] = 0x3E; mem[1] = 0x80;
    mem[2] = 0x07;  /* RLCA */
    mem[3] = 0x76;
    ecpu_z80_ops.run((ecpu_state_t *)&cpu);
    assert(cpu.a == 0x01);
    assert(cpu.f & FLAG_C);

    /* RRCA: 0x01 → 0x80, C=1 */
    setup();
    mem[0] = 0x3E; mem[1] = 0x01;
    mem[2] = 0x0F;  /* RRCA */
    mem[3] = 0x76;
    ecpu_z80_ops.run((ecpu_state_t *)&cpu);
    assert(cpu.a == 0x80);
    assert(cpu.f & FLAG_C);

    /* RLA: 0x80 with C=0 → 0x00, C=1 */
    setup();
    mem[0] = 0x3E; mem[1] = 0x80;
    mem[2] = 0x17;  /* RLA */
    mem[3] = 0x76;
    ecpu_z80_ops.run((ecpu_state_t *)&cpu);
    assert(cpu.a == 0x00);
    assert(cpu.f & FLAG_C);

    /* RLA: 0x80 with C=1 → 0x01, C=1 */
    setup();
    cpu.f = FLAG_C;
    mem[0] = 0x3E; mem[1] = 0x80;
    mem[2] = 0x17;  /* RLA */
    mem[3] = 0x76;
    ecpu_z80_ops.run((ecpu_state_t *)&cpu);
    assert(cpu.a == 0x01);
    assert(cpu.f & FLAG_C);

    /* RRA: 0x01 with C=0 → 0x00, C=1 */
    setup();
    mem[0] = 0x3E; mem[1] = 0x01;
    mem[2] = 0x1F;  /* RRA */
    mem[3] = 0x76;
    ecpu_z80_ops.run((ecpu_state_t *)&cpu);
    assert(cpu.a == 0x00);
    assert(cpu.f & FLAG_C);

    /* RRA: 0x01 with C=1 → 0x80, C=1 */
    setup();
    cpu.f = FLAG_C;
    mem[0] = 0x3E; mem[1] = 0x01;
    mem[2] = 0x1F;  /* RRA */
    mem[3] = 0x76;
    ecpu_z80_ops.run((ecpu_state_t *)&cpu);
    assert(cpu.a == 0x80);
    assert(cpu.f & FLAG_C);

    /* Rotates preserve S, Z, PV */
    setup();
    cpu.f = FLAG_S | FLAG_Z | FLAG_PV;
    mem[0] = 0x3E; mem[1] = 0x42;
    mem[2] = 0x07;  /* RLCA */
    mem[3] = 0x76;
    ecpu_z80_ops.run((ecpu_state_t *)&cpu);
    assert(cpu.f & FLAG_S);
    assert(cpu.f & FLAG_Z);
    assert(cpu.f & FLAG_PV);

    printf("  PASS: rotates\n");
}

/* ── Test: DAA ───────────────────────────────────────────────────────────── */

static void test_daa(void)
{
    /* BCD: 0x15 + 0x27 = 0x42 (BCD) */
    setup();
    mem[0] = 0x3E; mem[1] = 0x15;
    mem[2] = 0xC6; mem[3] = 0x27;  /* ADD A, 0x27 → 0x3C */
    mem[4] = 0x27;                  /* DAA → should give 0x42 */
    mem[5] = 0x76;
    ecpu_z80_ops.run((ecpu_state_t *)&cpu);
    assert(cpu.a == 0x42);
    assert(!(cpu.f & FLAG_C));

    /* BCD with carry: 0x99 + 0x01 = 0x00 (BCD), C=1 */
    setup();
    mem[0] = 0x3E; mem[1] = 0x99;
    mem[2] = 0xC6; mem[3] = 0x01;  /* ADD A, 0x01 → 0x9A */
    mem[4] = 0x27;                  /* DAA → 0x00, C=1 */
    mem[5] = 0x76;
    ecpu_z80_ops.run((ecpu_state_t *)&cpu);
    assert(cpu.a == 0x00);
    assert(cpu.f & FLAG_C);
    assert(cpu.f & FLAG_Z);

    /* BCD subtraction: 0x42 - 0x15 with DAA */
    setup();
    mem[0] = 0x3E; mem[1] = 0x42;
    mem[2] = 0xD6; mem[3] = 0x15;  /* SUB 0x15 → 0x2D */
    mem[4] = 0x27;                  /* DAA → 0x27 */
    mem[5] = 0x76;
    ecpu_z80_ops.run((ecpu_state_t *)&cpu);
    assert(cpu.a == 0x27);

    printf("  PASS: daa\n");
}

/* ── Test: CPL ───────────────────────────────────────────────────────────── */

static void test_cpl(void)
{
    setup();
    mem[0] = 0x3E; mem[1] = 0x55;
    mem[2] = 0x2F;                  /* CPL → ~0x55 = 0xAA */
    mem[3] = 0x76;
    ecpu_z80_ops.run((ecpu_state_t *)&cpu);
    assert(cpu.a == 0xAA);
    assert(cpu.f & FLAG_H);
    assert(cpu.f & FLAG_N);

    printf("  PASS: cpl\n");
}

/* ── Test: SCF / CCF ─────────────────────────────────────────────────────── */

static void test_scf_ccf(void)
{
    /* SCF: set carry */
    setup();
    mem[0] = 0x37;  /* SCF */
    mem[1] = 0x76;
    ecpu_z80_ops.run((ecpu_state_t *)&cpu);
    assert(cpu.f & FLAG_C);
    assert(!(cpu.f & FLAG_N));
    assert(!(cpu.f & FLAG_H));

    /* CCF: complement carry (1 → 0, old C → H) */
    setup();
    cpu.f = FLAG_C;
    mem[0] = 0x3F;  /* CCF */
    mem[1] = 0x76;
    ecpu_z80_ops.run((ecpu_state_t *)&cpu);
    assert(!(cpu.f & FLAG_C));
    assert(cpu.f & FLAG_H);  /* old carry → H */
    assert(!(cpu.f & FLAG_N));

    /* CCF: 0 → 1 */
    setup();
    cpu.f = 0;
    mem[0] = 0x3F;  /* CCF */
    mem[1] = 0x76;
    ecpu_z80_ops.run((ecpu_state_t *)&cpu);
    assert(cpu.f & FLAG_C);
    assert(!(cpu.f & FLAG_H));

    printf("  PASS: scf_ccf\n");
}

/* ── Test: arithmetic program (ADD, CP, conditional flow coming in Step 3) ─ */

static void test_arith_program(void)
{
    /* Sum 1+2+3+4+5 using INC and ADD */
    setup();
    cpu.pc = 0x0100;
    /* LD A, 0; LD B, 1; ADD A,B; INC B; ADD A,B; INC B;
     * ADD A,B; INC B; ADD A,B; INC B; ADD A,B; HALT */
    uint16_t pc = 0x0100;
    mem[pc++] = 0x3E; mem[pc++] = 0x00;  /* LD A, 0 */
    mem[pc++] = 0x06; mem[pc++] = 0x01;  /* LD B, 1 */
    mem[pc++] = 0x80;                      /* ADD A, B → 1 */
    mem[pc++] = 0x04;                      /* INC B → 2 */
    mem[pc++] = 0x80;                      /* ADD A, B → 3 */
    mem[pc++] = 0x04;                      /* INC B → 3 */
    mem[pc++] = 0x80;                      /* ADD A, B → 6 */
    mem[pc++] = 0x04;                      /* INC B → 4 */
    mem[pc++] = 0x80;                      /* ADD A, B → 10 */
    mem[pc++] = 0x04;                      /* INC B → 5 */
    mem[pc++] = 0x80;                      /* ADD A, B → 15 */
    mem[pc++] = 0x76;                      /* HALT */

    ecpu_z80_ops.run((ecpu_state_t *)&cpu);
    assert(cpu.a == 15);
    assert(cpu.b == 5);
    printf("  PASS: arith_program\n");
}

/* ── Test: F3/F5 flags from ADD result ───────────────────────────────────── */

static void test_f35_add(void)
{
    /* ADD: result = 0x28 (bits 3,5 both set) → F3=1, F5=1 */
    setup();
    mem[0] = 0x3E; mem[1] = 0x20;
    mem[2] = 0xC6; mem[3] = 0x08;  /* ADD A, 0x08 → 0x28 */
    mem[4] = 0x76;
    ecpu_z80_ops.run((ecpu_state_t *)&cpu);
    assert(cpu.a == 0x28);
    assert(cpu.f & FLAG_F3);
    assert(cpu.f & FLAG_F5);

    /* ADD: result = 0x10 (bits 3,5 both clear) → F3=0, F5=0 */
    setup();
    mem[0] = 0x3E; mem[1] = 0x08;
    mem[2] = 0xC6; mem[3] = 0x08;  /* ADD A, 0x08 → 0x10 */
    mem[4] = 0x76;
    ecpu_z80_ops.run((ecpu_state_t *)&cpu);
    assert(cpu.a == 0x10);
    assert(!(cpu.f & FLAG_F3));
    assert(!(cpu.f & FLAG_F5));

    printf("  PASS: f35_add\n");
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    printf("test_ecpu_z80:\n");

    /* Step 1 tests */
    test_nop_halt();
    test_ld_r_n();
    test_ld_r_r();
    test_ld_rr_nn();
    test_jp_nn();
    test_jr_e();
    test_jr_backward();
    test_call_ret();
    test_call_trap();
    test_call_unhooked();
    test_call_trap_exit();
    test_common_interface();
    test_r_register();
    test_ld_hl_indirect();
    test_reset();

    /* Step 2 tests */
    test_add();
    test_sub();
    test_adc_sbc();
    test_logic();
    test_cp();
    test_inc_dec_8();
    test_inc_dec_16();
    test_add_hl();
    test_rotates();
    test_daa();
    test_cpl();
    test_scf_ccf();
    test_arith_program();
    test_f35_add();

    printf("All tests passed.\n");
    return 0;
}
