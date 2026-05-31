#ifndef PPAP_TESTS_HOST_INCLUDE_KERNEL_COMMON_IRQ_H
#define PPAP_TESTS_HOST_INCLUDE_KERNEL_COMMON_IRQ_H

#include <stdint.h>

extern int test_in_irq_context;

static inline uint32_t arch_irq_save(void) { return 0u; }

static inline void arch_irq_restore(uint32_t saved) { (void)saved; }

static inline int arch_in_irq_context(void) { return test_in_irq_context; }

#endif /* PPAP_TESTS_HOST_INCLUDE_KERNEL_COMMON_IRQ_H */
