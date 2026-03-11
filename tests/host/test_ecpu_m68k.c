/*
 * test_ecpu_m68k.c — Host-side unit tests for the m68k eCPU core (Step 1)
 *
 * Tests: MOVE.B/W/L, MOVEQ, LEA, CLR, NOP, STOP, ILLEGAL,
 *        A-line/F-line traps, EA modes 0–4, JSR/RTS, TRAP #n.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "kernel/ecpu/ecpu.h"
#include "kernel/ecpu/ecpu_m68k.h"

/* ── Helpers ─────────────────────────────────────────────────────────────── */

#define MEM_SIZE  (1 << 20)  /* 1 MB */

static m68k_state_t cpu;
static uint8_t mem[MEM_SIZE];

static void setup(void)
{
    memset(mem, 0, sizeof(mem));
    ecpu_m68k_ops.init((ecpu_state_t *)&cpu, mem, sizeof(mem));
    cpu.pc = 0x1000;
    /* Set up SP */
    cpu.a[7] = 0x10000;
}

/* Trap handler that returns EXIT on HALT to stop the loop */
static int halt_handler(ecpu_state_t *state, int trap_type,
                         uint32_t param, void *ctx)
{
    (void)state; (void)param; (void)ctx;
    if (trap_type == ECPU_TRAP_HALT)
        return ECPU_TRAP_EXIT;
    return ECPU_TRAP_UNHANDLED;
}

/* Write a STOP #$2700 instruction at addr (terminates execution) */
static void emit_stop(uint32_t addr)
{
    m68k_write16(&cpu, addr, 0x4E72);  /* STOP */
    m68k_write16(&cpu, addr + 2, 0x2700);
}

static void run(void)
{
    ecpu_m68k_ops.set_trap_handler((ecpu_state_t *)&cpu, halt_handler, 0);
    ecpu_m68k_ops.run((ecpu_state_t *)&cpu);
}

/* ── Test: NOP + STOP ───────────────────────────────────────────────────── */

static void test_nop_stop(void)
{
    setup();
    m68k_write16(&cpu, 0x1000, 0x4E71);  /* NOP */
    m68k_write16(&cpu, 0x1002, 0x4E71);  /* NOP */
    emit_stop(0x1004);

    run();

    assert(cpu.stopped == 1);
    assert(cpu.pc == 0x1008);
    printf("  PASS: nop_stop\n");
}

/* ── Test: MOVEQ ────────────────────────────────────────────────────────── */

static void test_moveq(void)
{
    setup();
    /* MOVEQ #$42, D0 → 7042 */
    m68k_write16(&cpu, 0x1000, 0x7042);
    /* MOVEQ #-1, D1 → 72FF */
    m68k_write16(&cpu, 0x1002, 0x72FF);
    /* MOVEQ #0, D2 → 7400 */
    m68k_write16(&cpu, 0x1004, 0x7400);
    emit_stop(0x1006);

    run();

    assert(cpu.d[0] == 0x42);
    assert(cpu.d[1] == 0xFFFFFFFF);  /* sign-extended */
    assert(cpu.d[2] == 0);
    printf("  PASS: moveq\n");
}

/* ── Test: MOVE.L immediate to Dn ───────────────────────────────────────── */

static void test_move_l_imm_dn(void)
{
    setup();
    /* MOVE.L #$12345678, D0 → 203C 1234 5678 */
    m68k_write16(&cpu, 0x1000, 0x203C);
    m68k_write32(&cpu, 0x1002, 0x12345678);
    emit_stop(0x1006);

    run();

    assert(cpu.d[0] == 0x12345678);
    printf("  PASS: move_l_imm_dn\n");
}

/* ── Test: MOVE.W immediate to Dn ──────────────────────────────────────── */

static void test_move_w_imm_dn(void)
{
    setup();
    cpu.d[0] = 0xAAAAAAAA;
    /* MOVE.W #$1234, D0 → 303C 1234 */
    m68k_write16(&cpu, 0x1000, 0x303C);
    m68k_write16(&cpu, 0x1002, 0x1234);
    emit_stop(0x1004);

    run();

    /* Only low word replaced */
    assert(cpu.d[0] == 0xAAAA1234);
    printf("  PASS: move_w_imm_dn\n");
}

/* ── Test: MOVE.B immediate to Dn ──────────────────────────────────────── */

static void test_move_b_imm_dn(void)
{
    setup();
    cpu.d[0] = 0xAAAAAAAA;
    /* MOVE.B #$56, D0 → 103C 0056 */
    m68k_write16(&cpu, 0x1000, 0x103C);
    m68k_write16(&cpu, 0x1002, 0x0056);
    emit_stop(0x1004);

    run();

    /* Only low byte replaced */
    assert(cpu.d[0] == 0xAAAAAA56);
    printf("  PASS: move_b_imm_dn\n");
}

/* ── Test: MOVE.L Dn to Dn ─────────────────────────────────────────────── */

static void test_move_l_dn_dn(void)
{
    setup();
    cpu.d[1] = 0xDEADBEEF;
    /* MOVE.L D1, D0 → 2001 */
    m68k_write16(&cpu, 0x1000, 0x2001);
    emit_stop(0x1002);

    run();

    assert(cpu.d[0] == 0xDEADBEEF);
    printf("  PASS: move_l_dn_dn\n");
}

/* ── Test: MOVE.L Dn to (An) ───────────────────────────────────────────── */

static void test_move_l_dn_an_ind(void)
{
    setup();
    cpu.d[0] = 0x12345678;
    cpu.a[0] = 0x2000;
    /* MOVE.L D0, (A0) → 2080 */
    m68k_write16(&cpu, 0x1000, 0x2080);
    emit_stop(0x1002);

    run();

    assert(m68k_read32(&cpu, 0x2000) == 0x12345678);
    printf("  PASS: move_l_dn_an_ind\n");
}

/* ── Test: MOVE.L (An) to Dn ───────────────────────────────────────────── */

static void test_move_l_an_ind_dn(void)
{
    setup();
    cpu.a[0] = 0x2000;
    m68k_write32(&cpu, 0x2000, 0xCAFEBABE);
    /* MOVE.L (A0), D0 → 2010 */
    m68k_write16(&cpu, 0x1000, 0x2010);
    emit_stop(0x1002);

    run();

    assert(cpu.d[0] == 0xCAFEBABE);
    printf("  PASS: move_l_an_ind_dn\n");
}

/* ── Test: MOVE.L (An)+ post-increment ──────────────────────────────────── */

static void test_move_l_postinc(void)
{
    setup();
    cpu.a[0] = 0x2000;
    m68k_write32(&cpu, 0x2000, 0x11111111);
    m68k_write32(&cpu, 0x2004, 0x22222222);
    /* MOVE.L (A0)+, D0 → 2018 */
    m68k_write16(&cpu, 0x1000, 0x2018);
    /* MOVE.L (A0)+, D1 → 2218 */
    m68k_write16(&cpu, 0x1002, 0x2218);
    emit_stop(0x1004);

    run();

    assert(cpu.d[0] == 0x11111111);
    assert(cpu.d[1] == 0x22222222);
    assert(cpu.a[0] == 0x2008);
    printf("  PASS: move_l_postinc\n");
}

/* ── Test: MOVE.L -(An) pre-decrement ───────────────────────────────────── */

static void test_move_l_predec(void)
{
    setup();
    cpu.a[0] = 0x2008;
    m68k_write32(&cpu, 0x2004, 0x33333333);
    /* MOVE.L -(A0), D0 → 2020 */
    m68k_write16(&cpu, 0x1000, 0x2020);
    emit_stop(0x1002);

    run();

    assert(cpu.d[0] == 0x33333333);
    assert(cpu.a[0] == 0x2004);
    printf("  PASS: move_l_predec\n");
}

/* ── Test: MOVE.B (An)+ with A7 alignment ──────────────────────────────── */

static void test_move_b_a7_postinc(void)
{
    setup();
    cpu.a[7] = 0x3000;
    m68k_write8(&cpu, 0x3000, 0x42);
    /* MOVE.B (A7)+, D0 → 101F */
    m68k_write16(&cpu, 0x1000, 0x101F);
    emit_stop(0x1002);

    run();

    assert((cpu.d[0] & 0xFF) == 0x42);
    /* A7 should increment by 2 (not 1) to keep word-aligned */
    assert(cpu.a[7] == 0x3002);
    printf("  PASS: move_b_a7_postinc\n");
}

/* ── Test: MOVEA.W (word to address register, sign-extended) ────────────── */

static void test_movea_w(void)
{
    setup();
    /* MOVEA.W #$FF00, A0 → 307C FF00 */
    m68k_write16(&cpu, 0x1000, 0x307C);
    m68k_write16(&cpu, 0x1002, 0xFF00);
    emit_stop(0x1004);

    run();

    /* Word sign-extended to 32 bits: $FF00 → $FFFFFF00 */
    assert(cpu.a[0] == 0xFFFFFF00);
    /* MOVEA does not affect flags */
    printf("  PASS: movea_w\n");
}

/* ── Test: MOVEA.L (long to address register) ──────────────────────────── */

static void test_movea_l(void)
{
    setup();
    /* MOVEA.L #$00012345, A2 → 247C 0001 2345 */
    m68k_write16(&cpu, 0x1000, 0x247C);
    m68k_write32(&cpu, 0x1002, 0x00012345);
    emit_stop(0x1006);

    run();

    assert(cpu.a[2] == 0x00012345);
    printf("  PASS: movea_l\n");
}

/* ── Test: CLR ──────────────────────────────────────────────────────────── */

static void test_clr(void)
{
    setup();
    cpu.d[0] = 0xDEADBEEF;
    /* CLR.L D0 → 4280 */
    m68k_write16(&cpu, 0x1000, 0x4280);
    emit_stop(0x1002);

    run();

    assert(cpu.d[0] == 0);
    printf("  PASS: clr\n");
}

/* ── Test: CLR.W memory ─────────────────────────────────────────────────── */

static void test_clr_w_mem(void)
{
    setup();
    cpu.a[0] = 0x2000;
    m68k_write16(&cpu, 0x2000, 0xFFFF);
    /* CLR.W (A0) → 4250 */
    m68k_write16(&cpu, 0x1000, 0x4250);
    emit_stop(0x1002);

    run();

    assert(m68k_read16(&cpu, 0x2000) == 0);
    printf("  PASS: clr_w_mem\n");
}

/* ── Test: LEA ──────────────────────────────────────────────────────────── */

static void test_lea(void)
{
    setup();
    cpu.a[0] = 0x5000;
    /* LEA (A0), A1 → 43D0  (0100 001 111 010 000) */
    m68k_write16(&cpu, 0x1000, 0x43D0);
    emit_stop(0x1002);

    run();

    assert(cpu.a[1] == 0x5000);
    printf("  PASS: lea\n");
}

/* ── Test: LEA with abs.L ───────────────────────────────────────────────── */

static void test_lea_abs(void)
{
    setup();
    /* LEA $00002000.L, A2 → 45F9 0000 2000 */
    m68k_write16(&cpu, 0x1000, 0x45F9);
    m68k_write32(&cpu, 0x1002, 0x00002000);
    emit_stop(0x1006);

    run();

    assert(cpu.a[2] == 0x00002000);
    printf("  PASS: lea_abs\n");
}

/* ── Test: TRAP #n ──────────────────────────────────────────────────────── */

static int trap_n_seen = -1;

static int trap_n_handler(ecpu_state_t *state, int trap_type,
                           uint32_t param, void *ctx)
{
    (void)state; (void)ctx;
    if (trap_type == ECPU_TRAP_SWI) {
        trap_n_seen = (int)param;
        return ECPU_TRAP_HANDLED;
    }
    if (trap_type == ECPU_TRAP_HALT)
        return ECPU_TRAP_EXIT;
    return ECPU_TRAP_UNHANDLED;
}

static void test_trap_n(void)
{
    setup();
    trap_n_seen = -1;
    /* TRAP #0 → 4E40 */
    m68k_write16(&cpu, 0x1000, 0x4E40);
    emit_stop(0x1002);

    ecpu_m68k_ops.set_trap_handler((ecpu_state_t *)&cpu, trap_n_handler, 0);
    ecpu_m68k_ops.run((ecpu_state_t *)&cpu);

    assert(trap_n_seen == 0);
    printf("  PASS: trap_n\n");
}

/* ── Test: TRAP #15 ─────────────────────────────────────────────────────── */

static void test_trap_15(void)
{
    setup();
    trap_n_seen = -1;
    /* TRAP #15 → 4E4F */
    m68k_write16(&cpu, 0x1000, 0x4E4F);
    emit_stop(0x1002);

    ecpu_m68k_ops.set_trap_handler((ecpu_state_t *)&cpu, trap_n_handler, 0);
    ecpu_m68k_ops.run((ecpu_state_t *)&cpu);

    assert(trap_n_seen == 15);
    printf("  PASS: trap_15\n");
}

/* ── Test: F-line trap ──────────────────────────────────────────────────── */

static uint32_t fline_opcode_seen = 0;

static int fline_handler(ecpu_state_t *state, int trap_type,
                          uint32_t param, void *ctx)
{
    (void)state; (void)ctx;
    if (trap_type == ECPU_TRAP_ILLEGAL) {
        fline_opcode_seen = param;
        return ECPU_TRAP_HANDLED;
    }
    if (trap_type == ECPU_TRAP_HALT)
        return ECPU_TRAP_EXIT;
    return ECPU_TRAP_UNHANDLED;
}

static void test_fline(void)
{
    setup();
    fline_opcode_seen = 0;
    /* F-line opcode: $FF05 (Human68k _DOS_PRINT-ish) */
    m68k_write16(&cpu, 0x1000, 0xFF05);
    emit_stop(0x1002);

    ecpu_m68k_ops.set_trap_handler((ecpu_state_t *)&cpu, fline_handler, 0);
    ecpu_m68k_ops.run((ecpu_state_t *)&cpu);

    assert(fline_opcode_seen == 0xFF05);
    printf("  PASS: fline\n");
}

/* ── Test: A-line trap ──────────────────────────────────────────────────── */

static void test_aline(void)
{
    setup();
    fline_opcode_seen = 0;
    /* A-line opcode: $A000 */
    m68k_write16(&cpu, 0x1000, 0xA000);
    emit_stop(0x1002);

    ecpu_m68k_ops.set_trap_handler((ecpu_state_t *)&cpu, fline_handler, 0);
    ecpu_m68k_ops.run((ecpu_state_t *)&cpu);

    assert(fline_opcode_seen == 0xA000);
    printf("  PASS: aline\n");
}

/* ── Test: JSR + RTS ────────────────────────────────────────────────────── */

static void test_jsr_rts(void)
{
    setup();
    /* JSR $2000.L → 4EB9 0000 2000 */
    m68k_write16(&cpu, 0x1000, 0x4EB9);
    m68k_write32(&cpu, 0x1002, 0x00002000);
    emit_stop(0x1006);  /* after return */

    /* Subroutine at $2000: MOVEQ #$42, D0; RTS */
    m68k_write16(&cpu, 0x2000, 0x7042);  /* MOVEQ #$42, D0 */
    m68k_write16(&cpu, 0x2002, 0x4E75);  /* RTS */

    run();

    assert(cpu.d[0] == 0x42);
    assert(cpu.pc == 0x100A);  /* past STOP */
    printf("  PASS: jsr_rts\n");
}

/* ── Test: MOVE.L to (An)+ then -(An) round-trip ───────────────────────── */

static void test_postinc_predec_roundtrip(void)
{
    setup();
    cpu.a[0] = 0x2000;
    cpu.d[0] = 0xAAAAAAAA;
    cpu.d[1] = 0xBBBBBBBB;

    uint32_t pc = 0x1000;
    /* MOVE.L D0, (A0)+ → 20C0 */
    m68k_write16(&cpu, pc, 0x20C0); pc += 2;
    /* MOVE.L D1, (A0)+ → 20C1 */
    m68k_write16(&cpu, pc, 0x20C1); pc += 2;
    /* MOVE.L -(A0), D2 → 2420 */
    m68k_write16(&cpu, pc, 0x2420); pc += 2;
    /* MOVE.L -(A0), D3 → 2620 */
    m68k_write16(&cpu, pc, 0x2620); pc += 2;
    emit_stop(pc);

    run();

    assert(cpu.d[2] == 0xBBBBBBBB);
    assert(cpu.d[3] == 0xAAAAAAAA);
    assert(cpu.a[0] == 0x2000);
    printf("  PASS: postinc_predec_roundtrip\n");
}

/* ── Test: Common interface get_reg/set_reg ──────────────────────────────── */

static void test_get_set_reg(void)
{
    setup();
    ecpu_m68k_ops.set_reg((ecpu_state_t *)&cpu, M68K_REG_D3, 0x12345678);
    assert(ecpu_m68k_ops.get_reg((ecpu_state_t *)&cpu, M68K_REG_D3) == 0x12345678);

    ecpu_m68k_ops.set_reg((ecpu_state_t *)&cpu, M68K_REG_A5, 0xABCD0000);
    assert(ecpu_m68k_ops.get_reg((ecpu_state_t *)&cpu, M68K_REG_A5) == 0xABCD0000);

    ecpu_m68k_ops.set_reg((ecpu_state_t *)&cpu, M68K_REG_PC, 0x4000);
    assert(ecpu_m68k_ops.get_reg((ecpu_state_t *)&cpu, M68K_REG_PC) == 0x4000);

    printf("  PASS: get_set_reg\n");
}

/* ── Test: MOVE.L to/from displacement d16(An) EA mode 5 ───────────────── */

static void test_move_l_disp(void)
{
    setup();
    cpu.a[0] = 0x2000;
    cpu.d[0] = 0xFEEDFACE;
    /* MOVE.L D0, $10(A0) → 2140 0010
     * dst EA: mode 5 (101), reg A0 (000) → dst_mode=5, dst_reg=0
     * encoding: 0010 000 101 000 000 = 0x2140
     * followed by displacement $0010 */
    m68k_write16(&cpu, 0x1000, 0x2140);
    m68k_write16(&cpu, 0x1002, 0x0010);
    /* MOVE.L $10(A0), D1 → 2228 0010
     * src EA: mode 5 (101), reg A0 (000) → src_mode=5, src_reg=0
     * encoding: 0010 001 000 101 000 = 0x2228
     * followed by displacement $0010 */
    m68k_write16(&cpu, 0x1004, 0x2228);
    m68k_write16(&cpu, 0x1006, 0x0010);
    emit_stop(0x1008);

    run();

    assert(m68k_read32(&cpu, 0x2010) == 0xFEEDFACE);
    assert(cpu.d[1] == 0xFEEDFACE);
    printf("  PASS: move_l_disp\n");
}

/* ── Test: MOVE.L d8(An,Dn) EA mode 6 ──────────────────────────────────── */

static void test_move_l_index(void)
{
    setup();
    cpu.a[0] = 0x2000;
    cpu.d[7] = 0x00000008;  /* index register */
    m68k_write32(&cpu, 0x200C, 0xBEEFCAFE);  /* at base+4+8 */
    /* MOVE.L 4(A0,D7.L), D0 → 2030 7804
     * src EA: mode 6 (110), reg A0 (000)
     * encoding: 0010 000 000 110 000 = 0x2030
     * brief ext: D7, long, disp=4 → 0x7804 */
    m68k_write16(&cpu, 0x1000, 0x2030);
    m68k_write16(&cpu, 0x1002, 0x7804);
    emit_stop(0x1004);

    run();

    assert(cpu.d[0] == 0xBEEFCAFE);
    printf("  PASS: move_l_index\n");
}

/* ── Test: MOVE.L abs.W and abs.L ───────────────────────────────────────── */

static void test_move_l_abs(void)
{
    setup();
    m68k_write32(&cpu, 0x3000, 0x11223344);
    /* MOVE.L $3000.W, D0 → 2038 3000
     * src EA: mode 7, reg 0 (abs.W)
     * encoding: 0010 000 000 111 000 = 0x2038 */
    m68k_write16(&cpu, 0x1000, 0x2038);
    m68k_write16(&cpu, 0x1002, 0x3000);
    /* MOVE.L $00004000.L, D1 → 2239 0000 4000
     * src EA: mode 7, reg 1 (abs.L)
     * encoding: 0010 001 000 111 001 = 0x2239 */
    m68k_write32(&cpu, 0x4000, 0x55667788);
    m68k_write16(&cpu, 0x1004, 0x2239);
    m68k_write32(&cpu, 0x1006, 0x00004000);
    emit_stop(0x100A);

    run();

    assert(cpu.d[0] == 0x11223344);
    assert(cpu.d[1] == 0x55667788);
    printf("  PASS: move_l_abs\n");
}

/* ── Test: MOVE.L d16(PC) — PC-relative ─────────────────────────────────── */

static void test_move_l_pc_rel(void)
{
    setup();
    /* MOVE.L d16(PC), D0 → 203A disp
     * src EA: mode 7, reg 2 (d16(PC))
     * encoding: 0010 000 000 111 010 = 0x203A
     * PC at time of fetch = 0x1002, data at 0x1010
     * disp = 0x1010 - 0x1002 = 0x000E */
    m68k_write32(&cpu, 0x1010, 0xDEADC0DE);
    m68k_write16(&cpu, 0x1000, 0x203A);
    m68k_write16(&cpu, 0x1002, 0x000E);
    emit_stop(0x1004);

    run();

    assert(cpu.d[0] == 0xDEADC0DE);
    printf("  PASS: move_l_pc_rel\n");
}

/* ── Test: init and reset ───────────────────────────────────────────────── */

static void test_init_reset(void)
{
    setup();
    cpu.d[0] = 0x12345678;
    cpu.a[3] = 0xAABBCCDD;

    ecpu_m68k_ops.reset((ecpu_state_t *)&cpu);

    assert(cpu.d[0] == 0);
    assert(cpu.a[3] == 0);
    assert(cpu.memory == mem);
    assert(cpu.mem_size == MEM_SIZE);
    /* Should start in supervisor mode */
    assert(cpu.sr & M68K_SR_S);
    printf("  PASS: init_reset\n");
}

/* ── Test: MOVE.L Dn to -(An) push pattern ──────────────────────────────── */

static void test_move_l_push(void)
{
    setup();
    cpu.d[0] = 0xCAFEBABE;
    cpu.a[6] = 0x3010;
    /* MOVE.L D0, -(A6) → 2D00
     * dst EA: mode 4 (100), reg A6 (110)
     * encoding: 0010 110 100 000 000 = 0x2D00 */
    m68k_write16(&cpu, 0x1000, 0x2D00);
    emit_stop(0x1002);

    run();

    assert(cpu.a[6] == 0x300C);
    assert(m68k_read32(&cpu, 0x300C) == 0xCAFEBABE);
    printf("  PASS: move_l_push\n");
}

/* ── Test: CCR after MOVE (use TRAP to exit without clobbering SR) ───────── */

static int exit_handler(ecpu_state_t *state, int trap_type,
                         uint32_t param, void *ctx)
{
    (void)state; (void)param; (void)ctx;
    if (trap_type == ECPU_TRAP_SWI || trap_type == ECPU_TRAP_HALT)
        return ECPU_TRAP_EXIT;
    return ECPU_TRAP_UNHANDLED;
}

static void test_move_ccr(void)
{
    setup();
    /* MOVEQ #0, D0 → Z set, N clear */
    m68k_write16(&cpu, 0x1000, 0x7000);
    /* TRAP #0 to exit without clobbering SR */
    m68k_write16(&cpu, 0x1002, 0x4E40);

    ecpu_m68k_ops.set_trap_handler((ecpu_state_t *)&cpu, exit_handler, 0);
    ecpu_m68k_ops.run((ecpu_state_t *)&cpu);

    assert(cpu.d[0] == 0);
    assert(cpu.sr & M68K_FLAG_Z);
    assert(!(cpu.sr & M68K_FLAG_N));
    assert(!(cpu.sr & M68K_FLAG_V));
    assert(!(cpu.sr & M68K_FLAG_C));

    /* MOVEQ #-1, D0 → N set, Z clear */
    setup();
    m68k_write16(&cpu, 0x1000, 0x70FF);
    m68k_write16(&cpu, 0x1002, 0x4E40);

    ecpu_m68k_ops.set_trap_handler((ecpu_state_t *)&cpu, exit_handler, 0);
    ecpu_m68k_ops.run((ecpu_state_t *)&cpu);

    assert(cpu.d[0] == 0xFFFFFFFF);
    assert(!(cpu.sr & M68K_FLAG_Z));
    assert(cpu.sr & M68K_FLAG_N);

    printf("  PASS: move_ccr\n");
}

/* ── Test: CLR CCR ──────────────────────────────────────────────────────── */

static void test_clr_ccr(void)
{
    setup();
    cpu.d[0] = 0xDEADBEEF;
    /* CLR.L D0 → 4280 */
    m68k_write16(&cpu, 0x1000, 0x4280);
    m68k_write16(&cpu, 0x1002, 0x4E40);  /* TRAP #0 exit */

    ecpu_m68k_ops.set_trap_handler((ecpu_state_t *)&cpu, exit_handler, 0);
    ecpu_m68k_ops.run((ecpu_state_t *)&cpu);

    assert(cpu.d[0] == 0);
    assert(cpu.sr & M68K_FLAG_Z);
    assert(!(cpu.sr & M68K_FLAG_N));
    assert(!(cpu.sr & M68K_FLAG_V));
    assert(!(cpu.sr & M68K_FLAG_C));
    printf("  PASS: clr_ccr\n");
}

/* ── Main ───────────────────────────────────────────────────────────────── */

int main(void)
{
    printf("test_ecpu_m68k:\n");

    test_nop_stop();
    test_moveq();
    test_move_l_imm_dn();
    test_move_w_imm_dn();
    test_move_b_imm_dn();
    test_move_l_dn_dn();
    test_move_l_dn_an_ind();
    test_move_l_an_ind_dn();
    test_move_l_postinc();
    test_move_l_predec();
    test_move_b_a7_postinc();
    test_movea_w();
    test_movea_l();
    test_clr();
    test_clr_w_mem();
    test_lea();
    test_lea_abs();
    test_trap_n();
    test_trap_15();
    test_fline();
    test_aline();
    test_jsr_rts();
    test_postinc_predec_roundtrip();
    test_get_set_reg();
    test_move_l_disp();
    test_move_l_index();
    test_move_l_abs();
    test_move_l_pc_rel();
    test_init_reset();
    test_move_l_push();
    test_move_ccr();
    test_clr_ccr();

    printf("All 32 tests passed.\n");
    return 0;
}
