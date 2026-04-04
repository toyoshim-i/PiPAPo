/*
 * arch.h — Architecture dispatch header
 *
 * Includes the correct arch-specific arch.h based on compiler defines.
 * Shared kernel code includes this instead of a specific architecture.
 */

#ifndef PPAP_ARCH_ARCH_H
#define PPAP_ARCH_ARCH_H

#include <stddef.h>

/* The arch-specific arch.h is found via the -I src/arch/<arch>/ overlay.
 * "kernel/core/arch.h" resolves to src/arch/<arch>/kernel/core/arch.h. */
#include "kernel/core/arch.h"

/* ── Initial stack frame builder ──────────────────────────────────────────
 *
 * Each architecture provides this function to build the stack frame that
 * the context-switch path expects when switching to a new process.
 * Called by proc_setup_stack().
 *
 *   sp    — top of stack (stack grows downward)
 *   entry — function to execute when the process is first scheduled
 *
 * Returns the new SP pointing to the base of the constructed frame.
 */
uint32_t *arch_build_initial_frame(uint32_t *sp, void (*entry)(void));

#endif /* PPAP_ARCH_ARCH_H */
