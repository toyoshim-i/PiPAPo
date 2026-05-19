/*
 * arch/riscv/.../backtrace.c — RISC-V frame-pointer chain walker
 *
 * Each stack frame stores
 *   [fp-4] return address (ra)
 *   [fp-8] previous frame pointer (saved s0)
 * Requires -fno-omit-frame-pointer in CFLAGS.
 */

#include "kernel/core/backtrace.h"

#include <stdint.h>

#include "kernel/common/mod/mod_vfs.h"

void stack_backtrace(void) {
  uintptr_t fp;
  __asm__ volatile("mv %0, s0" : "=r"(fp));

  mod_vfs.klogf("  backtrace:\n");
  for (uint32_t depth = 0; depth < 16 && fp; depth++) {
    uintptr_t ra = *(uintptr_t *)(fp - 4);
    uintptr_t prev_fp = *(uintptr_t *)(fp - 8);
    mod_vfs.klogf("    #%u ra=%lx fp=%lx\n", depth, (unsigned long)ra,
                  (unsigned long)fp);
    if (prev_fp <= fp) break; /* stack grows down — prev fp must be higher */
    fp = prev_fp;
  }
}
