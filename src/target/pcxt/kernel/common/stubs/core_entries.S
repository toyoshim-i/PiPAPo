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
saved_cs: .word 0
saved_ip: .word 0

  .section .text

/*
 * Entry stub: pop far return, call function, lret.
 *
 * Uses static saved_cs/saved_ip — NOT reentrant.  Interrupts are
 * disabled around the save/restore to prevent timer ISR from
 * making a nested far call that overwrites the saved values.
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
  cli
  pop  %ax
  pop  %dx
  mov  %ax, %ss:saved_ip
  mov  %dx, %ss:saved_cs
  sti
  /* Restore DS to core's data segment (SS=DS=0 for shared data).
   * VFS code uses DS as a scratch register, so DS may be garbage
   * when entering core via far call. */
  xor  %dx, %dx
  mov  %dx, %ds
  call \name
  cli
  /* Restore DS=0 before returning — the callee may have clobbered DS.
   * Save DX first: 32-bit return values travel in DX:AX.
   * Use CX (also caller-saved) as scratch for the DS restore. */
  push %dx
  xor  %cx, %cx
  mov  %cx, %ds
  pop  %dx
  push %ss:saved_cs
  push %ss:saved_ip
  sti
  lret
.endm

#define MOD_CORE_ENTRY(name, idx)  CORE_TARGET_STUB name
#include "kernel/common/mod/mod_core.inc"
#undef MOD_CORE_ENTRY
