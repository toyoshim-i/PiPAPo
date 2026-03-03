/*
 * cortex_m0plus.h — ARM Cortex-M0+ system register definitions
 *
 * Shared header for Private Peripheral Bus (PPB) registers used across
 * multiple kernel files (sched.c, xip.c, uart.c).
 *
 * MPU registers remain local to mpu.c (single consumer).
 */

#ifndef PPAP_HW_CORTEX_M0PLUS_H
#define PPAP_HW_CORTEX_M0PLUS_H

#include <stdint.h>

/* ── Register access helper ─────────────────────────────────────────────── */

#ifndef REG
#define REG(addr)  (*(volatile uint32_t *)(uintptr_t)(addr))
#endif

/* ── SysTick — ARM Cortex-M0+ system timer (§B3.3) ──────────────────────
 *
 * 24-bit down-counter clocked from the processor clock.
 * At 133 MHz, 0xFFFFFF ticks ≈ 126 ms.
 * ────────────────────────────────────────────────────────────────────────── */

#define SYST_CSR  REG(0xE000E010u)  /* Control and Status Register       */
#define SYST_RVR  REG(0xE000E014u)  /* Reload Value Register (24-bit)    */
#define SYST_CVR  REG(0xE000E018u)  /* Current Value Register (counts ↓) */

/* CSR bits */
#define SYST_CSR_ENABLE         (1u << 0)  /* counter enable               */
#define SYST_CSR_TICKINT        (1u << 1)  /* exception on count to 0      */
#define SYST_CSR_CLKSOURCE      (1u << 2)  /* 1 = processor clock          */

/* 24-bit maximum reload / mask */
#define SYST_MAX  0x00FFFFFFu

/* ── SCB — System Control Block ──────────────────────────────────────────
 *
 * ICSR:  Interrupt Control and State Register
 * VTOR:  Vector Table Offset Register
 * SHPR3: System Handler Priority Register 3 (PendSV + SysTick priority)
 * ────────────────────────────────────────────────────────────────────────── */

#define SCB_ICSR   (*(volatile uint32_t *)0xE000ED04u)
#define SCB_VTOR   (*(volatile uint32_t *)0xE000ED08u)
#define SCB_SHPR3  (*(volatile uint32_t *)0xE000ED20u)

/* ICSR bits */
#define PENDSVSET  (1u << 28)  /* set pending PendSV exception           */

/* SHPR3: PendSV priority at [23:16] */
#define PENDSV_PRIO_SHIFT  16u
#define PENDSV_PRIO_MASK   (0xFFu << PENDSV_PRIO_SHIFT)
#define PENDSV_PRIO_LOWEST (0xFFu << PENDSV_PRIO_SHIFT)

/* ── NVIC — Nested Vectored Interrupt Controller ─────────────────────────── */

#define NVIC_ISER  REG(0xE000E100u)  /* Interrupt Set-Enable Register     */

/* ── Exception return values ─────────────────────────────────────────────── */

#define EXC_RETURN_THREAD_PSP  0xFFFFFFFDu  /* return to Thread mode, PSP  */

/* ── Initial xPSR ────────────────────────────────────────────────────────── */

#define XPSR_THUMB_BIT  0x01000000u  /* Thumb state bit (T=1, required)   */

#endif /* PPAP_HW_CORTEX_M0PLUS_H */
