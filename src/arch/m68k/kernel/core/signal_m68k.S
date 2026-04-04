/* signal_m68k.S — Assembly thunk for m68k signal handler invocation
 *
 * m68k_call_signal_handler(handler, sig, got_base)
 *
 * Calls a user-space PIC signal handler with the correct a5 (GOT base).
 * Saves/restores the kernel's a5 around the call.  The handler receives
 * the signal number as its first (stack-passed) argument per m68k ABI.
 */

    .text
    .even

    .globl  m68k_call_signal_handler
    .type   m68k_call_signal_handler, @function
m68k_call_signal_handler:
    /* Stack on entry:
     *   4(%sp) = handler  (function pointer)
     *   8(%sp) = sig      (signal number)
     *  12(%sp) = got_base (a5 value for PIC)
     */
    move.l  %a5,%sp@-          /* save kernel a5              */

    /* Load got_base into a5 (offset +4 from the push above) */
    move.l  16(%sp),%a5        /* a5 = got_base               */

    /* Push sig as the handler's argument (m68k ABI: args on stack) */
    move.l  12(%sp),%sp@-      /* push sig (offset +4 for push) */

    /* Load handler address and call it */
    move.l  12(%sp),%a0        /* a0 = handler (8+4=12 for 2 pushes) */
    jsr     (%a0)

    /* Clean up handler argument */
    addq.l  #4,%sp

    /* Restore kernel a5 */
    move.l  %sp@+,%a5

    rts
    .size   m68k_call_signal_handler, . - m68k_call_signal_handler
