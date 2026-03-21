/*
 * ioregs.h — Architecture dispatch header for I/O register definitions
 *
 * Includes the correct arch-specific ioregs.h based on compiler defines.
 * Shared kernel code includes this instead of a specific architecture.
 */

#ifndef PPAP_ARCH_DISPATCH_IOREGS_H
#define PPAP_ARCH_DISPATCH_IOREGS_H

#if defined(__ARM_ARCH) || defined(__arm__) || defined(__thumb__)
#include "arm_m/ioregs.h"
#elif defined(__m68k__)
#include "m68k/ioregs.h"
#else
#error "Unsupported architecture"
#endif

#endif /* PPAP_ARCH_DISPATCH_IOREGS_H */
