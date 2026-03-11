/*
 * test_ecpu_m68k.c — Host-side unit tests for the m68k eCPU core (Step 1)
 *
 * Tests: MOVE.B/W/L, MOVEQ, LEA, CLR, NOP, STOP, ILLEGAL,
 *        A-line/F-line traps, EA modes 0–4, JSR/RTS, TRAP #n.
 * Step 2: ADD/SUB/CMP/NEG/TST/EXT/MULU/MULS/DIVU/DIVS,
 *         ADDI/SUBI/CMPI, ADDQ/SUBQ.
 * Step 3: Bcc/BRA/BSR, DBcc, Scc, LINK, UNLK.
 * Step 4: AND/OR/EOR/NOT, shifts/rotates, bit ops, EXG.
 * Step 5: MOVEM, PEA, MOVE to/from SR/CCR, MOVE USP.
 * Step 6: ADDX, SUBX, ABCD, SBCD, NBCD, TAS, CHK.
 * Step 7: Integration tests, edge cases, all Bcc conditions.
 * Step 8: PPAP cross-arch personality (TRAP #0 → syscall dispatch).
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "kernel/ecpu/ecpu.h"
#include "kernel/ecpu/ecpu_m68k.h"
#include "common/syscall_nr.h"

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

/* ══════════════════════════════════════════════════════════════════════════
 * Step 4 tests — Logic, shifts, bit operations
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── Test: AND.L Dn, Dn ─────────────────────────────────────────────────── */

static void test_and_l(void)
{
    setup();
    cpu.d[0] = 0xFF00FF00;
    cpu.d[1] = 0x12345678;
    /* AND.L D1, D0 → C081  (1100 000 010 000 001) */
    m68k_write16(&cpu, 0x1000, 0xC081);
    m68k_write16(&cpu, 0x1002, 0x4E40);

    run_exit();

    assert(cpu.d[0] == 0x12005600);
    assert(!(cpu.sr & M68K_FLAG_Z));
    assert(!(cpu.sr & M68K_FLAG_N));
    printf("  PASS: and_l\n");
}

/* ── Test: ANDI.L #imm, Dn ──────────────────────────────────────────────── */

static void test_andi(void)
{
    setup();
    cpu.d[0] = 0xFFFF0000;
    /* ANDI.L #$00FF00FF, D0 → 0280 00FF 00FF */
    m68k_write16(&cpu, 0x1000, 0x0280);
    m68k_write32(&cpu, 0x1002, 0x00FF00FF);
    m68k_write16(&cpu, 0x1006, 0x4E40);

    run_exit();

    assert(cpu.d[0] == 0x00FF0000);
    printf("  PASS: andi\n");
}

/* ── Test: OR.L Dn, Dn ──────────────────────────────────────────────────── */

static void test_or_l(void)
{
    setup();
    cpu.d[0] = 0xFF000000;
    cpu.d[1] = 0x000000FF;
    /* OR.L D1, D0 → 8081  (1000 000 010 000 001) */
    m68k_write16(&cpu, 0x1000, 0x8081);
    m68k_write16(&cpu, 0x1002, 0x4E40);

    run_exit();

    assert(cpu.d[0] == 0xFF0000FF);
    assert(cpu.sr & M68K_FLAG_N);
    printf("  PASS: or_l\n");
}

/* ── Test: ORI.B #imm, Dn ───────────────────────────────────────────────── */

static void test_ori(void)
{
    setup();
    cpu.d[0] = 0x10;
    /* ORI.B #$0F, D0 → 0000 000F */
    m68k_write16(&cpu, 0x1000, 0x0000);
    m68k_write16(&cpu, 0x1002, 0x000F);
    m68k_write16(&cpu, 0x1004, 0x4E40);

    run_exit();

    assert((cpu.d[0] & 0xFF) == 0x1F);
    printf("  PASS: ori\n");
}

/* ── Test: EOR.L Dn, Dn ─────────────────────────────────────────────────── */

static void test_eor_l(void)
{
    setup();
    cpu.d[0] = 0xAAAAAAAA;
    cpu.d[1] = 0xFFFFFFFF;
    /* EOR.L D1, D0 → B381  (1011 001 110 000 000)
     * Wait: EOR Dn, <ea> → 1011 rrr 1ss ea
     * D1 to D0: reg=1, size=10(long), ea=000 000
     * 1011 001 110 000 000 = 0xB380 */
    m68k_write16(&cpu, 0x1000, 0xB380);
    m68k_write16(&cpu, 0x1002, 0x4E40);

    run_exit();

    assert(cpu.d[0] == 0x55555555);
    assert(!(cpu.sr & M68K_FLAG_N));
    assert(!(cpu.sr & M68K_FLAG_Z));
    printf("  PASS: eor_l\n");
}

/* ── Test: EORI.W #imm, Dn ──────────────────────────────────────────────── */

static void test_eori(void)
{
    setup();
    cpu.d[0] = 0xFF00;
    /* EORI.W #$FFFF, D0 → 0A40 FFFF */
    m68k_write16(&cpu, 0x1000, 0x0A40);
    m68k_write16(&cpu, 0x1002, 0xFFFF);
    m68k_write16(&cpu, 0x1004, 0x4E40);

    run_exit();

    assert((cpu.d[0] & 0xFFFF) == 0x00FF);
    printf("  PASS: eori\n");
}

/* ── Test: NOT.L ─────────────────────────────────────────────────────────── */

static void test_not(void)
{
    setup();
    cpu.d[0] = 0x00000000;
    /* NOT.L D0 → 4680  (0100 0110 10 000 000) */
    m68k_write16(&cpu, 0x1000, 0x4680);
    m68k_write16(&cpu, 0x1002, 0x4E40);

    run_exit();

    assert(cpu.d[0] == 0xFFFFFFFF);
    assert(cpu.sr & M68K_FLAG_N);
    assert(!(cpu.sr & M68K_FLAG_Z));
    printf("  PASS: not\n");
}

/* ── Test: LSL/LSR register ──────────────────────────────────────────────── */

static void test_lsl_lsr(void)
{
    setup();
    cpu.d[0] = 0x01;
    /* LSL.L #4, D0 → E988  (1110 100 1 10 0 01 000)
     * cnt=4, left=1, size=10, i=0, type=01(LS), reg=0 */
    m68k_write16(&cpu, 0x1000, 0xE988);
    m68k_write16(&cpu, 0x1002, 0x4E40);

    run_exit();

    assert(cpu.d[0] == 0x10);
    assert(!(cpu.sr & M68K_FLAG_C));

    /* LSR.L #4, D0 */
    setup();
    cpu.d[0] = 0x10;
    /* LSR.L #4, D0 → E888  (1110 100 0 10 0 01 000) */
    m68k_write16(&cpu, 0x1000, 0xE888);
    m68k_write16(&cpu, 0x1002, 0x4E40);

    run_exit();

    assert(cpu.d[0] == 0x01);
    printf("  PASS: lsl_lsr\n");
}

/* ── Test: ASR (arithmetic shift right, preserves sign) ──────────────────── */

static void test_asr(void)
{
    setup();
    cpu.d[0] = 0x80000000;  /* -2147483648 */
    /* ASR.L #1, D0 → E280  (1110 001 0 10 0 00 000)
     * cnt=1, right, size=long, i=0, type=00(AS) */
    m68k_write16(&cpu, 0x1000, 0xE280);
    m68k_write16(&cpu, 0x1002, 0x4E40);

    run_exit();

    assert(cpu.d[0] == 0xC0000000);  /* sign preserved */
    assert(cpu.sr & M68K_FLAG_N);
    printf("  PASS: asr\n");
}

/* ── Test: ASL (arithmetic shift left, V flag on sign change) ────────────── */

static void test_asl(void)
{
    setup();
    cpu.d[0] = 0x40000000;
    /* ASL.L #1, D0 → E380  (1110 001 1 10 0 00 000) */
    m68k_write16(&cpu, 0x1000, 0xE380);
    m68k_write16(&cpu, 0x1002, 0x4E40);

    run_exit();

    assert(cpu.d[0] == 0x80000000);
    assert(cpu.sr & M68K_FLAG_V);  /* sign changed */
    assert(cpu.sr & M68K_FLAG_N);
    printf("  PASS: asl\n");
}

/* ── Test: ROL/ROR ───────────────────────────────────────────────────────── */

static void test_rol_ror(void)
{
    setup();
    cpu.d[0] = 0x80000001;
    /* ROL.L #1, D0 → E398  (1110 001 1 10 0 11 000) */
    m68k_write16(&cpu, 0x1000, 0xE398);
    m68k_write16(&cpu, 0x1002, 0x4E40);

    run_exit();

    assert(cpu.d[0] == 0x00000003);  /* MSB rotated to bit 0 */

    /* ROR.L #1, D0 */
    setup();
    cpu.d[0] = 0x00000003;
    /* ROR.L #1, D0 → E298  (1110 001 0 10 0 11 000) */
    m68k_write16(&cpu, 0x1000, 0xE298);
    m68k_write16(&cpu, 0x1002, 0x4E40);

    run_exit();

    assert(cpu.d[0] == 0x80000001);
    printf("  PASS: rol_ror\n");
}

/* ── Test: LSL with register count ───────────────────────────────────────── */

static void test_lsl_reg_count(void)
{
    setup();
    cpu.d[0] = 1;
    cpu.d[1] = 8;
    /* LSL.L D1, D0 → E3A8  (1110 001 1 10 1 01 000)
     * cnt_reg=1, left=1, size=long, i=1, type=01(LS), reg=0 */
    m68k_write16(&cpu, 0x1000, 0xE3A8);
    m68k_write16(&cpu, 0x1002, 0x4E40);

    run_exit();

    assert(cpu.d[0] == 256);
    printf("  PASS: lsl_reg_count\n");
}

/* ── Test: BTST static (immediate bit number) ───────────────────────────── */

static void test_btst_static(void)
{
    setup();
    cpu.d[0] = 0x00000010;  /* bit 4 set */
    /* BTST #4, D0 → 0800 0004 */
    m68k_write16(&cpu, 0x1000, 0x0800);
    m68k_write16(&cpu, 0x1002, 0x0004);
    m68k_write16(&cpu, 0x1004, 0x4E40);

    run_exit();

    assert(!(cpu.sr & M68K_FLAG_Z));  /* bit 4 is set → Z clear */

    /* Test bit 5 (not set) */
    setup();
    cpu.d[0] = 0x00000010;
    m68k_write16(&cpu, 0x1000, 0x0800);
    m68k_write16(&cpu, 0x1002, 0x0005);
    m68k_write16(&cpu, 0x1004, 0x4E40);

    run_exit();

    assert(cpu.sr & M68K_FLAG_Z);  /* bit 5 is clear → Z set */
    printf("  PASS: btst_static\n");
}

/* ── Test: BTST dynamic (Dn bit number) ──────────────────────────────────── */

static void test_btst_dynamic(void)
{
    setup();
    cpu.d[0] = 0x00000080;  /* bit 7 set */
    cpu.d[1] = 7;
    /* BTST D1, D0 → 0300  (0000 001 1 00 000 000) */
    m68k_write16(&cpu, 0x1000, 0x0300);
    m68k_write16(&cpu, 0x1002, 0x4E40);

    run_exit();

    assert(!(cpu.sr & M68K_FLAG_Z));
    printf("  PASS: btst_dynamic\n");
}

/* ── Test: BSET/BCLR/BCHG ───────────────────────────────────────────────── */

static void test_bset_bclr_bchg(void)
{
    setup();
    cpu.d[0] = 0;
    /* BSET #3, D0 → 08C0 0003 */
    m68k_write16(&cpu, 0x1000, 0x08C0);
    m68k_write16(&cpu, 0x1002, 0x0003);
    m68k_write16(&cpu, 0x1004, 0x4E40);

    run_exit();

    assert(cpu.d[0] == 0x08);
    assert(cpu.sr & M68K_FLAG_Z);  /* was 0 before set */

    /* BCLR #3, D0 → 0880 0003 */
    setup();
    cpu.d[0] = 0x08;
    m68k_write16(&cpu, 0x1000, 0x0880);
    m68k_write16(&cpu, 0x1002, 0x0003);
    m68k_write16(&cpu, 0x1004, 0x4E40);

    run_exit();

    assert(cpu.d[0] == 0);
    assert(!(cpu.sr & M68K_FLAG_Z));  /* was 1 before clear */

    /* BCHG #0, D0 → 0840 0000 */
    setup();
    cpu.d[0] = 0;
    m68k_write16(&cpu, 0x1000, 0x0840);
    m68k_write16(&cpu, 0x1002, 0x0000);
    m68k_write16(&cpu, 0x1004, 0x4E40);

    run_exit();

    assert(cpu.d[0] == 1);
    assert(cpu.sr & M68K_FLAG_Z);  /* was 0 before change */
    printf("  PASS: bset_bclr_bchg\n");
}

/* ── Test: EXG ───────────────────────────────────────────────────────────── */

static void test_exg(void)
{
    setup();
    cpu.d[0] = 0xAAAAAAAA;
    cpu.d[1] = 0xBBBBBBBB;
    /* EXG D0, D1 → C141  (1100 000 101000 001) */
    m68k_write16(&cpu, 0x1000, 0xC141);
    emit_stop(0x1002);

    run();

    assert(cpu.d[0] == 0xBBBBBBBB);
    assert(cpu.d[1] == 0xAAAAAAAA);

    /* EXG D0, A0 */
    setup();
    cpu.d[0] = 0x11111111;
    cpu.a[0] = 0x22222222;
    /* EXG D0, A0 → C188  (1100 000 110001 000) */
    m68k_write16(&cpu, 0x1000, 0xC188);
    emit_stop(0x1002);

    run();

    assert(cpu.d[0] == 0x22222222);
    assert(cpu.a[0] == 0x11111111);
    printf("  PASS: exg\n");
}

/* ── Test: ANDI/ORI/EORI to CCR ──────────────────────────────────────────── */

static void test_logic_ccr(void)
{
    setup();
    /* Set all CCR flags first via ORI to CCR */
    /* ORI #$1F, CCR → 003C 001F */
    m68k_write16(&cpu, 0x1000, 0x003C);
    m68k_write16(&cpu, 0x1002, 0x001F);
    /* ANDI #$04, CCR → 023C 0004 (keep only Z) */
    m68k_write16(&cpu, 0x1004, 0x023C);
    m68k_write16(&cpu, 0x1006, 0x0004);
    m68k_write16(&cpu, 0x1008, 0x4E40);

    run_exit();

    assert(cpu.sr & M68K_FLAG_Z);
    assert(!(cpu.sr & M68K_FLAG_N));
    assert(!(cpu.sr & M68K_FLAG_C));
    assert(!(cpu.sr & M68K_FLAG_V));
    assert(!(cpu.sr & M68K_FLAG_X));
    printf("  PASS: logic_ccr\n");
}

/* ── Test: OR/AND to memory ──────────────────────────────────────────────── */

static void test_or_and_mem(void)
{
    setup();
    cpu.a[0] = 0x2000;
    cpu.d[0] = 0x0F;
    m68k_write8(&cpu, 0x2000, 0xF0);
    /* OR.B D0, (A0) → 8110  (1000 000 100 010 000) */
    m68k_write16(&cpu, 0x1000, 0x8110);
    m68k_write16(&cpu, 0x1002, 0x4E40);

    run_exit();

    assert(m68k_read8(&cpu, 0x2000) == 0xFF);

    /* AND.B D0, (A0) — D0 still 0x0F */
    setup();
    cpu.a[0] = 0x2000;
    cpu.d[0] = 0x0F;
    m68k_write8(&cpu, 0x2000, 0xFF);
    /* AND.B D0, (A0) → C110  (1100 000 100 010 000) */
    m68k_write16(&cpu, 0x1000, 0xC110);
    m68k_write16(&cpu, 0x1002, 0x4E40);

    run_exit();

    assert(m68k_read8(&cpu, 0x2000) == 0x0F);
    printf("  PASS: or_and_mem\n");
}

/* ── Test: LSR carry flag ────────────────────────────────────────────────── */

static void test_lsr_carry(void)
{
    setup();
    cpu.d[0] = 0x03;  /* binary 11 */
    /* LSR.L #1, D0 → E480  (1110 010 0 10 0 01 000) — wait, cnt=2 */
    /* LSR.L #1, D0 → E288  (1110 001 0 10 0 01 000) */
    m68k_write16(&cpu, 0x1000, 0xE288);
    m68k_write16(&cpu, 0x1002, 0x4E40);

    run_exit();

    assert(cpu.d[0] == 0x01);
    assert(cpu.sr & M68K_FLAG_C);  /* bit 0 was 1, shifted out */
    assert(cpu.sr & M68K_FLAG_X);
    printf("  PASS: lsr_carry\n");
}

/* ── Test: Shift count 0 ────────────────────────────────────────────────── */

static void test_shift_zero(void)
{
    setup();
    cpu.d[0] = 0x12345678;
    cpu.d[1] = 0;  /* count = 0 */
    /* LSL.L D1, D0 → E3A8  (1110 001 1 10 1 01 000) — wait, that's D1 cnt */
    /* Actually I need cnt_reg in bits 11-9. D1 → bits 11-9 = 001 */
    /* LSL.L D1, D0 → E3A8 (1110 001 1 10 1 01 000) */
    m68k_write16(&cpu, 0x1000, 0xE3A8);
    m68k_write16(&cpu, 0x1002, 0x4E40);

    run_exit();

    assert(cpu.d[0] == 0x12345678);  /* unchanged */
    assert(!(cpu.sr & M68K_FLAG_C));  /* C cleared on count 0 */
    printf("  PASS: shift_zero\n");
}

/* ── Test: BTST on memory (byte) ─────────────────────────────────────────── */

static void test_btst_mem(void)
{
    setup();
    cpu.a[0] = 0x2000;
    m68k_write8(&cpu, 0x2000, 0x80);  /* bit 7 set */
    cpu.d[1] = 7;
    /* BTST D1, (A0) → 0310  (0000 001 1 00 010 000) */
    m68k_write16(&cpu, 0x1000, 0x0310);
    m68k_write16(&cpu, 0x1002, 0x4E40);

    run_exit();

    assert(!(cpu.sr & M68K_FLAG_Z));  /* bit 7 set → Z clear */
    printf("  PASS: btst_mem\n");
}

/* ── Test: Bit manipulation program ──────────────────────────────────────── */

static void test_bit_program(void)
{
    setup();
    /*
     * D0 = 0x00
     * BSET #0, D0      ; D0 = 0x01
     * BSET #4, D0      ; D0 = 0x11
     * ORI.B #$80, D0   ; D0 = 0x91
     * ANDI.B #$F0, D0  ; D0 = 0x90
     * NOT.B D0         ; D0 = 0x6F
     * TRAP #0
     */
    cpu.d[0] = 0;
    uint32_t pc = 0x1000;
    m68k_write16(&cpu, pc, 0x08C0); pc += 2;  /* BSET #0, D0 */
    m68k_write16(&cpu, pc, 0x0000); pc += 2;
    m68k_write16(&cpu, pc, 0x08C0); pc += 2;  /* BSET #4, D0 */
    m68k_write16(&cpu, pc, 0x0004); pc += 2;
    m68k_write16(&cpu, pc, 0x0000); pc += 2;  /* ORI.B #$80, D0 */
    m68k_write16(&cpu, pc, 0x0080); pc += 2;
    m68k_write16(&cpu, pc, 0x0200); pc += 2;  /* ANDI.B #$F0, D0 */
    m68k_write16(&cpu, pc, 0x00F0); pc += 2;
    m68k_write16(&cpu, pc, 0x4600); pc += 2;  /* NOT.B D0 */
    m68k_write16(&cpu, pc, 0x4E40);           /* TRAP #0 */

    run_exit();

    assert((cpu.d[0] & 0xFF) == 0x6F);
    printf("  PASS: bit_program\n");
}

/* ── Step 5: MOVEM, PEA, MOVE to/from SR/CCR, MOVE USP ──────────────────── */

/* ── Test: MOVEM.L reg→mem (pre-decrement) ─────────────────────────────── */

static void test_movem_predec(void)
{
    setup();
    cpu.d[0] = 0x11111111;
    cpu.d[1] = 0x22222222;
    cpu.a[0] = 0x33333333;
    cpu.a[1] = 0x44444444;
    /* MOVEM.L D0-D1/A0-A1, -(A7)
     * Opcode: 48E7  (0100 1000 1 1 100 111 = MOVEM.L reg→mem -(A7))
     * Mask (reversed for -(An)): A7=bit0, A6=bit1, ..., A1=bit6, A0=bit7,
     *                            D7=bit8, ..., D1=bit14, D0=bit15
     * D0=bit15, D1=bit14, A0=bit7, A1=bit6 → 0xC0C0 */
    m68k_write16(&cpu, 0x1000, 0x48E7);
    m68k_write16(&cpu, 0x1002, 0xC0C0);
    /* TRAP #0 */
    m68k_write16(&cpu, 0x1004, 0x4E40);

    run_exit();

    /* Stack should have been decremented by 4 regs × 4 bytes = 16 */
    assert(cpu.a[7] == 0x10000 - 16);
    /* Memory layout (low→high): D0, D1, A0, A1 */
    assert(m68k_read32(&cpu, cpu.a[7])      == 0x11111111);  /* D0 */
    assert(m68k_read32(&cpu, cpu.a[7] + 4)  == 0x22222222);  /* D1 */
    assert(m68k_read32(&cpu, cpu.a[7] + 8)  == 0x33333333);  /* A0 */
    assert(m68k_read32(&cpu, cpu.a[7] + 12) == 0x44444444);  /* A1 */
    printf("  PASS: movem_predec\n");
}

/* ── Test: MOVEM.L mem→reg (post-increment) ────────────────────────────── */

static void test_movem_postinc(void)
{
    setup();
    /* Store 4 longs at 0x2000 */
    m68k_write32(&cpu, 0x2000, 0xAAAAAAAA);
    m68k_write32(&cpu, 0x2004, 0xBBBBBBBB);
    m68k_write32(&cpu, 0x2008, 0xCCCCCCCC);
    m68k_write32(&cpu, 0x200C, 0xDDDDDDDD);
    cpu.a[0] = 0x2000;
    /* MOVEM.L (A0)+, D0-D1/A2-A3
     * Opcode: 4CD8 (0100 1100 1 1 011 000 = MOVEM.L mem→reg (A0)+)
     * Mask (normal order): D0=bit0, D1=bit1, A2=bit10, A3=bit11
     * → 0x0C03 */
    m68k_write16(&cpu, 0x1000, 0x4CD8);
    m68k_write16(&cpu, 0x1002, 0x0C03);
    m68k_write16(&cpu, 0x1004, 0x4E40);

    run_exit();

    assert(cpu.d[0] == 0xAAAAAAAA);
    assert(cpu.d[1] == 0xBBBBBBBB);
    assert(cpu.a[2] == 0xCCCCCCCC);
    assert(cpu.a[3] == 0xDDDDDDDD);
    assert(cpu.a[0] == 0x2010);  /* advanced past 4 longs */
    printf("  PASS: movem_postinc\n");
}

/* ── Test: MOVEM roundtrip (save + restore) ────────────────────────────── */

static void test_movem_roundtrip(void)
{
    setup();
    cpu.d[2] = 0x12345678;
    cpu.d[3] = 0x9ABCDEF0;
    cpu.a[4] = 0xFEDCBA98;
    /* MOVEM.L D2-D3/A4, -(A7)  → save
     * Reversed mask: D2=bit13, D3=bit12, A4=bit3 → 0x3008 */
    m68k_write16(&cpu, 0x1000, 0x48E7);
    m68k_write16(&cpu, 0x1002, 0x3008);
    /* Clear registers */
    /* CLR.L D2: 4282 */
    m68k_write16(&cpu, 0x1004, 0x4282);
    /* CLR.L D3: 4283 */
    m68k_write16(&cpu, 0x1006, 0x4283);
    /* SUB.L A4,A4 is tricky, use LEA 0.W,A4 instead: 49F8 0000 */
    m68k_write16(&cpu, 0x1008, 0x49F8);
    m68k_write16(&cpu, 0x100A, 0x0000);
    /* MOVEM.L (A7)+, D2-D3/A4  → restore
     * Normal mask: D2=bit2, D3=bit3, A4=bit12 → 0x100C */
    m68k_write16(&cpu, 0x100C, 0x4CDF);
    m68k_write16(&cpu, 0x100E, 0x100C);
    m68k_write16(&cpu, 0x1010, 0x4E40);

    run_exit();

    assert(cpu.d[2] == 0x12345678);
    assert(cpu.d[3] == 0x9ABCDEF0);
    assert(cpu.a[4] == 0xFEDCBA98);
    assert(cpu.a[7] == 0x10000);  /* SP restored */
    printf("  PASS: movem_roundtrip\n");
}

/* ── Test: MOVEM.W with sign extension ─────────────────────────────────── */

static void test_movem_w(void)
{
    setup();
    /* Store 2 words at 0x2000: 0x0042, 0xFF80 */
    m68k_write16(&cpu, 0x2000, 0x0042);
    m68k_write16(&cpu, 0x2002, 0xFF80);
    cpu.a[0] = 0x2000;
    /* MOVEM.W (A0)+, D0-D1
     * Opcode: 4C98 (0100 1100 1 0 011 000 = MOVEM.W mem→reg (A0)+)
     * Mask: D0=bit0, D1=bit1 → 0x0003 */
    m68k_write16(&cpu, 0x1000, 0x4C98);
    m68k_write16(&cpu, 0x1002, 0x0003);
    m68k_write16(&cpu, 0x1004, 0x4E40);

    run_exit();

    assert(cpu.d[0] == 0x00000042);   /* positive, sign-extended */
    assert(cpu.d[1] == 0xFFFFFF80);   /* negative, sign-extended */
    assert(cpu.a[0] == 0x2004);       /* advanced by 2 words */
    printf("  PASS: movem_w\n");
}

/* ── Test: MOVEM.L to/from displacement ────────────────────────────────── */

static void test_movem_disp(void)
{
    setup();
    cpu.d[0] = 0x11110000;
    cpu.d[1] = 0x22220000;
    cpu.a[5] = 0x3000;
    /* MOVEM.L D0-D1, 8(A5)
     * Opcode: 48ED (0100 1000 1 1 101 101 = MOVEM.L reg→mem d16(A5))
     * Mask: D0=bit0, D1=bit1 → 0x0003
     * Displacement: 0x0008 */
    m68k_write16(&cpu, 0x1000, 0x48ED);
    m68k_write16(&cpu, 0x1002, 0x0003);
    m68k_write16(&cpu, 0x1004, 0x0008);
    /* Verify by loading back with MOVEM.L 8(A5), D4-D5
     * Opcode: 4CED (0100 1100 1 1 101 101 = MOVEM.L mem→reg d16(A5))
     * Mask: D4=bit4, D5=bit5 → 0x0030
     * Displacement: 0x0008 */
    m68k_write16(&cpu, 0x1006, 0x4CED);
    m68k_write16(&cpu, 0x1008, 0x0030);
    m68k_write16(&cpu, 0x100A, 0x0008);
    m68k_write16(&cpu, 0x100C, 0x4E40);

    run_exit();

    assert(cpu.d[4] == 0x11110000);
    assert(cpu.d[5] == 0x22220000);
    printf("  PASS: movem_disp\n");
}

/* ── Test: PEA ──────────────────────────────────────────────────────────── */

static void test_pea(void)
{
    setup();
    cpu.a[2] = 0x5000;
    /* PEA (A2): 4852 (0100 1000 01 010 010) */
    m68k_write16(&cpu, 0x1000, 0x4852);
    /* PEA $1234.W: 4878 1234 */
    m68k_write16(&cpu, 0x1002, 0x4878);
    m68k_write16(&cpu, 0x1004, 0x1234);
    m68k_write16(&cpu, 0x1006, 0x4E40);

    run_exit();

    /* Two pushes: first 0x5000, then 0x1234 (on top) */
    assert(cpu.a[7] == 0x10000 - 8);
    assert(m68k_read32(&cpu, cpu.a[7])     == 0x00001234);
    assert(m68k_read32(&cpu, cpu.a[7] + 4) == 0x00005000);
    printf("  PASS: pea\n");
}

/* ── Test: MOVE from SR ─────────────────────────────────────────────────── */

static void test_move_from_sr(void)
{
    setup();
    cpu.sr = 0x2704;  /* S=1, IPM=7, Z=1 */
    /* MOVE SR, D0: 40C0 (0100 0000 11 000 000) */
    m68k_write16(&cpu, 0x1000, 0x40C0);
    m68k_write16(&cpu, 0x1002, 0x4E40);

    run_exit();

    assert((cpu.d[0] & 0xFFFF) == 0x2704);
    printf("  PASS: move_from_sr\n");
}

/* ── Test: MOVE to CCR ──────────────────────────────────────────────────── */

static void test_move_to_ccr(void)
{
    setup();
    cpu.sr = 0x2700;  /* supervisor, no flags */
    cpu.d[0] = 0x001F;  /* all CCR flags set */
    /* MOVE D0, CCR: 44C0 (0100 0100 11 000 000) */
    m68k_write16(&cpu, 0x1000, 0x44C0);
    m68k_write16(&cpu, 0x1002, 0x4E40);

    run_exit();

    assert(cpu.sr == 0x271F);  /* system byte unchanged, CCR = 0x1F */
    printf("  PASS: move_to_ccr\n");
}

/* ── Test: MOVE to SR ───────────────────────────────────────────────────── */

static void test_move_to_sr(void)
{
    setup();
    cpu.sr = 0x2700;
    cpu.d[0] = 0x0004;  /* Z flag only, user mode */
    /* MOVE D0, SR: 46C0 (0100 0110 11 000 000) */
    m68k_write16(&cpu, 0x1000, 0x46C0);
    m68k_write16(&cpu, 0x1002, 0x4E40);

    run_exit();

    assert(cpu.sr == 0x0004);  /* entire SR replaced */
    printf("  PASS: move_to_sr\n");
}

/* ── Test: MOVE USP ─────────────────────────────────────────────────────── */

static void test_move_usp(void)
{
    setup();
    cpu.a[3] = 0x12345678;
    /* MOVE A3, USP: 4E63 (0100 1110 0110 0 011) */
    m68k_write16(&cpu, 0x1000, 0x4E63);
    /* MOVE USP, A4: 4E6C (0100 1110 0110 1 100) */
    m68k_write16(&cpu, 0x1002, 0x4E6C);
    m68k_write16(&cpu, 0x1004, 0x4E40);

    run_exit();

    assert(cpu.usp == 0x12345678);
    assert(cpu.a[4] == 0x12345678);
    printf("  PASS: move_usp\n");
}

/* ── Test: MOVEM function prologue/epilogue pattern ────────────────────── */

static void test_movem_prologue(void)
{
    setup();
    cpu.d[2] = 0xAAAA0000;
    cpu.d[3] = 0xBBBB0000;
    cpu.a[2] = 0xCCCC0000;
    uint32_t sp_orig = cpu.a[7];

    /* Simulate function prologue/epilogue:
     * LINK A6, #-8        → 4E56 FFF8
     * MOVEM.L D2-D3/A2, -(A7) → 48E7 3020 (reversed: D2=bit13, D3=bit12, A2=bit5)
     * ... body (nop) ...
     * MOVEM.L (A7)+, D2-D3/A2 → 4CDF 040C (normal: D2=bit2, D3=bit3, A2=bit10)
     * UNLK A6             → 4E5E
     */
    uint32_t pc = 0x1000;
    /* LINK A6, #-8 */
    m68k_write16(&cpu, pc, 0x4E56); pc += 2;
    m68k_write16(&cpu, pc, 0xFFF8); pc += 2;  /* #-8 */
    /* MOVEM.L D2-D3/A2, -(A7) */
    m68k_write16(&cpu, pc, 0x48E7); pc += 2;
    m68k_write16(&cpu, pc, 0x3020); pc += 2;
    /* NOP (body) */
    m68k_write16(&cpu, pc, 0x4E71); pc += 2;
    /* MOVEM.L (A7)+, D2-D3/A2 */
    m68k_write16(&cpu, pc, 0x4CDF); pc += 2;
    m68k_write16(&cpu, pc, 0x040C); pc += 2;
    /* UNLK A6 */
    m68k_write16(&cpu, pc, 0x4E5E); pc += 2;
    /* TRAP #0 */
    m68k_write16(&cpu, pc, 0x4E40); pc += 2;

    run_exit();

    assert(cpu.d[2] == 0xAAAA0000);
    assert(cpu.d[3] == 0xBBBB0000);
    assert(cpu.a[2] == 0xCCCC0000);
    assert(cpu.a[7] == sp_orig);  /* SP fully restored */
    printf("  PASS: movem_prologue\n");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Step 6 tests — ADDX, SUBX, ABCD, SBCD, NBCD, TAS, CHK
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── Test: ADDX.L Dy, Dx ────────────────────────────────────────────────── */

static void test_addx_dn(void)
{
    setup();
    cpu.d[0] = 0xFFFFFFFF;
    cpu.d[1] = 0x00000001;
    cpu.sr |= M68K_FLAG_X;  /* X=1, so result = FFFFFFFF + 1 + 1 = 1 with carry */
    /* ADDX.L D1, D0: 1101 000 1 10 00 0 001 = D181 */
    m68k_write16(&cpu, 0x1000, 0xD181);
    m68k_write16(&cpu, 0x1002, 0x4E40);
    run_exit();
    assert(cpu.d[0] == 0x00000001);
    assert(cpu.sr & M68K_FLAG_C);
    assert(cpu.sr & M68K_FLAG_X);
    printf("  PASS: addx_dn\n");
}

/* ── Test: ADDX sticky Z ────────────────────────────────────────────────── */

static void test_addx_sticky_z(void)
{
    setup();
    cpu.d[0] = 0;
    cpu.d[1] = 0;
    cpu.sr |= M68K_FLAG_Z;  /* Z already set */
    cpu.sr &= ~M68K_FLAG_X; /* X=0 */
    /* ADDX.L D1, D0 → D181 → 0+0+0=0, Z stays set */
    m68k_write16(&cpu, 0x1000, 0xD181);
    m68k_write16(&cpu, 0x1002, 0x4E40);
    run_exit();
    assert(cpu.d[0] == 0);
    assert(cpu.sr & M68K_FLAG_Z);  /* Z preserved (sticky) */

    /* Now non-zero result clears Z */
    setup();
    cpu.d[0] = 1;
    cpu.d[1] = 0;
    cpu.sr |= M68K_FLAG_Z;
    cpu.sr &= ~M68K_FLAG_X;
    m68k_write16(&cpu, 0x1000, 0xD181);
    m68k_write16(&cpu, 0x1002, 0x4E40);
    run_exit();
    assert(cpu.d[0] == 1);
    assert(!(cpu.sr & M68K_FLAG_Z));  /* Z cleared */
    printf("  PASS: addx_sticky_z\n");
}

/* ── Test: SUBX.L Dy, Dx ────────────────────────────────────────────────── */

static void test_subx_dn(void)
{
    setup();
    cpu.d[0] = 0x00000003;
    cpu.d[1] = 0x00000001;
    cpu.sr |= M68K_FLAG_X;  /* X=1, so result = 3 - 1 - 1 = 1 */
    /* SUBX.L D1, D0: 1001 000 1 10 00 0 001 = 9181 */
    m68k_write16(&cpu, 0x1000, 0x9181);
    m68k_write16(&cpu, 0x1002, 0x4E40);
    run_exit();
    assert(cpu.d[0] == 0x00000001);
    assert(!(cpu.sr & M68K_FLAG_C));
    assert(!(cpu.sr & M68K_FLAG_X));
    printf("  PASS: subx_dn\n");
}

/* ── Test: SUBX with borrow ─────────────────────────────────────────────── */

static void test_subx_borrow(void)
{
    setup();
    cpu.d[0] = 0x00000000;
    cpu.d[1] = 0x00000001;
    cpu.sr |= M68K_FLAG_X;  /* X=1, result = 0 - 1 - 1 = FFFFFFFE, borrow */
    /* SUBX.L D1, D0: 9181 */
    m68k_write16(&cpu, 0x1000, 0x9181);
    m68k_write16(&cpu, 0x1002, 0x4E40);
    run_exit();
    assert(cpu.d[0] == 0xFFFFFFFE);
    assert(cpu.sr & M68K_FLAG_C);
    assert(cpu.sr & M68K_FLAG_X);
    assert(cpu.sr & M68K_FLAG_N);
    printf("  PASS: subx_borrow\n");
}

/* ── Test: ABCD Dy, Dx ──────────────────────────────────────────────────── */

static void test_abcd_dn(void)
{
    setup();
    cpu.d[0] = 0x25;  /* BCD 25 */
    cpu.d[1] = 0x37;  /* BCD 37 */
    cpu.sr &= ~M68K_FLAG_X;
    /* ABCD D1, D0: 1100 000 100 000 001 = C101 */
    m68k_write16(&cpu, 0x1000, 0xC101);
    m68k_write16(&cpu, 0x1002, 0x4E40);
    run_exit();
    assert((cpu.d[0] & 0xFF) == 0x62);  /* 25 + 37 = 62 */
    assert(!(cpu.sr & M68K_FLAG_C));
    printf("  PASS: abcd_dn\n");
}

/* ── Test: ABCD with carry ──────────────────────────────────────────────── */

static void test_abcd_carry(void)
{
    setup();
    cpu.d[0] = 0x85;  /* BCD 85 */
    cpu.d[1] = 0x27;  /* BCD 27 */
    cpu.sr &= ~M68K_FLAG_X;
    /* ABCD D1, D0 → C101 → 85 + 27 = 112 → result 12, carry */
    m68k_write16(&cpu, 0x1000, 0xC101);
    m68k_write16(&cpu, 0x1002, 0x4E40);
    run_exit();
    assert((cpu.d[0] & 0xFF) == 0x12);
    assert(cpu.sr & M68K_FLAG_C);
    assert(cpu.sr & M68K_FLAG_X);
    printf("  PASS: abcd_carry\n");
}

/* ── Test: SBCD Dy, Dx ──────────────────────────────────────────────────── */

static void test_sbcd_dn(void)
{
    setup();
    cpu.d[0] = 0x62;  /* BCD 62 */
    cpu.d[1] = 0x37;  /* BCD 37 */
    cpu.sr &= ~M68K_FLAG_X;
    /* SBCD D1, D0: 1000 000 100 000 001 = 8101 */
    m68k_write16(&cpu, 0x1000, 0x8101);
    m68k_write16(&cpu, 0x1002, 0x4E40);
    run_exit();
    assert((cpu.d[0] & 0xFF) == 0x25);  /* 62 - 37 = 25 */
    assert(!(cpu.sr & M68K_FLAG_C));
    printf("  PASS: sbcd_dn\n");
}

/* ── Test: SBCD with borrow ─────────────────────────────────────────────── */

static void test_sbcd_borrow(void)
{
    setup();
    cpu.d[0] = 0x10;  /* BCD 10 */
    cpu.d[1] = 0x25;  /* BCD 25 */
    cpu.sr &= ~M68K_FLAG_X;
    /* SBCD D1, D0: 8101 → 10 - 25 = -15 → BCD 85 with borrow */
    m68k_write16(&cpu, 0x1000, 0x8101);
    m68k_write16(&cpu, 0x1002, 0x4E40);
    run_exit();
    assert((cpu.d[0] & 0xFF) == 0x85);
    assert(cpu.sr & M68K_FLAG_C);
    assert(cpu.sr & M68K_FLAG_X);
    printf("  PASS: sbcd_borrow\n");
}

/* ── Test: NBCD ─────────────────────────────────────────────────────────── */

static void test_nbcd(void)
{
    setup();
    cpu.d[0] = 0x25;  /* BCD 25 */
    cpu.sr &= ~M68K_FLAG_X;
    /* NBCD D0: 0100 1000 0000 0000 = 4800 */
    m68k_write16(&cpu, 0x1000, 0x4800);
    m68k_write16(&cpu, 0x1002, 0x4E40);
    run_exit();
    assert((cpu.d[0] & 0xFF) == 0x75);  /* 0 - 25 = 75 with borrow */
    assert(cpu.sr & M68K_FLAG_C);
    printf("  PASS: nbcd\n");
}

/* ── Test: TAS ──────────────────────────────────────────────────────────── */

static void test_tas(void)
{
    setup();
    cpu.d[0] = 0x00;
    /* TAS D0: 0100 1010 11 000 000 = 4AC0 */
    m68k_write16(&cpu, 0x1000, 0x4AC0);
    m68k_write16(&cpu, 0x1002, 0x4E40);
    run_exit();
    assert((cpu.d[0] & 0xFF) == 0x80);  /* bit 7 set */
    assert(cpu.sr & M68K_FLAG_Z);       /* tested 0 before setting */
    assert(!(cpu.sr & M68K_FLAG_N));

    /* TAS on non-zero value */
    setup();
    cpu.d[0] = 0x42;
    m68k_write16(&cpu, 0x1000, 0x4AC0);
    m68k_write16(&cpu, 0x1002, 0x4E40);
    run_exit();
    assert((cpu.d[0] & 0xFF) == 0xC2);  /* 0x42 | 0x80 */
    assert(!(cpu.sr & M68K_FLAG_Z));
    assert(!(cpu.sr & M68K_FLAG_N));
    printf("  PASS: tas\n");
}

/* ── Test: CHK.W (in-range, no trap) ────────────────────────────────────── */

static void test_chk_ok(void)
{
    setup();
    cpu.d[0] = 5;     /* value to check */
    cpu.d[1] = 10;    /* upper bound */
    /* CHK.W D1, D0: 0100 000 110 000 001 = 4181 */
    m68k_write16(&cpu, 0x1000, 0x4181);
    m68k_write16(&cpu, 0x1002, 0x4E40);
    run_exit();
    /* No trap fired, execution continues normally */
    assert(cpu.pc == 0x1004);
    printf("  PASS: chk_ok\n");
}

/* ── Test: CHK.W (out-of-range, trap) ───────────────────────────────────── */

static int chk_trap_fired;
static int chk_handler(ecpu_state_t *state, int trap_type,
                        uint32_t param, void *ctx)
{
    (void)state; (void)ctx;
    if (trap_type == ECPU_TRAP_ILLEGAL && param == 6) {
        chk_trap_fired = 1;
        return ECPU_TRAP_EXIT;
    }
    if (trap_type == ECPU_TRAP_SWI)
        return ECPU_TRAP_EXIT;
    return ECPU_TRAP_UNHANDLED;
}

static void test_chk_trap(void)
{
    /* D0 > D1 (upper bound) → CHK trap */
    setup();
    chk_trap_fired = 0;
    cpu.d[0] = 15;
    cpu.d[1] = 10;
    m68k_write16(&cpu, 0x1000, 0x4181);
    m68k_write16(&cpu, 0x1002, 0x4E40);
    ecpu_m68k_ops.set_trap_handler((ecpu_state_t *)&cpu, chk_handler, 0);
    ecpu_m68k_ops.run((ecpu_state_t *)&cpu);
    assert(chk_trap_fired == 1);

    /* D0 < 0 → CHK trap with N flag */
    setup();
    chk_trap_fired = 0;
    cpu.d[0] = 0xFFFF;  /* -1 as word */
    cpu.d[1] = 10;
    m68k_write16(&cpu, 0x1000, 0x4181);
    m68k_write16(&cpu, 0x1002, 0x4E40);
    ecpu_m68k_ops.set_trap_handler((ecpu_state_t *)&cpu, chk_handler, 0);
    ecpu_m68k_ops.run((ecpu_state_t *)&cpu);
    assert(chk_trap_fired == 1);
    assert(cpu.sr & M68K_FLAG_N);
    printf("  PASS: chk_trap\n");
}

/* ── Test: ADDX/SUBX memory mode -(Ay), -(Ax) ──────────────────────────── */

static void test_addx_mem(void)
{
    setup();
    /* Set up two 4-byte values in memory for ADDX.L -(A1), -(A0) */
    cpu.a[0] = 0x2004;  /* points past the data */
    cpu.a[1] = 0x3004;
    m68k_write32(&cpu, 0x2000, 0x00000005);  /* dst */
    m68k_write32(&cpu, 0x3000, 0x00000003);  /* src */
    cpu.sr &= ~M68K_FLAG_X;
    /* ADDX.L -(A1), -(A0): 1101 000 1 10 00 1 001 = D189 */
    m68k_write16(&cpu, 0x1000, 0xD189);
    m68k_write16(&cpu, 0x1002, 0x4E40);
    run_exit();
    assert(m68k_read32(&cpu, 0x2000) == 0x00000008);
    assert(cpu.a[0] == 0x2000);
    assert(cpu.a[1] == 0x3000);
    printf("  PASS: addx_mem\n");
}

/* ── Test: ABCD memory mode -(Ay), -(Ax) ────────────────────────────────── */

static void test_abcd_mem(void)
{
    setup();
    cpu.a[0] = 0x2001;  /* past the byte */
    cpu.a[1] = 0x3001;
    m68k_write8(&cpu, 0x2000, 0x25);  /* dst BCD */
    m68k_write8(&cpu, 0x3000, 0x37);  /* src BCD */
    cpu.sr &= ~M68K_FLAG_X;
    /* ABCD -(A1), -(A0): 1100 000 100 001 001 = C109 */
    m68k_write16(&cpu, 0x1000, 0xC109);
    m68k_write16(&cpu, 0x1002, 0x4E40);
    run_exit();
    assert(m68k_read8(&cpu, 0x2000) == 0x62);
    assert(cpu.a[0] == 0x2000);
    assert(cpu.a[1] == 0x3000);
    printf("  PASS: abcd_mem\n");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Step 7 tests — Integration + edge cases
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── Test: BHI/BLS (unsigned higher / lower-or-same) ────────────────────── */

static void test_bhi_bls(void)
{
    /* BHI: taken when !C && !Z (unsigned >) */
    setup();
    cpu.d[0] = 10;
    cpu.d[1] = 5;
    /* CMP.L D1, D0 → B081 (D0 - D1 = 5, no carry, no zero) */
    m68k_write16(&cpu, 0x1000, 0xB081);
    /* BHI.S +4 → 6204 (cc=2) */
    m68k_write16(&cpu, 0x1002, 0x6204);
    m68k_write16(&cpu, 0x1004, 0x7400);  /* MOVEQ #0, D2 */
    m68k_write16(&cpu, 0x1006, 0x4E40);
    m68k_write16(&cpu, 0x1008, 0x7401);  /* MOVEQ #1, D2 */
    m68k_write16(&cpu, 0x100A, 0x4E40);
    run_exit();
    assert(cpu.d[2] == 1);  /* BHI taken */

    /* BLS: taken when C || Z (unsigned <=) */
    setup();
    cpu.d[0] = 5;
    cpu.d[1] = 5;
    m68k_write16(&cpu, 0x1000, 0xB081);
    /* BLS.S +4 → 6304 (cc=3) */
    m68k_write16(&cpu, 0x1002, 0x6304);
    m68k_write16(&cpu, 0x1004, 0x7400);
    m68k_write16(&cpu, 0x1006, 0x4E40);
    m68k_write16(&cpu, 0x1008, 0x7401);
    m68k_write16(&cpu, 0x100A, 0x4E40);
    run_exit();
    assert(cpu.d[2] == 1);  /* BLS taken (equal) */

    /* BLS also taken when lower */
    setup();
    cpu.d[0] = 3;
    cpu.d[1] = 10;
    m68k_write16(&cpu, 0x1000, 0xB081);
    m68k_write16(&cpu, 0x1002, 0x6304);
    m68k_write16(&cpu, 0x1004, 0x7400);
    m68k_write16(&cpu, 0x1006, 0x4E40);
    m68k_write16(&cpu, 0x1008, 0x7401);
    m68k_write16(&cpu, 0x100A, 0x4E40);
    run_exit();
    assert(cpu.d[2] == 1);  /* BLS taken (lower) */
    printf("  PASS: bhi_bls\n");
}

/* ── Test: BGE/BLT (signed >= / <) ──────────────────────────────────────── */

static void test_bge_blt(void)
{
    /* BGE: taken when N==V (signed >=) */
    setup();
    cpu.d[0] = 5;
    cpu.d[1] = 3;
    m68k_write16(&cpu, 0x1000, 0xB081);  /* CMP.L D1, D0 */
    /* BGE.S +4 → 6C04 (cc=C) */
    m68k_write16(&cpu, 0x1002, 0x6C04);
    m68k_write16(&cpu, 0x1004, 0x7400);
    m68k_write16(&cpu, 0x1006, 0x4E40);
    m68k_write16(&cpu, 0x1008, 0x7401);
    m68k_write16(&cpu, 0x100A, 0x4E40);
    run_exit();
    assert(cpu.d[2] == 1);  /* BGE taken */

    /* BLT: taken when N!=V (signed <) */
    setup();
    cpu.d[0] = (uint32_t)-10;
    cpu.d[1] = 5;
    m68k_write16(&cpu, 0x1000, 0xB081);  /* CMP.L D1, D0 */
    /* BLT.S +4 → 6D04 (cc=D) */
    m68k_write16(&cpu, 0x1002, 0x6D04);
    m68k_write16(&cpu, 0x1004, 0x7400);
    m68k_write16(&cpu, 0x1006, 0x4E40);
    m68k_write16(&cpu, 0x1008, 0x7401);
    m68k_write16(&cpu, 0x100A, 0x4E40);
    run_exit();
    assert(cpu.d[2] == 1);  /* BLT taken */

    /* BGE with equal values */
    setup();
    cpu.d[0] = (uint32_t)-3;
    cpu.d[1] = (uint32_t)-3;
    m68k_write16(&cpu, 0x1000, 0xB081);
    m68k_write16(&cpu, 0x1002, 0x6C04);  /* BGE.S +4 */
    m68k_write16(&cpu, 0x1004, 0x7400);
    m68k_write16(&cpu, 0x1006, 0x4E40);
    m68k_write16(&cpu, 0x1008, 0x7401);
    m68k_write16(&cpu, 0x100A, 0x4E40);
    run_exit();
    assert(cpu.d[2] == 1);  /* BGE taken (equal) */
    printf("  PASS: bge_blt\n");
}

/* ── Test: BVC/BVS (overflow clear/set) ─────────────────────────────────── */

static void test_bvc_bvs(void)
{
    /* BVC: taken when V==0 */
    setup();
    cpu.d[0] = 10;
    cpu.d[1] = 5;
    m68k_write16(&cpu, 0x1000, 0xB081);  /* CMP.L D1, D0 — no overflow */
    /* BVC.S +4 → 6804 (cc=8) */
    m68k_write16(&cpu, 0x1002, 0x6804);
    m68k_write16(&cpu, 0x1004, 0x7400);
    m68k_write16(&cpu, 0x1006, 0x4E40);
    m68k_write16(&cpu, 0x1008, 0x7401);
    m68k_write16(&cpu, 0x100A, 0x4E40);
    run_exit();
    assert(cpu.d[2] == 1);  /* BVC taken */

    /* BVS: taken when V==1 — trigger overflow with 0x7FFFFFFF + 1 */
    setup();
    cpu.d[0] = 0x7FFFFFFF;
    cpu.d[1] = 1;
    /* ADD.L D1, D0 → D081 — positive overflow */
    m68k_write16(&cpu, 0x1000, 0xD081);
    /* BVS.S +4 → 6904 (cc=9) */
    m68k_write16(&cpu, 0x1002, 0x6904);
    m68k_write16(&cpu, 0x1004, 0x7400);
    m68k_write16(&cpu, 0x1006, 0x4E40);
    m68k_write16(&cpu, 0x1008, 0x7401);
    m68k_write16(&cpu, 0x100A, 0x4E40);
    run_exit();
    assert(cpu.d[2] == 1);  /* BVS taken */
    printf("  PASS: bvc_bvs\n");
}

/* ── Test: DIVU overflow ────────────────────────────────────────────────── */

static void test_divu_overflow(void)
{
    setup();
    cpu.d[0] = 0x00020000;  /* 131072 */
    cpu.d[1] = 1;           /* divisor = 1, quotient = 131072 > 0xFFFF */
    /* DIVU D1, D0 → 80C1 */
    m68k_write16(&cpu, 0x1000, 0x80C1);
    m68k_write16(&cpu, 0x1002, 0x4E40);
    run_exit();
    /* Overflow: V set, D0 unchanged */
    assert(cpu.sr & M68K_FLAG_V);
    assert(cpu.d[0] == 0x00020000);  /* unchanged on overflow */
    printf("  PASS: divu_overflow\n");
}

/* ── Test: DIVS overflow ────────────────────────────────────────────────── */

static void test_divs_overflow(void)
{
    setup();
    cpu.d[0] = 0x00010000;  /* 65536 */
    cpu.d[1] = 1;           /* quotient = 65536 > 32767 */
    /* DIVS D1, D0 → 81C1 */
    m68k_write16(&cpu, 0x1000, 0x81C1);
    m68k_write16(&cpu, 0x1002, 0x4E40);
    run_exit();
    assert(cpu.sr & M68K_FLAG_V);
    assert(cpu.d[0] == 0x00010000);  /* unchanged */

    /* Negative overflow: -65536 / 1 = -65536 < -32768 */
    setup();
    cpu.d[0] = (uint32_t)-65536;
    cpu.d[1] = 1;
    m68k_write16(&cpu, 0x1000, 0x81C1);
    m68k_write16(&cpu, 0x1002, 0x4E40);
    run_exit();
    assert(cpu.sr & M68K_FLAG_V);
    printf("  PASS: divs_overflow\n");
}

/* ── Test: MOVEM empty mask (no registers) ──────────────────────────────── */

static void test_movem_empty(void)
{
    setup();
    uint32_t sp = cpu.a[7];
    /* MOVEM.L (nothing), -(A7) → 48E7 0000 (mask=0) */
    m68k_write16(&cpu, 0x1000, 0x48E7);
    m68k_write16(&cpu, 0x1002, 0x0000);
    m68k_write16(&cpu, 0x1004, 0x4E40);
    run_exit();
    assert(cpu.a[7] == sp);  /* SP unchanged, no regs pushed */
    printf("  PASS: movem_empty\n");
}

/* ── Test: MOVEM single register ────────────────────────────────────────── */

static void test_movem_single(void)
{
    setup();
    cpu.d[3] = 0xDEADBEEF;
    uint32_t sp = cpu.a[7];
    /* MOVEM.L D3, -(A7) → 48E7 1000 (reversed mask: bit 12 = D3) */
    m68k_write16(&cpu, 0x1000, 0x48E7);
    m68k_write16(&cpu, 0x1002, 0x1000);
    /* MOVEQ #0, D3 → 7600 */
    m68k_write16(&cpu, 0x1004, 0x7600);
    /* MOVEM.L (A7)+, D3 → 4CDF 0008 (normal mask: bit 3 = D3) */
    m68k_write16(&cpu, 0x1006, 0x4CDF);
    m68k_write16(&cpu, 0x1008, 0x0008);
    m68k_write16(&cpu, 0x100A, 0x4E40);
    run_exit();
    assert(cpu.d[3] == 0xDEADBEEF);
    assert(cpu.a[7] == sp);
    printf("  PASS: movem_single\n");
}

/* ── Test: Bubble sort ──────────────────────────────────────────────────── */

static void test_bubble_sort(void)
{
    setup();
    /* Sort 5 words in memory: [5, 3, 8, 1, 4] → [1, 3, 4, 5, 8] */
    uint32_t data = 0x2000;
    m68k_write16(&cpu, data + 0, 5);
    m68k_write16(&cpu, data + 2, 3);
    m68k_write16(&cpu, data + 4, 8);
    m68k_write16(&cpu, data + 6, 1);
    m68k_write16(&cpu, data + 8, 4);

    /*
     * Bubble sort (5 words at A0, count in D7):
     *   outer:  MOVE.L D7, D6         ; D6 = outer counter
     *           SUBQ.L #1, D6
     *   oloop:  LEA (A0), A1          ; A1 = start
     *           MOVE.L D6, D5         ; D5 = inner counter
     *   iloop:  MOVE.W (A1), D0       ; load arr[i]
     *           CMP.W 2(A1), D0       ; compare arr[i+1]
     *           BLE.S noswap          ; if arr[i] <= arr[i+1], skip
     *           MOVE.W 2(A1), D1      ; swap
     *           MOVE.W D0, 2(A1)
     *           MOVE.W D1, (A1)
     *   noswap: ADDQ.L #2, A1
     *           DBRA D5, iloop
     *           DBRA D6, oloop (re-init inner)
     *           TRAP #0
     *
     * But DBRA for outer loop needs to restart inner, so let's use
     * a simpler structure with Bcc loops.
     */
    uint32_t pc = 0x1000;
    cpu.a[0] = data;
    cpu.d[7] = 5;  /* count */

    /* LEA (A0), A2: save base → 45D0 (but let's just use A0 directly) */
    /* outer: MOVE.L D7, D6 */
    /* D6 = n-2 (outer DBRA count: loops n-1 times) */
    m68k_write16(&cpu, pc, 0x2C07); pc += 2;  /* MOVE.L D7, D6 */
    m68k_write16(&cpu, pc, 0x5586); pc += 2;  /* SUBQ.L #2, D6 */

    /* oloop: */
    uint32_t oloop = pc;
    /* MOVEA.L A0, A1 → 2248 */
    m68k_write16(&cpu, pc, 0x2248); pc += 2;
    /* D5 = n-2 (inner DBRA count: loops n-1 times = n-1 comparisons) */
    m68k_write16(&cpu, pc, 0x2A07); pc += 2;  /* MOVE.L D7, D5 */
    m68k_write16(&cpu, pc, 0x5585); pc += 2;  /* SUBQ.L #2, D5 */

    /* iloop: (pc = 0x1008) */
    uint32_t iloop = pc;
    /* MOVE.W (A1), D0 → 3011 */
    m68k_write16(&cpu, pc, 0x3011); pc += 2;
    /* CMP.W 2(A1), D0 → B069 0002 */
    m68k_write16(&cpu, pc, 0xB069); pc += 2;
    m68k_write16(&cpu, pc, 0x0002); pc += 2;
    /* BLE.S noswap (+10) → 6F0A */
    m68k_write16(&cpu, pc, 0x6F0A); pc += 2;
    /* MOVE.W 2(A1), D1 → 3229 0002 */
    m68k_write16(&cpu, pc, 0x3229); pc += 2;
    m68k_write16(&cpu, pc, 0x0002); pc += 2;
    /* MOVE.W D0, 2(A1) → 3340 0002 */
    m68k_write16(&cpu, pc, 0x3340); pc += 2;
    m68k_write16(&cpu, pc, 0x0002); pc += 2;
    /* MOVE.W D1, (A1) → 3281 */
    m68k_write16(&cpu, pc, 0x3281); pc += 2;
    /* noswap: */
    /* ADDQ.L #2, A1 → 5489 */
    m68k_write16(&cpu, pc, 0x5489); pc += 2;
    /* DBRA D5, iloop → 51CD xxxx */
    m68k_write16(&cpu, pc, 0x51CD); pc += 2;
    int16_t iback = (int16_t)(iloop - pc);
    m68k_write16(&cpu, pc, (uint16_t)iback); pc += 2;
    /* DBRA D6, oloop → 51CE xxxx */
    m68k_write16(&cpu, pc, 0x51CE); pc += 2;
    int16_t oback = (int16_t)(oloop - pc);
    m68k_write16(&cpu, pc, (uint16_t)oback); pc += 2;
    /* TRAP #0 */
    m68k_write16(&cpu, pc, 0x4E40); pc += 2;

    run_exit();

    assert(m68k_read16(&cpu, data + 0) == 1);
    assert(m68k_read16(&cpu, data + 2) == 3);
    assert(m68k_read16(&cpu, data + 4) == 4);
    assert(m68k_read16(&cpu, data + 6) == 5);
    assert(m68k_read16(&cpu, data + 8) == 8);
    printf("  PASS: bubble_sort\n");
}

/* ── Test: String reverse in-place ──────────────────────────────────────── */

static void test_strrev(void)
{
    setup();
    /* "Hello" at 0x2000 */
    uint32_t str = 0x2000;
    m68k_write8(&cpu, str + 0, 'H');
    m68k_write8(&cpu, str + 1, 'e');
    m68k_write8(&cpu, str + 2, 'l');
    m68k_write8(&cpu, str + 3, 'l');
    m68k_write8(&cpu, str + 4, 'o');
    m68k_write8(&cpu, str + 5, 0);

    /*
     * String reverse: A0 = start, A1 = end (last char)
     *   ; find end: scan for NUL
     *   MOVEA.L A0, A1
     * scan: TST.B (A1)+
     *   BNE.S scan
     *   SUBQ.L #2, A1        ; back past NUL and to last char
     * loop: CMPA.L A0, A1
     *   BLE.S done
     *   MOVE.B (A0), D0
     *   MOVE.B (A1), (A0)+
     *   MOVE.B D0, (A1)
     *   SUBQ.L #1, A1
     *   BRA.S loop
     * done: TRAP #0
     */
    uint32_t pc = 0x1000;
    cpu.a[0] = str;

    /* MOVEA.L A0, A1 → 2248 */
    m68k_write16(&cpu, pc, 0x2248); pc += 2;
    /* scan: TST.B (A1)+ → 4A19 */
    uint32_t scan = pc;
    m68k_write16(&cpu, pc, 0x4A19); pc += 2;
    /* BNE.S scan → 66xx */
    m68k_write16(&cpu, pc, 0x6600 | ((uint8_t)(int8_t)(scan - pc - 2))); pc += 2;
    /* SUBQ.L #2, A1 → 5589 */
    m68k_write16(&cpu, pc, 0x5589); pc += 2;

    /* loop: CMPA.L A0, A1 → B3C8 */
    uint32_t loop = pc;
    m68k_write16(&cpu, pc, 0xB3C8); pc += 2;
    /* BLE.S done → 6F0C */
    uint32_t ble_pc = pc;
    m68k_write16(&cpu, pc, 0x6F00); pc += 2;  /* placeholder, fix later */
    /* MOVE.B (A0), D0 → 1010 */
    m68k_write16(&cpu, pc, 0x1010); pc += 2;
    /* MOVE.B (A1), (A0)+ → 10D9... wait, (A1) to (A0)+ */
    /* MOVE.B (A1), (A0)+ → 10D1... no. Let me think:
     * MOVE.B src, dst. src=(A1)=mode 2 reg 1, dst=(A0)+=mode 3 reg 0
     * MOVE.B: 0001 dst dst src src src
     * dst = mode 3, reg 0 → 011 000
     * src = mode 2, reg 1 → 010 001
     * = 0001 011 000 010 001 = 1611... wait, let me re-encode.
     * MOVE.B = 0001 | dst_reg(3) | dst_mode(3) | src_mode(3) | src_reg(3)
     * dst = (A0)+ → mode=3, reg=0
     * src = (A1)  → mode=2, reg=1
     * = 0001 000 011 010 001 = 10D1 */
    m68k_write16(&cpu, pc, 0x10D1); pc += 2;
    /* But we already read (A0) into D0 and post-incremented A0.
     * Actually MOVE.B (A0), D0 doesn't post-increment. (A0) is mode 2.
     * So after MOVE.B (A1),(A0)+ A0 is incremented.
     * Now write D0 to (A1): MOVE.B D0, (A1) → 1280 */
    m68k_write16(&cpu, pc, 0x1280); pc += 2;
    /* SUBQ.L #1, A1 → 5389 */
    m68k_write16(&cpu, pc, 0x5389); pc += 2;
    /* BRA.S loop */
    m68k_write16(&cpu, pc, 0x6000 | ((uint8_t)(int8_t)(loop - pc - 2))); pc += 2;
    /* done: TRAP #0 */
    uint32_t done = pc;
    m68k_write16(&cpu, pc, 0x4E40); pc += 2;

    /* Fix up the BLE offset */
    int8_t ble_off = (int8_t)(done - ble_pc - 2);
    m68k_write16(&cpu, ble_pc, 0x6F00 | (uint8_t)ble_off);

    run_exit();

    assert(m68k_read8(&cpu, str + 0) == 'o');
    assert(m68k_read8(&cpu, str + 1) == 'l');
    assert(m68k_read8(&cpu, str + 2) == 'l');
    assert(m68k_read8(&cpu, str + 3) == 'e');
    assert(m68k_read8(&cpu, str + 4) == 'H');
    assert(m68k_read8(&cpu, str + 5) == 0);
    printf("  PASS: strrev\n");
}

/* ── Test: NEGX multi-precision (64-bit negate) ─────────────────────────── */

static void test_negx_multiprecision(void)
{
    setup();
    /* Negate 64-bit value 0x0000000100000000 stored in D0:D1 (D0=hi, D1=lo) */
    cpu.d[0] = 0x00000001;  /* hi */
    cpu.d[1] = 0x00000000;  /* lo */
    /* Clear X first */
    cpu.sr &= ~M68K_FLAG_X;
    /* NEG.L D1 → 4481 (negate low longword, sets X if non-zero) */
    m68k_write16(&cpu, 0x1000, 0x4481);
    /* NEGX.L D0 → 4080 (negate high with extend) */
    m68k_write16(&cpu, 0x1002, 0x4080);
    m68k_write16(&cpu, 0x1004, 0x4E40);
    run_exit();
    /* -0x100000000 = 0xFFFFFFFF:00000000 */
    assert(cpu.d[0] == 0xFFFFFFFF);
    assert(cpu.d[1] == 0x00000000);
    printf("  PASS: negx_multiprecision\n");
}

/* ── Test: ADDX multi-precision (64-bit add) ────────────────────────────── */

static void test_addx_multiprecision(void)
{
    setup();
    /* Add 0x00000001:FFFFFFFF + 0x00000000:00000002 = 0x00000002:00000001 */
    /* First pair: D1:D0 (hi:lo) */
    cpu.d[0] = 0xFFFFFFFF;  /* lo1 */
    cpu.d[1] = 0x00000001;  /* hi1 */
    /* Second pair: D3:D2 */
    cpu.d[2] = 0x00000002;  /* lo2 */
    cpu.d[3] = 0x00000000;  /* hi2 */
    cpu.sr &= ~M68K_FLAG_X;
    /* ADD.L D2, D0 → D082 (low add, sets X on carry) */
    m68k_write16(&cpu, 0x1000, 0xD082);
    /* ADDX.L D3, D1 → D383 (high add with carry) */
    m68k_write16(&cpu, 0x1002, 0xD383);
    m68k_write16(&cpu, 0x1004, 0x4E40);
    run_exit();
    assert(cpu.d[0] == 0x00000001);  /* FFFFFFFF + 2 = 1 with carry */
    assert(cpu.d[1] == 0x00000002);  /* 1 + 0 + carry = 2 */
    printf("  PASS: addx_multiprecision\n");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Step 8 tests — PPAP cross-arch personality
 * ══════════════════════════════════════════════════════════════════════════ */

/* Capture state from the PPAP m68k trap handler */
static uint32_t captured_syscall_nr;
static uint32_t captured_args[6];
static int ppap_trap_called;

/*
 * ppap_test_handler — mock PPAP personality trap handler.
 *
 * On TRAP #0 (ECPU_TRAP_SWI param=0): captures d0=syscall#, d1-d5=args,
 * a0=arg6 for verification.  Returns ECPU_TRAP_EXIT for SYS_EXIT,
 * ECPU_TRAP_HANDLED otherwise (sets d0 = 42 as mock return value).
 */
static int ppap_test_handler(ecpu_state_t *state, int trap_type,
                              uint32_t param, void *ctx)
{
    (void)ctx;
    m68k_state_t *m = (m68k_state_t *)state;

    if (trap_type == ECPU_TRAP_SWI && param == 0) {
        ppap_trap_called = 1;
        captured_syscall_nr = m->d[0];
        captured_args[0] = m->d[1];
        captured_args[1] = m->d[2];
        captured_args[2] = m->d[3];
        captured_args[3] = m->d[4];
        captured_args[4] = m->d[5];
        captured_args[5] = m->a[0];

        if (m->d[0] == SYS_EXIT || m->d[0] == SYS_EXIT_GROUP)
            return ECPU_TRAP_EXIT;

        /* Mock return value */
        m->d[0] = 42;
        return ECPU_TRAP_HANDLED;
    }
    if (trap_type == ECPU_TRAP_HALT)
        return ECPU_TRAP_EXIT;
    return ECPU_TRAP_UNHANDLED;
}

/* ── Test: TRAP #0 with SYS_EXIT ────────────────────────────────────────── */

static void test_ppap_trap_exit(void)
{
    setup();
    ppap_trap_called = 0;

    /* MOVEQ #0, D0 → d0 = SYS_EXIT (0x0000) */
    m68k_write16(&cpu, 0x1000, 0x7000);  /* MOVEQ #0, D0 */
    /* MOVEQ #5, D1 → d1 = exit status */
    m68k_write16(&cpu, 0x1002, 0x720A);  /* MOVEQ #10, D1 */
    /* TRAP #0 */
    m68k_write16(&cpu, 0x1004, 0x4E40);

    ecpu_m68k_ops.set_trap_handler((ecpu_state_t *)&cpu,
                                    ppap_test_handler, NULL);
    ecpu_m68k_ops.run((ecpu_state_t *)&cpu);

    assert(ppap_trap_called == 1);
    assert(captured_syscall_nr == SYS_EXIT);
    assert(captured_args[0] == 10);  /* d1 = exit status */
    printf("  PASS: ppap_trap_exit\n");
}

/* ── Test: TRAP #0 with SYS_WRITE (captures all args) ───────────────────── */

static void test_ppap_trap_write(void)
{
    setup();
    ppap_trap_called = 0;

    uint32_t pc = 0x1000;

    /* Set up registers for sys_write(fd=1, buf=0x2000, count=5):
     *   d0 = SYS_WRITE (0x0101)
     *   d1 = fd (1)
     *   d2 = buf addr (0x2000)
     *   d3 = count (5)
     */
    /* MOVE.L #$0101, D0 → 203C 00000101 */
    m68k_write16(&cpu, pc, 0x203C); pc += 2;
    m68k_write32(&cpu, pc, SYS_WRITE); pc += 4;
    /* MOVEQ #1, D1 */
    m68k_write16(&cpu, pc, 0x7201); pc += 2;
    /* MOVE.L #$2000, D2 → 243C 00002000 */
    m68k_write16(&cpu, pc, 0x243C); pc += 2;
    m68k_write32(&cpu, pc, 0x2000); pc += 4;
    /* MOVEQ #5, D3 */
    m68k_write16(&cpu, pc, 0x7605); pc += 2;
    /* TRAP #0 */
    m68k_write16(&cpu, pc, 0x4E40); pc += 2;
    /* After TRAP #0 returns with HANDLED, check d0 has mock return value */
    /* TRAP #0 with SYS_EXIT to stop */
    m68k_write16(&cpu, pc, 0x7000); pc += 2;  /* MOVEQ #0, D0 (=SYS_EXIT) */
    /* Save return value first: MOVE.L D0, D7 before clearing D0 */
    /* Actually, after TRAP #0 HANDLED, d0 = 42 (mock). Let's save it. */
    /* We need to capture d0 before overwriting it with SYS_EXIT.
     * Use: MOVE.L D0, D7; MOVEQ #0, D0; TRAP #0 */

    /* Rewrite: after first TRAP #0, d0 = 42 (mock return) */
    pc = 0x1000;
    m68k_write16(&cpu, pc, 0x203C); pc += 2;
    m68k_write32(&cpu, pc, SYS_WRITE); pc += 4;
    m68k_write16(&cpu, pc, 0x7201); pc += 2;     /* MOVEQ #1, D1 */
    m68k_write16(&cpu, pc, 0x243C); pc += 2;     /* MOVE.L #$2000, D2 */
    m68k_write32(&cpu, pc, 0x2000); pc += 4;
    m68k_write16(&cpu, pc, 0x7605); pc += 2;     /* MOVEQ #5, D3 */
    m68k_write16(&cpu, pc, 0x4E40); pc += 2;     /* TRAP #0 (sys_write) */
    /* d0 now = 42 (mock return). Save to d7: MOVE.L D0, D7 → 2E00 */
    m68k_write16(&cpu, pc, 0x2E00); pc += 2;
    /* SYS_EXIT: MOVEQ #0, D0; TRAP #0 */
    m68k_write16(&cpu, pc, 0x7000); pc += 2;     /* MOVEQ #0, D0 */
    m68k_write16(&cpu, pc, 0x4E40); pc += 2;     /* TRAP #0 (sys_exit) */

    ecpu_m68k_ops.set_trap_handler((ecpu_state_t *)&cpu,
                                    ppap_test_handler, NULL);
    ecpu_m68k_ops.run((ecpu_state_t *)&cpu);

    /* The last trap was SYS_EXIT, but we captured SYS_WRITE args first */
    assert(ppap_trap_called == 1);
    /* d7 should have the mock return value from the first TRAP #0 */
    assert(cpu.d[7] == 42);
    printf("  PASS: ppap_trap_write\n");
}

/* ── Test: TRAP #0 with all 6 args ──────────────────────────────────────── */

static int ppap_allargs_called;
static uint32_t ppap_allargs_nr;
static uint32_t ppap_allargs[6];

static int ppap_allargs_handler(ecpu_state_t *state, int trap_type,
                                 uint32_t param, void *ctx)
{
    (void)ctx;
    m68k_state_t *m = (m68k_state_t *)state;

    if (trap_type == ECPU_TRAP_SWI && param == 0) {
        if (m->d[0] == SYS_EXIT)
            return ECPU_TRAP_EXIT;

        ppap_allargs_called = 1;
        ppap_allargs_nr = m->d[0];
        ppap_allargs[0] = m->d[1];
        ppap_allargs[1] = m->d[2];
        ppap_allargs[2] = m->d[3];
        ppap_allargs[3] = m->d[4];
        ppap_allargs[4] = m->d[5];
        ppap_allargs[5] = m->a[0];
        m->d[0] = 99;
        return ECPU_TRAP_HANDLED;
    }
    if (trap_type == ECPU_TRAP_HALT)
        return ECPU_TRAP_EXIT;
    return ECPU_TRAP_UNHANDLED;
}

static void test_ppap_trap_allargs(void)
{
    setup();
    ppap_allargs_called = 0;

    uint32_t pc = 0x1000;
    /* Load d0 = 0x0101 (SYS_WRITE), d1-d5 = 11,22,33,44,55, a0 = 66 */
    m68k_write16(&cpu, pc, 0x203C); pc += 2;
    m68k_write32(&cpu, pc, 0x0101); pc += 4;     /* MOVE.L #$0101, D0 */
    m68k_write16(&cpu, pc, 0x223C); pc += 2;
    m68k_write32(&cpu, pc, 11); pc += 4;          /* MOVE.L #11, D1 */
    m68k_write16(&cpu, pc, 0x243C); pc += 2;
    m68k_write32(&cpu, pc, 22); pc += 4;          /* MOVE.L #22, D2 */
    m68k_write16(&cpu, pc, 0x263C); pc += 2;
    m68k_write32(&cpu, pc, 33); pc += 4;          /* MOVE.L #33, D3 */
    m68k_write16(&cpu, pc, 0x283C); pc += 2;
    m68k_write32(&cpu, pc, 44); pc += 4;          /* MOVE.L #44, D4 */
    m68k_write16(&cpu, pc, 0x2A3C); pc += 2;
    m68k_write32(&cpu, pc, 55); pc += 4;          /* MOVE.L #55, D5 */
    /* LEA $42, A0 → 207C 00000042 */
    m68k_write16(&cpu, pc, 0x207C); pc += 2;
    m68k_write32(&cpu, pc, 66); pc += 4;          /* LEA #66, A0 */
    /* TRAP #0 */
    m68k_write16(&cpu, pc, 0x4E40); pc += 2;
    /* Save return in D7 */
    m68k_write16(&cpu, pc, 0x2E00); pc += 2;      /* MOVE.L D0, D7 */
    /* SYS_EXIT */
    m68k_write16(&cpu, pc, 0x7000); pc += 2;
    m68k_write16(&cpu, pc, 0x4E40); pc += 2;

    ecpu_m68k_ops.set_trap_handler((ecpu_state_t *)&cpu,
                                    ppap_allargs_handler, NULL);
    ecpu_m68k_ops.run((ecpu_state_t *)&cpu);

    assert(ppap_allargs_called == 1);
    assert(ppap_allargs_nr == SYS_WRITE);
    assert(ppap_allargs[0] == 11);   /* d1 */
    assert(ppap_allargs[1] == 22);   /* d2 */
    assert(ppap_allargs[2] == 33);   /* d3 */
    assert(ppap_allargs[3] == 44);   /* d4 */
    assert(ppap_allargs[4] == 55);   /* d5 */
    assert(ppap_allargs[5] == 66);   /* a0 */
    assert(cpu.d[7] == 99);          /* mock return value */
    printf("  PASS: ppap_trap_allargs\n");
}

/* ── Test: "Hello" program via TRAP #0 ──────────────────────────────────── */

static int hello_write_fd;
static uint32_t hello_write_buf;
static uint32_t hello_write_len;

static int hello_handler(ecpu_state_t *state, int trap_type,
                          uint32_t param, void *ctx)
{
    (void)ctx;
    m68k_state_t *m = (m68k_state_t *)state;

    if (trap_type == ECPU_TRAP_SWI && param == 0) {
        if (m->d[0] == SYS_EXIT)
            return ECPU_TRAP_EXIT;
        if (m->d[0] == SYS_WRITE) {
            hello_write_fd  = (int)m->d[1];
            hello_write_buf = m->d[2];
            hello_write_len = m->d[3];
            m->d[0] = m->d[3];  /* return bytes written */
            return ECPU_TRAP_HANDLED;
        }
        return ECPU_TRAP_HANDLED;
    }
    if (trap_type == ECPU_TRAP_HALT)
        return ECPU_TRAP_EXIT;
    return ECPU_TRAP_UNHANDLED;
}

static void test_ppap_hello(void)
{
    setup();
    hello_write_fd = -1;
    hello_write_buf = 0;
    hello_write_len = 0;

    /* Place "Hello\n" at 0x2000 in emulated memory */
    m68k_write8(&cpu, 0x2000, 'H');
    m68k_write8(&cpu, 0x2001, 'e');
    m68k_write8(&cpu, 0x2002, 'l');
    m68k_write8(&cpu, 0x2003, 'l');
    m68k_write8(&cpu, 0x2004, 'o');
    m68k_write8(&cpu, 0x2005, '\n');

    uint32_t pc = 0x1000;

    /* sys_write(1, 0x2000, 6):
     *   d0 = SYS_WRITE (0x0101)
     *   d1 = 1 (stdout)
     *   d2 = 0x2000 (buf)
     *   d3 = 6 (len)
     */
    m68k_write16(&cpu, pc, 0x203C); pc += 2;
    m68k_write32(&cpu, pc, SYS_WRITE); pc += 4;
    m68k_write16(&cpu, pc, 0x7201); pc += 2;      /* MOVEQ #1, D1 */
    m68k_write16(&cpu, pc, 0x243C); pc += 2;
    m68k_write32(&cpu, pc, 0x2000); pc += 4;      /* MOVE.L #$2000, D2 */
    m68k_write16(&cpu, pc, 0x7606); pc += 2;      /* MOVEQ #6, D3 */
    m68k_write16(&cpu, pc, 0x4E40); pc += 2;      /* TRAP #0 */

    /* sys_exit(0):
     *   d0 = SYS_EXIT (0x0000)
     *   d1 = 0 (status)
     */
    m68k_write16(&cpu, pc, 0x7000); pc += 2;      /* MOVEQ #0, D0 */
    m68k_write16(&cpu, pc, 0x7200); pc += 2;      /* MOVEQ #0, D1 */
    m68k_write16(&cpu, pc, 0x4E40); pc += 2;      /* TRAP #0 */

    ecpu_m68k_ops.set_trap_handler((ecpu_state_t *)&cpu,
                                    hello_handler, NULL);
    ecpu_m68k_ops.run((ecpu_state_t *)&cpu);

    assert(hello_write_fd == 1);
    assert(hello_write_buf == 0x2000);
    assert(hello_write_len == 6);

    /* Verify the string in emulated memory */
    char buf[7];
    for (int i = 0; i < 6; i++)
        buf[i] = (char)m68k_read8(&cpu, hello_write_buf + i);
    buf[6] = '\0';
    assert(strcmp(buf, "Hello\n") == 0);

    printf("  PASS: ppap_hello\n");
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

    /* Step 4 */
    test_and_l();
    test_andi();
    test_or_l();
    test_ori();
    test_eor_l();
    test_eori();
    test_not();
    test_lsl_lsr();
    test_asr();
    test_asl();
    test_rol_ror();
    test_lsl_reg_count();
    test_btst_static();
    test_btst_dynamic();
    test_bset_bclr_bchg();
    test_exg();
    test_logic_ccr();
    test_or_and_mem();
    test_lsr_carry();
    test_shift_zero();
    test_btst_mem();
    test_bit_program();

    /* Step 5 */
    test_movem_predec();
    test_movem_postinc();
    test_movem_roundtrip();
    test_movem_w();
    test_movem_disp();
    test_pea();
    test_move_from_sr();
    test_move_to_ccr();
    test_move_to_sr();
    test_move_usp();
    test_movem_prologue();

    /* Step 6 */
    test_addx_dn();
    test_addx_sticky_z();
    test_subx_dn();
    test_subx_borrow();
    test_abcd_dn();
    test_abcd_carry();
    test_sbcd_dn();
    test_sbcd_borrow();
    test_nbcd();
    test_tas();
    test_chk_ok();
    test_chk_trap();
    test_addx_mem();
    test_abcd_mem();

    /* Step 7 */
    test_bhi_bls();
    test_bge_blt();
    test_bvc_bvs();
    test_divu_overflow();
    test_divs_overflow();
    test_movem_empty();
    test_movem_single();
    test_bubble_sort();
    test_strrev();
    test_negx_multiprecision();
    test_addx_multiprecision();

    /* Step 8 */
    test_ppap_trap_exit();
    test_ppap_trap_write();
    test_ppap_trap_allargs();
    test_ppap_hello();

    printf("All 137 tests passed.\n");
    return 0;
}
