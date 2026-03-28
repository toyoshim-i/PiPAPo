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

.macro CORE_TARGET_STUB name
  .global \name\()_entry
\name\()_entry:
  /* Stack: [args] [far_IP 2B] [far_CS 2B] ← SP */
  pop  %ax                      /* far_IP */
  pop  %bx                      /* far_CS */
  mov  %ax, saved_ip
  mov  %bx, saved_cs
  /* Stack: [args] ← SP — exactly what the function expects */
  call \name
  /* Function returned. Push far frame and lret. */
  push saved_cs
  push saved_ip
  lret
.endm

  CORE_TARGET_STUB klog
  CORE_TARGET_STUB klogf
  CORE_TARGET_STUB kmem_pool_init
  CORE_TARGET_STUB kmem_alloc
  CORE_TARGET_STUB kmem_free
  CORE_TARGET_STUB kmem_free_count

#endif /* __ia16__ */
