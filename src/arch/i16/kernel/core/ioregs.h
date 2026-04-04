/*
 * ioregs.h -- i8086 I/O register definitions
 *
 * On the IBM PC, peripherals are accessed via port I/O (in/out
 * instructions), not memory-mapped I/O.  The REG() macro is provided
 * for API compatibility but is not typically used on this architecture.
 *
 * Port I/O functions are in cpu.h (included via arch.h).
 */

#ifndef PPAP_ARCH_I16_IOREGS_H
#define PPAP_ARCH_I16_IOREGS_H

#include <stdint.h>

#ifndef REG
#define REG(addr) (*(volatile uint32_t *)(uintptr_t)(addr))
#endif

#endif /* PPAP_ARCH_I16_IOREGS_H */
