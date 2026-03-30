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
 * Linked into ppap_ibmpc (core binary).
 */

#ifdef __ia16__

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
 */
.macro CORE_TARGET_STUB name
  .global \name\()_entry
\name\()_entry:
  cli
  pop  %ax
  pop  %bx
  mov  %ax, %ss:saved_ip
  mov  %bx, %ss:saved_cs
  sti
  /* Restore DS to core's data segment (SS=DS=0 for shared data).
   * VFS code uses DS as a scratch register, so DS may be garbage
   * when entering core via far call. */
  xor  %ax, %ax
  mov  %ax, %ds
  call \name
  cli
  /* Restore DS=0 before returning — the callee may have clobbered DS.
   * Use BX to avoid clobbering AX (return value). */
  xor  %bx, %bx
  mov  %bx, %ds
  push %ss:saved_cs
  push %ss:saved_ip
  sti
  lret
.endm

/* blkdev wrappers in mod_core.c are named core_blkdev_read/write.
 * The .inc entry name is blkdev_read/write (matching the struct field).
 * Alias so the target stub can call the right function. */
  .global blkdev_read
  .set blkdev_read, core_blkdev_read
  .global blkdev_write
  .set blkdev_write, core_blkdev_write

#define MOD_CORE_ENTRY(name, idx)  CORE_TARGET_STUB name
#include "common/mod/mod_core.inc"
#undef MOD_CORE_ENTRY

#endif /* __ia16__ */
