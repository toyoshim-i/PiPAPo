/*
 * ioregs.h — ARM Cortex-M I/O register definitions
 *
 * Shared header for Private Peripheral Bus (PPB) registers used across
 * multiple kernel files (sched.c, xip.c, uart.c).
 * Works for both Cortex-M0+ (ARMv6-M) and Cortex-M33 (ARMv8-M).
 *
 * MPU registers remain local to mpu.c (single consumer).
 */

#ifndef PPAP_ARCH_ARM_M_KERNEL_COMMON_IOREGS_H
#define PPAP_ARCH_ARM_M_KERNEL_COMMON_IOREGS_H

#include <stdint.h>

/* ── Register access helper ─────────────────────────────────────────────── */

#ifndef REG
#define REG(addr) (*(volatile uint32_t *)(uintptr_t)(addr))
#endif

/* ── SysTick — ARM Cortex-M system timer (§B3.3) ───────────────────────
 *
 * 24-bit down-counter clocked from the processor clock.
 * ────────────────────────────────────────────────────────────────────────── */

#define SYST_CSR REG(0xE000E010u) /* Control and Status Register       */
#define SYST_RVR REG(0xE000E014u) /* Reload Value Register (24-bit)    */
#define SYST_CVR REG(0xE000E018u) /* Current Value Register (counts ↓) */

/* CSR bits */
#define SYST_CSR_ENABLE (1u << 0)    /* counter enable               */
#define SYST_CSR_TICKINT (1u << 1)   /* exception on count to 0      */
#define SYST_CSR_CLKSOURCE (1u << 2) /* 1 = processor clock          */

/* 24-bit maximum reload / mask */
#define SYST_MAX 0x00FFFFFFu

/* ── SCB — System Control Block ──────────────────────────────────────────
 *
 * VTOR:  Vector Table Offset Register
 * SHPR2: System Handler Priority Register 2 (SVCall priority byte)
 * ────────────────────────────────────────────────────────────────────────── */

#define SCB_VTOR (*(volatile uint32_t *)0xE000ED08u)

/* SHPR2: SVCall priority at [31:24] */
#define SCB_SHPR2 (*(volatile uint32_t *)0xE000ED1Cu)
#define SVCALL_PRIO_SHIFT 24u
#define SVCALL_PRIO_MASK (0xFFu << SVCALL_PRIO_SHIFT)

/* ── NVIC — Nested Vectored Interrupt Controller ─────────────────────────── */

#define NVIC_ISER \
  REG(0xE000E100u) /* Interrupt Set-Enable Register  (IRQ 0-31)  */
#define NVIC_ICER \
  REG(0xE000E180u) /* Interrupt Clear-Enable Register (IRQ 0-31) */
#define NVIC_ISPR \
  REG(0xE000E200u) /* Interrupt Set-Pending Register  (IRQ 0-31) */
#define NVIC_ICPR \
  REG(0xE000E280u) /* Interrupt Clear-Pending Register (IRQ 0-31)*/

/* Bank 1 — needed for RP2350 which has IRQs up to 51 */
#define NVIC_ISER1 \
  REG(0xE000E104u) /* Interrupt Set-Enable Register  (IRQ 32-63) */
#define NVIC_ICER1 \
  REG(0xE000E184u) /* Interrupt Clear-Enable Register (IRQ 32-63)*/
#define NVIC_ICPR1 \
  REG(0xE000E284u) /* Interrupt Clear-Pending Register (IRQ 32-63)*/

/* ── Exception return values ─────────────────────────────────────────────── */

#define EXC_RETURN_THREAD_PSP 0xFFFFFFFDu /* return to Thread mode, PSP  */

/* ── Initial xPSR ────────────────────────────────────────────────────────── */

#define XPSR_THUMB_BIT 0x01000000u /* Thumb state bit (T=1, required)   */

#endif /* PPAP_ARCH_ARM_M_KERNEL_COMMON_IOREGS_H */
