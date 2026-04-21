/*
 * timer.h -- Xtensa CCOMPARE0 scheduler-tick timer API
 *
 * xtensa_timer_init() programs CCOMPARE0 for periodic 10 ms ticks and
 * sets xtensa_timer_ready (declared in arch/xtensa/kernel/common/irq.h)
 * so arch_preempt_enable() stops being a no-op.  Targets call this
 * during late_init after all arch-level interrupt setup is complete.
 */

#ifndef PPAP_ARCH_XTENSA_KERNEL_CORE_TIMER_H
#define PPAP_ARCH_XTENSA_KERNEL_CORE_TIMER_H

void xtensa_timer_init(void);

#endif /* PPAP_ARCH_XTENSA_KERNEL_CORE_TIMER_H */
