/*
 * dos_trap.h -- ISR entry points defined in dos_trap.S
 */

#ifndef PPAP_ARCH_I16_KERNEL_CORE_DOS_TRAP_H
#define PPAP_ARCH_I16_KERNEL_CORE_DOS_TRAP_H

void i16_dos_isr(void);
void i16_dos_int20_isr(void);
void i16_dos_int23_isr(void);
void i16_dos_int24_isr(void);
void i16_dos_int27_isr(void);
void i16_dos_int29_isr(void);
void i16_dos_int2e_isr(void);
void i16_dos_int2f_isr(void);

#endif /* PPAP_ARCH_I16_KERNEL_CORE_DOS_TRAP_H */
