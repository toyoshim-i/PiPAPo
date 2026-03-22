# =============================================================================
# trap.S — Xtensa exception handler (stub for CC-1)
# =============================================================================
#
# Minimal exception vector for the initial CardComputer port.
# All exceptions halt in a loop — no syscall dispatch, no timer ISR yet.
# These will be implemented in CC-2 (timer) and CC-3 (context switch/syscall).
#
# ESP-IDF installs its own exception vectors by default.  For PPAP to take
# over, we will need to replace them via VECBASE in CC-2.  For CC-1, this
# file exists only for completeness (not yet wired).
# =============================================================================

    .text
    .align 4
    .global _xtensa_exception_stub
    .type   _xtensa_exception_stub, @function
_xtensa_exception_stub:
    # Save minimal context for post-mortem debugging
    # (a0 = return addr, a1 = sp are already in registers)
    waiti   15          # halt with interrupts disabled
    j       _xtensa_exception_stub
    .size   _xtensa_exception_stub, . - _xtensa_exception_stub
