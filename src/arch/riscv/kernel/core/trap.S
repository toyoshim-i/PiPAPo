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
# Trap frame layout on stack (36 words = 144 bytes, low → high):
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
#   [SP+128]  user_sp           ← saved user stack pointer (for mscratch split)
#   [SP+132]  (padding)         ← 16-byte alignment for C ABI
#   [SP+136]  (padding)
#   [SP+140]  (padding)
#
# x0 (zero) is not saved (always 0).
# x2 (sp) is saved explicitly in user_sp slot (for mscratch stack split).
# =============================================================================

.option norelax

# Trap frame size: 36 words = 144 bytes (16-byte aligned)
.equ TRAP_FRAME_SIZE, 144

# Offsets for special fields in the trap frame
.equ TF_MEPC,    120
.equ TF_MSTATUS, 124
.equ TF_USER_SP, 128

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

    # ── mscratch stack swap ──────────────────────────────────────────────
    # mscratch = kernel stack top.  Swap sp <-> mscratch.
    csrrw   sp, mscratch, sp

    # Check MPP to detect nesting (U-mode=0 → from user, M-mode=3 → nested).
    # Save t0 temporarily on the (possibly wrong) stack.
    addi    sp, sp, -4
    sw      t0, 0(sp)
    csrr    t0, mstatus
    srli    t0, t0, 11
    andi    t0, t0, 3               # MPP field
    bnez    t0, .Lfrom_kernel

    # ── From U-mode: swap was correct ────────────────────────────────────
    lw      t0, 0(sp)
    addi    sp, sp, 4
    addi    sp, sp, -TRAP_FRAME_SIZE
    csrr    t0, mscratch
    sw      t0, TF_USER_SP(sp)     # save user sp
    j       .Lsave_regs

.Lfrom_kernel:
    # ── From M-mode (nested): undo swap ──────────────────────────────────
    lw      t0, 0(sp)
    addi    sp, sp, 4
    csrrw   sp, mscratch, sp       # undo: sp = kernel sp, mscratch = ksp_top
    addi    sp, sp, -TRAP_FRAME_SIZE
    sw      zero, TF_USER_SP(sp)   # mark as kernel frame

.Lsave_regs:
    # ── Save full register set ───────────────────────────────────────────
    sw      x1,   0(sp)        # ra
    sw      x3,   4(sp)        # gp (user)
    # Restore kernel GP so GP-relative access in kernel code works.
    # Without this, the kernel runs with the user's GP and reads/writes
    # kernel globals at wrong addresses.
.option push
.option norelax
    la      gp, __global_pointer$
.option pop
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
    # Check for ecall (syscall): mcause=8 (U-mode) or mcause=11 (M-mode)
    li      t1, 8                   # EXC_ECALL_U
    beq     t0, t1, .Lecall
    li      t1, 11                  # EXC_ECALL_M
    beq     t0, t1, .Lecall

    # Not an ecall — fatal exception.
    csrc    mstatus, 0x8            # clear MIE
    mv      a0, t0                  # mcause
    csrr    a1, mepc                # mepc
    csrr    a2, mtval               # mtval
    lw      a3, 24(sp)              # saved s0/fp from trap frame
    lw      a4, TF_USER_SP(sp)     # saved user sp (0 if from kernel)
    call    riscv_exception_handler
    j       Default_Handler

.Lecall:
    # ── Syscall via ecall ───────────────────────────────────────────────
    # RISC-V syscall convention: a7 = syscall number, a0-a5 = arguments.
    # mepc points to the ecall instruction; advance it by 4 so mret
    # returns to the instruction AFTER ecall.
    lw      t0, TF_MEPC(sp)
    addi    t0, t0, 4
    sw      t0, TF_MEPC(sp)

    # Publish the trap-frame base for sys_rt_sigreturn to locate the
    # mepc/mstatus/user_sp slots it needs to rewrite when unwinding a
    # signal delivery.  Only the ecall path needs this — timer IRQs
    # don't dispatch syscalls.
    la      t0, rv32_trap_frame_sp
    sw      sp, 0(t0)

    # Re-enable interrupts during syscall processing (same as ARM SVC)
    csrs    mstatus, 0x8            # set MIE

    # Call syscall_dispatch(frame, nr, a4, a5)
    #   frame = &trap_frame[8] = sp+32 (points to saved a0 in trap frame)
    #   nr    = saved a7 (trap frame offset 60 = x17)
    #   a4    = saved a4 (trap frame offset 48 = x14)
    #   a5    = saved a5 (trap frame offset 52 = x15)
    addi    a0, sp, 32              # frame → saved a0..a3
    lw      a1, 60(sp)              # nr = saved a7 (x17)
    lw      a2, 48(sp)              # a4 = saved x14 (a4)
    lw      a3, 52(sp)              # a5 = saved x15 (a5)
    call    syscall_dispatch

    # syscall_dispatch writes return value into frame[0] = saved a0.

    # Deliver any pending user-handler signal before returning to user.
    # signal_check rewrites this trap frame's (mepc, sp, a0, ra) so the
    # trap-exit mret lands in the handler in user mode.
    mv      a0, sp                  # regs = &trap_frame
    call    signal_check

    # ── Check exec_pending (execve replaces the process image) ──────
    # After execve, the trap frame on the stack is stale — proc_setup_stack
    # already built a fresh frame at p->sp.  Reload SP from current->sp.
    la      t0, exec_pending
    lw      t1, 0(t0)
    beqz    t1, .Lno_exec
    sw      zero, 0(t0)
    la      t0, current_core
    lw      t0, 0(t0)              # t0 = current (pcb_t *)
    lw      sp, 48(t0)             # sp = current->sp (PCB_SP_OFFSET=48)
    lw      t1, 52(t0)             # t1 = current->kernel_sp
    csrw    mscratch, t1           # update mscratch for new kernel stack
    # Now on the NEW stack — safe to free the old stack page.
    call    exec_free_old_stack
    j       .Ltrap_return
.Lno_exec:

    # ── Check svc_restart (blocking syscall needs re-execution) ─────
    # If svc_restart is set, rewind mepc by 4 (back to ecall) and
    # restore the original a0 from svc_saved_a0, so the syscall
    # re-executes with original arguments when the process resumes.
    la      t0, svc_restart
    lw      t1, 0(t0)
    beqz    t1, .Lno_restart
    sw      zero, 0(t0)
    # Rewind mepc by 4 (ecall is 4 bytes)
    lw      t1, TF_MEPC(sp)
    addi    t1, t1, -4
    sw      t1, TF_MEPC(sp)
    # Restore original a0
    la      t0, svc_saved_a0
    lw      t1, 0(t0)
    sw      t1, 32(sp)             # frame[0] = saved a0 at sp+32
.Lno_restart:

    # ── Check switch_pending ──────────────────────────────────
    la      t0, switch_pending
    lw      t1, 0(t0)
    beqz    t1, .Ltrap_return
    sw      zero, 0(t0)
    mv      a0, sp
    call    riscv_do_switch
    mv      sp, a0
    # Update mscratch for the new process
    la      t0, current_core
    lw      t0, 0(t0)
    lw      t1, 52(t0)             # current->kernel_sp
    csrw    mscratch, t1
    j       .Ltrap_return

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
    # Call C handler: riscv_timer_handler(from_user)
    # a0 = from_user: 1 if interrupted code was U-mode, 0 if M-mode.
    # Check saved mstatus.MPP (bits 12:11): 0b00 = U-mode, 0b11 = M-mode.
    lw      a0, TF_MSTATUS(sp)
    srli    a0, a0, 11
    andi    a0, a0, 3           # a0 = MPP field
    seqz    a0, a0              # a0 = 1 if MPP==0 (U-mode), 0 otherwise
    call    riscv_timer_handler

    # ── Check if a context switch was requested ─────────────────────────
    # sched_timer_tick → sched_tick → arch_yield sets switch_pending.
    # If set, perform the switch now (same pattern as m68k port).
    la      t0, switch_pending
    lw      t1, 0(t0)
    beqz    t1, .Ltrap_return   # no switch needed
    sw      zero, 0(t0)         # clear the flag

    # Call the context switch: riscv_context_switch(old_sp_ptr, new_sp)
    # sched_next() returns the next pcb_t to run; we save current SP into
    # the current pcb and load the new SP.
    # For now, the full trap frame is already on the stack; just swap SP
    # through the pcb_t.sp field via a C helper.
    mv      a0, sp              # pass current SP (top of trap frame)
    call    riscv_do_switch     # returns new SP in a0
    mv      sp, a0              # switch to new process's kernel stack
    # Update mscratch for the new process
    la      t0, current_core
    lw      t0, 0(t0)
    lw      t1, 52(t0)             # current->kernel_sp
    csrw    mscratch, t1

.Ltrap_return:
    # ── Restore full register set ────────────────────────────────────────
    lw      t0, TF_MEPC(sp)
    lw      t1, TF_MSTATUS(sp)
    csrw    mepc, t0
    csrw    mstatus, t1

    # Check TF_USER_SP: non-zero → returning to U-mode (do swap)
    #                    zero    → returning to M-mode (no swap)
    lw      t0, TF_USER_SP(sp)
    beqz    t0, .Lret_kernel

    # ── Return to U-mode ─────────────────────────────────────────────────
    # Put user sp in mscratch, restore all regs, swap sp↔mscratch on exit
    csrw    mscratch, t0           # mscratch = user sp (temporarily)

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

    addi    sp, sp, TRAP_FRAME_SIZE    # sp = kernel stack top
    csrrw   sp, mscratch, sp           # sp = user sp, mscratch = ksp top
    mret

.Lret_kernel:
    # ── Return to M-mode (nested or kernel process) ──────────────────────
    lw      x1,   0(sp)
    lw      x3,   4(sp)
    lw      x4,   8(sp)
    lw      x5,  12(sp)
    lw      x6,  16(sp)
    lw      x7,  20(sp)
    lw      x8,  24(sp)
    lw      x9,  28(sp)
    lw      x10, 32(sp)
    lw      x11, 36(sp)
    lw      x12, 40(sp)
    lw      x13, 44(sp)
    lw      x14, 48(sp)
    lw      x15, 52(sp)
    lw      x16, 56(sp)
    lw      x17, 60(sp)
    lw      x18, 64(sp)
    lw      x19, 68(sp)
    lw      x20, 72(sp)
    lw      x21, 76(sp)
    lw      x22, 80(sp)
    lw      x23, 84(sp)
    lw      x24, 88(sp)
    lw      x25, 92(sp)
    lw      x26, 96(sp)
    lw      x27,100(sp)
    lw      x28,104(sp)
    lw      x29,108(sp)
    lw      x30,112(sp)
    lw      x31,116(sp)

    addi    sp, sp, TRAP_FRAME_SIZE
    mret
