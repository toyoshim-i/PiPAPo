/*
 * cpu.h — Xtensa special register definitions
 *
 * Defines special register (SR) numbers, bit fields, and access macros
 * for the Xtensa LX7 cores in the ESP32-S3.
 *
 * All code assumes Call0 ABI (-mabi=call0): no register windowing.
 */

#ifndef PPAP_ARCH_XTENSA_CPU_H
#define PPAP_ARCH_XTENSA_CPU_H

#include <stdint.h>

/* ── Register access helper ─────────────────────────────────────────────── */

#ifndef REG
#define REG(addr)  (*(volatile uint32_t *)(uintptr_t)(addr))
#endif

/* ── Special Register access macros ─────────────────────────────────────── *
 *
 * Xtensa special registers are accessed via RSR (read), WSR (write), and
 * XSR (exchange) instructions.  The SR number must be an immediate.
 */

#define rsr(sr) ({                                                      \
    uint32_t __v;                                                       \
    __asm__ volatile ("rsr %0, " #sr : "=a"(__v));                      \
    __v;                                                                \
})

#define wsr(sr, val) ({                                                 \
    uint32_t __v = (uint32_t)(val);                                     \
    __asm__ volatile ("wsr %0, " #sr :: "a"(__v));                      \
})

#define xsr(sr, val) ({                                                 \
    uint32_t __v = (uint32_t)(val);                                     \
    __asm__ volatile ("xsr %0, " #sr : "+a"(__v));                      \
    __v;                                                                \
})

/* ── PS — Processor State Register (SR 230) ─────────────────────────────── *
 *
 * Bits [3:0]  INTLEVEL  Current interrupt disable level (0=all enabled)
 * Bit  [4]    EXCM      Exception mode (1=in exception handler)
 * Bit  [5]    UM        User mode (1=user, 0=kernel)
 * Bits [7:6]  RING      Ring level (Call0 ABI: always 0)
 * Bit  [16]   CALLINC   Call increment (windowed ABI only, unused in Call0)
 * Bits [19:18] OWB      Old window base (windowed ABI only)
 */

#define PS_INTLEVEL_MASK    0x0Fu
#define PS_INTLEVEL_SHIFT   0
#define PS_EXCM             (1u << 4)
#define PS_UM               (1u << 5)
#define PS_RING_MASK        (3u << 6)
#define PS_RING_SHIFT       6

/* ── EXCCAUSE — Exception Cause Register (SR 232) ───────────────────────── */

#define EXCCAUSE_ILLEGAL_INSN       0u
#define EXCCAUSE_SYSCALL            1u
#define EXCCAUSE_INSN_FETCH_ERROR   2u
#define EXCCAUSE_LOAD_STORE_ERROR   3u
#define EXCCAUSE_LEVEL1_INT         4u
#define EXCCAUSE_ALLOCA             5u  /* windowed ABI only */
#define EXCCAUSE_DIVIDE_BY_ZERO     6u
#define EXCCAUSE_PRIVILEGED         8u
#define EXCCAUSE_LOAD_ALIGN         9u
#define EXCCAUSE_INSN_FETCH_PF     12u  /* PMS/MPU page fault */
#define EXCCAUSE_LOAD_PF           13u
#define EXCCAUSE_STORE_PF          15u
#define EXCCAUSE_LOAD_PROHIBITED   28u
#define EXCCAUSE_STORE_PROHIBITED  29u

/* ── Interrupt control registers ─────────────────────────────────────────── *
 *
 * ESP32-S3 LX7 has 32 interrupt sources.  INTENABLE is a bitmask of
 * which interrupts are enabled.  INTERRUPT is read-only (pending status).
 * INTCLEAR is write-only (clear edge-triggered/software interrupts).
 */

/* Timer interrupt: CCOMPARE0 is typically wired to interrupt number 6
 * on ESP32-S3.  The exact mapping is in the interrupt matrix. */
#define XTENSA_TIMER0_INTNUM    6u
#define XTENSA_TIMER0_INTMASK   (1u << XTENSA_TIMER0_INTNUM)

/* ── ESP32-S3 peripheral base addresses ──────────────────────────────────── */

#define DR_REG_UART_BASE        0x60000000u
#define DR_REG_UART1_BASE       0x60010000u
#define DR_REG_SYSTEM_BASE      0x600C0000u

/* UART0 registers (subset needed for polled I/O) */
#define UART0_FIFO              REG(DR_REG_UART_BASE + 0x00u)
#define UART0_STATUS            REG(DR_REG_UART_BASE + 0x1Cu)
#define UART0_CONF0             REG(DR_REG_UART_BASE + 0x20u)

/* UART_STATUS bits */
#define UART_TXFIFO_CNT_MASK    0x000003FFu  /* bits [9:0] */
#define UART_TXFIFO_CNT_SHIFT   0
#define UART_RXFIFO_CNT_MASK    0x000003FFu  /* bits [9:0] of STATUS[13:0]? */
#define UART_RXFIFO_CNT_SHIFT   0

/* TX FIFO depth */
#define UART_FIFO_DEPTH         128u

/* ── Timer — CCOUNT / CCOMPARE ──────────────────────────────────────────── *
 *
 * CCOUNT (SR 234) is a free-running 32-bit cycle counter.
 * CCOMPARE0 (SR 240) generates interrupt XTENSA_TIMER0_INTNUM when
 * CCOUNT matches CCOMPARE0.
 *
 * Timer tick interval for 10 ms slices.
 */

#ifndef PPAP_SYS_HZ
#define PPAP_SYS_HZ 240000000u
#endif
#define XTENSA_TICK_INTERVAL (PPAP_SYS_HZ / 100u)

#endif /* PPAP_ARCH_XTENSA_CPU_H */
