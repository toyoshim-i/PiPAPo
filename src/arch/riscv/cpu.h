/*
 * cpu.h — RISC-V system register (CSR) definitions
 *
 * Defines Machine-mode CSR addresses, bit fields, and register access
 * macros for the Hazard3 RV32IMAC cores in the RP2350.
 */

#ifndef PPAP_ARCH_RISCV_CPU_H
#define PPAP_ARCH_RISCV_CPU_H

#include <stdint.h>

/* ── Register access helper ─────────────────────────────────────────────── */

#ifndef REG
#define REG(addr)  (*(volatile uint32_t *)(uintptr_t)(addr))
#endif

/* ── CSR access macros ──────────────────────────────────────────────────── *
 *
 * RISC-V CSRs are accessed via dedicated instructions (csrr, csrw, etc.).
 * GCC provides __builtin_riscv_csrr* but they aren't portable — use
 * inline asm wrappers instead.
 */

#define csr_read(csr) ({                                        \
    uint32_t __v;                                               \
    __asm__ volatile ("csrr %0, " #csr : "=r"(__v));            \
    __v;                                                        \
})

#define csr_write(csr, val) ({                                  \
    uint32_t __v = (uint32_t)(val);                             \
    __asm__ volatile ("csrw " #csr ", %0" :: "r"(__v));         \
})

#define csr_set(csr, bits) ({                                   \
    uint32_t __v = (uint32_t)(bits);                            \
    __asm__ volatile ("csrs " #csr ", %0" :: "r"(__v));         \
})

#define csr_clear(csr, bits) ({                                 \
    uint32_t __v = (uint32_t)(bits);                            \
    __asm__ volatile ("csrc " #csr ", %0" :: "r"(__v));         \
})

/* ── mstatus — Machine Status Register (0x300) ─────────────────────────── */

#define MSTATUS_MIE     (1u << 3)   /* Machine Interrupt Enable            */
#define MSTATUS_MPIE    (1u << 7)   /* Previous Machine Interrupt Enable   */
#define MSTATUS_MPP_M   (3u << 11)  /* Previous privilege = M-mode (0b11)  */
#define MSTATUS_MPP_U   (0u << 11)  /* Previous privilege = U-mode (0b00)  */
#define MSTATUS_MPP_MASK (3u << 11)

/* ── mie — Machine Interrupt Enable Register (0x304) ───────────────────── */

#define MIE_MSIE        (1u << 3)   /* Machine Software Interrupt Enable   */
#define MIE_MTIE        (1u << 7)   /* Machine Timer Interrupt Enable      */
#define MIE_MEIE        (1u << 11)  /* Machine External Interrupt Enable   */

/* ── mip — Machine Interrupt Pending Register (0x344) ──────────────────── */

#define MIP_MSIP        (1u << 3)   /* Machine Software Interrupt Pending  */
#define MIP_MTIP        (1u << 7)   /* Machine Timer Interrupt Pending     */
#define MIP_MEIP        (1u << 11)  /* Machine External Interrupt Pending  */

/* ── mcause — Machine Cause Register (0x342) ───────────────────────────── *
 *
 * Bit 31: 1 = interrupt, 0 = exception.
 * Bits 30:0: cause code.
 */

#define MCAUSE_INTERRUPT    (1u << 31)

/* Exception codes (mcause[30:0] when bit 31 = 0) */
#define EXC_INSN_MISALIGN   0u
#define EXC_INSN_FAULT      1u
#define EXC_ILLEGAL_INSN    2u
#define EXC_BREAKPOINT      3u
#define EXC_LOAD_MISALIGN   4u
#define EXC_LOAD_FAULT      5u
#define EXC_STORE_MISALIGN  6u
#define EXC_STORE_FAULT     7u
#define EXC_ECALL_U         8u   /* ecall from U-mode (syscall)         */
#define EXC_ECALL_M         11u  /* ecall from M-mode                   */

/* Interrupt codes (mcause[30:0] when bit 31 = 1) */
#define IRQ_SOFT            3u   /* Machine software interrupt           */
#define IRQ_TIMER           7u   /* Machine timer interrupt              */
#define IRQ_EXTERNAL        11u  /* Machine external interrupt           */

/* ── mtvec — Machine Trap Vector Base (0x305) ──────────────────────────── *
 *
 * mtvec[1:0] = MODE:
 *   00 = Direct:   all traps go to BASE
 *   01 = Vectored: interrupts go to BASE + 4 * cause
 */

#define MTVEC_DIRECT    0u
#define MTVEC_VECTORED  1u

/* ── PMP — Physical Memory Protection ──────────────────────────────────── *
 *
 * pmpcfgN: 4 packed 8-bit config entries per 32-bit register.
 * Each entry: [L(7) | 00(6:5) | A(4:3) | X(2) | W(1) | R(0)]
 *
 * A field (address matching mode):
 *   00 = OFF, 01 = TOR, 10 = NA4, 11 = NAPOT
 */

#define PMP_R          (1u << 0)   /* Read                                */
#define PMP_W          (1u << 1)   /* Write                               */
#define PMP_X          (1u << 2)   /* Execute                             */
#define PMP_A_OFF      (0u << 3)   /* Disabled                            */
#define PMP_A_TOR      (1u << 3)   /* Top of Range                        */
#define PMP_A_NA4      (2u << 3)   /* Naturally aligned 4-byte            */
#define PMP_A_NAPOT    (3u << 3)   /* Naturally aligned power-of-two      */
#define PMP_L          (1u << 7)   /* Lock (also enforced in M-mode)      */

/* ── RP2350 timer (SIO-mapped) ─────────────────────────────────────────── *
 *
 * The Hazard3 RISC-V timer is mapped through the RP2350 SIO block at
 * base 0xD0000000.  mtime is shared between both cores; mtimecmp is
 * per-core (each core sees its own comparator at the same address).
 *
 * Timer interrupt (MTIP) asserts when mtime >= mtimecmp (unsigned 64-bit).
 * Writing mtimecmp clears the interrupt (no explicit acknowledge needed).
 *
 * Safe 64-bit mtimecmp write sequence (avoids spurious interrupts):
 *   1. Write 0xFFFFFFFF to mtimecmp_lo  (prevents match during update)
 *   2. Write new value to mtimecmph     (upper 32 bits)
 *   3. Write new value to mtimecmp_lo   (lower 32 bits)
 */

#define SIO_BASE            0xD0000000u
#define SIO_MTIME_CTRL      REG(SIO_BASE + 0x1A4u)
#define SIO_MTIME           REG(SIO_BASE + 0x1B0u)  /* lower 32 bits */
#define SIO_MTIMEH          REG(SIO_BASE + 0x1B4u)  /* upper 32 bits */
#define SIO_MTIMECMP        REG(SIO_BASE + 0x1B8u)  /* lower 32 bits (per-core) */
#define SIO_MTIMECMPH       REG(SIO_BASE + 0x1BCu)  /* upper 32 bits (per-core) */

/* mtime_ctrl bits */
#define MTIME_CTRL_EN       (1u << 0)   /* Timer enable                    */
#define MTIME_CTRL_FULLSPEED (1u << 1)  /* Run at full system clock        */

/* Timer tick interval for 10ms slices (set by target via PPAP_SYS_HZ) */
#ifndef PPAP_SYS_HZ
#define PPAP_SYS_HZ 150000000u
#endif
#define RISCV_TICK_INTERVAL (PPAP_SYS_HZ / 100u)

#endif /* PPAP_ARCH_RISCV_CPU_H */
