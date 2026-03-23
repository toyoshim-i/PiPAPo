/*
 * i16_common.c -- Shared i16 architecture state
 *
 * Phase P-1: minimal stubs.  Timer, exception handler, and context
 * switch will be added in P-2.
 */

#include <stdint.h>

/* Context switch pending flag.
 * Set by arch_yield().  Checked by timer ISR (P-2).
 * Same pattern as m68k_switch_pending / riscv_switch_pending. */
volatile uint16_t i16_switch_pending = 0;

/* SysTick-equivalent counter (P-2) */
volatile uint32_t i16_tick_count = 0;

/* -- Initial stack frame for new processes (P-2 stub) ------------------- */

uint32_t *arch_build_initial_frame(uint32_t *sp, void (*entry)(void))
{
  /* TODO: implement for P-2 context switch */
  (void)sp;
  (void)entry;
  return sp;
}
