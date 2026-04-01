/*
 * i16_common.c -- Shared i16 architecture state and helpers
 */

#include <stdint.h>
#include <stddef.h>

/* Context switch pending flag.
 * Set by arch_yield().  Checked by timer ISR in switch.S.
 * Same pattern as m68k_switch_pending / riscv_switch_pending. */
volatile uint16_t i16_switch_pending = 0;

/* Tick counter incremented by timer handler */
volatile uint32_t i16_tick_count = 0;

/* Current INT 30h saved-frame SP, captured by trap.S for sys_sigreturn. */
volatile uint16_t i16_trap_frame_sp = 0;

/* Non-zero when sys_sigreturn wants trap.S to restore a different frame. */
volatile uint16_t i16_sigreturn_restore_sp = 0;

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

/* ── i16 syscall dispatch (called from trap.S INT 30h handler) ─────────────
 *
 * Adapts the i16 register-based syscall ABI to the kernel's
 * syscall_dispatch() which expects a uint32_t frame pointer.
 *
 * user_ds is still passed from trap.S for future signal / segment work, but
 * the shared syscall layer now receives the raw 16-bit register values and
 * resolves user pointers itself through arch_user_ptr_to_page().
 */
long i16_syscall_dispatch(uint16_t nr, uint16_t a0, uint16_t a1,
                          uint16_t a2, uint16_t a3, uint16_t a4,
                          uint16_t user_ds)
{
  (void)user_ds;

  /* The kernel's syscall_dispatch reads args from frame[0..3] and
   * writes the return value back to frame[0]. */
  uint32_t frame[4];
  frame[0] = a0;
  frame[1] = a1;
  frame[2] = a2;
  frame[3] = a3;

  extern void syscall_dispatch(uint32_t *frame, uint32_t nr,
                               uint32_t a4, uint32_t a5);
  syscall_dispatch(frame, (uint32_t)nr, (uint32_t)a4, 0);

  /* syscall_dispatch stores result in frame[0] on ARM/m68k.
   * On i16 we return it to trap.S which puts it in saved AX. */
  return (long)(int16_t)frame[0];
}
