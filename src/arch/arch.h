/*
 * arch.h — Architecture dispatch header
 *
 * Includes the correct arch-specific arch.h based on compiler defines.
 * Shared kernel code includes this instead of a specific architecture.
 */

#ifndef PPAP_ARCH_ARCH_H
#define PPAP_ARCH_ARCH_H

#if defined(__ARM_ARCH) || defined(__arm__) || defined(__thumb__)
#include "arm_m/arch.h"
#elif defined(__m68k__)
#include "m68k/arch.h"
#elif defined(__riscv)
#include "riscv/arch.h"
#else
#error "Unsupported architecture"
#endif

#endif /* PPAP_ARCH_ARCH_H */
