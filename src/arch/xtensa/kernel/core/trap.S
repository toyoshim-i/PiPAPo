# =============================================================================
# trap.S — Xtensa exception handler (stub)
# =============================================================================
#
# Minimal exception vector for the CardComputer port.  All exceptions halt
# in a loop — no syscall dispatch, no timer ISR yet.
#
# ESP-IDF installs its own exception vectors by default.  For PPAP to take
# over we will need to replace them via VECBASE.  This file exists for
# completeness (not yet wired).
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
