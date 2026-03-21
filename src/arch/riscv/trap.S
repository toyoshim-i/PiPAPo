# =============================================================================
# trap.S — RISC-V Trap Entry and Dispatch
# =============================================================================
#
# Single entry point for ALL traps (interrupts + exceptions).  Installed
# in mtvec (direct mode).  Saves the full register set, reads mcause to
# determine the trap type, and dispatches to the appropriate C handler.
#
# Unlike ARM Cortex-M, RISC-V does NOT auto-push any registers on trap
# entry.  We must save everything in software — same pattern as the m68k
# port (movem.l d0-d7/a0-a6 save/restore).
#
# Trap frame layout on stack (32 words = 128 bytes, low → high):
#
#   [SP+0  ]  x1  (ra)         ← pcb_t.sp will point here (after switch)
#   [SP+4  ]  x3  (gp)
#   [SP+8  ]  x4  (tp)
#   [SP+12 ]  x5  (t0)
#   [SP+16 ]  x6  (t1)
#   [SP+20 ]  x7  (t2)
#   [SP+24 ]  x8  (s0/fp)
#   [SP+28 ]  x9  (s1)
#   [SP+32 ]  x10 (a0)
#   [SP+36 ]  x11 (a1)
#   [SP+40 ]  x12 (a2)
#   [SP+44 ]  x13 (a3)
#   [SP+48 ]  x14 (a4)
#   [SP+52 ]  x15 (a5)
#   [SP+56 ]  x16 (a6)
#   [SP+60 ]  x17 (a7)
#   [SP+64 ]  x18 (s2)
#   [SP+68 ]  x19 (s3)
#   [SP+72 ]  x20 (s4)
#   [SP+76 ]  x21 (s5)
#   [SP+80 ]  x22 (s6)
#   [SP+84 ]  x23 (s7)
#   [SP+88 ]  x24 (s8)
#   [SP+92 ]  x25 (s9)
#   [SP+96 ]  x26 (s10)
#   [SP+100]  x27 (s11)
#   [SP+104]  x28 (t3)
#   [SP+108]  x29 (t4)
#   [SP+112]  x30 (t5)
#   [SP+116]  x31 (t6)
#   [SP+120]  mepc
#   [SP+124]  mstatus
#
# x0 (zero) is not saved (always 0).
# x2 (sp) is implicit — it's the stack pointer itself.
# =============================================================================

.option norelax

# Trap frame size: 32 words = 128 bytes
.equ TRAP_FRAME_SIZE, 128

# Offsets for mepc and mstatus in the trap frame
.equ TF_MEPC,    120
.equ TF_MSTATUS, 124

# mcause bit 31: interrupt flag
.equ MCAUSE_INT_BIT, 31

# Interrupt cause codes (mcause[30:0] when bit 31 = 1)
.equ IRQ_TIMER_CAUSE, 7

# =============================================================================
# _trap_vector — entry point for all traps (installed in mtvec)
# =============================================================================

.section .vectors, "ax"
.align 2
.global _trap_vector
_trap_vector:
    j       _trap_entry


# =============================================================================
# _trap_entry — save context, dispatch, restore context
# =============================================================================

.section .text.trap_entry, "ax"
.align 2
.global _trap_entry
_trap_entry:

    # ── Save full register set ───────────────────────────────────────────
    # Allocate trap frame on stack
    addi    sp, sp, -TRAP_FRAME_SIZE

    # Save x1 (ra), x3-x31
    sw      x1,   0(sp)        # ra
    sw      x3,   4(sp)        # gp
    sw      x4,   8(sp)        # tp
    sw      x5,  12(sp)        # t0
    sw      x6,  16(sp)        # t1
    sw      x7,  20(sp)        # t2
    sw      x8,  24(sp)        # s0/fp
    sw      x9,  28(sp)        # s1
    sw      x10, 32(sp)        # a0
    sw      x11, 36(sp)        # a1
    sw      x12, 40(sp)        # a2
    sw      x13, 44(sp)        # a3
    sw      x14, 48(sp)        # a4
    sw      x15, 52(sp)        # a5
    sw      x16, 56(sp)        # a6
    sw      x17, 60(sp)        # a7
    sw      x18, 64(sp)        # s2
    sw      x19, 68(sp)        # s3
    sw      x20, 72(sp)        # s4
    sw      x21, 76(sp)        # s5
    sw      x22, 80(sp)        # s6
    sw      x23, 84(sp)        # s7
    sw      x24, 88(sp)        # s8
    sw      x25, 92(sp)        # s9
    sw      x26, 96(sp)        # s10
    sw      x27,100(sp)        # s11
    sw      x28,104(sp)        # t3
    sw      x29,108(sp)        # t4
    sw      x30,112(sp)        # t5
    sw      x31,116(sp)        # t6

    # Save mepc and mstatus
    csrr    t0, mepc
    csrr    t1, mstatus
    sw      t0, TF_MEPC(sp)
    sw      t1, TF_MSTATUS(sp)

    # ── Dispatch by mcause ───────────────────────────────────────────────
    csrr    t0, mcause

    # Check interrupt bit (bit 31)
    bltz    t0, .Linterrupt     # mcause < 0 means bit 31 set = interrupt

    # ── Exception path ───────────────────────────────────────────────────
    # Disable interrupts before calling handler to avoid nested traps.
    csrc    mstatus, 0x8            # clear MIE
    # Call C handler with (mcause, mepc, mtval) for diagnostics.
    # Phase RV-4 will add ecall dispatch for syscalls.
    mv      a0, t0                  # mcause (already in t0)
    csrr    a1, mepc                # mepc
    csrr    a2, mtval               # mtval (faulting address/instruction)
    call    riscv_exception_handler
    j       Default_Handler         # fallback if handler returns

.Linterrupt:
    # ── Interrupt path ───────────────────────────────────────────────────
    # Mask off the interrupt bit to get the cause code
    andi    t0, t0, 0x7FF       # cause code in bits 10:0

    # Check for timer interrupt (cause = 7)
    li      t1, IRQ_TIMER_CAUSE
    beq     t0, t1, .Ltimer_irq

    # Unknown interrupt — ignore and return
    j       .Ltrap_return

.Ltimer_irq:
    # ── Timer interrupt handler ──────────────────────────────────────────
    # Call C handler: riscv_timer_handler()
    # It resets mtimecmp to clear the interrupt.
    call    riscv_timer_handler
    # Fall through to trap return

.Ltrap_return:
    # ── Restore full register set ────────────────────────────────────────
    # Restore mepc and mstatus
    lw      t0, TF_MEPC(sp)
    lw      t1, TF_MSTATUS(sp)
    csrw    mepc, t0
    csrw    mstatus, t1

    # Restore x1 (ra), x3-x31
    lw      x1,   0(sp)        # ra
    lw      x3,   4(sp)        # gp
    lw      x4,   8(sp)        # tp
    lw      x5,  12(sp)        # t0
    lw      x6,  16(sp)        # t1
    lw      x7,  20(sp)        # t2
    lw      x8,  24(sp)        # s0/fp
    lw      x9,  28(sp)        # s1
    lw      x10, 32(sp)        # a0
    lw      x11, 36(sp)        # a1
    lw      x12, 40(sp)        # a2
    lw      x13, 44(sp)        # a3
    lw      x14, 48(sp)        # a4
    lw      x15, 52(sp)        # a5
    lw      x16, 56(sp)        # a6
    lw      x17, 60(sp)        # a7
    lw      x18, 64(sp)        # s2
    lw      x19, 68(sp)        # s3
    lw      x20, 72(sp)        # s4
    lw      x21, 76(sp)        # s5
    lw      x22, 80(sp)        # s6
    lw      x23, 84(sp)        # s7
    lw      x24, 88(sp)        # s8
    lw      x25, 92(sp)        # s9
    lw      x26, 96(sp)        # s10
    lw      x27,100(sp)        # s11
    lw      x28,104(sp)        # t3
    lw      x29,108(sp)        # t4
    lw      x30,112(sp)        # t5
    lw      x31,116(sp)        # t6

    # Deallocate trap frame
    addi    sp, sp, TRAP_FRAME_SIZE

    # Return from trap
    mret
