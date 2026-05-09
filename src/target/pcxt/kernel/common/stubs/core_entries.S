/*
 * core_entries.S — Core-side target entry stubs for core functions
 *
 * These stubs live in the core module's segment. They are the targets
 * of ljmp from VFS caller stubs.
 *
 * The caller stub (core_stubs.S) converts the compiler's near call
 * into a far frame: [args] [caller_CS] [near_ret_IP] <- SP.
 * It then does `ljmp` (no push) to the entry point below.
 *
 * The entry stub simply calls the real C function (near call within
 * core's segment) and does `lret` to return via the far frame.
 *
 * Function list auto-generated from mod_core.inc.
 *
 * Linked into ppap_pcxt (core binary).
 */

  .code16
  .section .text

  .section .bss
  /* Static far-return save slots used by BOTH the core entry stubs
   * (in this file) and the VFS entry stubs (in vfs_entries.S).  They
   * live in core's .bss so core's switch.S can reach them via ordinary
   * symbol addressing when it needs to swap them against per-PCB save
   * fields on a cooperative yield or timer preemption (see
   * i16_ctx_switch and i16_timer_isr).  VFS's link pulls the addresses
   * out of core_exports.ld, so `%ss:vfs_saved_cs` in VFS asm resolves
   * to this same memory. */
  .globl saved_cs
  .globl saved_ip
  .globl vfs_saved_cs
  .globl vfs_saved_ip
saved_cs:     .word 0
saved_ip:     .word 0
vfs_saved_cs: .word 0
vfs_saved_ip: .word 0

  .section .text

/*
 * Entry stub: pop far return, call function, lret.
 *
 * Uses static saved_cs/saved_ip — NOT reentrant.  Interrupts are
 * disabled around the save/restore to prevent timer ISR from
 * making a nested far call that overwrites the saved values.
 *
 * The caller's IF state is saved (pushf/popf via CX) and restored
 * after each critical section, so syscall callers (IF=0) stay with
 * interrupts disabled, while boot/normal callers (IF=1) get them
 * re-enabled.
 *
 * ia16 C ABI note:
 *   - AX carries the return value, so the stub must not clobber AX
 *     after the real C function returns.
 *   - BX, SI, DI, and BP are callee-saved, so the stub must not use
 *     them as scratch across the call boundary.
 *   - CX and DX are caller-saved, so DX is the safe scratch register
 *     for shuffling far-return state and restoring DS.
 *   - DS is not a stable data segment in ia16 small model; the
 *     compiler uses it as scratch while actual kernel data accesses
 *     are SS-relative.
 */
.macro CORE_TARGET_STUB name
  .global \name\()_entry
\name\()_entry:
  pushf
  pop  %cx                  /* CX = caller's flags (including IF) */
  cli
  pop  %ax
  pop  %dx
  mov  %ax, %ss:saved_ip
  mov  %dx, %ss:saved_cs
  push %cx
  popf                      /* restore caller's IF state */
  /* Restore DS to core's data segment (SS=DS=0 for shared data).
   * VFS code uses DS as a scratch register, so DS may be garbage
   * when entering core via far call. */
  xor  %dx, %dx
  mov  %dx, %ds
  call \name
  pushf
  pop  %cx                  /* CX = current flags */
  cli
  /* Restore DS=0 before returning — the callee may have clobbered DS.
   * Save DX first: 32-bit return values travel in DX:AX. */
  push %dx
  xor  %dx, %dx
  mov  %dx, %ds
  pop  %dx
  push %ss:saved_cs
  push %ss:saved_ip
  push %cx
  popf                      /* restore IF state */
  lret
.endm

#define MOD_CORE_ENTRY(name, idx)  CORE_TARGET_STUB name
#include "kernel/common/mod/mod_core.inc"
#undef MOD_CORE_ENTRY
