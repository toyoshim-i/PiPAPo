/*
 * core_entries.S — Core-side target entry stubs for core functions
 *
 * These stubs live in the core module's segment. They are the targets
 * of far calls from VFS (or other modules calling core).
 *
 * The problem: when VFS does `call *struct_field` → stub does `lcall`
 * → entry does `call real_fn`, there are 3 return addresses on the
 * stack (8 bytes) between the args and SP. The real function would
 * read wrong argument offsets.
 *
 * Solution: the entry stub uses `jmp` (not `call`) to the real
 * function. The function's `ret` pops the 2-byte IP that the
 * caller-side stub's `lcall` pushed... wait, lcall pushes CS:IP
 * (4 bytes), and `ret` only pops 2. So we need the function to
 * do `lret` — but we can't change the C function.
 *
 * Actual solution: the entry stub swaps the far return address
 * with a near trampoline address, then `jmp` to the real function.
 * The function's `ret` returns to the trampoline, which does `lret`.
 *
 * Stack on entry (after lcall):
 *   [args]       ← what the function expects
 *   [near IP]    ← from caller's `call *struct_field` (2B)
 *   [far CS:IP]  ← from stub's `lcall` (4B)  ← SP
 *
 * We need the function to see:
 *   [args]
 *   [return IP]  ← SP (points to trampoline)
 *
 * So: pop far return (save it), pop near return (save it),
 * push trampoline address, jmp to real function.
 * When function does `ret`, it returns to trampoline.
 * Trampoline pushes saved far return, does `lret`.
 * Then caller stub's `ret` uses the saved near return.
 *
 * This is complex. Simpler approach: the CALLER-side stub
 * already handles this. Instead of `lcall; ret`, the caller
 * stub saves args, does the far call, and cleans up.
 *
 * Simplest correct approach for now: the entry stub pops the
 * far return address, saves it, and pushes a near return to
 * a local trampoline. The function returns to trampoline via
 * normal `ret`, trampoline does `lret` with saved CS:IP.
 */

#ifdef __ia16__

  .code16
  .section .text

/*
 * Entry stub pattern:
 *   1. Pop far return CS:IP into BX:AX (saved in scratch vars)
 *   2. Push near trampoline address
 *   3. jmp to real function (it sees args at correct SP offset)
 *   4. Function does `ret` → returns to trampoline
 *   5. Trampoline pushes saved CS:IP and does `lret`
 *
 * Clobbers: uses .bss scratch for saved CS:IP.
 * NOT reentrant (single scratch area). OK for single-threaded boot.
 */

  .section .bss
saved_cs: .word 0
saved_ip: .word 0

  .section .text

.macro CORE_TARGET_STUB name
  .global \name\()_entry
\name\()_entry:
  /* Pop far return address (pushed by lcall) */
  pop  %ax              /* IP */
  pop  %bx              /* CS */
  mov  %ax, saved_ip
  mov  %bx, saved_cs
  /* Now stack has: [args] [near_return_IP] ← what function expects */
  /* jmp to real function */
  jmp  \name

  /* Trampoline — function's `ret` pops near IP and ends up here
   * ... actually no. After jmp, the function does `ret` which pops
   * the CALLER's near return IP. That returns to the caller stub's
   * `ret`, which returns to vfs_init. But the saved far return
   * (CS:IP for lret) was never used!
   *
   * This approach is wrong. Let me use a different method. */
.endm

/*
 * Actually, the correct approach: the caller-side stub must adjust
 * the calling convention. Instead of:
 *   call *struct → lcall → call fn → ret → lret → ret
 * We need:
 *   call *struct → push args → lcall → fn reads args → lret → cleanup → ret
 *
 * But we can't change how the compiler calls struct fields.
 *
 * The REAL fix: the entry stub must be a trampoline that adjusts SP.
 * On entry after lcall, stack is:
 *   [args...] [near_ret_IP 2B] [far_CS 2B] [far_IP 2B] ← SP
 *
 * The function expects:
 *   [args...] [ret_IP 2B] ← SP
 *
 * Difference: 4 extra bytes (far CS:IP) between near_ret and SP.
 * Solution: copy the near_ret_IP down past the far return:
 */

  .section .text

  /* Re-define the macro with the correct approach */
  .purgem CORE_TARGET_STUB

.macro CORE_TARGET_STUB name
  .global \name\()_entry
\name\()_entry:
  /* Stack: [args] [near_IP 2B] [far_IP 2B] [far_CS 2B] ← SP
   * Move near_IP down to just above SP, overwriting far return.
   * Save far return first. */
  pop  %ax              /* far_IP */
  pop  %bx              /* far_CS */
  mov  %ax, saved_ip
  mov  %bx, saved_cs
  /* Stack now: [args] [near_IP 2B] ← SP
   * This is what the function expects! */
  call \name
  /* Function returned. Restore far return and lret. */
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
