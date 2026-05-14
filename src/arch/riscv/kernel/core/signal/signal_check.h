/*
 * arch/riscv/kernel/core/signal/signal_check.h — RV32 signal_check
 *
 * Paired with src/arch/riscv/kernel/core/signal/signal_check.c.  Called
 * from trap.S on the ecall-return path; `regs` points at the live trap
 * frame so the body can rewrite mepc / sp / a0 / ra to enter a user
 * handler.
 *
 * Reached from non-arch callers via "kernel/core/signal/signal_check.h",
 * which resolves to this file through the arch overlay (-I src/arch/riscv
 * ahead of -I src).
 */

#ifndef PPAP_ARCH_RISCV_KERNEL_CORE_SIGNAL_SIGNAL_CHECK_H
#define PPAP_ARCH_RISCV_KERNEL_CORE_SIGNAL_SIGNAL_CHECK_H

#include <stdint.h>

void signal_check(uint32_t *regs);

#endif /* PPAP_ARCH_RISCV_KERNEL_CORE_SIGNAL_SIGNAL_CHECK_H */
