/*
 * arch/xtensa/kernel/core/signal/signal_check.h — Xtensa signal_check
 *
 * Paired with src/arch/xtensa/kernel/core/signal/signal_check.c.  Stub
 * until trap glue wires real delivery.
 *
 * Reached from non-arch callers via "kernel/core/signal/signal_check.h",
 * which resolves to this file through the arch overlay (-I src/arch/xtensa
 * ahead of -I src).
 */

#ifndef PPAP_ARCH_XTENSA_KERNEL_CORE_SIGNAL_SIGNAL_CHECK_H
#define PPAP_ARCH_XTENSA_KERNEL_CORE_SIGNAL_SIGNAL_CHECK_H

#include "xtensa_api.h" /* XtExcFrame */

void signal_check(XtExcFrame *frame);

#endif /* PPAP_ARCH_XTENSA_KERNEL_CORE_SIGNAL_SIGNAL_CHECK_H */
