/*
 * test_ecpu_z80.c — Host-side unit tests for the Z80 eCPU core (Step 1)
 *
 * Tests the minimal instruction set: NOP, HALT, LD r,r', LD r,n,
 * LD rr,nn, JP nn, JR e, CALL nn, RET, and the common interface.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "kernel/core/cpu/cpu.h"
#include "kernel/core/cpu/ecpu_z80.h"

/* ── Stubs for page allocator (not linked in host tests) ────────────────── */
void *page_alloc(void) { return NULL; }
void  page_free(void *p) { (void)p; }

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static z80_state_t cpu;
static uint8_t mem[65536];

static void setup(void)
{
    memset(mem, 0, sizeof(mem));
    ecpu_z80_ops.init((cpu_state_t *)&cpu, mem, sizeof(mem));
}

/* ── Test: NOP + HALT ────────────────────────────────────────────────────── */

static void test_nop_halt(void)
{
    setup();
    mem[0] = 0x00;  /* NOP */
    mem[1] = 0x00;  /* NOP */
    mem[2] = 0x76;  /* HALT */

    ecpu_z80_ops.run((cpu_state_t *)&cpu);

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

    ecpu_z80_ops.run((cpu_state_t *)&cpu);

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

    ecpu_z80_ops.run((cpu_state_t *)&cpu);

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

    ecpu_z80_ops.run((cpu_state_t *)&cpu);

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

    ecpu_z80_ops.run((cpu_state_t *)&cpu);

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

    ecpu_z80_ops.run((cpu_state_t *)&cpu);

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
    ecpu_z80_ops.run((cpu_state_t *)&cpu);

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

    ecpu_z80_ops.run((cpu_state_t *)&cpu);

    assert(cpu.a == 0x42);
    assert(cpu.b == 0x42);
    assert(cpu.c == 0x11);
    assert(cpu.halted == 1);
    assert(cpu.sp == 0xFFFE);  /* SP restored after RET */
    assert(cpu.pc == 0x0106);  /* HALT consumed PC */
    printf("  PASS: call_ret\n");
}

/* ── Test: RST-based trap via CALL → JP → RST stub ─────────────────────── */

static int trap_call_count;
static uint32_t trap_last_addr;

static int test_trap_handler(cpu_state_t *state, int trap_type,
                             uint32_t param, void *ctx)
{
    (void)ctx;
    /* Handle RST 0 from stub at 0x0050 (simulated BDOS stub) */
    if (trap_type == CPU_TRAP_RST && param == 0x0000) {
        z80_state_t *cpu = (z80_state_t *)state;
        if (cpu->pc - 1 == 0x0050) {
            trap_call_count++;
            trap_last_addr = 0x0050;
            cpu->a = 0xFF;
            return CPU_TRAP_HANDLED;
        }
    }
    return CPU_TRAP_UNHANDLED;
}

static void test_call_trap(void)
{
    setup();
    cpu.pc = 0x0100;
    cpu.sp = 0xFFFE;
    trap_call_count = 0;
    trap_last_addr = 0;

    ecpu_z80_ops.set_trap_handler((cpu_state_t *)&cpu, test_trap_handler,
                                  NULL);

    /* Set up stub: 0x0005 = JP 0x0050;  0x0050 = RST 0; RET */
    mem[0x05] = 0xC3;  /* JP 0x0050 */
    mem[0x06] = 0x50;
    mem[0x07] = 0x00;
    mem[0x50] = 0xC7;  /* RST 0 */
    mem[0x51] = 0xC9;  /* RET */

    /* LD A, 0x42; CALL 0x0005; HALT */
    mem[0x100] = 0x3E;  /* LD A, 0x42 */
    mem[0x101] = 0x42;
    mem[0x102] = 0xCD;  /* CALL 0x0005 */
    mem[0x103] = 0x05;
    mem[0x104] = 0x00;
    mem[0x105] = 0x76;  /* HALT */

    ecpu_z80_ops.run((cpu_state_t *)&cpu);

    assert(trap_call_count == 1);
    assert(trap_last_addr == 0x0050);
    assert(cpu.a == 0xFF);  /* trap handler set this */
    assert(cpu.sp == 0xFFFE);  /* CALL pushed, RET popped */
    printf("  PASS: call_trap\n");
}

/* ── Test: CALL executes normally (no trap on CALL) ──────────────────────── */

static void test_call_unhooked(void)
{
    setup();
    cpu.pc = 0x0100;
    cpu.sp = 0xFFFE;

    /* CALL 0x0200: CPU pushes PC, jumps, subroutine returns */
    mem[0x100] = 0xCD;  /* CALL 0x0200 */
    mem[0x101] = 0x00;
    mem[0x102] = 0x02;
    mem[0x103] = 0x76;  /* HALT (return target) */

    mem[0x200] = 0x3E;  /* LD A, 0xBB */
    mem[0x201] = 0xBB;
    mem[0x202] = 0xC9;  /* RET */

    ecpu_z80_ops.run((cpu_state_t *)&cpu);

    assert(cpu.a == 0xBB);
    assert(cpu.sp == 0xFFFE);
    printf("  PASS: call_unhooked\n");
}

/* ── Test: RST trap EXIT ─────────────────────────────────────────────────── */

static int exit_trap_handler(cpu_state_t *state, int trap_type,
                             uint32_t param, void *ctx)
{
    (void)state;
    (void)param;
    (void)ctx;
    if (trap_type == CPU_TRAP_RST)
        return CPU_TRAP_EXIT;
    return CPU_TRAP_UNHANDLED;
}

static void test_call_trap_exit(void)
{
    setup();
    cpu.pc = 0x0100;
    cpu.sp = 0xFFFE;

    ecpu_z80_ops.set_trap_handler((cpu_state_t *)&cpu, exit_trap_handler,
                                  NULL);

    mem[0x100] = 0xC7;  /* RST 0 — fires trap, returns EXIT */
    /* Should never reach here */
    mem[0x101] = 0x3E;
    mem[0x102] = 0xAA;
    mem[0x103] = 0x76;

    ecpu_z80_ops.run((cpu_state_t *)&cpu);

    assert(cpu.a == 0x00);  /* LD A,0xAA never executed */
    printf("  PASS: call_trap_exit\n");
}

/* ── Test: common interface get_reg / set_reg ────────────────────────────── */

static void test_common_interface(void)
{
    setup();
    const cpu_ops_t *ops = &ecpu_z80_ops;
    cpu_state_t *s = (cpu_state_t *)&cpu;

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

    ecpu_z80_ops.run((cpu_state_t *)&cpu);

    /* 4 instructions fetched (3 NOP + HALT), R increments low 7 bits */
    assert((cpu.r & 0x7F) == 4);

    /* R bit-7 preservation: set R=0x80, run 3 NOPs + HALT */
    setup();
    cpu.r = 0x80;
    mem[0] = 0x00;  /* NOP */
    mem[1] = 0x00;  /* NOP */
    mem[2] = 0x00;  /* NOP */
    mem[3] = 0x76;  /* HALT */
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    /* low 7 bits: (0x80 + 4) & 0x7F = 0x04; bit 7: 0x80 */
    assert(cpu.r == 0x84);

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

    ecpu_z80_ops.run((cpu_state_t *)&cpu);

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

    ecpu_z80_ops.init((cpu_state_t *)&cpu, mem, sizeof(mem));

    assert(cpu.a == 0);
    assert(cpu.pc == 0);
    assert(cpu.sp == 0);
    assert(cpu.memory == mem);
    /* init zeros trap handler — re-test that memory is preserved */
    assert(mem[0x1000] == 0xAB);  /* memory preserved */
    printf("  PASS: reset (via init)\n");
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
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
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
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x00);
    assert(cpu.f & FLAG_Z);
    assert(cpu.f & FLAG_C);

    /* ADD with overflow: 0x7F + 0x01 = 0x80, PV=1, S=1 */
    setup();
    mem[0] = 0x3E; mem[1] = 0x7F;
    mem[2] = 0xC6; mem[3] = 0x01;
    mem[4] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x80);
    assert(cpu.f & FLAG_PV);
    assert(cpu.f & FLAG_S);
    assert(!(cpu.f & FLAG_C));

    /* ADD with half-carry: 0x0F + 0x01 = 0x10, H=1 */
    setup();
    mem[0] = 0x3E; mem[1] = 0x0F;
    mem[2] = 0xC6; mem[3] = 0x01;
    mem[4] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x10);
    assert(cpu.f & FLAG_H);

    /* ADD A, B (register) */
    setup();
    mem[0] = 0x3E; mem[1] = 0x20;  /* LD A, 0x20 */
    mem[2] = 0x06; mem[3] = 0x15;  /* LD B, 0x15 */
    mem[4] = 0x80;                  /* ADD A, B */
    mem[5] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
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
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x30);
    assert(cpu.f & FLAG_N);
    assert(!(cpu.f & FLAG_C));

    /* SUB with borrow: 0x00 - 0x01 = 0xFF, C=1, S=1 */
    setup();
    mem[0] = 0x3E; mem[1] = 0x00;
    mem[2] = 0xD6; mem[3] = 0x01;
    mem[4] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0xFF);
    assert(cpu.f & FLAG_C);
    assert(cpu.f & FLAG_S);

    /* SUB with overflow: 0x80 - 0x01 = 0x7F, PV=1 */
    setup();
    mem[0] = 0x3E; mem[1] = 0x80;
    mem[2] = 0xD6; mem[3] = 0x01;
    mem[4] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x7F);
    assert(cpu.f & FLAG_PV);

    /* SUB half-carry set: 0x10 - 0x09 = 0x07, H=1 */
    setup();
    mem[0] = 0x3E; mem[1] = 0x10;
    mem[2] = 0xD6; mem[3] = 0x09;
    mem[4] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x07);
    assert(cpu.f & FLAG_H);

    /* SUB half-carry clear: 0x19 - 0x01 = 0x18, H=0 */
    setup();
    mem[0] = 0x3E; mem[1] = 0x19;
    mem[2] = 0xD6; mem[3] = 0x01;
    mem[4] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x18);
    assert(!(cpu.f & FLAG_H));

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
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x44);

    /* SBC: 0x42 - 0x01 - C(1) = 0x40 */
    setup();
    mem[0] = 0x3E; mem[1] = 0xFF;
    mem[2] = 0xC6; mem[3] = 0x01;  /* ADD → C=1 */
    mem[4] = 0x3E; mem[5] = 0x42;
    mem[6] = 0xDE; mem[7] = 0x01;  /* SBC A, 0x01 → 0x42-0x01-1=0x40 */
    mem[8] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x40);

    /* ADC overflow: 0x7F + 0x00 + C(1) = 0x80, PV=1, S=1 */
    setup();
    cpu.f = FLAG_C;
    mem[0] = 0x3E; mem[1] = 0x7F;  /* LD A, 0x7F */
    mem[2] = 0xCE; mem[3] = 0x00;  /* ADC A, 0x00 */
    mem[4] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x80);
    assert(cpu.f & FLAG_PV);
    assert(cpu.f & FLAG_S);
    assert(!(cpu.f & FLAG_C));

    /* ADC half-carry: 0x0F + 0x00 + C(1) = 0x10, H=1 */
    setup();
    cpu.f = FLAG_C;
    mem[0] = 0x3E; mem[1] = 0x0F;
    mem[2] = 0xCE; mem[3] = 0x00;
    mem[4] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x10);
    assert(cpu.f & FLAG_H);

    /* ADC carry+zero: 0xFF + 0x00 + C(1) = 0x00, Z=1, C=1, H=1 */
    setup();
    cpu.f = FLAG_C;
    mem[0] = 0x3E; mem[1] = 0xFF;
    mem[2] = 0xCE; mem[3] = 0x00;
    mem[4] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x00);
    assert(cpu.f & FLAG_Z);
    assert(cpu.f & FLAG_C);
    assert(cpu.f & FLAG_H);

    /* SBC overflow: 0x80 - 0x01 - C(0) = 0x7F, PV=1 */
    setup();
    mem[0] = 0x3E; mem[1] = 0x80;
    mem[2] = 0xDE; mem[3] = 0x01;  /* SBC A, 0x01 (C=0) */
    mem[4] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x7F);
    assert(cpu.f & FLAG_PV);

    /* SBC zero: 0x01 - 0x00 - C(1) = 0x00, Z=1 */
    setup();
    cpu.f = FLAG_C;
    mem[0] = 0x3E; mem[1] = 0x01;
    mem[2] = 0xDE; mem[3] = 0x00;
    mem[4] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x00);
    assert(cpu.f & FLAG_Z);

    /* SBC carry: 0x00 - 0x01 - C(0) = 0xFF, C=1, S=1, H=1 */
    setup();
    mem[0] = 0x3E; mem[1] = 0x00;
    mem[2] = 0xDE; mem[3] = 0x01;
    mem[4] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0xFF);
    assert(cpu.f & FLAG_C);
    assert(cpu.f & FLAG_S);
    assert(cpu.f & FLAG_H);

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
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x0F);
    assert(cpu.f & FLAG_H);
    assert(!(cpu.f & FLAG_C));
    assert(!(cpu.f & FLAG_N));

    /* OR: 0xF0 | 0x0F = 0xFF, parity(0xFF)=even */
    setup();
    mem[0] = 0x3E; mem[1] = 0xF0;
    mem[2] = 0xF6; mem[3] = 0x0F;  /* OR 0x0F */
    mem[4] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0xFF);
    assert(cpu.f & FLAG_PV);  /* even parity */
    assert(cpu.f & FLAG_S);
    assert(!(cpu.f & FLAG_H));

    /* XOR A, A = 0, Z=1, P=1 (even parity of 0) */
    setup();
    mem[0] = 0x3E; mem[1] = 0x42;
    mem[2] = 0xAF;                  /* XOR A (self) */
    mem[3] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x00);
    assert(cpu.f & FLAG_Z);
    assert(cpu.f & FLAG_PV);

    /* AND producing zero: 0xF0 & 0x0F = 0x00, Z=1, PV=1, H=1 */
    setup();
    mem[0] = 0x3E; mem[1] = 0xF0;
    mem[2] = 0xE6; mem[3] = 0x0F;  /* AND 0x0F */
    mem[4] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x00);
    assert(cpu.f & FLAG_Z);
    assert(cpu.f & FLAG_PV);  /* even parity of 0 */
    assert(cpu.f & FLAG_H);

    /* XOR odd parity: 0x01 ^ 0x00 = 0x01, PV=0 */
    setup();
    mem[0] = 0x3E; mem[1] = 0x01;
    mem[2] = 0xEE; mem[3] = 0x00;  /* XOR 0x00 */
    mem[4] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x01);
    assert(!(cpu.f & FLAG_PV));  /* odd parity */
    assert(!(cpu.f & FLAG_Z));

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
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x42);  /* A not modified */
    assert(cpu.f & FLAG_Z);
    assert(cpu.f & FLAG_N);
    assert(!(cpu.f & FLAG_C));

    /* CP: F3/F5 come from operand (0x28), not result */
    setup();
    mem[0] = 0x3E; mem[1] = 0x50;
    mem[2] = 0xFE; mem[3] = 0x28;  /* CP 0x28 → result 0x28 */
    mem[4] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x50);
    /* F3 = bit 3 of 0x28 = 1, F5 = bit 5 of 0x28 = 1 */
    assert(cpu.f & FLAG_F3);
    assert(cpu.f & FLAG_F5);

    /* CP with borrow: A=0x10, CP 0x20 → C=1 */
    setup();
    mem[0] = 0x3E; mem[1] = 0x10;
    mem[2] = 0xFE; mem[3] = 0x20;
    mem[4] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
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
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.b == 0x10);
    assert(cpu.f & FLAG_H);
    assert(!(cpu.f & FLAG_N));

    /* INC: 0x7F → 0x80, PV=1 (overflow) */
    setup();
    mem[0] = 0x3E; mem[1] = 0x7F;
    mem[2] = 0x3C;                  /* INC A */
    mem[3] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x80);
    assert(cpu.f & FLAG_PV);
    assert(cpu.f & FLAG_S);

    /* INC: 0xFF → 0x00, Z=1, H=1 */
    setup();
    mem[0] = 0x3E; mem[1] = 0xFF;
    mem[2] = 0x3C;
    mem[3] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x00);
    assert(cpu.f & FLAG_Z);
    assert(cpu.f & FLAG_H);

    /* DEC B: 0x01 → 0x00, Z=1 */
    setup();
    mem[0] = 0x06; mem[1] = 0x01;
    mem[2] = 0x05;                  /* DEC B */
    mem[3] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.b == 0x00);
    assert(cpu.f & FLAG_Z);
    assert(cpu.f & FLAG_N);

    /* DEC: 0x80 → 0x7F, PV=1 */
    setup();
    mem[0] = 0x3E; mem[1] = 0x80;
    mem[2] = 0x3D;                  /* DEC A */
    mem[3] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x7F);
    assert(cpu.f & FLAG_PV);

    /* DEC: 0x00 → 0xFF, H=1 (borrow from bit 4) */
    setup();
    mem[0] = 0x3E; mem[1] = 0x00;
    mem[2] = 0x3D;
    mem[3] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0xFF);
    assert(cpu.f & FLAG_H);

    /* INC/DEC preserve carry */
    setup();
    cpu.f = FLAG_C;  /* set carry manually */
    mem[0] = 0x3E; mem[1] = 0x10;
    mem[2] = 0x3C;  /* INC A */
    mem[3] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
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
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(z80_bc(&cpu) == 0x0100);

    /* DEC BC: 0x0100 → 0x00FF */
    setup();
    mem[0] = 0x01; mem[1] = 0x00; mem[2] = 0x01;
    mem[3] = 0x0B;                                   /* DEC BC */
    mem[4] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(z80_bc(&cpu) == 0x00FF);

    /* INC SP: 0xFFFE → 0xFFFF */
    setup();
    mem[0] = 0x31; mem[1] = 0xFE; mem[2] = 0xFF;  /* LD SP, 0xFFFE */
    mem[3] = 0x33;                                   /* INC SP */
    mem[4] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.sp == 0xFFFF);

    /* Wrap: INC from 0xFFFF → 0x0000 */
    setup();
    mem[0] = 0x21; mem[1] = 0xFF; mem[2] = 0xFF;  /* LD HL, 0xFFFF */
    mem[3] = 0x23;                                   /* INC HL */
    mem[4] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
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
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(z80_hl(&cpu) == 0x3000);
    assert(!(cpu.f & FLAG_C));
    assert(!(cpu.f & FLAG_N));

    /* With carry: 0xFFFF + 0x0001 = 0x0000, C=1 */
    setup();
    mem[0] = 0x21; mem[1] = 0xFF; mem[2] = 0xFF;
    mem[3] = 0x01; mem[4] = 0x01; mem[5] = 0x00;
    mem[6] = 0x09;
    mem[7] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(z80_hl(&cpu) == 0x0000);
    assert(cpu.f & FLAG_C);

    /* ADD HL, HL (doubles) */
    setup();
    mem[0] = 0x21; mem[1] = 0x34; mem[2] = 0x12;  /* LD HL, 0x1234 */
    mem[3] = 0x29;                                   /* ADD HL, HL */
    mem[4] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(z80_hl(&cpu) == 0x2468);

    /* ADD HL preserves S, Z, PV */
    setup();
    cpu.f = FLAG_S | FLAG_Z | FLAG_PV;
    mem[0] = 0x21; mem[1] = 0x00; mem[2] = 0x10;
    mem[3] = 0x01; mem[4] = 0x00; mem[5] = 0x20;
    mem[6] = 0x09;
    mem[7] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.f & FLAG_S);
    assert(cpu.f & FLAG_Z);
    assert(cpu.f & FLAG_PV);

    /* ADD HL half-carry: 0x0FFF + 0x0001 = 0x1000, H=1 */
    setup();
    mem[0] = 0x21; mem[1] = 0xFF; mem[2] = 0x0F;  /* LD HL, 0x0FFF */
    mem[3] = 0x01; mem[4] = 0x01; mem[5] = 0x00;  /* LD BC, 0x0001 */
    mem[6] = 0x09;
    mem[7] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(z80_hl(&cpu) == 0x1000);
    assert(cpu.f & FLAG_H);

    /* ADD HL no half-carry: 0x1000 + 0x0100 = 0x1100, H=0 */
    setup();
    mem[0] = 0x21; mem[1] = 0x00; mem[2] = 0x10;
    mem[3] = 0x01; mem[4] = 0x00; mem[5] = 0x01;
    mem[6] = 0x09;
    mem[7] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(z80_hl(&cpu) == 0x1100);
    assert(!(cpu.f & FLAG_H));

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
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x01);
    assert(cpu.f & FLAG_C);

    /* RRCA: 0x01 → 0x80, C=1 */
    setup();
    mem[0] = 0x3E; mem[1] = 0x01;
    mem[2] = 0x0F;  /* RRCA */
    mem[3] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x80);
    assert(cpu.f & FLAG_C);

    /* RLA: 0x80 with C=0 → 0x00, C=1 */
    setup();
    mem[0] = 0x3E; mem[1] = 0x80;
    mem[2] = 0x17;  /* RLA */
    mem[3] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x00);
    assert(cpu.f & FLAG_C);

    /* RLA: 0x80 with C=1 → 0x01, C=1 */
    setup();
    cpu.f = FLAG_C;
    mem[0] = 0x3E; mem[1] = 0x80;
    mem[2] = 0x17;  /* RLA */
    mem[3] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x01);
    assert(cpu.f & FLAG_C);

    /* RRA: 0x01 with C=0 → 0x00, C=1 */
    setup();
    mem[0] = 0x3E; mem[1] = 0x01;
    mem[2] = 0x1F;  /* RRA */
    mem[3] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x00);
    assert(cpu.f & FLAG_C);

    /* RRA: 0x01 with C=1 → 0x80, C=1 */
    setup();
    cpu.f = FLAG_C;
    mem[0] = 0x3E; mem[1] = 0x01;
    mem[2] = 0x1F;  /* RRA */
    mem[3] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x80);
    assert(cpu.f & FLAG_C);

    /* Rotates preserve S, Z, PV */
    setup();
    cpu.f = FLAG_S | FLAG_Z | FLAG_PV;
    mem[0] = 0x3E; mem[1] = 0x42;
    mem[2] = 0x07;  /* RLCA */
    mem[3] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
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
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x42);
    assert(!(cpu.f & FLAG_C));

    /* BCD with carry: 0x99 + 0x01 = 0x00 (BCD), C=1 */
    setup();
    mem[0] = 0x3E; mem[1] = 0x99;
    mem[2] = 0xC6; mem[3] = 0x01;  /* ADD A, 0x01 → 0x9A */
    mem[4] = 0x27;                  /* DAA → 0x00, C=1 */
    mem[5] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x00);
    assert(cpu.f & FLAG_C);
    assert(cpu.f & FLAG_Z);

    /* BCD subtraction: 0x42 - 0x15 with DAA */
    setup();
    mem[0] = 0x3E; mem[1] = 0x42;
    mem[2] = 0xD6; mem[3] = 0x15;  /* SUB 0x15 → 0x2D */
    mem[4] = 0x27;                  /* DAA → 0x27 */
    mem[5] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x27);

    /* BCD 0x90 + 0x90: ADD → 0x20 C=1, DAA → 0x80, C=1 */
    setup();
    mem[0] = 0x3E; mem[1] = 0x90;
    mem[2] = 0xC6; mem[3] = 0x90;  /* ADD A, 0x90 → 0x20, C=1 */
    mem[4] = 0x27;                  /* DAA */
    mem[5] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x80);
    assert(cpu.f & FLAG_C);

    /* DAA after SUB with borrow: 0x00 - 0x01 → 0xFF, C=1 H=1 N=1
     * DAA: correction=0x66 (both nibbles), A=0xFF-0x66=0x99, C=1 */
    setup();
    mem[0] = 0x3E; mem[1] = 0x00;
    mem[2] = 0xD6; mem[3] = 0x01;  /* SUB 0x01 → 0xFF, C=1, H=1, N=1 */
    mem[4] = 0x27;                  /* DAA → 0x99 */
    mem[5] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x99);
    assert(cpu.f & FLAG_C);

    printf("  PASS: daa\n");
}

/* ── Test: CPL ───────────────────────────────────────────────────────────── */

static void test_cpl(void)
{
    setup();
    mem[0] = 0x3E; mem[1] = 0x55;
    mem[2] = 0x2F;                  /* CPL → ~0x55 = 0xAA */
    mem[3] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
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
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.f & FLAG_C);
    assert(!(cpu.f & FLAG_N));
    assert(!(cpu.f & FLAG_H));

    /* CCF: complement carry (1 → 0, old C → H) */
    setup();
    cpu.f = FLAG_C;
    mem[0] = 0x3F;  /* CCF */
    mem[1] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(!(cpu.f & FLAG_C));
    assert(cpu.f & FLAG_H);  /* old carry → H */
    assert(!(cpu.f & FLAG_N));

    /* CCF: 0 → 1 */
    setup();
    cpu.f = 0;
    mem[0] = 0x3F;  /* CCF */
    mem[1] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.f & FLAG_C);
    assert(!(cpu.f & FLAG_H));

    /* SCF F3/F5: A=0x28, F=0 → F3/F5 from (A|F)&0x28 = 0x28 */
    setup();
    cpu.f = 0;
    mem[0] = 0x3E; mem[1] = 0x28;  /* LD A, 0x28 */
    mem[2] = 0x37;  /* SCF */
    mem[3] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.f & FLAG_C);
    assert(cpu.f & FLAG_F3);
    assert(cpu.f & FLAG_F5);

    /* SCF F3 from F: A=0x00, F=FLAG_Z|FLAG_F3 → (0x00|0x48)&0x28 = 0x08 */
    setup();
    cpu.f = FLAG_Z | FLAG_F3;
    mem[0] = 0x37;  /* SCF */
    mem[1] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.f & FLAG_C);
    assert(cpu.f & FLAG_F3);
    assert(!(cpu.f & FLAG_F5));

    /* CCF F3/F5: A=0x20, F=FLAG_C → (0x20|0x01)&0x28 = 0x20 → F5=1, F3=0 */
    setup();
    cpu.f = FLAG_C;
    mem[0] = 0x3E; mem[1] = 0x20;  /* LD A, 0x20 */
    mem[2] = 0x3F;  /* CCF */
    mem[3] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(!(cpu.f & FLAG_C));  /* complemented */
    assert(cpu.f & FLAG_H);    /* old C → H */
    assert(cpu.f & FLAG_F5);
    assert(!(cpu.f & FLAG_F3));

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

    ecpu_z80_ops.run((cpu_state_t *)&cpu);
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
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x28);
    assert(cpu.f & FLAG_F3);
    assert(cpu.f & FLAG_F5);

    /* ADD: result = 0x10 (bits 3,5 both clear) → F3=0, F5=0 */
    setup();
    mem[0] = 0x3E; mem[1] = 0x08;
    mem[2] = 0xC6; mem[3] = 0x08;  /* ADD A, 0x08 → 0x10 */
    mem[4] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x10);
    assert(!(cpu.f & FLAG_F3));
    assert(!(cpu.f & FLAG_F5));

    printf("  PASS: f35_add\n");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Step 3 tests — Control flow, PUSH/POP, EX/EXX, RST, DI/EI
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── Test: JP cc, nn (conditional jumps) ─────────────────────────────────── */

static void test_jp_cc(void)
{
    /* JP NZ taken (Z=0) */
    setup();
    cpu.pc = 0x100;
    cpu.f = 0;  /* Z=0 */
    mem[0x100] = 0xC2;  /* JP NZ, 0x200 */
    mem[0x101] = 0x00;
    mem[0x102] = 0x02;
    mem[0x200] = 0x3E; mem[0x201] = 0xAA;  /* LD A, 0xAA */
    mem[0x202] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0xAA);

    /* JP NZ not taken (Z=1) */
    setup();
    cpu.pc = 0x100;
    cpu.f = FLAG_Z;
    mem[0x100] = 0xC2;  /* JP NZ, 0x200 */
    mem[0x101] = 0x00;
    mem[0x102] = 0x02;
    mem[0x103] = 0x3E; mem[0x104] = 0xBB;  /* LD A, 0xBB (fallthrough) */
    mem[0x105] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0xBB);

    /* JP Z taken */
    setup();
    cpu.pc = 0x100;
    cpu.f = FLAG_Z;
    mem[0x100] = 0xCA;  /* JP Z, 0x200 */
    mem[0x101] = 0x00;
    mem[0x102] = 0x02;
    mem[0x200] = 0x3E; mem[0x201] = 0xCC;
    mem[0x202] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0xCC);

    /* JP C taken */
    setup();
    cpu.pc = 0x100;
    cpu.f = FLAG_C;
    mem[0x100] = 0xDA;  /* JP C, 0x200 */
    mem[0x101] = 0x00;
    mem[0x102] = 0x02;
    mem[0x200] = 0x3E; mem[0x201] = 0xDD;
    mem[0x202] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0xDD);

    /* JP NC not taken (C=1) */
    setup();
    cpu.pc = 0x100;
    cpu.f = FLAG_C;
    mem[0x100] = 0xD2;  /* JP NC, 0x200 */
    mem[0x101] = 0x00;
    mem[0x102] = 0x02;
    mem[0x103] = 0x3E; mem[0x104] = 0xEE;
    mem[0x105] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0xEE);

    /* JP PO taken (E2): PV=0 → jump */
    setup();
    cpu.pc = 0x100;
    cpu.f = 0;  /* PV=0 */
    mem[0x100] = 0xE2; mem[0x101] = 0x00; mem[0x102] = 0x02;  /* JP PO, 0x200 */
    mem[0x200] = 0x3E; mem[0x201] = 0x11;
    mem[0x202] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x11);

    /* JP PO not taken: PV=1 → falls through */
    setup();
    cpu.pc = 0x100;
    cpu.f = FLAG_PV;
    mem[0x100] = 0xE2; mem[0x101] = 0x00; mem[0x102] = 0x02;
    mem[0x103] = 0x3E; mem[0x104] = 0x22;
    mem[0x105] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x22);

    /* JP PE taken (EA): PV=1 → jump */
    setup();
    cpu.pc = 0x100;
    cpu.f = FLAG_PV;
    mem[0x100] = 0xEA; mem[0x101] = 0x00; mem[0x102] = 0x02;
    mem[0x200] = 0x3E; mem[0x201] = 0x33;
    mem[0x202] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x33);

    /* JP P taken (F2): S=0 → jump */
    setup();
    cpu.pc = 0x100;
    cpu.f = 0;  /* S=0 */
    mem[0x100] = 0xF2; mem[0x101] = 0x00; mem[0x102] = 0x02;
    mem[0x200] = 0x3E; mem[0x201] = 0x44;
    mem[0x202] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x44);

    /* JP M taken (FA): S=1 → jump */
    setup();
    cpu.pc = 0x100;
    cpu.f = FLAG_S;
    mem[0x100] = 0xFA; mem[0x101] = 0x00; mem[0x102] = 0x02;
    mem[0x200] = 0x3E; mem[0x201] = 0x55;
    mem[0x202] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x55);

    /* JP M not taken: S=0 → falls through */
    setup();
    cpu.pc = 0x100;
    cpu.f = 0;
    mem[0x100] = 0xFA; mem[0x101] = 0x00; mem[0x102] = 0x02;
    mem[0x103] = 0x3E; mem[0x104] = 0x66;
    mem[0x105] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x66);

    printf("  PASS: jp_cc\n");
}

/* ── Test: JR cc, e (conditional relative jumps) ─────────────────────────── */

static void test_jr_cc(void)
{
    /* JR NZ taken */
    setup();
    cpu.pc = 0x100;
    cpu.f = 0;
    mem[0x100] = 0x20;  /* JR NZ, +4 */
    mem[0x101] = 0x04;
    mem[0x106] = 0x3E; mem[0x107] = 0x11;
    mem[0x108] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x11);

    /* JR NZ not taken (Z=1) → falls through */
    setup();
    cpu.pc = 0x100;
    cpu.f = FLAG_Z;
    mem[0x100] = 0x20;  /* JR NZ, +4 */
    mem[0x101] = 0x04;
    mem[0x102] = 0x3E; mem[0x103] = 0x22;
    mem[0x104] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x22);

    /* JR Z taken */
    setup();
    cpu.pc = 0x100;
    cpu.f = FLAG_Z;
    mem[0x100] = 0x28;  /* JR Z, +2 */
    mem[0x101] = 0x02;
    mem[0x104] = 0x3E; mem[0x105] = 0x33;
    mem[0x106] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x33);

    /* JR C taken */
    setup();
    cpu.pc = 0x100;
    cpu.f = FLAG_C;
    mem[0x100] = 0x38;  /* JR C, +2 */
    mem[0x101] = 0x02;
    mem[0x104] = 0x3E; mem[0x105] = 0x44;
    mem[0x106] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x44);

    /* JR NC not taken (C=1) */
    setup();
    cpu.pc = 0x100;
    cpu.f = FLAG_C;
    mem[0x100] = 0x30;  /* JR NC, +4 */
    mem[0x101] = 0x04;
    mem[0x102] = 0x3E; mem[0x103] = 0x55;
    mem[0x104] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x55);

    printf("  PASS: jr_cc\n");
}

/* ── Test: DJNZ (decrement B and jump if not zero) ──────────────────────── */

static void test_djnz(void)
{
    /* Sum 1+2+3+4+5 using DJNZ loop */
    setup();
    cpu.pc = 0x100;
    uint16_t pc = 0x100;
    mem[pc++] = 0x3E; mem[pc++] = 0x00;  /* LD A, 0 */
    mem[pc++] = 0x06; mem[pc++] = 0x05;  /* LD B, 5 */
    mem[pc++] = 0x0E; mem[pc++] = 0x01;  /* LD C, 1 — loop start at 0x104 */
    /* loop body at 0x106 */
    mem[pc++] = 0x81;                      /* ADD A, C */
    mem[pc++] = 0x0C;                      /* INC C */
    mem[pc++] = 0x10;                      /* DJNZ -4 → back to 0x106 */
    mem[pc++] = (uint8_t)-4;               /* offset: pc(0x10A) + (-4) = 0x106 */
    mem[pc++] = 0x76;                      /* HALT */

    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 15);   /* 1+2+3+4+5 */
    assert(cpu.b == 0);    /* B decremented to 0 */
    assert(cpu.c == 6);    /* C incremented past 5 */
    printf("  PASS: djnz\n");
}

/* ── Test: PUSH/POP round-trip ───────────────────────────────────────────── */

static void test_push_pop(void)
{
    setup();
    cpu.pc = 0x100;
    cpu.sp = 0xFFFE;

    uint16_t pc = 0x100;
    /* Load values and push */
    mem[pc++] = 0x01; mem[pc++] = 0x34; mem[pc++] = 0x12;  /* LD BC, 0x1234 */
    mem[pc++] = 0x11; mem[pc++] = 0x78; mem[pc++] = 0x56;  /* LD DE, 0x5678 */
    mem[pc++] = 0x21; mem[pc++] = 0xBC; mem[pc++] = 0x9A;  /* LD HL, 0x9ABC */
    mem[pc++] = 0xC5;  /* PUSH BC */
    mem[pc++] = 0xD5;  /* PUSH DE */
    mem[pc++] = 0xE5;  /* PUSH HL */

    /* Clear registers */
    mem[pc++] = 0x01; mem[pc++] = 0x00; mem[pc++] = 0x00;  /* LD BC, 0 */
    mem[pc++] = 0x11; mem[pc++] = 0x00; mem[pc++] = 0x00;  /* LD DE, 0 */
    mem[pc++] = 0x21; mem[pc++] = 0x00; mem[pc++] = 0x00;  /* LD HL, 0 */

    /* Pop in reverse order */
    mem[pc++] = 0xE1;  /* POP HL — gets 0x9ABC */
    mem[pc++] = 0xD1;  /* POP DE — gets 0x5678 */
    mem[pc++] = 0xC1;  /* POP BC — gets 0x1234 */
    mem[pc++] = 0x76;  /* HALT */

    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(z80_bc(&cpu) == 0x1234);
    assert(z80_de(&cpu) == 0x5678);
    assert(z80_hl(&cpu) == 0x9ABC);
    assert(cpu.sp == 0xFFFE);

    printf("  PASS: push_pop\n");
}

/* ── Test: PUSH/POP AF ───────────────────────────────────────────────────── */

static void test_push_pop_af(void)
{
    setup();
    cpu.pc = 0x100;
    cpu.sp = 0xFFFE;
    cpu.a = 0x42;
    cpu.f = FLAG_C | FLAG_Z | FLAG_S;

    uint16_t pc = 0x100;
    mem[pc++] = 0xF5;  /* PUSH AF */
    /* Clobber AF */
    mem[pc++] = 0x3E; mem[pc++] = 0x00;  /* LD A, 0 */
    /* XOR A to clear flags */
    mem[pc++] = 0xAF;  /* XOR A */
    /* Restore AF */
    mem[pc++] = 0xF1;  /* POP AF */
    mem[pc++] = 0x76;

    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x42);
    assert(cpu.f == (FLAG_C | FLAG_Z | FLAG_S));

    printf("  PASS: push_pop_af\n");
}

/* ── Test: EX AF, AF' ────────────────────────────────────────────────────── */

static void test_ex_af(void)
{
    setup();
    cpu.pc = 0x100;
    cpu.a = 0x11; cpu.f = FLAG_C;
    cpu.a2 = 0x22; cpu.f2 = FLAG_Z;

    mem[0x100] = 0x08;  /* EX AF, AF' */
    mem[0x101] = 0x76;

    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x22);
    assert(cpu.f == FLAG_Z);
    assert(cpu.a2 == 0x11);
    assert(cpu.f2 == FLAG_C);

    printf("  PASS: ex_af\n");
}

/* ── Test: EXX ───────────────────────────────────────────────────────────── */

static void test_exx(void)
{
    setup();
    cpu.pc = 0x100;
    cpu.b = 0x11; cpu.c = 0x22;
    cpu.d = 0x33; cpu.e = 0x44;
    cpu.h = 0x55; cpu.l = 0x66;
    cpu.b2 = 0xAA; cpu.c2 = 0xBB;
    cpu.d2 = 0xCC; cpu.e2 = 0xDD;
    cpu.h2 = 0xEE; cpu.l2 = 0xFF;

    mem[0x100] = 0xD9;  /* EXX */
    mem[0x101] = 0x76;

    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.b == 0xAA && cpu.c == 0xBB);
    assert(cpu.d == 0xCC && cpu.e == 0xDD);
    assert(cpu.h == 0xEE && cpu.l == 0xFF);
    assert(cpu.b2 == 0x11 && cpu.c2 == 0x22);
    assert(cpu.d2 == 0x33 && cpu.e2 == 0x44);
    assert(cpu.h2 == 0x55 && cpu.l2 == 0x66);

    printf("  PASS: exx\n");
}

/* ── Test: EX DE, HL ─────────────────────────────────────────────────────── */

static void test_ex_de_hl(void)
{
    setup();
    cpu.pc = 0x100;

    uint16_t pc = 0x100;
    mem[pc++] = 0x11; mem[pc++] = 0x34; mem[pc++] = 0x12;  /* LD DE, 0x1234 */
    mem[pc++] = 0x21; mem[pc++] = 0x78; mem[pc++] = 0x56;  /* LD HL, 0x5678 */
    mem[pc++] = 0xEB;  /* EX DE, HL */
    mem[pc++] = 0x76;

    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(z80_de(&cpu) == 0x5678);
    assert(z80_hl(&cpu) == 0x1234);

    printf("  PASS: ex_de_hl\n");
}

/* ── Test: EX (SP), HL ───────────────────────────────────────────────────── */

static void test_ex_sp_hl(void)
{
    setup();
    cpu.pc = 0x100;
    cpu.sp = 0xFF00;

    /* Put 0xABCD on top of stack */
    mem[0xFF00] = 0xCD;  /* low byte */
    mem[0xFF01] = 0xAB;  /* high byte */

    uint16_t pc = 0x100;
    mem[pc++] = 0x21; mem[pc++] = 0x34; mem[pc++] = 0x12;  /* LD HL, 0x1234 */
    mem[pc++] = 0xE3;  /* EX (SP), HL */
    mem[pc++] = 0x76;

    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(z80_hl(&cpu) == 0xABCD);
    assert(mem[0xFF00] == 0x34);  /* stack now has 0x1234 */
    assert(mem[0xFF01] == 0x12);
    assert(cpu.wz == 0xABCD);

    printf("  PASS: ex_sp_hl\n");
}

/* ── Test: RET cc (conditional return) ───────────────────────────────────── */

static void test_ret_cc(void)
{
    /* RET NZ taken */
    setup();
    cpu.pc = 0x200;
    cpu.sp = 0xFFFE;
    cpu.f = 0;  /* Z=0 → NZ true */
    /* Push return address 0x0105 onto stack */
    z80_push16(&cpu, 0x0105);
    mem[0x200] = 0xC0;  /* RET NZ */
    mem[0x105] = 0x3E; mem[0x106] = 0xAA;
    mem[0x107] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0xAA);

    /* RET NZ not taken (Z=1) */
    setup();
    cpu.pc = 0x200;
    cpu.sp = 0xFFFE;
    cpu.f = FLAG_Z;
    z80_push16(&cpu, 0x0105);
    mem[0x200] = 0xC0;  /* RET NZ — not taken */
    mem[0x201] = 0x3E; mem[0x202] = 0xBB;
    mem[0x203] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0xBB);

    /* RET Z taken */
    setup();
    cpu.pc = 0x200;
    cpu.sp = 0xFFFE;
    cpu.f = FLAG_Z;
    z80_push16(&cpu, 0x0105);
    mem[0x200] = 0xC8;  /* RET Z */
    mem[0x105] = 0x3E; mem[0x106] = 0xCC;
    mem[0x107] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0xCC);

    /* RET PO taken (E0): PV=0 → returns */
    setup();
    cpu.pc = 0x200;
    cpu.sp = 0xFFFE;
    cpu.f = 0;  /* PV=0 */
    z80_push16(&cpu, 0x0105);
    mem[0x200] = 0xE0;  /* RET PO */
    mem[0x105] = 0x3E; mem[0x106] = 0xDD;
    mem[0x107] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0xDD);

    printf("  PASS: ret_cc\n");
}

/* ── Test: CALL cc, nn (conditional call) ────────────────────────────────── */

static void test_call_cc(void)
{
    /* CALL NZ taken */
    setup();
    cpu.pc = 0x100;
    cpu.sp = 0xFFFE;
    cpu.f = 0;
    mem[0x100] = 0xC4;  /* CALL NZ, 0x200 */
    mem[0x101] = 0x00;
    mem[0x102] = 0x02;
    mem[0x103] = 0x76;  /* HALT (return target) */
    mem[0x200] = 0x3E; mem[0x201] = 0x11;
    mem[0x202] = 0xC9;  /* RET */
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x11);
    assert(cpu.sp == 0xFFFE);

    /* CALL NZ not taken (Z=1) */
    setup();
    cpu.pc = 0x100;
    cpu.sp = 0xFFFE;
    cpu.f = FLAG_Z;
    mem[0x100] = 0xC4;  /* CALL NZ, 0x200 */
    mem[0x101] = 0x00;
    mem[0x102] = 0x02;
    mem[0x103] = 0x3E; mem[0x104] = 0x22;
    mem[0x105] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x22);
    assert(cpu.sp == 0xFFFE);

    /* CALL PE taken (EC): PV=1 → calls subroutine */
    setup();
    cpu.pc = 0x100;
    cpu.sp = 0xFFFE;
    cpu.f = FLAG_PV;
    mem[0x100] = 0xEC;  /* CALL PE, 0x200 */
    mem[0x101] = 0x00;
    mem[0x102] = 0x02;
    mem[0x200] = 0x3E; mem[0x201] = 0x33;
    mem[0x202] = 0xC9;  /* RET */
    mem[0x103] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x33);

    printf("  PASS: call_cc\n");
}

/* ── Test: RST trap hook ─────────────────────────────────────────────────── */

static int rst_trap_count;
static uint32_t rst_trap_addr;

static int rst_trap_handler(cpu_state_t *state, int trap_type,
                            uint32_t param, void *ctx)
{
    (void)state; (void)ctx;
    if (trap_type == CPU_TRAP_RST) {
        rst_trap_count++;
        rst_trap_addr = param;
        return CPU_TRAP_HANDLED;
    }
    return CPU_TRAP_UNHANDLED;
}

static void test_rst_trap(void)
{
    setup();
    cpu.pc = 0x100;
    cpu.sp = 0xFFFE;
    rst_trap_count = 0;

    ecpu_z80_ops.set_trap_handler((cpu_state_t *)&cpu, rst_trap_handler, NULL);

    mem[0x100] = 0xC7;  /* RST 0x00 */
    mem[0x101] = 0xCF;  /* RST 0x08 */
    mem[0x102] = 0xD7;  /* RST 0x10 */
    mem[0x103] = 0xDF;  /* RST 0x18 */
    mem[0x104] = 0xE7;  /* RST 0x20 */
    mem[0x105] = 0xEF;  /* RST 0x28 */
    mem[0x106] = 0xF7;  /* RST 0x30 */
    mem[0x107] = 0xFF;  /* RST 0x38 */
    mem[0x108] = 0x76;  /* HALT */

    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(rst_trap_count == 8);
    assert(rst_trap_addr == 0x38);  /* last RST address */
    assert(cpu.sp == 0xFFFE);       /* all handled, no pushes */

    printf("  PASS: rst_trap\n");
}

/* ── Test: RST without trap → pushes PC and jumps ────────────────────────── */

static void test_rst_no_trap(void)
{
    setup();
    cpu.pc = 0x100;
    cpu.sp = 0xFFFE;

    mem[0x100] = 0xCF;  /* RST 0x08 */
    /* At 0x08: return immediately */
    mem[0x08] = 0x3E; mem[0x09] = 0x77;
    mem[0x0A] = 0xC9;  /* RET */
    mem[0x101] = 0x76;  /* HALT (return target) */

    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x77);
    assert(cpu.sp == 0xFFFE);

    printf("  PASS: rst_no_trap\n");
}

/* ── Test: DI / EI ───────────────────────────────────────────────────────── */

static void test_di_ei(void)
{
    setup();
    cpu.pc = 0x100;
    cpu.iff1 = 1; cpu.iff2 = 1;

    mem[0x100] = 0xF3;  /* DI */
    mem[0x101] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.iff1 == 0);
    assert(cpu.iff2 == 0);

    setup();
    cpu.pc = 0x100;
    cpu.iff1 = 0; cpu.iff2 = 0;
    mem[0x100] = 0xFB;  /* EI */
    mem[0x101] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.iff1 == 1);
    assert(cpu.iff2 == 1);

    printf("  PASS: di_ei\n");
}

/* ── Test: JP (HL) ───────────────────────────────────────────────────────── */

static void test_jp_hl(void)
{
    setup();
    cpu.pc = 0x100;

    uint16_t pc = 0x100;
    mem[pc++] = 0x21; mem[pc++] = 0x00; mem[pc++] = 0x02;  /* LD HL, 0x200 */
    mem[pc++] = 0xE9;  /* JP (HL) */
    mem[0x200] = 0x3E; mem[0x201] = 0x99;
    mem[0x202] = 0x76;

    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x99);

    printf("  PASS: jp_hl\n");
}

/* ── Test: LD SP, HL ─────────────────────────────────────────────────────── */

static void test_ld_sp_hl(void)
{
    setup();
    cpu.pc = 0x100;

    uint16_t pc = 0x100;
    mem[pc++] = 0x21; mem[pc++] = 0x00; mem[pc++] = 0x80;  /* LD HL, 0x8000 */
    mem[pc++] = 0xF9;  /* LD SP, HL */
    mem[pc++] = 0x76;

    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.sp == 0x8000);

    printf("  PASS: ld_sp_hl\n");
}

/* ── Test: nested CALL/RET with conditionals ─────────────────────────────── */

static void test_nested_call_ret(void)
{
    setup();
    cpu.pc = 0x100;
    cpu.sp = 0xFFFE;

    /* Main: LD A,0; CALL 0x200; HALT */
    uint16_t pc = 0x100;
    mem[pc++] = 0x3E; mem[pc++] = 0x00;  /* LD A, 0 */
    mem[pc++] = 0xCD;                      /* CALL 0x200 */
    mem[pc++] = 0x00; mem[pc++] = 0x02;
    mem[pc++] = 0x76;                      /* HALT (at 0x105) */

    /* Sub1 at 0x200: INC A; CALL 0x300; INC A; RET */
    pc = 0x200;
    mem[pc++] = 0x3C;                      /* INC A → 1 */
    mem[pc++] = 0xCD;                      /* CALL 0x300 */
    mem[pc++] = 0x00; mem[pc++] = 0x03;
    mem[pc++] = 0x3C;                      /* INC A → 3 */
    mem[pc++] = 0xC9;                      /* RET */

    /* Sub2 at 0x300: INC A; RET */
    pc = 0x300;
    mem[pc++] = 0x3C;                      /* INC A → 2 */
    mem[pc++] = 0xC9;                      /* RET */

    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 3);
    assert(cpu.sp == 0xFFFE);

    printf("  PASS: nested_call_ret\n");
}

/* ── Test: WZ register set by JP/CALL/RET ────────────────────────────────── */

static void test_wz_tracking(void)
{
    /* JP nn sets WZ */
    setup();
    cpu.pc = 0x100;
    mem[0x100] = 0xC3; mem[0x101] = 0x34; mem[0x102] = 0x12;
    mem[0x1234] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.wz == 0x1234);

    /* JP cc sets WZ even when not taken */
    setup();
    cpu.pc = 0x100;
    cpu.f = FLAG_Z;  /* NZ not met */
    mem[0x100] = 0xC2; mem[0x101] = 0x78; mem[0x102] = 0x56;  /* JP NZ */
    mem[0x103] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.wz == 0x5678);

    /* RET sets WZ */
    setup();
    cpu.pc = 0x200;
    cpu.sp = 0xFFFE;
    z80_push16(&cpu, 0xABCD);
    mem[0x200] = 0xC9;  /* RET */
    mem[0xABCD] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.wz == 0xABCD);

    printf("  PASS: wz_tracking\n");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Step 4 tests — Memory indirect loads, IN/OUT port trapping
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── Test: LD (BC),A / LD A,(BC) ─────────────────────────────────────────── */

static void test_ld_bc_indirect(void)
{
    setup();
    cpu.pc = 0x100;
    uint16_t pc = 0x100;
    mem[pc++] = 0x3E; mem[pc++] = 0x42;  /* LD A, 0x42 */
    mem[pc++] = 0x01; mem[pc++] = 0x00; mem[pc++] = 0x80;  /* LD BC, 0x8000 */
    mem[pc++] = 0x02;  /* LD (BC), A */
    mem[pc++] = 0x3E; mem[pc++] = 0x00;  /* LD A, 0 */
    mem[pc++] = 0x0A;  /* LD A, (BC) */
    mem[pc++] = 0x76;

    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x42);
    assert(mem[0x8000] == 0x42);
    printf("  PASS: ld_bc_indirect\n");
}

/* ── Test: LD (DE),A / LD A,(DE) ─────────────────────────────────────────── */

static void test_ld_de_indirect(void)
{
    setup();
    cpu.pc = 0x100;
    uint16_t pc = 0x100;
    mem[pc++] = 0x3E; mem[pc++] = 0x77;  /* LD A, 0x77 */
    mem[pc++] = 0x11; mem[pc++] = 0x00; mem[pc++] = 0x90;  /* LD DE, 0x9000 */
    mem[pc++] = 0x12;  /* LD (DE), A */
    mem[pc++] = 0x3E; mem[pc++] = 0x00;  /* LD A, 0 */
    mem[pc++] = 0x1A;  /* LD A, (DE) */
    mem[pc++] = 0x76;

    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x77);
    assert(mem[0x9000] == 0x77);
    printf("  PASS: ld_de_indirect\n");
}

/* ── Test: LD (nn),HL / LD HL,(nn) ──────────────────────────────────────── */

static void test_ld_nn_hl(void)
{
    setup();
    cpu.pc = 0x100;
    uint16_t pc = 0x100;
    mem[pc++] = 0x21; mem[pc++] = 0xCD; mem[pc++] = 0xAB;  /* LD HL, 0xABCD */
    mem[pc++] = 0x22; mem[pc++] = 0x00; mem[pc++] = 0x80;  /* LD (0x8000), HL */
    mem[pc++] = 0x21; mem[pc++] = 0x00; mem[pc++] = 0x00;  /* LD HL, 0 */
    mem[pc++] = 0x2A; mem[pc++] = 0x00; mem[pc++] = 0x80;  /* LD HL, (0x8000) */
    mem[pc++] = 0x76;

    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(z80_hl(&cpu) == 0xABCD);
    assert(mem[0x8000] == 0xCD);
    assert(mem[0x8001] == 0xAB);
    printf("  PASS: ld_nn_hl\n");
}

/* ── Test: LD (nn),A / LD A,(nn) ─────────────────────────────────────────── */

static void test_ld_nn_a(void)
{
    setup();
    cpu.pc = 0x100;
    uint16_t pc = 0x100;
    mem[pc++] = 0x3E; mem[pc++] = 0x99;  /* LD A, 0x99 */
    mem[pc++] = 0x32; mem[pc++] = 0x00; mem[pc++] = 0x80;  /* LD (0x8000), A */
    mem[pc++] = 0x3E; mem[pc++] = 0x00;  /* LD A, 0 */
    mem[pc++] = 0x3A; mem[pc++] = 0x00; mem[pc++] = 0x80;  /* LD A, (0x8000) */
    mem[pc++] = 0x76;

    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x99);
    assert(mem[0x8000] == 0x99);
    printf("  PASS: ld_nn_a\n");
}

/* ── Test: OUT (n),A / IN A,(n) with trap handler ────────────────────────── */

static int io_out_count;
static uint32_t io_out_addr;
static uint8_t io_out_data;
static int io_in_count;
static uint32_t io_in_addr;
static uint8_t io_in_data;

static int io_trap_handler(cpu_state_t *state, int trap_type,
                           uint32_t param, void *ctx)
{
    (void)ctx;
    z80_state_t *z = (z80_state_t *)state;
    if (trap_type == CPU_TRAP_IO_OUT) {
        io_out_count++;
        io_out_addr = param;
        io_out_data = z->a;
        return CPU_TRAP_HANDLED;
    }
    if (trap_type == CPU_TRAP_IO_IN) {
        io_in_count++;
        io_in_addr = param;
        z->a = io_in_data;  /* return pre-set value */
        return CPU_TRAP_HANDLED;
    }
    return CPU_TRAP_UNHANDLED;
}

static void test_out_in(void)
{
    /* OUT (0x01), A — port address = (A<<8)|port */
    setup();
    cpu.pc = 0x100;
    io_out_count = 0;
    io_in_count = 0;
    io_in_data = 0xBB;

    ecpu_z80_ops.set_trap_handler((cpu_state_t *)&cpu, io_trap_handler, NULL);

    uint16_t pc = 0x100;
    mem[pc++] = 0x3E; mem[pc++] = 0x42;  /* LD A, 0x42 */
    mem[pc++] = 0xD3; mem[pc++] = 0x01;  /* OUT (0x01), A → addr=0x4201 */
    mem[pc++] = 0xDB; mem[pc++] = 0x10;  /* IN A, (0x10) → addr=0x4210 (A=0x42 at fetch time) */
    mem[pc++] = 0x76;

    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(io_out_count == 1);
    assert(io_out_addr == 0x4201);
    assert(io_out_data == 0x42);
    assert(io_in_count == 1);
    assert(io_in_addr == 0x4210);
    assert(cpu.a == 0xBB);  /* IN loaded value from trap handler */

    printf("  PASS: out_in\n");
}

/* ── Test: I/O without trap (unhandled) ──────────────────────────────────── */

static void test_io_no_trap(void)
{
    setup();
    cpu.pc = 0x100;

    uint16_t pc = 0x100;
    mem[pc++] = 0x3E; mem[pc++] = 0x42;  /* LD A, 0x42 */
    mem[pc++] = 0xD3; mem[pc++] = 0x01;  /* OUT (0x01), A — no handler */
    mem[pc++] = 0xDB; mem[pc++] = 0x10;  /* IN A, (0x10) — no handler */
    mem[pc++] = 0x76;

    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    /* Should not crash; A unchanged from IN since no handler modifies it */
    assert(cpu.a == 0x42);
    printf("  PASS: io_no_trap\n");
}

/* ── Test: memory copy program using indirect loads ──────────────────────── */

static void test_memcpy_program(void)
{
    setup();
    cpu.pc = 0x100;

    /* Copy 4 bytes from 0x8000 to 0x9000 using LD A,(DE) / LD (BC),A loop */
    mem[0x8000] = 0x11; mem[0x8001] = 0x22;
    mem[0x8002] = 0x33; mem[0x8003] = 0x44;

    uint16_t pc = 0x100;
    mem[pc++] = 0x11; mem[pc++] = 0x00; mem[pc++] = 0x80;  /* LD DE, 0x8000 (src) */
    mem[pc++] = 0x01; mem[pc++] = 0x00; mem[pc++] = 0x90;  /* LD BC, 0x9000 (dst) */
    mem[pc++] = 0x21; mem[pc++] = 0x04; mem[pc++] = 0x00;  /* LD HL, 4 (count in L) */
    /* loop at 0x109: */
    mem[pc++] = 0x1A;  /* LD A, (DE) */
    mem[pc++] = 0x02;  /* LD (BC), A */
    mem[pc++] = 0x13;  /* INC DE */
    mem[pc++] = 0x03;  /* INC BC */
    mem[pc++] = 0x2D;  /* DEC L */
    mem[pc++] = 0x20;  /* JR NZ, -7 → back to 0x109 */
    mem[pc++] = (uint8_t)-7;
    mem[pc++] = 0x76;

    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(mem[0x9000] == 0x11);
    assert(mem[0x9001] == 0x22);
    assert(mem[0x9002] == 0x33);
    assert(mem[0x9003] == 0x44);
    printf("  PASS: memcpy_program\n");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Step 5 tests — CB prefix (shifts, rotates, BIT/RES/SET)
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── Test: RLC / RRC ─────────────────────────────────────────────────────── */

static void test_cb_rlc_rrc(void)
{
    /* RLC B: 0x85 → 0x0B, C=1 */
    setup();
    cpu.pc = 0x100;
    mem[0x100] = 0x06; mem[0x101] = 0x85;  /* LD B, 0x85 */
    mem[0x102] = 0xCB; mem[0x103] = 0x00;  /* RLC B */
    mem[0x104] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.b == 0x0B);
    assert(cpu.f & FLAG_C);
    assert(!(cpu.f & FLAG_Z));
    assert(z80_parity_table[0x0B] == (cpu.f & FLAG_PV));

    /* RRC C: 0x01 → 0x80, C=1 */
    setup();
    cpu.pc = 0x100;
    mem[0x100] = 0x0E; mem[0x101] = 0x01;  /* LD C, 0x01 */
    mem[0x102] = 0xCB; mem[0x103] = 0x09;  /* RRC C */
    mem[0x104] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.c == 0x80);
    assert(cpu.f & FLAG_C);
    assert(cpu.f & FLAG_S);

    /* RLC: 0x00 → 0x00, C=0, Z=1 */
    setup();
    cpu.pc = 0x100;
    mem[0x100] = 0x06; mem[0x101] = 0x00;
    mem[0x102] = 0xCB; mem[0x103] = 0x00;
    mem[0x104] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.b == 0x00);
    assert(cpu.f & FLAG_Z);
    assert(!(cpu.f & FLAG_C));

    printf("  PASS: cb_rlc_rrc\n");
}

/* ── Test: RL / RR (through carry) ───────────────────────────────────────── */

static void test_cb_rl_rr(void)
{
    /* RL D: 0x80, C=0 → 0x00, C=1, Z=1 */
    setup();
    cpu.pc = 0x100;
    mem[0x100] = 0x16; mem[0x101] = 0x80;  /* LD D, 0x80 */
    mem[0x102] = 0xCB; mem[0x103] = 0x12;  /* RL D */
    mem[0x104] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.d == 0x00);
    assert(cpu.f & FLAG_C);
    assert(cpu.f & FLAG_Z);

    /* RL D: 0x80, C=1 → 0x01, C=1 */
    setup();
    cpu.pc = 0x100;
    cpu.f = FLAG_C;
    mem[0x100] = 0x16; mem[0x101] = 0x80;
    mem[0x102] = 0xCB; mem[0x103] = 0x12;
    mem[0x104] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.d == 0x01);
    assert(cpu.f & FLAG_C);

    /* RR E: 0x01, C=0 → 0x00, C=1, Z=1 */
    setup();
    cpu.pc = 0x100;
    mem[0x100] = 0x1E; mem[0x101] = 0x01;  /* LD E, 0x01 */
    mem[0x102] = 0xCB; mem[0x103] = 0x1B;  /* RR E */
    mem[0x104] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.e == 0x00);
    assert(cpu.f & FLAG_C);
    assert(cpu.f & FLAG_Z);

    /* RR E: 0x01, C=1 → 0x80, C=1 */
    setup();
    cpu.pc = 0x100;
    cpu.f = FLAG_C;
    mem[0x100] = 0x1E; mem[0x101] = 0x01;
    mem[0x102] = 0xCB; mem[0x103] = 0x1B;
    mem[0x104] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.e == 0x80);
    assert(cpu.f & FLAG_C);
    assert(cpu.f & FLAG_S);

    printf("  PASS: cb_rl_rr\n");
}

/* ── Test: SLA / SRA / SRL ───────────────────────────────────────────────── */

static void test_cb_shifts(void)
{
    /* SLA A: 0x81 → 0x02, C=1 */
    setup();
    cpu.pc = 0x100;
    mem[0x100] = 0x3E; mem[0x101] = 0x81;
    mem[0x102] = 0xCB; mem[0x103] = 0x27;  /* SLA A */
    mem[0x104] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x02);
    assert(cpu.f & FLAG_C);

    /* SRA A: 0x81 → 0xC0, C=1 (bit 7 preserved) */
    setup();
    cpu.pc = 0x100;
    mem[0x100] = 0x3E; mem[0x101] = 0x81;
    mem[0x102] = 0xCB; mem[0x103] = 0x2F;  /* SRA A */
    mem[0x104] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0xC0);
    assert(cpu.f & FLAG_C);
    assert(cpu.f & FLAG_S);

    /* SRL A: 0x81 → 0x40, C=1 (bit 7 cleared) */
    setup();
    cpu.pc = 0x100;
    mem[0x100] = 0x3E; mem[0x101] = 0x81;
    mem[0x102] = 0xCB; mem[0x103] = 0x3F;  /* SRL A */
    mem[0x104] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x40);
    assert(cpu.f & FLAG_C);
    assert(!(cpu.f & FLAG_S));

    /* SLA: 0x00 → 0x00, Z=1 */
    setup();
    cpu.pc = 0x100;
    mem[0x100] = 0x3E; mem[0x101] = 0x00;
    mem[0x102] = 0xCB; mem[0x103] = 0x27;
    mem[0x104] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x00);
    assert(cpu.f & FLAG_Z);

    /* SRA positive: 0x40 → 0x20, C=0, S=0 (bit 7 stays 0) */
    setup();
    cpu.pc = 0x100;
    mem[0x100] = 0x3E; mem[0x101] = 0x40;
    mem[0x102] = 0xCB; mem[0x103] = 0x2F;  /* SRA A */
    mem[0x104] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x20);
    assert(!(cpu.f & FLAG_C));
    assert(!(cpu.f & FLAG_S));

    /* SRA 0x01 → 0x00, C=1, Z=1 */
    setup();
    cpu.pc = 0x100;
    mem[0x100] = 0x3E; mem[0x101] = 0x01;
    mem[0x102] = 0xCB; mem[0x103] = 0x2F;  /* SRA A */
    mem[0x104] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x00);
    assert(cpu.f & FLAG_C);
    assert(cpu.f & FLAG_Z);

    printf("  PASS: cb_shifts\n");
}

/* ── Test: SLL (undocumented) ────────────────────────────────────────────── */

static void test_cb_sll(void)
{
    /* SLL A: 0x80 → 0x01, C=1 (bit 0 set to 1) */
    setup();
    cpu.pc = 0x100;
    mem[0x100] = 0x3E; mem[0x101] = 0x80;
    mem[0x102] = 0xCB; mem[0x103] = 0x37;  /* SLL A (undocumented) */
    mem[0x104] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x01);
    assert(cpu.f & FLAG_C);

    /* SLL A: 0x00 → 0x01, C=0 */
    setup();
    cpu.pc = 0x100;
    mem[0x100] = 0x3E; mem[0x101] = 0x00;
    mem[0x102] = 0xCB; mem[0x103] = 0x37;
    mem[0x104] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x01);
    assert(!(cpu.f & FLAG_C));
    assert(!(cpu.f & FLAG_Z));

    printf("  PASS: cb_sll\n");
}

/* ── Test: BIT n, r ──────────────────────────────────────────────────────── */

static void test_cb_bit(void)
{
    /* BIT 0, A: A=0x01 → bit 0 set, Z=0 */
    setup();
    cpu.pc = 0x100;
    mem[0x100] = 0x3E; mem[0x101] = 0x01;
    mem[0x102] = 0xCB; mem[0x103] = 0x47;  /* BIT 0, A */
    mem[0x104] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(!(cpu.f & FLAG_Z));
    assert(cpu.f & FLAG_H);
    assert(!(cpu.f & FLAG_N));

    /* BIT 1, A: A=0x01 → bit 1 clear, Z=1 */
    setup();
    cpu.pc = 0x100;
    mem[0x100] = 0x3E; mem[0x101] = 0x01;
    mem[0x102] = 0xCB; mem[0x103] = 0x4F;  /* BIT 1, A */
    mem[0x104] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.f & FLAG_Z);
    assert(cpu.f & FLAG_PV);  /* PV=Z for BIT */

    /* BIT 7, A: A=0x80 → bit 7 set, S=1 */
    setup();
    cpu.pc = 0x100;
    mem[0x100] = 0x3E; mem[0x101] = 0x80;
    mem[0x102] = 0xCB; mem[0x103] = 0x7F;  /* BIT 7, A */
    mem[0x104] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(!(cpu.f & FLAG_Z));
    assert(cpu.f & FLAG_S);

    /* BIT preserves carry */
    setup();
    cpu.pc = 0x100;
    cpu.f = FLAG_C;
    mem[0x100] = 0x3E; mem[0x101] = 0x00;
    mem[0x102] = 0xCB; mem[0x103] = 0x47;  /* BIT 0, A */
    mem[0x104] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.f & FLAG_Z);
    assert(cpu.f & FLAG_C);

    /* BIT F3/F5 set: A=0x28 (bits 3,5 set), BIT 0,A → Z=0, F3=1, F5=1 */
    setup();
    cpu.pc = 0x100;
    mem[0x100] = 0x3E; mem[0x101] = 0x29;  /* LD A, 0x29 (bit0=1, bit3=1, bit5=1) */
    mem[0x102] = 0xCB; mem[0x103] = 0x47;  /* BIT 0, A */
    mem[0x104] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(!(cpu.f & FLAG_Z));
    assert(cpu.f & FLAG_F3);
    assert(cpu.f & FLAG_F5);

    /* BIT F3/F5 clear: A=0x01, BIT 0,A → F3=0, F5=0 */
    setup();
    cpu.pc = 0x100;
    mem[0x100] = 0x3E; mem[0x101] = 0x01;
    mem[0x102] = 0xCB; mem[0x103] = 0x47;
    mem[0x104] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(!(cpu.f & FLAG_Z));
    assert(!(cpu.f & FLAG_F3));
    assert(!(cpu.f & FLAG_F5));

    /* BIT (HL) F3/F5: undocumented — come from WZ high byte, not value.
     * Set WZ high byte = 0x29 (bits 3,5 set) so F3=1, F5=1. */
    setup();
    cpu.pc = 0x100;
    mem[0x8000] = 0x01;   /* bit 0 set so BIT 0,(HL) → NZ */
    cpu.wz = 0x2900;      /* high byte 0x29: bits 3 and 5 set */
    uint16_t pc = 0x100;
    mem[pc++] = 0x21; mem[pc++] = 0x00; mem[pc++] = 0x80;  /* LD HL, 0x8000 */
    mem[pc++] = 0xCB; mem[pc++] = 0x46;  /* BIT 0, (HL) */
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(!(cpu.f & FLAG_Z));
    assert(cpu.f & FLAG_F3);
    assert(cpu.f & FLAG_F5);

    printf("  PASS: cb_bit\n");
}

/* ── Test: RES / SET n, r ────────────────────────────────────────────────── */

static void test_cb_res_set(void)
{
    /* SET 3, A: 0x00 → 0x08 */
    setup();
    cpu.pc = 0x100;
    mem[0x100] = 0x3E; mem[0x101] = 0x00;
    mem[0x102] = 0xCB; mem[0x103] = 0xDF;  /* SET 3, A */
    mem[0x104] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x08);

    /* SET 7, B: 0x00 → 0x80 */
    setup();
    cpu.pc = 0x100;
    mem[0x100] = 0x06; mem[0x101] = 0x00;
    mem[0x102] = 0xCB; mem[0x103] = 0xF8;  /* SET 7, B */
    mem[0x104] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.b == 0x80);

    /* RES 3, A: 0xFF → 0xF7 */
    setup();
    cpu.pc = 0x100;
    mem[0x100] = 0x3E; mem[0x101] = 0xFF;
    mem[0x102] = 0xCB; mem[0x103] = 0x9F;  /* RES 3, A */
    mem[0x104] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0xF7);

    /* RES 0, C: 0x01 → 0x00 */
    setup();
    cpu.pc = 0x100;
    mem[0x100] = 0x0E; mem[0x101] = 0x01;
    mem[0x102] = 0xCB; mem[0x103] = 0x81;  /* RES 0, C */
    mem[0x104] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.c == 0x00);

    printf("  PASS: cb_res_set\n");
}

/* ── Test: CB ops on (HL) ────────────────────────────────────────────────── */

static void test_cb_hl_indirect(void)
{
    /* RLC (HL): mem[0x8000]=0x81 → 0x03, C=1 */
    setup();
    cpu.pc = 0x100;
    mem[0x8000] = 0x81;
    uint16_t pc = 0x100;
    mem[pc++] = 0x21; mem[pc++] = 0x00; mem[pc++] = 0x80;  /* LD HL, 0x8000 */
    mem[pc++] = 0xCB; mem[pc++] = 0x06;  /* RLC (HL) */
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(mem[0x8000] == 0x03);
    assert(cpu.f & FLAG_C);

    /* BIT 7, (HL): mem[0x8000]=0x03 → bit 7 clear, Z=1 */
    setup();
    cpu.pc = 0x100;
    mem[0x8000] = 0x03;
    pc = 0x100;
    mem[pc++] = 0x21; mem[pc++] = 0x00; mem[pc++] = 0x80;
    mem[pc++] = 0xCB; mem[pc++] = 0x7E;  /* BIT 7, (HL) */
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.f & FLAG_Z);

    /* SET 7, (HL): mem[0x8000]=0x00 → 0x80 */
    setup();
    cpu.pc = 0x100;
    mem[0x8000] = 0x00;
    pc = 0x100;
    mem[pc++] = 0x21; mem[pc++] = 0x00; mem[pc++] = 0x80;
    mem[pc++] = 0xCB; mem[pc++] = 0xFE;  /* SET 7, (HL) */
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(mem[0x8000] == 0x80);

    /* RES 7, (HL): mem[0x8000]=0xFF → 0x7F */
    setup();
    cpu.pc = 0x100;
    mem[0x8000] = 0xFF;
    pc = 0x100;
    mem[pc++] = 0x21; mem[pc++] = 0x00; mem[pc++] = 0x80;
    mem[pc++] = 0xCB; mem[pc++] = 0xBE;  /* RES 7, (HL) */
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(mem[0x8000] == 0x7F);

    printf("  PASS: cb_hl_indirect\n");
}

/* ── Test: multi-bit shift program ───────────────────────────────────────── */

static void test_cb_shift_program(void)
{
    /* Multiply A by 4 using two SLA A instructions */
    setup();
    cpu.pc = 0x100;
    mem[0x100] = 0x3E; mem[0x101] = 0x0A;  /* LD A, 10 */
    mem[0x102] = 0xCB; mem[0x103] = 0x27;  /* SLA A → 20 */
    mem[0x104] = 0xCB; mem[0x105] = 0x27;  /* SLA A → 40 */
    mem[0x106] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 40);

    /* Count bits using RLC + ADC: count set bits in 0xA5 (4 bits) */
    setup();
    cpu.pc = 0x100;
    uint16_t pc = 0x100;
    mem[pc++] = 0x06; mem[pc++] = 0xA5;  /* LD B, 0xA5 */
    mem[pc++] = 0x0E; mem[pc++] = 0x08;  /* LD C, 8 (counter) */
    mem[pc++] = 0x3E; mem[pc++] = 0x00;  /* LD A, 0 */
    /* loop at pc=0x106: */
    mem[pc++] = 0xCB; mem[pc++] = 0x00;  /* RLC B → bit into carry */
    mem[pc++] = 0xCE; mem[pc++] = 0x00;  /* ADC A, 0 → add carry to A */
    mem[pc++] = 0x0D;                      /* DEC C */
    mem[pc++] = 0x20; mem[pc++] = (uint8_t)-7;  /* JR NZ, back to 0x106 */
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 4);  /* 0xA5 = 10100101 = 4 bits set */

    printf("  PASS: cb_shift_program\n");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Step 6 tests — ED prefix (block ops, 16-bit arith, NEG, RLD/RRD, etc.)
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── Test: NEG ───────────────────────────────────────────────────────────── */

static void test_ed_neg(void)
{
    /* NEG: A=0x42 → A=0xBE, C=1, N=1 */
    setup();
    cpu.pc = 0x100;
    mem[0x100] = 0x3E; mem[0x101] = 0x42;  /* LD A, 0x42 */
    mem[0x102] = 0xED; mem[0x103] = 0x44;  /* NEG */
    mem[0x104] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0xBE);
    assert(cpu.f & FLAG_C);
    assert(cpu.f & FLAG_N);

    /* NEG: A=0x00 → A=0x00, C=0, Z=1 */
    setup();
    cpu.pc = 0x100;
    mem[0x100] = 0x3E; mem[0x101] = 0x00;
    mem[0x102] = 0xED; mem[0x103] = 0x44;
    mem[0x104] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x00);
    assert(!(cpu.f & FLAG_C));
    assert(cpu.f & FLAG_Z);

    /* NEG: A=0x80 → A=0x80, PV=1 (overflow) */
    setup();
    cpu.pc = 0x100;
    mem[0x100] = 0x3E; mem[0x101] = 0x80;
    mem[0x102] = 0xED; mem[0x103] = 0x44;
    mem[0x104] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x80);
    assert(cpu.f & FLAG_PV);

    /* NEG H=1: A=0x01 → 0xFF, half-carry from low nibble */
    setup();
    cpu.pc = 0x100;
    mem[0x100] = 0x3E; mem[0x101] = 0x01;
    mem[0x102] = 0xED; mem[0x103] = 0x44;
    mem[0x104] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0xFF);
    assert(cpu.f & FLAG_H);

    /* NEG H=0: A=0x10 → 0xF0, no half-carry */
    setup();
    cpu.pc = 0x100;
    mem[0x100] = 0x3E; mem[0x101] = 0x10;
    mem[0x102] = 0xED; mem[0x103] = 0x44;
    mem[0x104] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0xF0);
    assert(!(cpu.f & FLAG_H));

    printf("  PASS: ed_neg\n");
}

/* ── Test: ADC HL,rr / SBC HL,rr ────────────────────────────────────────── */

static void test_ed_adc_sbc_hl(void)
{
    /* SBC HL, BC: HL=0x1000 - BC=0x0100 - C=0 = 0x0F00 */
    setup();
    cpu.pc = 0x100;
    uint16_t pc = 0x100;
    mem[pc++] = 0x21; mem[pc++] = 0x00; mem[pc++] = 0x10;  /* LD HL, 0x1000 */
    mem[pc++] = 0x01; mem[pc++] = 0x00; mem[pc++] = 0x01;  /* LD BC, 0x0100 */
    mem[pc++] = 0xED; mem[pc++] = 0x42;  /* SBC HL, BC */
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(z80_hl(&cpu) == 0x0F00);
    assert(cpu.f & FLAG_N);
    assert(!(cpu.f & FLAG_C));
    assert(!(cpu.f & FLAG_Z));

    /* SBC with borrow: set carry first */
    setup();
    cpu.pc = 0x100;
    cpu.f = FLAG_C;
    pc = 0x100;
    mem[pc++] = 0x21; mem[pc++] = 0x00; mem[pc++] = 0x10;
    mem[pc++] = 0x01; mem[pc++] = 0x00; mem[pc++] = 0x01;
    mem[pc++] = 0xED; mem[pc++] = 0x42;
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(z80_hl(&cpu) == 0x0EFF);

    /* ADC HL, DE: HL=0x1000 + DE=0x2000 + C=0 = 0x3000 */
    setup();
    cpu.pc = 0x100;
    pc = 0x100;
    mem[pc++] = 0x21; mem[pc++] = 0x00; mem[pc++] = 0x10;
    mem[pc++] = 0x11; mem[pc++] = 0x00; mem[pc++] = 0x20;
    mem[pc++] = 0xED; mem[pc++] = 0x5A;  /* ADC HL, DE */
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(z80_hl(&cpu) == 0x3000);
    assert(!(cpu.f & FLAG_N));

    /* ADC with carry */
    setup();
    cpu.pc = 0x100;
    cpu.f = FLAG_C;
    pc = 0x100;
    mem[pc++] = 0x21; mem[pc++] = 0xFF; mem[pc++] = 0xFF;
    mem[pc++] = 0x11; mem[pc++] = 0x00; mem[pc++] = 0x00;
    mem[pc++] = 0xED; mem[pc++] = 0x5A;
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(z80_hl(&cpu) == 0x0000);
    assert(cpu.f & FLAG_C);
    assert(cpu.f & FLAG_Z);

    /* ADC HL overflow: 0x7FFF + 0x0001 = 0x8000, PV=1, S=1 */
    setup();
    cpu.pc = 0x100;
    pc = 0x100;
    mem[pc++] = 0x21; mem[pc++] = 0xFF; mem[pc++] = 0x7F;  /* LD HL, 0x7FFF */
    mem[pc++] = 0x01; mem[pc++] = 0x01; mem[pc++] = 0x00;  /* LD BC, 0x0001 */
    mem[pc++] = 0xED; mem[pc++] = 0x4A;  /* ADC HL, BC */
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(z80_hl(&cpu) == 0x8000);
    assert(cpu.f & FLAG_PV);
    assert(cpu.f & FLAG_S);

    /* ADC HL half-carry: 0x0FFF + 0x0001 = 0x1000, H=1 */
    setup();
    cpu.pc = 0x100;
    pc = 0x100;
    mem[pc++] = 0x21; mem[pc++] = 0xFF; mem[pc++] = 0x0F;
    mem[pc++] = 0x01; mem[pc++] = 0x01; mem[pc++] = 0x00;
    mem[pc++] = 0xED; mem[pc++] = 0x4A;
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(z80_hl(&cpu) == 0x1000);
    assert(cpu.f & FLAG_H);

    /* SBC HL overflow: 0x8000 - 0x0001 = 0x7FFF, PV=1 */
    setup();
    cpu.pc = 0x100;
    pc = 0x100;
    mem[pc++] = 0x21; mem[pc++] = 0x00; mem[pc++] = 0x80;  /* LD HL, 0x8000 */
    mem[pc++] = 0x01; mem[pc++] = 0x01; mem[pc++] = 0x00;  /* LD BC, 0x0001 */
    mem[pc++] = 0xED; mem[pc++] = 0x42;  /* SBC HL, BC */
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(z80_hl(&cpu) == 0x7FFF);
    assert(cpu.f & FLAG_PV);

    /* SBC HL half-carry: 0x1000 - 0x0001 = 0x0FFF, H=1 */
    setup();
    cpu.pc = 0x100;
    pc = 0x100;
    mem[pc++] = 0x21; mem[pc++] = 0x00; mem[pc++] = 0x10;
    mem[pc++] = 0x01; mem[pc++] = 0x01; mem[pc++] = 0x00;
    mem[pc++] = 0xED; mem[pc++] = 0x42;
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(z80_hl(&cpu) == 0x0FFF);
    assert(cpu.f & FLAG_H);

    printf("  PASS: ed_adc_sbc_hl\n");
}

/* ── Test: ED LD (nn),rr / LD rr,(nn) ───────────────────────────────────── */

static void test_ed_ld_rr_nn(void)
{
    /* ED 43 nn nn: LD (nn), BC */
    setup();
    cpu.pc = 0x100;
    uint16_t pc = 0x100;
    mem[pc++] = 0x01; mem[pc++] = 0x34; mem[pc++] = 0x12;  /* LD BC, 0x1234 */
    mem[pc++] = 0xED; mem[pc++] = 0x43;  /* LD (0x8000), BC */
    mem[pc++] = 0x00; mem[pc++] = 0x80;
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(mem[0x8000] == 0x34);
    assert(mem[0x8001] == 0x12);

    /* ED 4B nn nn: LD BC, (nn) */
    setup();
    cpu.pc = 0x100;
    mem[0x8000] = 0x78; mem[0x8001] = 0x56;
    pc = 0x100;
    mem[pc++] = 0xED; mem[pc++] = 0x4B;  /* LD BC, (0x8000) */
    mem[pc++] = 0x00; mem[pc++] = 0x80;
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(z80_bc(&cpu) == 0x5678);

    /* ED 73: LD (nn), SP */
    setup();
    cpu.pc = 0x100;
    cpu.sp = 0xABCD;
    pc = 0x100;
    mem[pc++] = 0xED; mem[pc++] = 0x73;  /* LD (0x8000), SP */
    mem[pc++] = 0x00; mem[pc++] = 0x80;
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(mem[0x8000] == 0xCD);
    assert(mem[0x8001] == 0xAB);

    printf("  PASS: ed_ld_rr_nn\n");
}

/* ── Test: LD I,A / LD R,A / LD A,I / LD A,R ────────────────────────────── */

static void test_ed_ld_i_r(void)
{
    /* LD I, A */
    setup();
    cpu.pc = 0x100;
    mem[0x100] = 0x3E; mem[0x101] = 0x42;
    mem[0x102] = 0xED; mem[0x103] = 0x47;  /* LD I, A */
    mem[0x104] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.i == 0x42);

    /* LD A, I — sets flags, PV=IFF2 */
    setup();
    cpu.pc = 0x100;
    cpu.i = 0x80;
    cpu.iff2 = 1;
    cpu.f = FLAG_C;  /* preserve carry */
    mem[0x100] = 0xED; mem[0x101] = 0x57;  /* LD A, I */
    mem[0x102] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x80);
    assert(cpu.f & FLAG_S);
    assert(cpu.f & FLAG_C);  /* preserved */
    assert(cpu.f & FLAG_PV); /* IFF2=1 */
    assert(!(cpu.f & FLAG_Z));

    /* LD R, A and LD A, R */
    setup();
    cpu.pc = 0x100;
    mem[0x100] = 0x3E; mem[0x101] = 0x55;
    mem[0x102] = 0xED; mem[0x103] = 0x4F;  /* LD R, A */
    mem[0x104] = 0xED; mem[0x105] = 0x5F;  /* LD A, R */
    mem[0x106] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    /* R was set to 0x55, then incremented by each instruction fetch.
     * After LD R,A (R=0x55), ED 5F fetches increment R twice more:
     * R = (0x55+2) & 0x7F | (0x55 & 0x80) = 0x57 | 0x00 = 0x57 */
    assert(cpu.a == 0x57);

    /* LD A,I with I=0, IFF2=0: Z=1, PV=0, carry preserved */
    setup();
    cpu.pc = 0x100;
    cpu.i = 0x00;
    cpu.iff2 = 0;
    cpu.f = FLAG_C;
    mem[0x100] = 0xED; mem[0x101] = 0x57;  /* LD A, I */
    mem[0x102] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x00);
    assert(cpu.f & FLAG_Z);
    assert(!(cpu.f & FLAG_PV));  /* IFF2=0 */
    assert(cpu.f & FLAG_C);     /* preserved */

    /* LD A,I with I=0x28: F3=1, F5=1 */
    setup();
    cpu.pc = 0x100;
    cpu.i = 0x28;
    cpu.iff2 = 1;
    mem[0x100] = 0xED; mem[0x101] = 0x57;
    mem[0x102] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x28);
    assert(cpu.f & FLAG_F3);
    assert(cpu.f & FLAG_F5);
    assert(cpu.f & FLAG_PV);  /* IFF2=1 */

    /* LD A,R with IFF2=1: PV=1 */
    setup();
    cpu.pc = 0x100;
    cpu.r = 0x40;
    cpu.iff2 = 1;
    mem[0x100] = 0xED; mem[0x101] = 0x5F;  /* LD A, R */
    mem[0x102] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.f & FLAG_PV);

    printf("  PASS: ed_ld_i_r\n");
}

/* ── Test: IM 0/1/2 ──────────────────────────────────────────────────────── */

static void test_ed_im(void)
{
    setup();
    cpu.pc = 0x100;
    mem[0x100] = 0xED; mem[0x101] = 0x56;  /* IM 1 */
    mem[0x102] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.im == 1);

    setup();
    cpu.pc = 0x100;
    mem[0x100] = 0xED; mem[0x101] = 0x5E;  /* IM 2 */
    mem[0x102] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.im == 2);

    setup();
    cpu.pc = 0x100;
    mem[0x100] = 0xED; mem[0x101] = 0x46;  /* IM 0 */
    mem[0x102] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.im == 0);

    printf("  PASS: ed_im\n");
}

/* ── Test: RLD / RRD ─────────────────────────────────────────────────────── */

static void test_ed_rld_rrd(void)
{
    /* RLD: A=0x12, (HL)=0x34 → A=0x13, (HL)=0x42 */
    setup();
    cpu.pc = 0x100;
    mem[0x8000] = 0x34;
    uint16_t pc = 0x100;
    mem[pc++] = 0x3E; mem[pc++] = 0x12;  /* LD A, 0x12 */
    mem[pc++] = 0x21; mem[pc++] = 0x00; mem[pc++] = 0x80;  /* LD HL, 0x8000 */
    mem[pc++] = 0xED; mem[pc++] = 0x6F;  /* RLD */
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x13);
    assert(mem[0x8000] == 0x42);

    /* RRD: A=0x12, (HL)=0x34 → A=0x14, (HL)=0x23 */
    setup();
    cpu.pc = 0x100;
    mem[0x8000] = 0x34;
    pc = 0x100;
    mem[pc++] = 0x3E; mem[pc++] = 0x12;
    mem[pc++] = 0x21; mem[pc++] = 0x00; mem[pc++] = 0x80;
    mem[pc++] = 0xED; mem[pc++] = 0x67;  /* RRD */
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x14);
    assert(mem[0x8000] == 0x23);

    /* RLD zero result: A=0x00, (HL)=0x00 → A=0x00, Z=1, PV=1 */
    setup();
    cpu.pc = 0x100;
    mem[0x8000] = 0x00;
    pc = 0x100;
    mem[pc++] = 0x3E; mem[pc++] = 0x00;
    mem[pc++] = 0x21; mem[pc++] = 0x00; mem[pc++] = 0x80;
    mem[pc++] = 0xED; mem[pc++] = 0x6F;
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x00);
    assert(cpu.f & FLAG_Z);
    assert(cpu.f & FLAG_PV);  /* parity of 0 = even */

    /* RLD sign: A=0x90, (HL)=0x80 → A=0x98, S=1 */
    setup();
    cpu.pc = 0x100;
    mem[0x8000] = 0x80;
    pc = 0x100;
    mem[pc++] = 0x3E; mem[pc++] = 0x90;
    mem[pc++] = 0x21; mem[pc++] = 0x00; mem[pc++] = 0x80;
    mem[pc++] = 0xED; mem[pc++] = 0x6F;
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x98);
    assert(cpu.f & FLAG_S);

    /* RLD preserves carry */
    setup();
    cpu.pc = 0x100;
    cpu.f = FLAG_C;
    mem[0x8000] = 0x12;
    pc = 0x100;
    mem[pc++] = 0x3E; mem[pc++] = 0x34;
    mem[pc++] = 0x21; mem[pc++] = 0x00; mem[pc++] = 0x80;
    mem[pc++] = 0xED; mem[pc++] = 0x6F;
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.f & FLAG_C);

    /* RRD zero result: A=0x00, (HL)=0x00 → A=0x00, Z=1, PV=1 */
    setup();
    cpu.pc = 0x100;
    mem[0x8000] = 0x00;
    pc = 0x100;
    mem[pc++] = 0x3E; mem[pc++] = 0x00;
    mem[pc++] = 0x21; mem[pc++] = 0x00; mem[pc++] = 0x80;
    mem[pc++] = 0xED; mem[pc++] = 0x67;
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x00);
    assert(cpu.f & FLAG_Z);
    assert(cpu.f & FLAG_PV);

    printf("  PASS: ed_rld_rrd\n");
}

/* ── Test: LDI / LDD ────────────────────────────────────────────────────── */

static void test_ed_ldi_ldd(void)
{
    /* LDI: copy one byte from (HL) to (DE), HL++, DE++, BC-- */
    setup();
    cpu.pc = 0x100;
    mem[0x8000] = 0xAA;
    uint16_t pc = 0x100;
    mem[pc++] = 0x21; mem[pc++] = 0x00; mem[pc++] = 0x80;  /* LD HL, 0x8000 */
    mem[pc++] = 0x11; mem[pc++] = 0x00; mem[pc++] = 0x90;  /* LD DE, 0x9000 */
    mem[pc++] = 0x01; mem[pc++] = 0x03; mem[pc++] = 0x00;  /* LD BC, 3 */
    mem[pc++] = 0xED; mem[pc++] = 0xA0;  /* LDI */
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(mem[0x9000] == 0xAA);
    assert(z80_hl(&cpu) == 0x8001);
    assert(z80_de(&cpu) == 0x9001);
    assert(z80_bc(&cpu) == 0x0002);
    assert(cpu.f & FLAG_PV);  /* BC != 0 */

    /* LDD: copy one byte, HL--, DE--, BC-- */
    setup();
    cpu.pc = 0x100;
    mem[0x8003] = 0xBB;
    pc = 0x100;
    mem[pc++] = 0x21; mem[pc++] = 0x03; mem[pc++] = 0x80;  /* LD HL, 0x8003 */
    mem[pc++] = 0x11; mem[pc++] = 0x03; mem[pc++] = 0x90;  /* LD DE, 0x9003 */
    mem[pc++] = 0x01; mem[pc++] = 0x01; mem[pc++] = 0x00;  /* LD BC, 1 */
    mem[pc++] = 0xED; mem[pc++] = 0xA8;  /* LDD */
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(mem[0x9003] == 0xBB);
    assert(z80_hl(&cpu) == 0x8002);
    assert(z80_de(&cpu) == 0x9002);
    assert(z80_bc(&cpu) == 0x0000);
    assert(!(cpu.f & FLAG_PV));  /* BC == 0 */

    printf("  PASS: ed_ldi_ldd\n");
}

/* ── Test: LDIR (block copy) ─────────────────────────────────────────────── */

static void test_ed_ldir(void)
{
    setup();
    cpu.pc = 0x100;
    /* Source data */
    mem[0x8000] = 0x11; mem[0x8001] = 0x22;
    mem[0x8002] = 0x33; mem[0x8003] = 0x44;

    uint16_t pc = 0x100;
    mem[pc++] = 0x21; mem[pc++] = 0x00; mem[pc++] = 0x80;  /* LD HL, 0x8000 */
    mem[pc++] = 0x11; mem[pc++] = 0x00; mem[pc++] = 0x90;  /* LD DE, 0x9000 */
    mem[pc++] = 0x01; mem[pc++] = 0x04; mem[pc++] = 0x00;  /* LD BC, 4 */
    mem[pc++] = 0xED; mem[pc++] = 0xB0;  /* LDIR */
    mem[pc++] = 0x76;

    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(mem[0x9000] == 0x11);
    assert(mem[0x9001] == 0x22);
    assert(mem[0x9002] == 0x33);
    assert(mem[0x9003] == 0x44);
    assert(z80_bc(&cpu) == 0x0000);
    assert(z80_hl(&cpu) == 0x8004);
    assert(z80_de(&cpu) == 0x9004);
    assert(!(cpu.f & FLAG_PV));  /* BC == 0 at end */

    printf("  PASS: ed_ldir\n");
}

/* ── Test: CPI / CPD ─────────────────────────────────────────────────────── */

static void test_ed_cpi_cpd(void)
{
    /* CPI: compare A with (HL), HL++, BC-- */
    setup();
    cpu.pc = 0x100;
    mem[0x8000] = 0x42;
    uint16_t pc = 0x100;
    mem[pc++] = 0x3E; mem[pc++] = 0x42;  /* LD A, 0x42 */
    mem[pc++] = 0x21; mem[pc++] = 0x00; mem[pc++] = 0x80;
    mem[pc++] = 0x01; mem[pc++] = 0x02; mem[pc++] = 0x00;  /* LD BC, 2 */
    mem[pc++] = 0xED; mem[pc++] = 0xA1;  /* CPI */
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.f & FLAG_Z);    /* A == (HL) */
    assert(cpu.f & FLAG_N);
    assert(z80_hl(&cpu) == 0x8001);
    assert(z80_bc(&cpu) == 0x0001);
    assert(cpu.f & FLAG_PV);  /* BC != 0 */

    /* CPI: no match */
    setup();
    cpu.pc = 0x100;
    mem[0x8000] = 0x99;
    pc = 0x100;
    mem[pc++] = 0x3E; mem[pc++] = 0x42;
    mem[pc++] = 0x21; mem[pc++] = 0x00; mem[pc++] = 0x80;
    mem[pc++] = 0x01; mem[pc++] = 0x01; mem[pc++] = 0x00;
    mem[pc++] = 0xED; mem[pc++] = 0xA1;
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(!(cpu.f & FLAG_Z));
    assert(!(cpu.f & FLAG_PV));  /* BC == 0 */

    printf("  PASS: ed_cpi_cpd\n");
}

/* ── Test: CPIR (search) ─────────────────────────────────────────────────── */

static void test_ed_cpir(void)
{
    setup();
    cpu.pc = 0x100;
    /* Search for 0x33 in array */
    mem[0x8000] = 0x11; mem[0x8001] = 0x22;
    mem[0x8002] = 0x33; mem[0x8003] = 0x44;

    uint16_t pc = 0x100;
    mem[pc++] = 0x3E; mem[pc++] = 0x33;  /* LD A, 0x33 */
    mem[pc++] = 0x21; mem[pc++] = 0x00; mem[pc++] = 0x80;
    mem[pc++] = 0x01; mem[pc++] = 0x04; mem[pc++] = 0x00;  /* LD BC, 4 */
    mem[pc++] = 0xED; mem[pc++] = 0xB1;  /* CPIR */
    mem[pc++] = 0x76;

    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.f & FLAG_Z);             /* found match */
    assert(z80_hl(&cpu) == 0x8003);     /* HL past the match */
    assert(z80_bc(&cpu) == 0x0001);     /* BC decremented 3 times */

    /* CPIR: not found */
    setup();
    cpu.pc = 0x100;
    mem[0x8000] = 0x11; mem[0x8001] = 0x22;
    pc = 0x100;
    mem[pc++] = 0x3E; mem[pc++] = 0xFF;
    mem[pc++] = 0x21; mem[pc++] = 0x00; mem[pc++] = 0x80;
    mem[pc++] = 0x01; mem[pc++] = 0x02; mem[pc++] = 0x00;
    mem[pc++] = 0xED; mem[pc++] = 0xB1;
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(!(cpu.f & FLAG_Z));
    assert(z80_bc(&cpu) == 0x0000);

    printf("  PASS: ed_cpir\n");
}

/* ── Test: RETN / RETI ───────────────────────────────────────────────────── */

static void test_ed_retn_reti(void)
{
    /* RETN: pops PC, copies IFF2 → IFF1 */
    setup();
    cpu.pc = 0x200;
    cpu.sp = 0xFFFE;
    cpu.iff1 = 0;
    cpu.iff2 = 1;
    z80_push16(&cpu, 0x0105);
    mem[0x200] = 0xED; mem[0x201] = 0x45;  /* RETN */
    mem[0x105] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.pc == 0x0106);  /* past HALT */
    assert(cpu.iff1 == 1);     /* copied from IFF2 */

    /* RETI: same behavior */
    setup();
    cpu.pc = 0x200;
    cpu.sp = 0xFFFE;
    cpu.iff1 = 0;
    cpu.iff2 = 1;
    z80_push16(&cpu, 0x0105);
    mem[0x200] = 0xED; mem[0x201] = 0x4D;  /* RETI */
    mem[0x105] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.pc == 0x0106);
    assert(cpu.iff1 == 1);

    printf("  PASS: ed_retn_reti\n");
}

/* ── Test: IN r,(C) — ED register I/O ────────────────────────────────────── */

static void test_ed_in_r_c(void)
{
    /* IN A,(C): BC=0x0310, io_in_data=0x42 → A=0x42, flags from result */
    setup();
    cpu.pc = 0x100;
    cpu.f = FLAG_C;  /* carry should be preserved */
    io_in_count = 0;
    io_in_data = 0x42;
    ecpu_z80_ops.set_trap_handler((cpu_state_t *)&cpu, io_trap_handler, NULL);
    uint16_t pc = 0x100;
    mem[pc++] = 0x01; mem[pc++] = 0x10; mem[pc++] = 0x03;  /* LD BC, 0x0310 */
    mem[pc++] = 0xED; mem[pc++] = 0x78;  /* IN A,(C) */
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x42);
    assert(io_in_count == 1);
    assert(io_in_addr == 0x0310);
    assert(!(cpu.f & FLAG_Z));
    assert(!(cpu.f & FLAG_S));
    assert(!(cpu.f & FLAG_N));
    assert(cpu.f & FLAG_C);  /* preserved */
    assert(cpu.wz == 0x0311);

    /* IN A,(C): zero → Z=1, PV=1 (even parity of 0) */
    setup();
    cpu.pc = 0x100;
    io_in_count = 0;
    io_in_data = 0x00;
    ecpu_z80_ops.set_trap_handler((cpu_state_t *)&cpu, io_trap_handler, NULL);
    pc = 0x100;
    mem[pc++] = 0x01; mem[pc++] = 0x10; mem[pc++] = 0x03;
    mem[pc++] = 0xED; mem[pc++] = 0x78;
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x00);
    assert(cpu.f & FLAG_Z);
    assert(cpu.f & FLAG_PV);

    /* IN A,(C): 0x80 → S=1 */
    setup();
    cpu.pc = 0x100;
    io_in_count = 0;
    io_in_data = 0x80;
    ecpu_z80_ops.set_trap_handler((cpu_state_t *)&cpu, io_trap_handler, NULL);
    pc = 0x100;
    mem[pc++] = 0x01; mem[pc++] = 0x10; mem[pc++] = 0x03;
    mem[pc++] = 0xED; mem[pc++] = 0x78;
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x80);
    assert(cpu.f & FLAG_S);

    /* IN F,(C) (ED 70): flags-only, handler writes A as side-effect */
    setup();
    cpu.pc = 0x100;
    cpu.f = FLAG_C;
    io_in_count = 0;
    io_in_data = 0x28;  /* bits 3,5 set */
    ecpu_z80_ops.set_trap_handler((cpu_state_t *)&cpu, io_trap_handler, NULL);
    pc = 0x100;
    mem[pc++] = 0x01; mem[pc++] = 0x10; mem[pc++] = 0x03;
    mem[pc++] = 0xED; mem[pc++] = 0x70;  /* IN F,(C) */
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(io_in_count == 1);
    assert(cpu.f & FLAG_F3);
    assert(cpu.f & FLAG_F5);
    assert(cpu.f & FLAG_C);  /* preserved */

    printf("  PASS: ed_in_r_c\n");
}

/* ── Test: OUT (C),r — ED register output ────────────────────────────────── */

static void test_ed_out_c_r(void)
{
    /* OUT (C),A (ED 79): A=0x42, BC=0x0310 */
    setup();
    cpu.pc = 0x100;
    io_out_count = 0;
    ecpu_z80_ops.set_trap_handler((cpu_state_t *)&cpu, io_trap_handler, NULL);
    uint16_t pc = 0x100;
    mem[pc++] = 0x3E; mem[pc++] = 0x42;  /* LD A, 0x42 */
    mem[pc++] = 0x01; mem[pc++] = 0x10; mem[pc++] = 0x03;  /* LD BC, 0x0310 */
    mem[pc++] = 0xED; mem[pc++] = 0x79;  /* OUT (C),A */
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(io_out_count == 1);
    assert(io_out_addr == 0x0310);
    assert(cpu.wz == 0x0311);

    /* OUT (C),0 (ED 71): undocumented — outputs 0 */
    setup();
    cpu.pc = 0x100;
    io_out_count = 0;
    ecpu_z80_ops.set_trap_handler((cpu_state_t *)&cpu, io_trap_handler, NULL);
    pc = 0x100;
    mem[pc++] = 0x01; mem[pc++] = 0x10; mem[pc++] = 0x03;  /* LD BC, 0x0310 */
    mem[pc++] = 0xED; mem[pc++] = 0x71;  /* OUT (C),0 */
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(io_out_count == 1);
    assert(io_out_addr == 0x0310);

    printf("  PASS: ed_out_c_r\n");
}

/* ── Test: Block I/O — INI/IND/INIR/INDR/OUTI/OUTD/OTIR/OTDR ───────────── */

static void test_ed_block_io(void)
{
    /* INI (ED A2): B=3, C=0x10, HL=0x9000 → B=2, HL=0x9001 */
    setup();
    cpu.pc = 0x100;
    io_in_count = 0;
    ecpu_z80_ops.set_trap_handler((cpu_state_t *)&cpu, io_trap_handler, NULL);
    uint16_t pc = 0x100;
    mem[pc++] = 0x06; mem[pc++] = 0x03;  /* LD B, 3 */
    mem[pc++] = 0x0E; mem[pc++] = 0x10;  /* LD C, 0x10 */
    mem[pc++] = 0x21; mem[pc++] = 0x00; mem[pc++] = 0x90;  /* LD HL, 0x9000 */
    mem[pc++] = 0xED; mem[pc++] = 0xA2;  /* INI */
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.b == 2);
    assert(z80_hl(&cpu) == 0x9001);
    assert(io_in_count == 1);

    /* IND (ED AA): B=3, C=0x10, HL=0x9003 → B=2, HL=0x9002 */
    setup();
    cpu.pc = 0x100;
    io_in_count = 0;
    ecpu_z80_ops.set_trap_handler((cpu_state_t *)&cpu, io_trap_handler, NULL);
    pc = 0x100;
    mem[pc++] = 0x06; mem[pc++] = 0x03;
    mem[pc++] = 0x0E; mem[pc++] = 0x10;
    mem[pc++] = 0x21; mem[pc++] = 0x03; mem[pc++] = 0x90;
    mem[pc++] = 0xED; mem[pc++] = 0xAA;  /* IND */
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.b == 2);
    assert(z80_hl(&cpu) == 0x9002);
    assert(io_in_count == 1);

    /* INIR (ED B2): B=3 → repeats until B=0 */
    setup();
    cpu.pc = 0x100;
    io_in_count = 0;
    ecpu_z80_ops.set_trap_handler((cpu_state_t *)&cpu, io_trap_handler, NULL);
    pc = 0x100;
    mem[pc++] = 0x06; mem[pc++] = 0x03;
    mem[pc++] = 0x0E; mem[pc++] = 0x10;
    mem[pc++] = 0x21; mem[pc++] = 0x00; mem[pc++] = 0x90;
    mem[pc++] = 0xED; mem[pc++] = 0xB2;  /* INIR */
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.b == 0);
    assert(z80_hl(&cpu) == 0x9003);
    assert(io_in_count == 3);
    assert(cpu.f & FLAG_Z);  /* B=0 from dec8 */

    /* INDR (ED BA): B=2 → repeats until B=0 */
    setup();
    cpu.pc = 0x100;
    io_in_count = 0;
    ecpu_z80_ops.set_trap_handler((cpu_state_t *)&cpu, io_trap_handler, NULL);
    pc = 0x100;
    mem[pc++] = 0x06; mem[pc++] = 0x02;
    mem[pc++] = 0x0E; mem[pc++] = 0x10;
    mem[pc++] = 0x21; mem[pc++] = 0x02; mem[pc++] = 0x90;
    mem[pc++] = 0xED; mem[pc++] = 0xBA;  /* INDR */
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.b == 0);
    assert(z80_hl(&cpu) == 0x9000);
    assert(io_in_count == 2);

    /* OUTI (ED A3): B=3, C=0x10, HL=0x8000 → B=2, HL=0x8001 */
    setup();
    cpu.pc = 0x100;
    io_out_count = 0;
    ecpu_z80_ops.set_trap_handler((cpu_state_t *)&cpu, io_trap_handler, NULL);
    mem[0x8000] = 0xAA;
    pc = 0x100;
    mem[pc++] = 0x06; mem[pc++] = 0x03;
    mem[pc++] = 0x0E; mem[pc++] = 0x10;
    mem[pc++] = 0x21; mem[pc++] = 0x00; mem[pc++] = 0x80;
    mem[pc++] = 0xED; mem[pc++] = 0xA3;  /* OUTI */
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.b == 2);
    assert(z80_hl(&cpu) == 0x8001);
    assert(io_out_count == 1);

    /* OUTD (ED AB): B=3, C=0x10, HL=0x8003 → B=2, HL=0x8002 */
    setup();
    cpu.pc = 0x100;
    io_out_count = 0;
    ecpu_z80_ops.set_trap_handler((cpu_state_t *)&cpu, io_trap_handler, NULL);
    mem[0x8003] = 0xBB;
    pc = 0x100;
    mem[pc++] = 0x06; mem[pc++] = 0x03;
    mem[pc++] = 0x0E; mem[pc++] = 0x10;
    mem[pc++] = 0x21; mem[pc++] = 0x03; mem[pc++] = 0x80;
    mem[pc++] = 0xED; mem[pc++] = 0xAB;  /* OUTD */
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.b == 2);
    assert(z80_hl(&cpu) == 0x8002);
    assert(io_out_count == 1);

    /* OTIR (ED B3): B=3 → repeats until B=0 */
    setup();
    cpu.pc = 0x100;
    io_out_count = 0;
    ecpu_z80_ops.set_trap_handler((cpu_state_t *)&cpu, io_trap_handler, NULL);
    mem[0x8000] = 0x11; mem[0x8001] = 0x22; mem[0x8002] = 0x33;
    pc = 0x100;
    mem[pc++] = 0x06; mem[pc++] = 0x03;
    mem[pc++] = 0x0E; mem[pc++] = 0x10;
    mem[pc++] = 0x21; mem[pc++] = 0x00; mem[pc++] = 0x80;
    mem[pc++] = 0xED; mem[pc++] = 0xB3;  /* OTIR */
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.b == 0);
    assert(z80_hl(&cpu) == 0x8003);
    assert(io_out_count == 3);

    /* OTDR (ED BB): B=3, HL=0x8003 → repeats backward until B=0 */
    setup();
    cpu.pc = 0x100;
    io_out_count = 0;
    ecpu_z80_ops.set_trap_handler((cpu_state_t *)&cpu, io_trap_handler, NULL);
    mem[0x8001] = 0x44; mem[0x8002] = 0x55; mem[0x8003] = 0x66;
    pc = 0x100;
    mem[pc++] = 0x06; mem[pc++] = 0x03;
    mem[pc++] = 0x0E; mem[pc++] = 0x10;
    mem[pc++] = 0x21; mem[pc++] = 0x03; mem[pc++] = 0x80;
    mem[pc++] = 0xED; mem[pc++] = 0xBB;  /* OTDR */
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.b == 0);
    assert(z80_hl(&cpu) == 0x8000);
    assert(io_out_count == 3);

    printf("  PASS: ed_block_io\n");
}

/* ── Test: LDDR (reverse block copy with repeat) ─────────────────────────── */

static void test_ed_lddr(void)
{
    setup();
    cpu.pc = 0x100;
    /* Source data at 0x8000..0x8003 */
    mem[0x8000] = 0x11; mem[0x8001] = 0x22;
    mem[0x8002] = 0x33; mem[0x8003] = 0x44;

    uint16_t pc = 0x100;
    mem[pc++] = 0x21; mem[pc++] = 0x03; mem[pc++] = 0x80;  /* LD HL, 0x8003 */
    mem[pc++] = 0x11; mem[pc++] = 0x03; mem[pc++] = 0x90;  /* LD DE, 0x9003 */
    mem[pc++] = 0x01; mem[pc++] = 0x04; mem[pc++] = 0x00;  /* LD BC, 4 */
    mem[pc++] = 0xED; mem[pc++] = 0xB8;  /* LDDR */
    mem[pc++] = 0x76;

    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(mem[0x9000] == 0x11);
    assert(mem[0x9001] == 0x22);
    assert(mem[0x9002] == 0x33);
    assert(mem[0x9003] == 0x44);
    assert(z80_bc(&cpu) == 0x0000);
    assert(z80_hl(&cpu) == 0x7FFF);
    assert(z80_de(&cpu) == 0x8FFF);
    assert(!(cpu.f & FLAG_PV));  /* BC == 0 at end */

    printf("  PASS: ed_lddr\n");
}

/* ── Test: CPDR (reverse block search with repeat) ───────────────────────── */

static void test_ed_cpdr(void)
{
    /* CPDR found: search backward for 0x33 */
    setup();
    cpu.pc = 0x100;
    mem[0x8000] = 0x11; mem[0x8001] = 0x22;
    mem[0x8002] = 0x33; mem[0x8003] = 0x44;

    uint16_t pc = 0x100;
    mem[pc++] = 0x3E; mem[pc++] = 0x33;  /* LD A, 0x33 */
    mem[pc++] = 0x21; mem[pc++] = 0x03; mem[pc++] = 0x80;  /* LD HL, 0x8003 */
    mem[pc++] = 0x01; mem[pc++] = 0x04; mem[pc++] = 0x00;  /* LD BC, 4 */
    mem[pc++] = 0xED; mem[pc++] = 0xB9;  /* CPDR */
    mem[pc++] = 0x76;

    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.f & FLAG_Z);             /* found match */
    assert(z80_hl(&cpu) == 0x8001);     /* HL past the match (backward) */
    assert(z80_bc(&cpu) == 0x0002);     /* BC decremented 2 times */
    assert(cpu.f & FLAG_PV);            /* BC != 0 */

    /* CPDR not found */
    setup();
    cpu.pc = 0x100;
    mem[0x8000] = 0x11; mem[0x8001] = 0x22;
    mem[0x8002] = 0x33; mem[0x8003] = 0x44;
    pc = 0x100;
    mem[pc++] = 0x3E; mem[pc++] = 0xFF;
    mem[pc++] = 0x21; mem[pc++] = 0x03; mem[pc++] = 0x80;
    mem[pc++] = 0x01; mem[pc++] = 0x04; mem[pc++] = 0x00;
    mem[pc++] = 0xED; mem[pc++] = 0xB9;
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(!(cpu.f & FLAG_Z));
    assert(z80_bc(&cpu) == 0x0000);
    assert(!(cpu.f & FLAG_PV));

    printf("  PASS: ed_cpdr\n");
}

/* ── Test: DD LD IX,nn ───────────────────────────────────────────────────── */

static void test_dd_ld_ix_nn(void)
{
    setup();
    cpu.pc = 0x100;
    uint16_t pc = 0x100;
    mem[pc++] = 0xDD; mem[pc++] = 0x21; mem[pc++] = 0x34; mem[pc++] = 0x12; /* LD IX,0x1234 */
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.ix == 0x1234);

    printf("  PASS: dd_ld_ix_nn\n");
}

/* ── Test: LD r,(IX+d) / LD (IX+d),r / LD (IX+d),n ─────────────────────── */

static void test_dd_ld_r_ixd(void)
{
    setup();
    cpu.pc = 0x100;
    cpu.ix = 0x8000;
    mem[0x8005] = 0x42;
    uint16_t pc = 0x100;
    /* LD A,(IX+5) = DD 7E 05 */
    mem[pc++] = 0xDD; mem[pc++] = 0x7E; mem[pc++] = 0x05;
    /* LD B,(IX-1) = DD 46 FF */
    mem[0x7FFF] = 0x99;
    mem[pc++] = 0xDD; mem[pc++] = 0x46; mem[pc++] = 0xFF;
    /* LD (IX+3),0xAB = DD 36 03 AB */
    mem[pc++] = 0xDD; mem[pc++] = 0x36; mem[pc++] = 0x03; mem[pc++] = 0xAB;
    /* LD (IX+0),C = DD 71 00 (set C first) */
    mem[pc++] = 0x0E; mem[pc++] = 0x77;  /* LD C, 0x77 */
    mem[pc++] = 0xDD; mem[pc++] = 0x71; mem[pc++] = 0x00;
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x42);
    assert(cpu.b == 0x99);
    assert(mem[0x8003] == 0xAB);
    assert(mem[0x8000] == 0x77);

    /* d=-128 (0x80): IX=0x8080, data at 0x8000 → A reads from 0x8000 */
    setup();
    cpu.pc = 0x100;
    cpu.ix = 0x8080;
    mem[0x8000] = 0xBE;
    mem[0x100] = 0xDD; mem[0x101] = 0x7E; mem[0x102] = 0x80;  /* LD A,(IX-128) */
    mem[0x103] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0xBE);

    printf("  PASS: dd_ld_r_ixd\n");
}

/* ── Test: ALU A,(IX+d) ─────────────────────────────────────────────────── */

static void test_dd_alu_ixd(void)
{
    setup();
    cpu.pc = 0x100;
    cpu.ix = 0x8000;
    mem[0x8002] = 0x10;
    uint16_t pc = 0x100;
    mem[pc++] = 0x3E; mem[pc++] = 0x20;  /* LD A, 0x20 */
    /* ADD A,(IX+2) = DD 86 02 */
    mem[pc++] = 0xDD; mem[pc++] = 0x86; mem[pc++] = 0x02;
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x30);

    /* SUB (IX+d) */
    setup();
    cpu.pc = 0x100;
    cpu.ix = 0x8000;
    mem[0x8001] = 0x05;
    pc = 0x100;
    mem[pc++] = 0x3E; mem[pc++] = 0x20;
    /* SUB (IX+1) = DD 96 01 */
    mem[pc++] = 0xDD; mem[pc++] = 0x96; mem[pc++] = 0x01;
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x1B);

    /* CP (IX+d) */
    setup();
    cpu.pc = 0x100;
    cpu.ix = 0x8000;
    mem[0x8000] = 0x42;
    pc = 0x100;
    mem[pc++] = 0x3E; mem[pc++] = 0x42;
    /* CP (IX+0) = DD BE 00 */
    mem[pc++] = 0xDD; mem[pc++] = 0xBE; mem[pc++] = 0x00;
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.f & FLAG_Z);

    /* ADD overflow: A=0x7F + (IX+1)=0x01 → 0x80, PV=1, S=1 */
    setup();
    cpu.pc = 0x100;
    cpu.ix = 0x8000;
    mem[0x8001] = 0x01;
    mem[0x100] = 0x3E; mem[0x101] = 0x7F;  /* LD A, 0x7F */
    mem[0x102] = 0xDD; mem[0x103] = 0x86; mem[0x104] = 0x01;  /* ADD A,(IX+1) */
    mem[0x105] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x80);
    assert(cpu.f & FLAG_PV);
    assert(cpu.f & FLAG_S);

    /* ADD carry+zero: A=0xFF + (IX+1)=0x01 → 0x00, C=1, Z=1 */
    setup();
    cpu.pc = 0x100;
    cpu.ix = 0x8000;
    mem[0x8001] = 0x01;
    mem[0x100] = 0x3E; mem[0x101] = 0xFF;  /* LD A, 0xFF */
    mem[0x102] = 0xDD; mem[0x103] = 0x86; mem[0x104] = 0x01;  /* ADD A,(IX+1) */
    mem[0x105] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x00);
    assert(cpu.f & FLAG_C);
    assert(cpu.f & FLAG_Z);

    /* SUB half-carry: A=0x10 - (IX+1)=0x09 → 0x07, H=1 */
    setup();
    cpu.pc = 0x100;
    cpu.ix = 0x8000;
    mem[0x8001] = 0x09;
    mem[0x100] = 0x3E; mem[0x101] = 0x10;  /* LD A, 0x10 */
    mem[0x102] = 0xDD; mem[0x103] = 0x96; mem[0x104] = 0x01;  /* SUB (IX+1) */
    mem[0x105] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x07);
    assert(cpu.f & FLAG_H);

    printf("  PASS: dd_alu_ixd\n");
}

/* ── Test: INC/DEC (IX+d) ───────────────────────────────────────────────── */

static void test_dd_inc_dec_ixd(void)
{
    setup();
    cpu.pc = 0x100;
    cpu.ix = 0x8000;
    mem[0x8003] = 0x0F;
    uint16_t pc = 0x100;
    /* INC (IX+3) = DD 34 03 */
    mem[pc++] = 0xDD; mem[pc++] = 0x34; mem[pc++] = 0x03;
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(mem[0x8003] == 0x10);
    assert(cpu.f & FLAG_H);  /* half carry from 0x0F+1 */

    /* DEC (IX+3) */
    setup();
    cpu.pc = 0x100;
    cpu.ix = 0x8000;
    mem[0x8003] = 0x01;
    pc = 0x100;
    /* DEC (IX+3) = DD 35 03 */
    mem[pc++] = 0xDD; mem[pc++] = 0x35; mem[pc++] = 0x03;
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(mem[0x8003] == 0x00);
    assert(cpu.f & FLAG_Z);

    /* INC overflow: (IX+2)=0x7F → 0x80, PV=1, S=1 */
    setup();
    cpu.pc = 0x100;
    cpu.ix = 0x8000;
    mem[0x8002] = 0x7F;
    mem[0x100] = 0xDD; mem[0x101] = 0x34; mem[0x102] = 0x02;  /* INC (IX+2) */
    mem[0x103] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(mem[0x8002] == 0x80);
    assert(cpu.f & FLAG_PV);
    assert(cpu.f & FLAG_S);

    /* DEC overflow: (IX+2)=0x80 → 0x7F, PV=1 */
    setup();
    cpu.pc = 0x100;
    cpu.ix = 0x8000;
    mem[0x8002] = 0x80;
    mem[0x100] = 0xDD; mem[0x101] = 0x35; mem[0x102] = 0x02;  /* DEC (IX+2) */
    mem[0x103] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(mem[0x8002] == 0x7F);
    assert(cpu.f & FLAG_PV);

    printf("  PASS: dd_inc_dec_ixd\n");
}

/* ── Test: PUSH IX / POP IX ─────────────────────────────────────────────── */

static void test_dd_push_pop(void)
{
    setup();
    cpu.pc = 0x100;
    cpu.sp = 0xFFFE;
    cpu.ix = 0xABCD;
    uint16_t pc = 0x100;
    mem[pc++] = 0xDD; mem[pc++] = 0xE5;  /* PUSH IX */
    mem[pc++] = 0xDD; mem[pc++] = 0x21;  /* LD IX,0 */
    mem[pc++] = 0x00; mem[pc++] = 0x00;
    mem[pc++] = 0xDD; mem[pc++] = 0xE1;  /* POP IX */
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.ix == 0xABCD);
    assert(cpu.sp == 0xFFFE);

    printf("  PASS: dd_push_pop\n");
}

/* ── Test: EX (SP),IX ───────────────────────────────────────────────────── */

static void test_dd_ex_sp_ix(void)
{
    setup();
    cpu.pc = 0x100;
    cpu.sp = 0xFFF0;
    cpu.ix = 0x1234;
    z80_write16(&cpu, 0xFFF0, 0x5678);
    uint16_t pc = 0x100;
    mem[pc++] = 0xDD; mem[pc++] = 0xE3;  /* EX (SP),IX */
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.ix == 0x5678);
    assert(z80_read16(&cpu, 0xFFF0) == 0x1234);

    printf("  PASS: dd_ex_sp_ix\n");
}

/* ── Test: ADD IX,rr ────────────────────────────────────────────────────── */

static void test_dd_add_ix_rr(void)
{
    setup();
    cpu.pc = 0x100;
    cpu.ix = 0x1000;
    uint16_t pc = 0x100;
    mem[pc++] = 0x01; mem[pc++] = 0x00; mem[pc++] = 0x02;  /* LD BC,0x0200 */
    mem[pc++] = 0xDD; mem[pc++] = 0x09;  /* ADD IX,BC */
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.ix == 0x1200);
    assert(!(cpu.f & FLAG_C));
    assert(!(cpu.f & FLAG_N));

    /* ADD IX,IX (doubles) */
    setup();
    cpu.pc = 0x100;
    cpu.ix = 0x4000;
    pc = 0x100;
    mem[pc++] = 0xDD; mem[pc++] = 0x29;  /* ADD IX,IX */
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.ix == 0x8000);

    /* ADD IX,SP with carry */
    setup();
    cpu.pc = 0x100;
    cpu.ix = 0xF000;
    cpu.sp = 0x2000;
    pc = 0x100;
    mem[pc++] = 0xDD; mem[pc++] = 0x39;  /* ADD IX,SP */
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.ix == 0x1000);
    assert(cpu.f & FLAG_C);

    printf("  PASS: dd_add_ix_rr\n");
}

/* ── Test: LD (nn),IX / LD IX,(nn) ──────────────────────────────────────── */

static void test_dd_ld_nn_ix(void)
{
    setup();
    cpu.pc = 0x100;
    cpu.ix = 0xBEEF;
    uint16_t pc = 0x100;
    /* LD (0x9000),IX = DD 22 00 90 */
    mem[pc++] = 0xDD; mem[pc++] = 0x22; mem[pc++] = 0x00; mem[pc++] = 0x90;
    /* LD IX,0 to clear, then reload */
    mem[pc++] = 0xDD; mem[pc++] = 0x21; mem[pc++] = 0x00; mem[pc++] = 0x00;
    /* LD IX,(0x9000) = DD 2A 00 90 */
    mem[pc++] = 0xDD; mem[pc++] = 0x2A; mem[pc++] = 0x00; mem[pc++] = 0x90;
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.ix == 0xBEEF);
    assert(z80_read16(&cpu, 0x9000) == 0xBEEF);

    printf("  PASS: dd_ld_nn_ix\n");
}

/* ── Test: DD CB (indexed bit operations) ────────────────────────────────── */

static void test_dd_cb_prefix(void)
{
    setup();
    cpu.pc = 0x100;
    cpu.ix = 0x8000;
    mem[0x8005] = 0x80;  /* bit 7 set */
    uint16_t pc = 0x100;
    /* BIT 7,(IX+5) = DD CB 05 7E */
    mem[pc++] = 0xDD; mem[pc++] = 0xCB; mem[pc++] = 0x05; mem[pc++] = 0x7E;
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(!(cpu.f & FLAG_Z));  /* bit 7 is set */

    /* BIT 0,(IX+5) — should be zero */
    setup();
    cpu.pc = 0x100;
    cpu.ix = 0x8000;
    mem[0x8005] = 0x80;
    pc = 0x100;
    mem[pc++] = 0xDD; mem[pc++] = 0xCB; mem[pc++] = 0x05; mem[pc++] = 0x46;
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.f & FLAG_Z);

    /* SET 0,(IX+5) = DD CB 05 C6 */
    setup();
    cpu.pc = 0x100;
    cpu.ix = 0x8000;
    mem[0x8005] = 0x00;
    pc = 0x100;
    mem[pc++] = 0xDD; mem[pc++] = 0xCB; mem[pc++] = 0x05; mem[pc++] = 0xC6;
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(mem[0x8005] == 0x01);

    /* RES 7,(IX+5) = DD CB 05 BE */
    setup();
    cpu.pc = 0x100;
    cpu.ix = 0x8000;
    mem[0x8005] = 0xFF;
    pc = 0x100;
    mem[pc++] = 0xDD; mem[pc++] = 0xCB; mem[pc++] = 0x05; mem[pc++] = 0xBE;
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(mem[0x8005] == 0x7F);

    /* RLC (IX+2) with store to B (undocumented): DD CB 02 00 */
    setup();
    cpu.pc = 0x100;
    cpu.ix = 0x8000;
    mem[0x8002] = 0x81;  /* 10000001 → RLC → 00000011 */
    pc = 0x100;
    mem[pc++] = 0xDD; mem[pc++] = 0xCB; mem[pc++] = 0x02; mem[pc++] = 0x00;
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(mem[0x8002] == 0x03);
    assert(cpu.b == 0x03);  /* undocumented: stored to B */

    printf("  PASS: dd_cb_prefix\n");
}

/* ── Test: IXH/IXL undocumented half-register ops ────────────────────────── */

static void test_dd_ixhl(void)
{
    setup();
    cpu.pc = 0x100;
    cpu.ix = 0x1234;
    uint16_t pc = 0x100;
    /* LD A,IXH = DD 7C */
    mem[pc++] = 0xDD; mem[pc++] = 0x7C;
    /* LD B,IXL = DD 45 */
    mem[pc++] = 0xDD; mem[pc++] = 0x45;
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x12);
    assert(cpu.b == 0x34);

    /* LD IXH,n = DD 26 nn */
    setup();
    cpu.pc = 0x100;
    cpu.ix = 0x0000;
    pc = 0x100;
    mem[pc++] = 0xDD; mem[pc++] = 0x26; mem[pc++] = 0xAB;  /* LD IXH,0xAB */
    mem[pc++] = 0xDD; mem[pc++] = 0x2E; mem[pc++] = 0xCD;  /* LD IXL,0xCD */
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.ix == 0xABCD);

    /* ADD A,IXH = DD 84 */
    setup();
    cpu.pc = 0x100;
    cpu.ix = 0x1000;
    pc = 0x100;
    mem[pc++] = 0x3E; mem[pc++] = 0x05;  /* LD A,5 */
    mem[pc++] = 0xDD; mem[pc++] = 0x84;  /* ADD A,IXH */
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x15);

    /* INC IXH = DD 24 */
    setup();
    cpu.pc = 0x100;
    cpu.ix = 0x0FFF;
    pc = 0x100;
    mem[pc++] = 0xDD; mem[pc++] = 0x24;  /* INC IXH */
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.ix == 0x10FF);

    printf("  PASS: dd_ixhl\n");
}

/* ── Test: FD prefix (IY) works identically ─────────────────────────────── */

static void test_fd_iy(void)
{
    /* LD IY,nn; LD A,(IY+d); PUSH IY; POP IY */
    setup();
    cpu.pc = 0x100;
    cpu.sp = 0xFFFE;
    mem[0x9003] = 0x55;
    uint16_t pc = 0x100;
    mem[pc++] = 0xFD; mem[pc++] = 0x21; mem[pc++] = 0x00; mem[pc++] = 0x90; /* LD IY,0x9000 */
    mem[pc++] = 0xFD; mem[pc++] = 0x7E; mem[pc++] = 0x03;  /* LD A,(IY+3) */
    mem[pc++] = 0xFD; mem[pc++] = 0xE5;  /* PUSH IY */
    mem[pc++] = 0xFD; mem[pc++] = 0x21; mem[pc++] = 0x00; mem[pc++] = 0x00; /* LD IY,0 */
    mem[pc++] = 0xFD; mem[pc++] = 0xE1;  /* POP IY */
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 0x55);
    assert(cpu.iy == 0x9000);

    /* ADD IY,DE */
    setup();
    cpu.pc = 0x100;
    cpu.iy = 0x1000;
    pc = 0x100;
    mem[pc++] = 0x11; mem[pc++] = 0x00; mem[pc++] = 0x05;  /* LD DE,0x0500 */
    mem[pc++] = 0xFD; mem[pc++] = 0x19;  /* ADD IY,DE */
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.iy == 0x1500);

    printf("  PASS: fd_iy\n");
}

/* ── Test: IX array access program ──────────────────────────────────────── */

static void test_ix_array_program(void)
{
    /* Sum array[0..3] using IX indexed addressing */
    setup();
    cpu.pc = 0x100;
    mem[0x8000] = 10; mem[0x8001] = 20;
    mem[0x8002] = 30; mem[0x8003] = 40;
    uint16_t pc = 0x100;
    mem[pc++] = 0xDD; mem[pc++] = 0x21; mem[pc++] = 0x00; mem[pc++] = 0x80; /* LD IX,0x8000 */
    mem[pc++] = 0x3E; mem[pc++] = 0x00;  /* LD A,0 */
    mem[pc++] = 0xDD; mem[pc++] = 0x86; mem[pc++] = 0x00;  /* ADD A,(IX+0) */
    mem[pc++] = 0xDD; mem[pc++] = 0x86; mem[pc++] = 0x01;  /* ADD A,(IX+1) */
    mem[pc++] = 0xDD; mem[pc++] = 0x86; mem[pc++] = 0x02;  /* ADD A,(IX+2) */
    mem[pc++] = 0xDD; mem[pc++] = 0x86; mem[pc++] = 0x03;  /* ADD A,(IX+3) */
    mem[pc++] = 0x76;
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    assert(cpu.a == 100);

    printf("  PASS: ix_array_program\n");
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

    /* Step 3 tests */
    test_jp_cc();
    test_jr_cc();
    test_djnz();
    test_push_pop();
    test_push_pop_af();
    test_ex_af();
    test_exx();
    test_ex_de_hl();
    test_ex_sp_hl();
    test_ret_cc();
    test_call_cc();
    test_rst_trap();
    test_rst_no_trap();
    test_di_ei();
    test_jp_hl();
    test_ld_sp_hl();
    test_nested_call_ret();
    test_wz_tracking();

    /* Step 4 tests */
    test_ld_bc_indirect();
    test_ld_de_indirect();
    test_ld_nn_hl();
    test_ld_nn_a();
    test_out_in();
    test_io_no_trap();
    test_memcpy_program();

    /* Step 5 tests */
    test_cb_rlc_rrc();
    test_cb_rl_rr();
    test_cb_shifts();
    test_cb_sll();
    test_cb_bit();
    test_cb_res_set();
    test_cb_hl_indirect();
    test_cb_shift_program();

    /* Step 6 tests */
    test_ed_neg();
    test_ed_adc_sbc_hl();
    test_ed_ld_rr_nn();
    test_ed_ld_i_r();
    test_ed_im();
    test_ed_rld_rrd();
    test_ed_ldi_ldd();
    test_ed_ldir();
    test_ed_cpi_cpd();
    test_ed_cpir();
    test_ed_retn_reti();
    test_ed_in_r_c();
    test_ed_out_c_r();
    test_ed_block_io();
    test_ed_lddr();
    test_ed_cpdr();

    /* Step 7 tests */
    test_dd_ld_ix_nn();
    test_dd_ld_r_ixd();
    test_dd_alu_ixd();
    test_dd_inc_dec_ixd();
    test_dd_push_pop();
    test_dd_ex_sp_ix();
    test_dd_add_ix_rr();
    test_dd_ld_nn_ix();
    test_dd_cb_prefix();
    test_dd_ixhl();
    test_fd_iy();
    test_ix_array_program();

    printf("All tests passed.\n");
    return 0;
}
