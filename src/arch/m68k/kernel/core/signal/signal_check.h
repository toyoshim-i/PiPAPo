/*
 * arch/m68k/kernel/core/signal/signal_check.h — m68k signal_check
 *
 * Paired with src/arch/m68k/kernel/core/signal/signal_check.c.  Called
 * from the m68k target wrappers (qemu_m68k, x68k) after syscall_dispatch
 * on the trap-return path; `regs` points at the saved d0-d7/a0-a6/SR/PC
 * frame on the supervisor stack so the body can rewrite the SR/PC slots
 * to enter a user handler.
 *
 * Reached from non-arch callers via "kernel/core/signal/signal_check.h",
 * which resolves to this file through the arch overlay (-I src/arch/m68k
 * ahead of -I src).
 */

#ifndef PPAP_ARCH_M68K_KERNEL_CORE_SIGNAL_SIGNAL_CHECK_H
#define PPAP_ARCH_M68K_KERNEL_CORE_SIGNAL_SIGNAL_CHECK_H

#include <stdint.h>

void signal_check(uint32_t *regs);

#endif /* PPAP_ARCH_M68K_KERNEL_CORE_SIGNAL_SIGNAL_CHECK_H */
