/*
 * arch/arm_m/kernel/core/signal/signal_check.h — ARM Cortex-M signal_check
 *
 * Paired with src/arch/arm_m/kernel/core/signal/signal_check.c.  Called
 * from SVC_Handler in trap.S on the syscall-return path; manipulates PSP
 * directly to build a user-handler delivery frame.
 *
 * Reached from non-arch callers via "kernel/core/signal/signal_check.h",
 * which resolves to this file through the arch overlay (-I src/arch/arm_m
 * ahead of -I src).
 */

#ifndef PPAP_ARCH_ARM_M_KERNEL_CORE_SIGNAL_SIGNAL_CHECK_H
#define PPAP_ARCH_ARM_M_KERNEL_CORE_SIGNAL_SIGNAL_CHECK_H

void signal_check(void);

#endif /* PPAP_ARCH_ARM_M_KERNEL_CORE_SIGNAL_SIGNAL_CHECK_H */
