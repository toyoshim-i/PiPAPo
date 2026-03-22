# =============================================================================
# switch.S — Xtensa Cooperative Context Switch (Windowed ABI)
# =============================================================================
#
# Follows the FreeRTOS "solicited frame" pattern for Xtensa windowed ABI:
#   1. ENTRY — rotates register window, allocates frame
#   2. Save return PC (a0) and PS to the solicited frame on the stack
#   3. Spill all other register windows to their respective stacks
#   4. Save SP to current PCB, call sched_next(), load new SP
#   5. Restore PS and return PC from the new process's solicited frame
#   6. RETW — rotates window back, underflow reloads from new stack
#
# Solicited frame layout (32 bytes on stack, 16-byte aligned):
#
#   [SP+0 ]  exit marker  (0 = solicited frame)
#   [SP+4 ]  return PC    (a0, encoded with window rotation info)
#   [SP+8 ]  saved PS
#   [SP+12]  (padding)
#
# The ENTRY instruction also writes the base save area (caller's a0-a3)
# at [SP - 16..SP - 4], which RETW's underflow handler reads to restore
# the caller's window.
#
# PCB layout: sp is at offset 0 in pcb_t (PCB_SP_OFFSET = 0).
# =============================================================================

    .equ SOL_EXIT,   0
    .equ SOL_PC,     4
    .equ SOL_PS,     8
    .equ SOL_FRMSZ,  32    # must be multiple of 16 for alignment

    .text
    .align 4
    .global xtensa_do_yield
    .type   xtensa_do_yield, @function

# -----------------------------------------------------------------------------
# xtensa_do_yield — cooperative context switch
#
# Called from sched_yield() via normal windowed call (CALL4).
# Saves current context, picks next process, restores its context.
# Returns to the new (or same) process's caller.
# -----------------------------------------------------------------------------
xtensa_do_yield:
    entry   a1, SOL_FRMSZ

    # Save return address and PS to solicited frame
    s32i    a0, a1, SOL_PC
    rsr     a2, ps
    s32i    a2, a1, SOL_PS
    movi    a3, 0
    s32i    a3, a1, SOL_EXIT       # mark as solicited frame

    # ── Spill all register windows ──────────────────────────────────────
    # Must clear PS.WOE and raise INTLEVEL to EXCM_LEVEL before call0
    # to xthal_window_spill_nw (a Xtensa HAL function using call0 ABI).
    #
    # PS.WOE must be cleared so window overflow exceptions don't fire
    # during the spill sequence.  INTLEVEL >= EXCM_LEVEL prevents
    # level-1 interrupts from arriving mid-spill.

    movi    a3, ~(0x00040000 | 0x0F)  # ~(PS_WOE | PS_INTLEVEL_MASK)
    and     a2, a2, a3
    movi    a3, 3                     # XCHAL_EXCM_LEVEL = 3
    or      a2, a2, a3
    wsr     a2, ps
    rsync
    call0   xthal_window_spill_nw     # spill all windows (call0 ABI)

    # Restore PS (still with WOE set, original INTLEVEL)
    l32i    a2, a1, SOL_PS
    wsr     a2, ps

    # ── Disable interrupts for the switch ───────────────────────────────
    rsil    a2, 15                    # save old PS, set INTLEVEL=15

    # ── Call C helper: xtensa_do_switch(current_sp) → new_sp ───────────
    # Windowed call (call4): our a6 → callee's a2 (arg), callee's a2
    # (return) → our a6.
    mov     a6, a1                    # pass current SP as argument
    call4   xtensa_do_switch
    mov     a1, a6                    # a1 = new process's saved SP

    # ── Restore from new process's solicited frame ──────────────────────
    l32i    a3, a1, SOL_PS
    wsr     a3, ps                    # restore PS (re-enables interrupts)
    rsync
    l32i    a0, a1, SOL_PC            # restore return PC

    retw                              # return to new process's caller
    .size   xtensa_do_yield, . - xtensa_do_yield
