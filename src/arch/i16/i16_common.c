/*
 * i16_common.c -- Shared i16 architecture state and helpers
 */

#include <stdint.h>
#include <stddef.h>

#include "kernel/proc/proc.h"

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

/* ── i16 syscall dispatch (called from trap.S INT 30h handler) ─────────────
 *
 * Adapts the i16 register-based syscall ABI to the kernel's
 * syscall_dispatch() which expects a uint32_t frame pointer.
 *
 * user_ds is the saved DS from the user process.  For syscalls that pass
 * user-space pointers (read, write, open, etc.), the offset argument is
 * resolved to a 20-bit linear address: user_ds * 16 + offset.
 */
long i16_syscall_dispatch(uint16_t nr, uint16_t a0, uint16_t a1,
                          uint16_t a2, uint16_t a3, uint16_t a4,
                          uint16_t user_ds)
{
  uint32_t seg_base = (uint32_t)user_ds << 4;

  /* Save user DS for uaccess (copy_from_user / copy_to_user). */
  current->user_ds = user_ds;

  /* The kernel's syscall_dispatch reads args from frame[0..3] and
   * writes the return value back to frame[0]. */
  uint32_t frame[4];
  frame[0] = a0;
  frame[1] = a1;
  frame[2] = a2;
  frame[3] = a3;

  /* Resolve user-space pointer arguments to 20-bit linear addresses.
   * Syscalls that use uaccess (copy_from_user, strncpy_from_user) need
   * the linear address; the uaccess functions handle page resolution.
   * This switch will shrink as more syscalls move to uaccess. */
  switch (nr) {
    case 0x0003: /* SYS_READ:  a1 = buf */
    case 0x0004: /* SYS_WRITE: a1 = buf */
      frame[1] = seg_base + a1;
      break;
    case 0x0005: /* SYS_OPEN:  a0 = path */
    case 0x000B: /* SYS_EXECVE: a0 = path */
    case 0x000C: /* SYS_CHDIR: a0 = path */
    case 0x0015: /* SYS_ACCESS: a0 = path */
    case 0x0026: /* SYS_MKDIR: a0 = path */
    case 0x000A: /* SYS_UNLINK: a0 = path */
    case 0x0028: /* SYS_RMDIR: a0 = path */
      frame[0] = seg_base + a0;
      break;
    case 0x0036: /* SYS_IOCTL: a2 = arg */
      frame[2] = seg_base + a2;
      break;
    default:
      break;
  }

  extern void syscall_dispatch(uint32_t *frame, uint32_t nr,
                               uint32_t a4, uint32_t a5);
  syscall_dispatch(frame, (uint32_t)nr, (uint32_t)a4, 0);

  /* syscall_dispatch stores result in frame[0] on ARM/m68k.
   * On i16 we return it to trap.S which puts it in saved AX. */
  return (long)(int16_t)frame[0];
}

/* ── Signal return stubs (TODO: implement for P-4) ────────────────────────── */

long sys_sigreturn(void)
{
  return 0;
}

long sys_rt_sigreturn(void)
{
  return 0;
}
