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
#elif defined(__xtensa__)
#include "xtensa/arch.h"
#else
#error "Unsupported architecture"
#endif

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
