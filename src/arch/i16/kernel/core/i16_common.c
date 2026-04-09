/*
 * i16_common.c -- Shared i16 architecture state and helpers
 */

#include <stddef.h>
#include <stdint.h>

/* Current process's kernel stack pointer.  ISR/syscall entry loads
 * SS:SP from SS=0:i16_current_ksp.  Updated on context switch and
 * at boot.  Avoids dereferencing current_core before SS=0 is set. */
volatile uint16_t i16_current_ksp = 0xE800; /* kernel_stack[0] top */

/* Tick counter incremented by timer handler */
volatile uint32_t i16_tick_count = 0;

/* Current INT 30h saved-frame SP, captured by trap.S for sys_sigreturn. */
volatile uint16_t i16_trap_frame_sp = 0;

/* Non-zero when sys_sigreturn wants trap.S to restore a different frame. */
volatile uint16_t i16_sigreturn_restore_sp = 0;

/* -- Initial stack frame for new processes --------------------------------
 *
 * New frame layout (split between user stack and kernel stack):
 *
 * User/interrupted stack (24 bytes, popped by ISR restore + IRET):
 *   [uSP+0 ] ES    [uSP+2 ] DS    [uSP+4 ] BP    [uSP+6 ] DI
 *   [uSP+8 ] SI    [uSP+10] DX    [uSP+12] CX    [uSP+14] BX
 *   [uSP+16] AX    [uSP+18] IP    [uSP+20] CS    [uSP+22] FLAGS
 *
 * Kernel stack (4 bytes, top of frame = pcb.sp):
 *   [kSP+0] user_SS    [kSP+2] user_SP
 *
 * This function builds the kernel-only case (SS=0, PID 0) where
 * both parts are on the same stack.  User processes use elf16_loader
 * which builds the split frame directly.
 */
uint32_t *arch_build_initial_frame(uint32_t *sp_arg, void (*entry)(void)) {
  uint16_t *sp = (uint16_t *)(uintptr_t)sp_arg;

  /* Hardware interrupt frame (popped by IRET) */
  *--sp = 0x0200;                     /* FLAGS: IF=1 (interrupts enabled) */
  *--sp = 0x0000;                     /* CS = 0 (kernel code segment) */
  *--sp = (uint16_t)(uintptr_t)entry; /* IP = entry point */

  /* Software-saved registers (popped by ISR restore path) */
  *--sp = 0; /* AX */
  *--sp = 0; /* BX */
  *--sp = 0; /* CX */
  *--sp = 0; /* DX */
  *--sp = 0; /* SI */
  *--sp = 0; /* DI */
  *--sp = 0; /* BP */
  *--sp = 0; /* DS = 0 */
  *--sp = 0; /* ES = 0 */

  /* user_SS:user_SP — for PID 0 (kernel), SS=0 and SP points to
   * the GP frame above on the same stack. */
  uint16_t user_sp = (uint16_t)(uintptr_t)sp;
  *--sp = 0;       /* user_SS = 0 */
  *--sp = user_sp; /* user_SP = points to ES at top of GP frame */

  /* Reserve 24-byte vfork-save slot — matches the trap.S/switch.S
   * convention.  Restore paths always addw $24,%sp before popping
   * user_SP/user_SS. */
  sp -= 12;

  return (uint32_t *)(uintptr_t)sp;
}

/* ── vfork parent frame restore ───────────────────────────────────────────
 *
 * Called from trap.S before the restore path when the current process
 * is a vfork parent whose GP+IRET frame was saved on the kernel stack.
 * Copies the 24-byte saved frame back to the user stack so the parent
 * resumes with its original register state.
 *
 * kstack_sp: current kernel stack pointer (points to [user_SP, user_SS,
 *            24B saved frame]).
 * Returns the same kstack_sp (frame stays on kstack for pop). */
#include "kernel/common/mod/mod_vfs.h"
#include "kernel/core/mm/mem_region.h"
#include "kernel/core/proc/proc.h"

int i16_timer_can_preempt(uint16_t interrupted_ss) {
  if (interrupted_ss != 0) return 1;
  return current && current->is_idle;
}

int i16_trap_should_skip_ret_store(void) {
  return current && current->vfork_frame_saved;
}

int i16_trap_should_switch_now(void) {
  return !current || current->state != PROC_RUNNABLE;
}

void i16_vfork_restore_frame(void) {
  if (!current || !current->vfork_frame_saved) return;
  current->vfork_frame_saved = 0;

  /* Layout at the top of the parent's kernel stack (anchored to
   * kernel_stack_top by trap.S, independent of current SP):
   *
   *   ktop - 2  ┐ user_SS  (pushed by trap.S i16_syscall_isr)
   *   ktop - 4  ┘ user_SP
   *   ktop - 28 ┐ saved 24-byte GP+IRET frame slot
   *             │  (reserved by `subw $24,%sp` in trap.S, written by
   *             │   sys_vfork; lives ABOVE the C call chain so the
   *             │   compiler-managed frames never alias it)
   *   ktop - 28 ┘
   *   ...        C call chain frames live below ktop - 28 */
  uint16_t ktop = current->kernel_stack_top;
  uint16_t *frame_top = (uint16_t *)(uintptr_t)(uint16_t)(ktop - 4u);
  uint16_t user_sp = frame_top[0];
  uint16_t user_ss = frame_top[1];
  uint8_t *saved = (uint8_t *)(uintptr_t)(uint16_t)(ktop - 28u);

  /* AX slot (offset 16) in the saved frame already contains the
   * parent's vfork return value (child PID), patched by sys_vfork. */

  /* Write 24 bytes back to user stack at user_SS:user_SP */
  uint32_t frame_linear = (uint32_t)user_ss * 16 + user_sp;
  uint32_t data_base = mem_region_page_linear(current->image.data.base_page);
  uint32_t rel = frame_linear - data_base;
  page_id_t page = current->image.data.base_page + (page_id_t)(rel / PAGE_SIZE);
  uint16_t off = (uint16_t)(rel % PAGE_SIZE);
  mem_region_page_write(page, off, saved, 24);

  /* Catch any kernel-stack overrun before we iret to user mode. */
  proc_check_kstack_canary_panic();
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
long i16_syscall_dispatch(uint16_t nr, uint16_t a0, uint16_t a1, uint16_t a2,
                          uint16_t a3, uint16_t a4, uint16_t user_ds) {
  (void)user_ds;

  /* Snapshot our own saved return IP (the word at 2(%bp), placed by
   * the call from trap.S).  We re-read it before returning and panic
   * if it has been overwritten — that's the case where a wild kernel
   * write corrupted our stack frame and our `ret` would jump into
   * garbage instead of back to i16_syscall_isr. */
  uint16_t saved_ret;
  /* The compiler's prologue is `push si; push di; push es; push bp;
   * mov sp, bp` so bp points at the saved-bp slot.  Skipping over
   * three callee-saved words (si, di, es) gives us the return IP at
   * 8(bp), not 2(bp). */
  __asm__ volatile("mov 8(%%bp), %0" : "=r"(saved_ret));

  /* The kernel's syscall_dispatch reads args from frame[0..3] and
   * writes the return value back to frame[0]. */
  uint32_t frame[4];
  frame[0] = a0;
  frame[1] = a1;
  frame[2] = a2;
  frame[3] = a3;

  extern void syscall_dispatch(uint32_t * frame, uint32_t nr, uint32_t a4,
                               uint32_t a5);
  syscall_dispatch(frame, (uint32_t)nr, (uint32_t)a4, 0);

  /* Catch any kernel-stack overrun before we return to trap.S. */
  proc_check_kstack_canary_panic();

  uint16_t now_ret;
  __asm__ volatile("mov 8(%%bp), %0" : "=r"(now_ret));
  if (now_ret != saved_ret || now_ret == 0u) {
    pcb_t *cur = current;
    mod_vfs.klogf(
        "PANIC: i16_syscall_dispatch return IP corrupted  "
        "saved=%x now=%x  nr=%u  pid=%u comm=%s\n",
        (unsigned)saved_ret, (unsigned)now_ret, (unsigned)nr,
        cur ? (unsigned)cur->pid : 0u, cur ? cur->comm : "(null)");
    for (;;) __asm__ volatile("cli\n hlt");
  }

  /* syscall_dispatch stores result in frame[0] on ARM/m68k.
   * On i16 we return it to trap.S which puts it in saved AX. */
  return (long)(int16_t)frame[0];
}
