/*
 * linker.h -- Symbols provided by the RISC-V linker script
 */

#ifndef PPAP_ARCH_RISCV_KERNEL_CORE_LINKER_H
#define PPAP_ARCH_RISCV_KERNEL_CORE_LINKER_H

/* RISC-V global pointer.  Placed by the linker at the middle of .sdata
 * so gp-relative addressing can reach the full .sdata/.sbss range with
 * 12-bit signed offsets.  boot.S / trap.S set the gp register from
 * this symbol; arch_build_initial_frame() seeds the initial gp in each
 * new process's trap frame. */
extern char __global_pointer$[];

#endif /* PPAP_ARCH_RISCV_KERNEL_CORE_LINKER_H */
