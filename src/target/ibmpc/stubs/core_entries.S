/*
 * core_entries.S — Core-side target entry stubs for core functions
 *
 * These stubs live in the core module's segment. They are the targets
 * of ljmp from VFS caller stubs.
 *
 * The caller stub (core_stubs.S) converts the compiler's near call
 * into a far frame: [args] [caller_CS] [near_ret_IP] ← SP.
 * It then does `ljmp` (no push) to the entry point below.
 *
 * The entry stub simply calls the real C function (near call within
 * core's segment) and does `lret` to return via the far frame.
 *
 * Stack on entry (after ljmp):
 *   [args]  [caller_CS 2B]  [near_ret_IP 2B] ← SP
 *
 * This is a valid far return frame. The function call adds one
 * more near IP, so the function sees [args] at the correct offset
 * (BP+4 for first arg after push BP; mov SP,BP).
 *
 * Wait — the function expects [args] [ret_IP] ← SP (2-byte ret),
 * but we have [args] [CS 2B] [IP 2B] ← SP (4-byte far frame).
 * The `call` below adds another 2B. Function sees args at BP+8
 * instead of BP+4. That's wrong.
 *
 * Fix: the entry stub must remove the far frame, call the function
 * (so it sees [args] [ret_IP]), then restore the far frame and lret.
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
  call \name
  cli
  push %ss:saved_cs
  push %ss:saved_ip
  sti
  lret
.endm

  CORE_TARGET_STUB klog
  CORE_TARGET_STUB klogf
  CORE_TARGET_STUB kmem_pool_init
  CORE_TARGET_STUB kmem_alloc
  CORE_TARGET_STUB kmem_free
  CORE_TARGET_STUB kmem_free_count
  CORE_TARGET_STUB mem_region_alloc
  CORE_TARGET_STUB mem_region_free
  CORE_TARGET_STUB mem_region_total_bytes
  CORE_TARGET_STUB mem_region_free_bytes
  CORE_TARGET_STUB core_blkdev_read
  CORE_TARGET_STUB core_blkdev_write

#endif /* __ia16__ */
