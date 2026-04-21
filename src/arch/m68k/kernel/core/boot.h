/*
 * boot.h -- Symbols exported by arch/m68k/boot/boot.S
 */

#ifndef PPAP_ARCH_M68K_KERNEL_CORE_BOOT_H
#define PPAP_ARCH_M68K_KERNEL_CORE_BOOT_H

/* No-op IRQ handler (RTE-only) used to plug IVT vectors that have no
 * dedicated kernel handler.  Targets that install custom ISRs point
 * any remaining "auto-vector" slots here so spurious IRQs don't fault. */
void m68k_irq_ignore(void);

#endif /* PPAP_ARCH_M68K_KERNEL_CORE_BOOT_H */
