/*
 * ioregs.h — ARM Cortex-M I/O register definitions
 *
 * Shared header for Private Peripheral Bus (PPB) registers used across
 * multiple kernel files (sched.c, xip.c, uart.c).
 * Works for both Cortex-M0+ (ARMv6-M) and Cortex-M33 (ARMv8-M).
 *
 * MPU registers remain local to mpu.c (single consumer).
 */

#ifndef PPAP_ARCH_ARM_M_IOREGS_H
#define PPAP_ARCH_ARM_M_IOREGS_H

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
 * ICSR:  Interrupt Control and State Register
 * VTOR:  Vector Table Offset Register
 * SHPR3: System Handler Priority Register 3 (PendSV + SysTick priority)
 * ────────────────────────────────────────────────────────────────────────── */

#define SCB_ICSR (*(volatile uint32_t *)0xE000ED04u)
#define SCB_VTOR (*(volatile uint32_t *)0xE000ED08u)
#define SCB_SHPR3 (*(volatile uint32_t *)0xE000ED20u)

/* ICSR bits */
#define PENDSVSET (1u << 28) /* set pending PendSV exception           */

/* SHPR2: SVCall priority at [31:24] */
#define SCB_SHPR2 (*(volatile uint32_t *)0xE000ED1Cu)
#define SVCALL_PRIO_SHIFT 24u
#define SVCALL_PRIO_MASK (0xFFu << SVCALL_PRIO_SHIFT)

/* SHPR3: PendSV priority at [23:16] */
#define PENDSV_PRIO_SHIFT 16u
#define PENDSV_PRIO_MASK (0xFFu << PENDSV_PRIO_SHIFT)
#define PENDSV_PRIO_LOWEST (0xFFu << PENDSV_PRIO_SHIFT)

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

/* ── SAU — Security Attribution Unit (ARMv8-M only) ─────────────────────
 *
 * Partitions memory into Secure, Non-Secure, and Non-Secure Callable (NSC)
 * regions.  Works alongside the IDAU (Implementation Defined Attribution
 * Unit) which provides the chip-level default security map.
 *
 * On RP2350: IDAU marks 0x10xxxxxx/0x20xxxxxx as Secure,
 * 0x00xxxxxx/0x30xxxxxx as Non-Secure aliases for the same physical memory.
 * ────────────────────────────────────────────────────────────────────────── */
#if __ARM_ARCH >= 8

#define SAU_CTRL REG(0xE000EDD0u) /* SAU Control Register              */
#define SAU_TYPE REG(0xE000EDD4u) /* SAU Type (number of regions)      */
#define SAU_RNR REG(0xE000EDD8u)  /* Region Number Register            */
#define SAU_RBAR REG(0xE000EDDCu) /* Region Base Address Register      */
#define SAU_RLAR REG(0xE000EDE0u) /* Region Limit Address Register     */

/* SAU_CTRL bits */
#define SAU_CTRL_ENABLE (1u << 0) /* enable SAU                          */
#define SAU_CTRL_ALLNS (1u << 1)  /* default: 1=all NS, 0=all Secure    */

/* SAU_RLAR bits */
#define SAU_RLAR_ENABLE (1u << 0) /* region enable                       */
#define SAU_RLAR_NSC (1u << 1)    /* 1=Non-Secure Callable, 0=Non-Secure */

/* NSACR — Non-Secure Access Control Register */
#define NSACR REG(0xE000ED8Cu)

/* SCB_AIRCR — Application Interrupt and Reset Control Register */
#define SCB_AIRCR REG(0xE000ED0Cu)
#define AIRCR_VECTKEY (0x05FAu << 16)
#define AIRCR_BFHFNMINS (1u << 13) /* BusFault/HardFault/NMI Non-Secure */

#endif /* __ARM_ARCH >= 8 */

/* ── Exception return values ─────────────────────────────────────────────── */

#define EXC_RETURN_THREAD_PSP 0xFFFFFFFDu /* return to Thread mode, PSP  */

#if __ARM_ARCH >= 8
/* ARMv8-M Non-Secure EXC_RETURN: Thread mode, PSP_NS, no FPU, DCRS=0.
 * DCRS=0 (bit 5) means the additional state context (r4-r11 +
 * IntegritySignature) is present on PSP_NS and will be popped by
 * hardware on exception return.  Used for NS process initial exec
 * and all subsequent context switches. */
#define EXC_RETURN_NS_THREAD_PSP 0xFFFFFF9Du  /* DCRS=0: HW pops r4-r11+IntSig */
#define EXC_RETURN_NS_THREAD_PSP_NODCRS 0xFFFFFFBDu /* DCRS=1: no additional ctx */

/* NS integrity signature pushed by hardware on NS→S exception entry.
 * Value depends on SPSEL: 0xFEFA125A (MSP) or 0xFEFA125B (PSP). */
#define NS_INTEGRITY_SIG_PSP 0xFEFA125Bu
#endif

/* ── Initial xPSR ────────────────────────────────────────────────────────── */

#define XPSR_THUMB_BIT 0x01000000u /* Thumb state bit (T=1, required)   */

#endif /* PPAP_ARCH_ARM_M_IOREGS_H */
