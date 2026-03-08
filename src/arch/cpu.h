/*
 * cpu.h — Architecture dispatch header
 *
 * Includes the correct arch-specific cpu.h based on compiler defines.
 * Shared kernel code includes this instead of a specific architecture.
 */

#ifndef PPAP_ARCH_DISPATCH_CPU_H
#define PPAP_ARCH_DISPATCH_CPU_H

#if defined(__ARM_ARCH) || defined(__arm__) || defined(__thumb__)
#include "arm_m/cpu.h"
#elif defined(__m68k__)
#include "m68k/cpu.h"
#else
#error "Unsupported architecture"
#endif

#endif /* PPAP_ARCH_DISPATCH_CPU_H */
