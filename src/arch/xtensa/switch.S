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
# Solicited frame layout (metadata above ABI scratch area):
#
#   [SP+0 ..+15] ABI scratch area (kept unused by PPAP metadata)
#   [SP+16]  exit marker  (0 = solicited frame, 1 = new-process frame)
#   [SP+20]  return PC    (a0, encoded with window rotation info)
#   [SP+24]  saved PS
#   [SP+28]  user SP      (new-process frame only)
#   [SP+32]  saved a0     (new-process frames only; vfork return addr)
#
# New-process frame (exit marker = 1):
#   Built by arch_build_initial_frame() for exec'd processes.
#   [SP+20] = entry address (NOT encoded), [SP+28] = user stack pointer,
#   [SP+32] = a0 (return address — needed by vfork child which starts
#   at the `ret` after SYSCALL and needs a0 to jump back to caller).
#   Restored via direct jump (jx) instead of retw, avoiding the
#   base-save-area overlap that would corrupt the user SP.
#
# PCB layout: sp is at offset 0 in pcb_t (PCB_SP_OFFSET = 0).
# =============================================================================

    .equ SOL_EXIT,   16
    .equ SOL_PC,     20
    .equ SOL_PS,     24
    .equ SOL_SP,     28
    .equ SOL_A0,     32    # saved a0 (new-process frames only; vfork return addr)
    .equ SOL_A3,     36    # saved a3 (vfork child needs entry ptr preserved)
    .equ SOL_FRMSZ,  64    # keep low 16 bytes free for ABI scratch

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

    # ── Check exit marker: solicited (0) vs new-process (1) ───────────
    l32i    a3, a1, SOL_EXIT
    bnez    a3, .Lnew_process

    # ── Solicited restore: normal windowed return ─────────────────────
    l32i    a3, a1, SOL_PS
    wsr     a3, ps                    # restore PS (re-enables interrupts)
    rsync
    l32i    a0, a1, SOL_PC            # restore return PC

    retw                              # return to new process's caller

.Lnew_process:
    # ── New-process restore: direct jump to entry point ───────────────
    # This path is used for freshly exec'd processes whose initial frame
    # was built by arch_build_initial_frame().  Also used by vfork child
    # (entry = instruction after syscall, a2 = 0 = vfork return value).
    # We set all registers directly and jump, avoiding retw (which would
    # read the base save area and corrupt a1 with the entry address).
    #
    # a8 is used as the jump target so a3 can be restored from the frame
    # (vfork child needs a3 preserved — the compiler does not clobber it
    # across the inline syscall wrapper).
    l32i    a8, a1, SOL_PC            # a8 = entry address (temp for jx)
    l32i    a4, a1, SOL_PS            # a4 = PS
    l32i    a0, a1, SOL_A0            # a0 = return addr (vfork needs this)
    l32i    a3, a1, SOL_A3            # a3 = saved a3 (vfork entry ptr)
    l32i    a1, a1, SOL_SP            # a1 = user SP (load LAST — clobbers sp)
    wsr     a4, ps
    rsync
    movi    a2, 0                     # a2 = 0 (vfork return; harmless for exec)
    jx      a8                        # jump to entry

    .size   xtensa_do_yield, . - xtensa_do_yield
