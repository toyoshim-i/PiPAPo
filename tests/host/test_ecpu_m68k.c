/*
 * test_ecpu_m68k.c — Host-side unit tests for the m68k eCPU core (Step 1)
 *
 * Tests: MOVE.B/W/L, MOVEQ, LEA, CLR, NOP, STOP, ILLEGAL,
 *        A-line/F-line traps, EA modes 0–4, JSR/RTS, TRAP #n.
 * Step 2: ADD/SUB/CMP/NEG/TST/EXT/MULU/MULS/DIVU/DIVS,
 *         ADDI/SUBI/CMPI, ADDQ/SUBQ.
 * Step 3: Bcc/BRA/BSR, DBcc, Scc, LINK, UNLK.
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

/* ══════════════════════════════════════════════════════════════════════════
 * Step 2 tests — Integer arithmetic
 * ══════════════════════════════════════════════════════════════════════════ */

/* Helper: emit instruction + TRAP #0 exit, run, return */
static void run_exit(void)
{
    ecpu_m68k_ops.set_trap_handler((ecpu_state_t *)&cpu, exit_handler, 0);
    ecpu_m68k_ops.run((ecpu_state_t *)&cpu);
}

/* ── Test: ADD.L Dn, Dn ─────────────────────────────────────────────────── */

static void test_add_l_dn(void)
{
    setup();
    cpu.d[0] = 100;
    cpu.d[1] = 200;
    /* ADD.L D1, D0 → D081  (1101 000 010 000 001) */
    m68k_write16(&cpu, 0x1000, 0xD081);
    m68k_write16(&cpu, 0x1002, 0x4E40);  /* TRAP #0 exit */

    run_exit();

    assert(cpu.d[0] == 300);
    assert(!(cpu.sr & M68K_FLAG_Z));
    assert(!(cpu.sr & M68K_FLAG_N));
    assert(!(cpu.sr & M68K_FLAG_V));
    assert(!(cpu.sr & M68K_FLAG_C));
    printf("  PASS: add_l_dn\n");
}

/* ── Test: ADD.L overflow ───────────────────────────────────────────────── */

static void test_add_l_overflow(void)
{
    setup();
    cpu.d[0] = 0x7FFFFFFF;
    cpu.d[1] = 1;
    /* ADD.L D1, D0 → D081 */
    m68k_write16(&cpu, 0x1000, 0xD081);
    m68k_write16(&cpu, 0x1002, 0x4E40);

    run_exit();

    assert(cpu.d[0] == 0x80000000);
    assert(cpu.sr & M68K_FLAG_N);
    assert(cpu.sr & M68K_FLAG_V);
    printf("  PASS: add_l_overflow\n");
}

/* ── Test: ADD.W carry ──────────────────────────────────────────────────── */

static void test_add_w_carry(void)
{
    setup();
    cpu.d[0] = 0xFFFF;
    cpu.d[1] = 1;
    /* ADD.W D1, D0 → D041  (1101 000 001 000 001) */
    m68k_write16(&cpu, 0x1000, 0xD041);
    m68k_write16(&cpu, 0x1002, 0x4E40);

    run_exit();

    /* Word result = 0, with carry */
    assert((cpu.d[0] & 0xFFFF) == 0);
    assert(cpu.sr & M68K_FLAG_Z);
    assert(cpu.sr & M68K_FLAG_C);
    assert(cpu.sr & M68K_FLAG_X);
    printf("  PASS: add_w_carry\n");
}

/* ── Test: ADDI.L #imm, Dn ──────────────────────────────────────────────── */

static void test_addi(void)
{
    setup();
    cpu.d[0] = 10;
    /* ADDI.L #$1000, D0 → 0680 0000 1000 */
    m68k_write16(&cpu, 0x1000, 0x0680);
    m68k_write32(&cpu, 0x1002, 0x00001000);
    m68k_write16(&cpu, 0x1006, 0x4E40);

    run_exit();

    assert(cpu.d[0] == 0x100A);
    printf("  PASS: addi\n");
}

/* ── Test: ADDQ.L #n, Dn ───────────────────────────────────────────────── */

static void test_addq(void)
{
    setup();
    cpu.d[0] = 10;
    /* ADDQ.L #5, D0 → 5A80  (0101 101 0 10 000 000) */
    m68k_write16(&cpu, 0x1000, 0x5A80);
    m68k_write16(&cpu, 0x1002, 0x4E40);

    run_exit();

    assert(cpu.d[0] == 15);
    printf("  PASS: addq\n");
}

/* ── Test: ADDQ.L #8, An (data=0 means 8) ──────────────────────────────── */

static void test_addq_an(void)
{
    setup();
    cpu.a[0] = 0x1000;
    /* ADDQ.L #8, A0 → 5088  (0101 000 0 10 001 000) */
    m68k_write16(&cpu, 0x1000, 0x5088);
    emit_stop(0x1002);

    run();

    assert(cpu.a[0] == 0x1008);
    printf("  PASS: addq_an\n");
}

/* ── Test: SUB.L Dn, Dn ─────────────────────────────────────────────────── */

static void test_sub_l_dn(void)
{
    setup();
    cpu.d[0] = 300;
    cpu.d[1] = 100;
    /* SUB.L D1, D0 → 9081  (1001 000 010 000 001) */
    m68k_write16(&cpu, 0x1000, 0x9081);
    m68k_write16(&cpu, 0x1002, 0x4E40);

    run_exit();

    assert(cpu.d[0] == 200);
    assert(!(cpu.sr & M68K_FLAG_N));
    assert(!(cpu.sr & M68K_FLAG_C));
    printf("  PASS: sub_l_dn\n");
}

/* ── Test: SUB.L borrow ─────────────────────────────────────────────────── */

static void test_sub_l_borrow(void)
{
    setup();
    cpu.d[0] = 10;
    cpu.d[1] = 20;
    /* SUB.L D1, D0 → 9081 */
    m68k_write16(&cpu, 0x1000, 0x9081);
    m68k_write16(&cpu, 0x1002, 0x4E40);

    run_exit();

    assert(cpu.d[0] == (uint32_t)-10);
    assert(cpu.sr & M68K_FLAG_N);
    assert(cpu.sr & M68K_FLAG_C);
    assert(cpu.sr & M68K_FLAG_X);
    printf("  PASS: sub_l_borrow\n");
}

/* ── Test: SUBI.W #imm, Dn ──────────────────────────────────────────────── */

static void test_subi(void)
{
    setup();
    cpu.d[0] = 0x1000;
    /* SUBI.W #$100, D0 → 0440 0100 */
    m68k_write16(&cpu, 0x1000, 0x0440);
    m68k_write16(&cpu, 0x1002, 0x0100);
    m68k_write16(&cpu, 0x1004, 0x4E40);

    run_exit();

    assert((cpu.d[0] & 0xFFFF) == 0x0F00);
    printf("  PASS: subi\n");
}

/* ── Test: SUBQ.L #n, Dn ───────────────────────────────────────────────── */

static void test_subq(void)
{
    setup();
    cpu.d[0] = 15;
    /* SUBQ.L #5, D0 → 5B80  (0101 101 1 10 000 000) */
    m68k_write16(&cpu, 0x1000, 0x5B80);
    m68k_write16(&cpu, 0x1002, 0x4E40);

    run_exit();

    assert(cpu.d[0] == 10);
    printf("  PASS: subq\n");
}

/* ── Test: CMP.L Dn, Dn ─────────────────────────────────────────────────── */

static void test_cmp_l(void)
{
    setup();
    cpu.d[0] = 42;
    cpu.d[1] = 42;
    /* CMP.L D1, D0 → B081  (1011 000 010 000 001) */
    m68k_write16(&cpu, 0x1000, 0xB081);
    m68k_write16(&cpu, 0x1002, 0x4E40);

    run_exit();

    assert(cpu.sr & M68K_FLAG_Z);
    assert(!(cpu.sr & M68K_FLAG_N));
    assert(!(cpu.sr & M68K_FLAG_C));

    /* CMP with different values */
    setup();
    cpu.d[0] = 10;
    cpu.d[1] = 20;
    m68k_write16(&cpu, 0x1000, 0xB081);
    m68k_write16(&cpu, 0x1002, 0x4E40);

    run_exit();

    assert(!(cpu.sr & M68K_FLAG_Z));
    assert(cpu.sr & M68K_FLAG_N);
    assert(cpu.sr & M68K_FLAG_C);
    printf("  PASS: cmp_l\n");
}

/* ── Test: CMPI.L #imm, Dn ──────────────────────────────────────────────── */

static void test_cmpi(void)
{
    setup();
    cpu.d[0] = 0x12345678;
    /* CMPI.L #$12345678, D0 → 0C80 1234 5678 */
    m68k_write16(&cpu, 0x1000, 0x0C80);
    m68k_write32(&cpu, 0x1002, 0x12345678);
    m68k_write16(&cpu, 0x1006, 0x4E40);

    run_exit();

    assert(cpu.sr & M68K_FLAG_Z);
    printf("  PASS: cmpi\n");
}

/* ── Test: NEG.L ─────────────────────────────────────────────────────────── */

static void test_neg(void)
{
    setup();
    cpu.d[0] = 42;
    /* NEG.L D0 → 4480  (0100 0100 10 000 000) */
    m68k_write16(&cpu, 0x1000, 0x4480);
    m68k_write16(&cpu, 0x1002, 0x4E40);

    run_exit();

    assert(cpu.d[0] == (uint32_t)-42);
    assert(cpu.sr & M68K_FLAG_N);
    assert(cpu.sr & M68K_FLAG_C);
    assert(cpu.sr & M68K_FLAG_X);

    /* NEG 0 → Z set, no carry */
    setup();
    cpu.d[0] = 0;
    m68k_write16(&cpu, 0x1000, 0x4480);
    m68k_write16(&cpu, 0x1002, 0x4E40);

    run_exit();

    assert(cpu.d[0] == 0);
    assert(cpu.sr & M68K_FLAG_Z);
    assert(!(cpu.sr & M68K_FLAG_C));
    printf("  PASS: neg\n");
}

/* ── Test: TST.L ─────────────────────────────────────────────────────────── */

static void test_tst(void)
{
    setup();
    cpu.d[0] = 0;
    /* TST.L D0 → 4A80  (0100 1010 10 000 000) */
    m68k_write16(&cpu, 0x1000, 0x4A80);
    m68k_write16(&cpu, 0x1002, 0x4E40);

    run_exit();

    assert(cpu.sr & M68K_FLAG_Z);
    assert(!(cpu.sr & M68K_FLAG_N));

    /* TST negative */
    setup();
    cpu.d[0] = 0x80000000;
    m68k_write16(&cpu, 0x1000, 0x4A80);
    m68k_write16(&cpu, 0x1002, 0x4E40);

    run_exit();

    assert(!(cpu.sr & M68K_FLAG_Z));
    assert(cpu.sr & M68K_FLAG_N);
    printf("  PASS: tst\n");
}

/* ── Test: EXT.W and EXT.L ──────────────────────────────────────────────── */

static void test_ext(void)
{
    setup();
    cpu.d[0] = 0x000000FF;  /* byte -1 */
    /* EXT.W D0 → 4880 */
    m68k_write16(&cpu, 0x1000, 0x4880);
    m68k_write16(&cpu, 0x1002, 0x4E40);

    run_exit();

    assert((cpu.d[0] & 0xFFFF) == 0xFFFF);
    assert(cpu.sr & M68K_FLAG_N);

    /* EXT.L D0 */
    setup();
    cpu.d[0] = 0x0000FF80;  /* word -128 */
    /* EXT.L D0 → 48C0 */
    m68k_write16(&cpu, 0x1000, 0x48C0);
    m68k_write16(&cpu, 0x1002, 0x4E40);

    run_exit();

    assert(cpu.d[0] == 0xFFFFFF80);
    assert(cpu.sr & M68K_FLAG_N);
    printf("  PASS: ext\n");
}

/* ── Test: SWAP ─────────────────────────────────────────────────────────── */

static void test_swap(void)
{
    setup();
    cpu.d[0] = 0x12345678;
    /* SWAP D0 → 4840 */
    m68k_write16(&cpu, 0x1000, 0x4840);
    m68k_write16(&cpu, 0x1002, 0x4E40);

    run_exit();

    assert(cpu.d[0] == 0x56781234);
    printf("  PASS: swap\n");
}

/* ── Test: MULU ─────────────────────────────────────────────────────────── */

static void test_mulu(void)
{
    setup();
    cpu.d[0] = 100;
    cpu.d[1] = 200;
    /* MULU D1, D0 → C0C1  (1100 000 011 000 001) */
    m68k_write16(&cpu, 0x1000, 0xC0C1);
    m68k_write16(&cpu, 0x1002, 0x4E40);

    run_exit();

    assert(cpu.d[0] == 20000);
    assert(!(cpu.sr & M68K_FLAG_Z));
    assert(!(cpu.sr & M68K_FLAG_N));
    printf("  PASS: mulu\n");
}

/* ── Test: MULS ─────────────────────────────────────────────────────────── */

static void test_muls(void)
{
    setup();
    cpu.d[0] = 0xFFFF;  /* -1 as word */
    cpu.d[1] = 100;
    /* MULS D1, D0 → C1C1  (1100 000 111 000 001) */
    m68k_write16(&cpu, 0x1000, 0xC1C1);
    m68k_write16(&cpu, 0x1002, 0x4E40);

    run_exit();

    assert(cpu.d[0] == (uint32_t)-100);
    assert(cpu.sr & M68K_FLAG_N);
    printf("  PASS: muls\n");
}

/* ── Test: DIVU ─────────────────────────────────────────────────────────── */

static void test_divu(void)
{
    setup();
    cpu.d[0] = 100;
    cpu.d[1] = 7;
    /* DIVU D1, D0 → 80C1  (1000 000 011 000 001) */
    m68k_write16(&cpu, 0x1000, 0x80C1);
    m68k_write16(&cpu, 0x1002, 0x4E40);

    run_exit();

    /* 100/7 = 14 remainder 2 → result = 0x0002000E */
    assert((cpu.d[0] & 0xFFFF) == 14);
    assert((cpu.d[0] >> 16) == 2);
    printf("  PASS: divu\n");
}

/* ── Test: DIVS ─────────────────────────────────────────────────────────── */

static void test_divs(void)
{
    setup();
    cpu.d[0] = (uint32_t)-100;  /* signed -100 */
    cpu.d[1] = 7;
    /* DIVS D1, D0 → 81C1  (1000 000 111 000 001) */
    m68k_write16(&cpu, 0x1000, 0x81C1);
    m68k_write16(&cpu, 0x1002, 0x4E40);

    run_exit();

    /* -100/7 = -14 remainder -2 */
    int16_t quotient = (int16_t)(cpu.d[0] & 0xFFFF);
    int16_t remainder = (int16_t)(cpu.d[0] >> 16);
    assert(quotient == -14);
    assert(remainder == -2);
    printf("  PASS: divs\n");
}

/* ── Test: DIVU divide by zero ──────────────────────────────────────────── */

static int divzero_trapped = 0;

static int divzero_handler(ecpu_state_t *state, int trap_type,
                            uint32_t param, void *ctx)
{
    (void)state; (void)ctx;
    if (trap_type == ECPU_TRAP_ILLEGAL && param == 5) {
        divzero_trapped = 1;
        return ECPU_TRAP_HANDLED;
    }
    if (trap_type == ECPU_TRAP_SWI || trap_type == ECPU_TRAP_HALT)
        return ECPU_TRAP_EXIT;
    return ECPU_TRAP_UNHANDLED;
}

static void test_divu_zero(void)
{
    setup();
    divzero_trapped = 0;
    cpu.d[0] = 100;
    cpu.d[1] = 0;
    /* DIVU D1, D0 → 80C1 */
    m68k_write16(&cpu, 0x1000, 0x80C1);
    m68k_write16(&cpu, 0x1002, 0x4E40);

    ecpu_m68k_ops.set_trap_handler((ecpu_state_t *)&cpu, divzero_handler, 0);
    ecpu_m68k_ops.run((ecpu_state_t *)&cpu);

    assert(divzero_trapped == 1);
    printf("  PASS: divu_zero\n");
}

/* ── Test: ADDA.L ───────────────────────────────────────────────────────── */

static void test_adda(void)
{
    setup();
    cpu.a[0] = 0x1000;
    cpu.d[0] = 0x100;
    /* ADDA.L D0, A0 → D1C0  (1101 000 111 000 000) */
    m68k_write16(&cpu, 0x1000, 0xD1C0);
    emit_stop(0x1002);

    run();

    assert(cpu.a[0] == 0x1100);
    printf("  PASS: adda\n");
}

/* ── Test: SUBA.W (sign-extended) ───────────────────────────────────────── */

static void test_suba_w(void)
{
    setup();
    cpu.a[0] = 0x2000;
    cpu.d[0] = 0xFFF0;  /* -16 as word */
    /* SUBA.W D0, A0 → 90C0  (1001 000 011 000 000) */
    m68k_write16(&cpu, 0x1000, 0x90C0);
    emit_stop(0x1002);

    run();

    /* SUBA.W sign-extends: 0xFFF0 → 0xFFFFFFF0 → sub -16 = add 16 */
    assert(cpu.a[0] == 0x2010);
    printf("  PASS: suba_w\n");
}

/* ── Test: CMPA.L ───────────────────────────────────────────────────────── */

static void test_cmpa(void)
{
    setup();
    cpu.a[0] = 0x2000;
    cpu.d[0] = 0x2000;
    /* CMPA.L D0, A0 → B1C0  (1011 000 111 000 000) */
    m68k_write16(&cpu, 0x1000, 0xB1C0);
    m68k_write16(&cpu, 0x1002, 0x4E40);

    run_exit();

    assert(cpu.sr & M68K_FLAG_Z);
    printf("  PASS: cmpa\n");
}

/* ── Test: CMPM (An)+, (An)+ ───────────────────────────────────────────── */

static void test_cmpm(void)
{
    setup();
    cpu.a[0] = 0x2000;
    cpu.a[1] = 0x3000;
    m68k_write32(&cpu, 0x2000, 0x12345678);
    m68k_write32(&cpu, 0x3000, 0x12345678);
    /* CMPM.L (A0)+, (A1)+ → B389  (1011 001 110 001 000) ... wait
     * CMPM.L (A0)+,(A1)+: reg=A1(001), omode=110, ea_mode=001, ea_reg=A0(000)
     * 1011 001 110 001 000 = 0xB388 */
    m68k_write16(&cpu, 0x1000, 0xB388);
    m68k_write16(&cpu, 0x1002, 0x4E40);

    run_exit();

    assert(cpu.sr & M68K_FLAG_Z);
    assert(cpu.a[0] == 0x2004);
    assert(cpu.a[1] == 0x3004);
    printf("  PASS: cmpm\n");
}

/* ── Test: ADD/SUB with memory ──────────────────────────────────────────── */

static void test_add_sub_mem(void)
{
    setup();
    cpu.a[0] = 0x2000;
    m68k_write32(&cpu, 0x2000, 100);
    cpu.d[0] = 50;
    /* ADD.L D0, (A0) → D190  (1101 000 110 010 000) */
    m68k_write16(&cpu, 0x1000, 0xD190);
    emit_stop(0x1002);

    run();

    assert(m68k_read32(&cpu, 0x2000) == 150);
    printf("  PASS: add_sub_mem\n");
}

/* ── Test: Arithmetic program (sum 1..10) ───────────────────────────────── */

static void test_sum_program(void)
{
    setup();
    /*
     * MOVEQ #0, D0       ; sum = 0
     * MOVEQ #10, D1      ; counter = 10
     * loop:
     *   ADD.L D1, D0     ; sum += counter
     *   SUBQ.L #1, D1    ; counter--
     *   ... (need Bcc — skip for now, use unrolled)
     */
    /* Unrolled: sum 1+2+3+4+5 = 15 */
    uint32_t pc = 0x1000;
    m68k_write16(&cpu, pc, 0x7000); pc += 2;  /* MOVEQ #0, D0 */
    m68k_write16(&cpu, pc, 0xD07C); pc += 2;  /* ADD.W #1, D0 */
    m68k_write16(&cpu, pc, 0x0001); pc += 2;
    m68k_write16(&cpu, pc, 0xD07C); pc += 2;  /* ADD.W #2, D0 */
    m68k_write16(&cpu, pc, 0x0002); pc += 2;
    m68k_write16(&cpu, pc, 0xD07C); pc += 2;  /* ADD.W #3, D0 */
    m68k_write16(&cpu, pc, 0x0003); pc += 2;
    m68k_write16(&cpu, pc, 0xD07C); pc += 2;  /* ADD.W #4, D0 */
    m68k_write16(&cpu, pc, 0x0004); pc += 2;
    m68k_write16(&cpu, pc, 0xD07C); pc += 2;  /* ADD.W #5, D0 */
    m68k_write16(&cpu, pc, 0x0005); pc += 2;
    m68k_write16(&cpu, pc, 0x4E40); pc += 2;  /* TRAP #0 */

    run_exit();

    assert((cpu.d[0] & 0xFFFF) == 15);
    printf("  PASS: sum_program\n");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Step 3 tests — Branches, jumps, subroutines
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── Test: BRA (8-bit displacement) ─────────────────────────────────────── */

static void test_bra(void)
{
    setup();
    /* BRA.S +4 → 6004  (skip 4 bytes forward from PC after opcode) */
    m68k_write16(&cpu, 0x1000, 0x6004);
    /* These two words are skipped */
    m68k_write16(&cpu, 0x1002, 0x4AFC);  /* ILLEGAL (should not execute) */
    m68k_write16(&cpu, 0x1004, 0x4AFC);  /* ILLEGAL */
    /* Land here: MOVEQ #$42, D0; TRAP #0 */
    m68k_write16(&cpu, 0x1006, 0x7042);
    m68k_write16(&cpu, 0x1008, 0x4E40);

    run_exit();

    assert(cpu.d[0] == 0x42);
    printf("  PASS: bra\n");
}

/* ── Test: BRA.W (16-bit displacement) ──────────────────────────────────── */

static void test_bra_w(void)
{
    setup();
    /* BRA.W disp16 → 6000 0100 (jump to PC+0x100 = 0x1002+0x100 = 0x1102) */
    m68k_write16(&cpu, 0x1000, 0x6000);
    m68k_write16(&cpu, 0x1002, 0x0100);
    /* Target at 0x1102 */
    m68k_write16(&cpu, 0x1102, 0x7042);
    m68k_write16(&cpu, 0x1104, 0x4E40);

    run_exit();

    assert(cpu.d[0] == 0x42);
    printf("  PASS: bra_w\n");
}

/* ── Test: BSR + RTS ────────────────────────────────────────────────────── */

static void test_bsr(void)
{
    setup();
    /* BSR.S +6 → 6106 (call subroutine at PC+6 = 0x1002+6 = 0x1008) */
    m68k_write16(&cpu, 0x1000, 0x6106);
    /* Return here: TRAP #0 */
    m68k_write16(&cpu, 0x1002, 0x4E40);
    /* Subroutine at 0x1008: MOVEQ #$55, D0; RTS */
    m68k_write16(&cpu, 0x1008, 0x7055);
    m68k_write16(&cpu, 0x100A, 0x4E75);

    run_exit();

    assert(cpu.d[0] == 0x55);
    assert(cpu.pc == 0x1004);  /* after TRAP #0 */
    printf("  PASS: bsr\n");
}

/* ── Test: BEQ (branch if equal / Z set) ───────────────────────────────── */

static void test_beq(void)
{
    setup();
    cpu.d[0] = 42;
    cpu.d[1] = 42;
    /* CMP.L D1, D0 → B081 */
    m68k_write16(&cpu, 0x1000, 0xB081);
    /* BEQ.S +4 → 6704 */
    m68k_write16(&cpu, 0x1002, 0x6704);
    /* Not taken: MOVEQ #0, D2 + TRAP */
    m68k_write16(&cpu, 0x1004, 0x7400);
    m68k_write16(&cpu, 0x1006, 0x4E40);
    /* Taken: MOVEQ #1, D2 + TRAP */
    m68k_write16(&cpu, 0x1008, 0x7401);
    m68k_write16(&cpu, 0x100A, 0x4E40);

    run_exit();

    assert(cpu.d[2] == 1);  /* branch was taken */
    printf("  PASS: beq\n");
}

/* ── Test: BNE (branch if not equal) ────────────────────────────────────── */

static void test_bne(void)
{
    setup();
    cpu.d[0] = 10;
    cpu.d[1] = 20;
    /* CMP.L D1, D0 → B081 */
    m68k_write16(&cpu, 0x1000, 0xB081);
    /* BNE.S +4 → 6604 */
    m68k_write16(&cpu, 0x1002, 0x6604);
    /* Not taken path */
    m68k_write16(&cpu, 0x1004, 0x7400);
    m68k_write16(&cpu, 0x1006, 0x4E40);
    /* Taken path */
    m68k_write16(&cpu, 0x1008, 0x7401);
    m68k_write16(&cpu, 0x100A, 0x4E40);

    run_exit();

    assert(cpu.d[2] == 1);
    printf("  PASS: bne\n");
}

/* ── Test: BGT/BLE (signed greater than / less or equal) ─────────────── */

static void test_bgt_ble(void)
{
    setup();
    cpu.d[0] = 10;
    cpu.d[1] = 5;
    /* CMP.L D1, D0 → B081 (D0 - D1 = 10 - 5 = 5 > 0) */
    m68k_write16(&cpu, 0x1000, 0xB081);
    /* BGT.S +4 → 6E04 */
    m68k_write16(&cpu, 0x1002, 0x6E04);
    m68k_write16(&cpu, 0x1004, 0x7400);
    m68k_write16(&cpu, 0x1006, 0x4E40);
    m68k_write16(&cpu, 0x1008, 0x7401);
    m68k_write16(&cpu, 0x100A, 0x4E40);

    run_exit();

    assert(cpu.d[2] == 1);  /* BGT taken */

    /* Now test BLE: D0 == D1 → should take */
    setup();
    cpu.d[0] = 5;
    cpu.d[1] = 5;
    m68k_write16(&cpu, 0x1000, 0xB081);
    /* BLE.S +4 → 6F04 */
    m68k_write16(&cpu, 0x1002, 0x6F04);
    m68k_write16(&cpu, 0x1004, 0x7400);
    m68k_write16(&cpu, 0x1006, 0x4E40);
    m68k_write16(&cpu, 0x1008, 0x7401);
    m68k_write16(&cpu, 0x100A, 0x4E40);

    run_exit();

    assert(cpu.d[2] == 1);  /* BLE taken */
    printf("  PASS: bgt_ble\n");
}

/* ── Test: BCC/BCS (unsigned carry clear/set) ──────────────────────────── */

static void test_bcc_bcs(void)
{
    setup();
    cpu.d[0] = 0x100;
    cpu.d[1] = 0x50;
    /* CMP.L D1, D0 → B081 (0x100 - 0x50, no carry) */
    m68k_write16(&cpu, 0x1000, 0xB081);
    /* BCC.S +4 → 6404 (carry clear → take) */
    m68k_write16(&cpu, 0x1002, 0x6404);
    m68k_write16(&cpu, 0x1004, 0x7400);
    m68k_write16(&cpu, 0x1006, 0x4E40);
    m68k_write16(&cpu, 0x1008, 0x7401);
    m68k_write16(&cpu, 0x100A, 0x4E40);

    run_exit();

    assert(cpu.d[2] == 1);  /* BCC taken */
    printf("  PASS: bcc_bcs\n");
}

/* ── Test: BMI/BPL (negative/positive) ──────────────────────────────────── */

static void test_bmi_bpl(void)
{
    setup();
    /* TST.L D0 with D0=-1 → N set */
    cpu.d[0] = 0xFFFFFFFF;
    m68k_write16(&cpu, 0x1000, 0x4A80);  /* TST.L D0 */
    /* BMI.S +4 → 6B04 */
    m68k_write16(&cpu, 0x1002, 0x6B04);
    m68k_write16(&cpu, 0x1004, 0x7400);
    m68k_write16(&cpu, 0x1006, 0x4E40);
    m68k_write16(&cpu, 0x1008, 0x7401);
    m68k_write16(&cpu, 0x100A, 0x4E40);

    run_exit();

    assert(cpu.d[2] == 1);  /* BMI taken */
    printf("  PASS: bmi_bpl\n");
}

/* ── Test: Scc (SEQ/SNE) ────────────────────────────────────────────────── */

static void test_scc(void)
{
    setup();
    cpu.d[0] = 0;
    /* TST.L D0 → Z set */
    m68k_write16(&cpu, 0x1000, 0x4A80);
    /* SEQ D1 → 57C1  (0101 0111 11 000 001, cc=7=EQ) */
    m68k_write16(&cpu, 0x1002, 0x57C1);
    /* SNE D2 → 56C2  (0101 0110 11 000 010, cc=6=NE) */
    m68k_write16(&cpu, 0x1004, 0x56C2);
    m68k_write16(&cpu, 0x1006, 0x4E40);

    run_exit();

    assert((cpu.d[1] & 0xFF) == 0xFF);  /* SEQ: Z set → $FF */
    assert((cpu.d[2] & 0xFF) == 0x00);  /* SNE: Z set → $00 */
    printf("  PASS: scc\n");
}

/* ── Test: DBcc (DBRA = DBF loop) ───────────────────────────────────────── */

static void test_dbra(void)
{
    setup();
    /* Loop: D0 = 0, D1 = 4 (loop 5 times: 4,3,2,1,0,-1)
     * loop: ADDQ.L #1, D0; DBRA D1, loop */
    cpu.d[0] = 0;
    cpu.d[1] = 4;
    uint32_t pc = 0x1000;
    /* ADDQ.L #1, D0 → 5280 */
    m68k_write16(&cpu, pc, 0x5280); pc += 2;
    /* DBF D1, -4 → 51C9 FFFC
     * cc=1 (F=always false) → always decrement+branch
     * disp = -4 (FFFC) relative to PC of disp word = 0x1004 → 0x1000 */
    m68k_write16(&cpu, pc, 0x51C9); pc += 2;
    m68k_write16(&cpu, pc, 0xFFFC); pc += 2;
    m68k_write16(&cpu, pc, 0x4E40); /* TRAP #0 exit */

    run_exit();

    assert(cpu.d[0] == 5);     /* incremented 5 times */
    assert((cpu.d[1] & 0xFFFF) == 0xFFFF);  /* decremented past 0 to -1 */
    printf("  PASS: dbra\n");
}

/* ── Test: DBcc with condition true (no loop) ───────────────────────────── */

static void test_dbcc_true(void)
{
    setup();
    cpu.d[0] = 10;
    cpu.d[1] = 5;
    /* CMP.L D1, D0 → B081 (sets flags: 10 != 5, NE is true) */
    m68k_write16(&cpu, 0x1000, 0xB081);
    /* DBNE D1, -6 → 56C9 FFFA
     * cc=6 (NE) → condition true → exit loop, don't decrement */
    m68k_write16(&cpu, 0x1002, 0x56C9);
    m68k_write16(&cpu, 0x1004, 0xFFFA);
    m68k_write16(&cpu, 0x1006, 0x4E40);

    run_exit();

    assert(cpu.d[1] == 5);  /* not decremented */
    printf("  PASS: dbcc_true\n");
}

/* ── Test: LINK/UNLK ────────────────────────────────────────────────────── */

static void test_link_unlk(void)
{
    setup();
    cpu.a[6] = 0xAAAAAAAA;
    uint32_t sp_before = cpu.a[7];
    /* LINK A6, #-8 → 4E56 FFF8 */
    m68k_write16(&cpu, 0x1000, 0x4E56);
    m68k_write16(&cpu, 0x1002, 0xFFF8);
    /* UNLK A6 → 4E5E */
    m68k_write16(&cpu, 0x1004, 0x4E5E);
    m68k_write16(&cpu, 0x1006, 0x4E40);

    run_exit();

    /* After LINK+UNLK, A6 and SP should be restored */
    assert(cpu.a[6] == 0xAAAAAAAA);
    assert(cpu.a[7] == sp_before);
    printf("  PASS: link_unlk\n");
}

/* ── Test: LINK frame pointer pattern ───────────────────────────────────── */

static void test_link_frame(void)
{
    setup();
    uint32_t sp_before = cpu.a[7];
    cpu.a[6] = 0x12345678;
    /* LINK A6, #-16 → 4E56 FFF0 */
    m68k_write16(&cpu, 0x1000, 0x4E56);
    m68k_write16(&cpu, 0x1002, 0xFFF0);
    m68k_write16(&cpu, 0x1004, 0x4E40);

    run_exit();

    /* Old A6 pushed to stack */
    uint32_t fp = cpu.a[6];
    assert(fp == sp_before - 4);
    assert(m68k_read32(&cpu, fp) == 0x12345678);
    /* SP = FP - 16 */
    assert(cpu.a[7] == fp - 16);
    printf("  PASS: link_frame\n");
}

/* ── Test: Sum 1..10 with a real loop ───────────────────────────────────── */

static void test_sum_loop(void)
{
    setup();
    /*
     * MOVEQ #0, D0       ; sum = 0
     * MOVEQ #10, D1      ; counter = 10
     * loop:
     *   ADD.L D1, D0     ; sum += counter
     *   SUBQ.L #1, D1    ; counter--
     *   BNE.S loop       ; if counter != 0, loop
     * TRAP #0            ; exit
     */
    uint32_t pc = 0x1000;
    m68k_write16(&cpu, pc, 0x7000); pc += 2;  /* MOVEQ #0, D0 */
    m68k_write16(&cpu, pc, 0x720A); pc += 2;  /* MOVEQ #10, D1 */
    /* loop: at 0x1004 */
    m68k_write16(&cpu, pc, 0xD081); pc += 2;  /* ADD.L D1, D0 */
    m68k_write16(&cpu, pc, 0x5381); pc += 2;  /* SUBQ.L #1, D1 */
    /* BNE.S -6 → 66FA (disp = -6, from PC=0x100A to 0x1004) */
    m68k_write16(&cpu, pc, 0x66FA); pc += 2;
    m68k_write16(&cpu, pc, 0x4E40);           /* TRAP #0 */

    run_exit();

    assert(cpu.d[0] == 55);  /* 1+2+...+10 = 55 */
    assert(cpu.d[1] == 0);
    printf("  PASS: sum_loop\n");
}

/* ── Test: Fibonacci with BSR ───────────────────────────────────────────── */

static void test_bsr_fib(void)
{
    setup();
    /*
     * Iterative fib(10) using BSR:
     * Main:
     *   MOVEQ #10, D0     ; n = 10
     *   BSR.S fib
     *   TRAP #0
     * fib:
     *   MOVEQ #0, D1      ; a = 0
     *   MOVEQ #1, D2      ; b = 1
     *   SUBQ.L #1, D0     ; n--
     *   BEQ.S done
     * loop:
     *   MOVE.L D2, D3     ; temp = b
     *   ADD.L D1, D2      ; b = a + b
     *   MOVE.L D3, D1     ; a = temp
     *   SUBQ.L #1, D0     ; n--
     *   BNE.S loop
     * done:
     *   MOVE.L D2, D0     ; result = b
     *   RTS
     */
    uint32_t pc = 0x1000;
    m68k_write16(&cpu, pc, 0x700A); pc += 2;  /* MOVEQ #10, D0 */
    m68k_write16(&cpu, pc, 0x6104); pc += 2;  /* BSR.S +4 → fib */
    m68k_write16(&cpu, pc, 0x4E40); pc += 2;  /* TRAP #0 */
    m68k_write16(&cpu, pc, 0x4E71); pc += 2;  /* NOP (padding) */
    /* fib: at 0x1008 */
    m68k_write16(&cpu, pc, 0x7200); pc += 2;  /* MOVEQ #0, D1 (a) */
    m68k_write16(&cpu, pc, 0x7401); pc += 2;  /* MOVEQ #1, D2 (b) */
    m68k_write16(&cpu, pc, 0x5380); pc += 2;  /* SUBQ.L #1, D0 */
    m68k_write16(&cpu, pc, 0x670C); pc += 2;  /* BEQ.S +12 → done */
    /* loop: at 0x1010 */
    m68k_write16(&cpu, pc, 0x2602); pc += 2;  /* MOVE.L D2, D3 */
    m68k_write16(&cpu, pc, 0xD481); pc += 2;  /* ADD.L D1, D2 */
    m68k_write16(&cpu, pc, 0x2203); pc += 2;  /* MOVE.L D3, D1 */
    m68k_write16(&cpu, pc, 0x5380); pc += 2;  /* SUBQ.L #1, D0 */
    m68k_write16(&cpu, pc, 0x66F6); pc += 2;  /* BNE.S -10 → loop */
    /* done: at 0x101A */
    m68k_write16(&cpu, pc, 0x2002); pc += 2;  /* MOVE.L D2, D0 */
    m68k_write16(&cpu, pc, 0x4E75); pc += 2;  /* RTS */

    run_exit();

    assert(cpu.d[0] == 55);  /* fib(10) = 55 */
    printf("  PASS: bsr_fib\n");
}

/* ── Test: backward BRA ─────────────────────────────────────────────────── */

static void test_bra_backward(void)
{
    setup();
    /* Jump forward to setup, then backward BRA to exit */
    /* BRA.S +4 → 6004 (to 0x1006) */
    m68k_write16(&cpu, 0x1000, 0x6004);
    /* exit point: TRAP #0 */
    m68k_write16(&cpu, 0x1002, 0x4E40);
    m68k_write16(&cpu, 0x1004, 0x4E71);  /* NOP (skipped) */
    /* At 0x1006: MOVEQ #$77, D0 */
    m68k_write16(&cpu, 0x1006, 0x7077);
    /* BRA.S -8 → 60F8 (to 0x1002) */
    m68k_write16(&cpu, 0x1008, 0x60F8);

    run_exit();

    assert(cpu.d[0] == 0x77);
    printf("  PASS: bra_backward\n");
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

    /* Step 2 */
    test_add_l_dn();
    test_add_l_overflow();
    test_add_w_carry();
    test_addi();
    test_addq();
    test_addq_an();
    test_sub_l_dn();
    test_sub_l_borrow();
    test_subi();
    test_subq();
    test_cmp_l();
    test_cmpi();
    test_neg();
    test_tst();
    test_ext();
    test_swap();
    test_mulu();
    test_muls();
    test_divu();
    test_divs();
    test_divu_zero();
    test_adda();
    test_suba_w();
    test_cmpa();
    test_cmpm();
    test_add_sub_mem();
    test_sum_program();

    /* Step 3 */
    test_bra();
    test_bra_w();
    test_bsr();
    test_beq();
    test_bne();
    test_bgt_ble();
    test_bcc_bcs();
    test_bmi_bpl();
    test_scc();
    test_dbra();
    test_dbcc_true();
    test_link_unlk();
    test_link_frame();
    test_sum_loop();
    test_bsr_fib();
    test_bra_backward();

    printf("All 75 tests passed.\n");
    return 0;
}
