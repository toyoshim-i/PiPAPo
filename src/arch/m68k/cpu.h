/*
 * cpu.h — Motorola 68000 system register definitions
 *
 * Status Register (SR) bits, exception frame layout, and
 * hardware register access for 68k targets.
 */

#ifndef PPAP_ARCH_M68K_CPU_H
#define PPAP_ARCH_M68K_CPU_H

#include <stdint.h>

/* ── Register access helper ─────────────────────────────────────────────── */

#ifndef REG
#define REG(addr)  (*(volatile uint32_t *)(uintptr_t)(addr))
#endif

#ifndef REG16
#define REG16(addr)  (*(volatile uint16_t *)(uintptr_t)(addr))
#endif

#ifndef REG8
#define REG8(addr)  (*(volatile uint8_t *)(uintptr_t)(addr))
#endif

/* ── Status Register (SR) ───────────────────────────────────────────────── *
 *
 * Bits 15-8: System byte
 *   15:    T1 (Trace)
 *   14:    T0 (Trace, 68020+)
 *   13:    S  (Supervisor mode: 1=supervisor, 0=user)
 *   12:    M  (Master/Interrupt stack, 68020+)
 *   10-8:  I2-I0 (Interrupt Priority Mask, 0-7)
 *
 * Bits 4-0: Condition Code Register (CCR)
 *   4: X (Extend)
 *   3: N (Negative)
 *   2: Z (Zero)
 *   1: V (Overflow)
 *   0: C (Carry)
 * ────────────────────────────────────────────────────────────────────────── */

#define SR_SUPERVISOR   (1u << 13)   /* S bit: supervisor mode           */
#define SR_IPL_MASK     0x0700u      /* interrupt priority level mask     */
#define SR_IPL_SHIFT    8u
#define SR_IPL_7        0x0700u      /* all interrupts masked (NMI only)  */
#define SR_TRACE        (1u << 15)   /* T1: trace mode                   */

/* Supervisor mode with all interrupts masked */
#define SR_SUPV_NOIRQ   (SR_SUPERVISOR | SR_IPL_7)    /* 0x2700 */

/* Supervisor mode with interrupts enabled */
#define SR_SUPV_IRQ     SR_SUPERVISOR                  /* 0x2000 */

/* User mode with interrupts enabled */
#define SR_USER         0x0000u

/* ── Exception frame (68000) ────────────────────────────────────────────── *
 *
 * On exception, the 68000 pushes onto SSP:
 *   [SSP+0]  SR  (16-bit status register)
 *   [SSP+2]  PC  (32-bit return address)
 *
 * Total: 6 bytes (3 words).
 *
 * Note: 68010+ adds a format/vector word, making the frame 8 bytes.
 * QEMU virt uses 68040 by default, which has the extended format.
 * We target 68000 ISA for X68000 compatibility, but when running on
 * 68010+ the format word is handled transparently by 'rte'.
 * ────────────────────────────────────────────────────────────────────────── */

/* 68000 exception frame (pushed by hardware) */
typedef struct {
    uint16_t sr;            /* saved status register */
    uint32_t pc;            /* saved program counter */
} __attribute__((packed)) m68k_exc_frame_t;

/* ── TRAP instruction numbers ───────────────────────────────────────────── */

#define TRAP_SYSCALL    0   /* TRAP #0: PPAP syscall (matches Linux m68k) */
#define TRAP_YIELD      1   /* TRAP #1: cooperative yield                 */

/* Exception vector numbers */
#define VEC_TRAP0       32  /* TRAP #0 vector number                      */
#define VEC_TRAP1       33  /* TRAP #1 vector number                      */

#endif /* PPAP_ARCH_M68K_CPU_H */
