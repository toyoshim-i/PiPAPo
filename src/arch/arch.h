/*
 * arch.h — Architecture dispatch header
 *
 * Includes the correct arch-specific arch.h based on compiler defines.
 * Shared kernel code includes this instead of a specific architecture.
 */

#ifndef PPAP_ARCH_ARCH_H
#define PPAP_ARCH_ARCH_H

#include <stddef.h>

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

/* ── User-space memory access ───────────────────────────────────────────
 *
 * read_user_byte(ptr) — read one byte from a user-space address.
 *
 * Each arch/<arch>/arch.h provides this.  On most architectures it is a
 * plain byte dereference.  Xtensa needs word-aligned reads because user
 * code/rodata lives in IRAM (word-access only).
 *
 * copy_from_user(dst, src, n) — copy n bytes from user-space to kernel.
 * Default implementation using read_user_byte(); architectures may
 * override with a more efficient version.
 */
static inline void copy_from_user(void *dst, const void *src, size_t n) {
  uint8_t *d = (uint8_t *)dst;
  const uint8_t *s = (const uint8_t *)src;
  for (size_t i = 0; i < n; i++)
    d[i] = read_user_byte(s + i);
}

#endif /* PPAP_ARCH_ARCH_H */
