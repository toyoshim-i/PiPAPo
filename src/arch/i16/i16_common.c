/*
 * i16_common.c -- Shared i16 architecture state and helpers
 */

#include <stdint.h>

/* Context switch pending flag.
 * Set by arch_yield().  Checked by timer ISR in switch.S.
 * Same pattern as m68k_switch_pending / riscv_switch_pending. */
volatile uint16_t i16_switch_pending = 0;

/* Tick counter incremented by timer handler */
volatile uint32_t i16_tick_count = 0;

/* -- Initial stack frame for new processes --------------------------------
 *
 * Build a 24-byte frame matching what i16_timer_isr saves/restores:
 *   [SP+0 ] ES    [SP+2 ] DS    [SP+4 ] BP    [SP+6 ] DI
 *   [SP+8 ] SI    [SP+10] DX    [SP+12] CX    [SP+14] BX
 *   [SP+16] AX    [SP+18] IP    [SP+20] CS    [SP+22] FLAGS
 *
 * When the ISR's restore path pops this frame and executes IRET,
 * the CPU loads FLAGS/CS/IP and the process starts at entry().
 */
uint32_t *arch_build_initial_frame(uint32_t *sp_arg, void (*entry)(void))
{
  uint16_t *sp = (uint16_t *)(uintptr_t)sp_arg;

  /* Hardware interrupt frame (popped by IRET) */
  *--sp = 0x0200;                     /* FLAGS: IF=1 (interrupts enabled) */
  *--sp = 0x0000;                     /* CS = 0 (flat model) */
  *--sp = (uint16_t)(uintptr_t)entry; /* IP = entry point */

  /* Software-saved registers (popped by ISR restore path) */
  *--sp = 0;  /* AX */
  *--sp = 0;  /* BX */
  *--sp = 0;  /* CX */
  *--sp = 0;  /* DX */
  *--sp = 0;  /* SI */
  *--sp = 0;  /* DI */
  *--sp = 0;  /* BP */
  *--sp = 0;  /* DS = 0 (flat model) */
  *--sp = 0;  /* ES = 0 (flat model) */

  return (uint32_t *)(uintptr_t)sp;
}
