/*
 * ioregs.h — Architecture dispatch header for I/O register definitions
 *
 * Includes the correct arch-specific ioregs.h based on compiler defines.
 * Shared kernel code includes this instead of a specific architecture.
 */

#ifndef PPAP_ARCH_IOREGS_H
#define PPAP_ARCH_IOREGS_H

/* The arch-specific ioregs.h is found via the -I src/arch/<arch>/ overlay.
 * "kernel/core/ioregs.h" resolves to src/arch/<arch>/kernel/core/ioregs.h. */
#include "kernel/core/ioregs.h"

#endif /* PPAP_ARCH_IOREGS_H */
