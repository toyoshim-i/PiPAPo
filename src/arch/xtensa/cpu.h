/*
 * cpu.h — Xtensa special register definitions
 *
 * Defines special register (SR) numbers, bit fields, and access macros
 * for the Xtensa LX7 cores in the ESP32-S3.
 *
 * Windowed ABI: ESP-IDF requires windowed register ABI.
 * Context switch uses solicited-frame pattern with window spill.
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
 * Bits [17:16] CALLINC   Call increment (windowed ABI)
 * Bit  [18]    WOE       Window Overflow Enable
 * Bits [11:8]  OWB       Old Window Base (windowed ABI)
 */

#define PS_INTLEVEL_MASK    0x0Fu
#define PS_INTLEVEL_SHIFT   0
#define PS_EXCM             (1u << 4)
#define PS_UM               (1u << 5)
#define PS_RING_MASK        (3u << 6)
#define PS_RING_SHIFT       6
#define PS_OWB_SHIFT        8
#define PS_OWB_MASK         (0xFu << PS_OWB_SHIFT)
#define PS_CALLINC_SHIFT    16
#define PS_CALLINC_MASK     (3u << PS_CALLINC_SHIFT)
#define PS_WOE              (1u << 18)

/* EXCM level for ESP32-S3 (max level at which EXCM exceptions can occur) */
#ifndef XCHAL_EXCM_LEVEL
#define XCHAL_EXCM_LEVEL    3u
#endif

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

/* ── ESP32-S3 memory map constants ─────────────────────────────────────── *
 *
 * Flash is accessible through two virtual address regions:
 *   DROM: 0x3C000000 (data reads via data cache)
 *   IROM: 0x42000000 (instruction fetches via instruction cache)
 *
 * The same flash content is at both addresses; XIP binaries stored in
 * romfs (which lives in .rodata → DROM) must have their entry point
 * shifted to IROM for the instruction bus to fetch them.
 */
#define XTENSA_DROM_BASE         0x3C000000u
#define XTENSA_IROM_BASE         0x42000000u
#define XTENSA_DROM_TO_IROM      (XTENSA_IROM_BASE - XTENSA_DROM_BASE)

/* Internal SRAM dual-mapping (ESP32-S3 SRAM1):
 *   DRAM: 0x3FC88000  (data bus — page_alloc returns these addresses)
 *   IRAM: 0x40380000  (instruction bus — CPU fetches from here)
 * Linear forward mapping: IRAM_addr = DRAM_addr + 0x6F8000 */
#define XTENSA_DRAM_BASE         0x3FC88000u
#define XTENSA_IRAM_BASE         0x40380000u
#define XTENSA_DRAM_TO_IRAM      (XTENSA_IRAM_BASE - XTENSA_DRAM_BASE)

#ifndef PPAP_SYS_HZ
#define PPAP_SYS_HZ 240000000u
#endif
#define XTENSA_TICK_INTERVAL (PPAP_SYS_HZ / 100u)

#endif /* PPAP_ARCH_XTENSA_CPU_H */
