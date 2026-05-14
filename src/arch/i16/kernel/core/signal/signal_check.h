/*
 * arch/i16/kernel/core/signal/signal_check.h — ia16 signal_check
 *
 * Paired with src/arch/i16/kernel/core/signal/signal_check.c.  Called
 * from trap.S on the INT 30h-return path with (user_sp, user_ss) from
 * the kernel-stack slot; returns the new user_SP that trap.S restores
 * into the slot.
 *
 * Reached from non-arch callers via "kernel/core/signal/signal_check.h",
 * which resolves to this file through the arch overlay (-I src/arch/i16
 * ahead of -I src).
 */

#ifndef PPAP_ARCH_I16_KERNEL_CORE_SIGNAL_SIGNAL_CHECK_H
#define PPAP_ARCH_I16_KERNEL_CORE_SIGNAL_SIGNAL_CHECK_H

#include <stdint.h>

uint16_t signal_check(uint16_t user_sp, uint16_t user_ss);

#endif /* PPAP_ARCH_I16_KERNEL_CORE_SIGNAL_SIGNAL_CHECK_H */
