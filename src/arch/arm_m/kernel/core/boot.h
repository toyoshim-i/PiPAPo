/*
 * boot.h -- Symbols exported by arch/arm_m/boot/boot.S
 */

#ifndef PPAP_ARCH_ARM_M_KERNEL_CORE_BOOT_H
#define PPAP_ARCH_ARM_M_KERNEL_CORE_BOOT_H

/* Infinite-WFE landing pad used as a "safe place to return to" after
 * a kernel hardfault.  arm_m_common.c rewrites the trapped PSP frame's
 * return PC to point here so the faulting kernel context unwinds into
 * a deterministic halt instead of executing arbitrary memory. */
void Default_Handler(void);

#endif /* PPAP_ARCH_ARM_M_KERNEL_CORE_BOOT_H */
