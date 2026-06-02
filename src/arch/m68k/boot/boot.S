| =============================================================================
| boot.S — Motorola 68000 Vector Table and Reset Handler
| =============================================================================
|
| Provides:
|   .vectors section — placed at 0x000000 by the linker script
|   Reset_Handler     — copies .data, zeros .bss, calls kmain()
|   Default_Handler   — infinite loop for unhandled exceptions
|
| 68000 vector table layout (256 entries × 4 bytes = 1024 bytes):
|   0:     Initial SSP (Supervisor Stack Pointer)
|   1:     Initial PC (Reset vector → Reset_Handler)
|   2:     Bus Error
|   3:     Address Error
|   4:     Illegal Instruction
|   5:     Zero Divide
|   6:     CHK instruction
|   7:     TRAPV instruction
|   8:     Privilege Violation
|   9:     Trace
|   10:    Line 1010 (A-line) emulator
|   11:    Line 1111 (F-line) emulator
|   12-23: Reserved
|   24:    Spurious Interrupt
|   25-31: Autovector interrupts (Level 1-7)
|   32-47: TRAP #0 - TRAP #15
|   48-63: Reserved
|   64-255: User interrupt vectors
| =============================================================================

    .section .vectors, "a"
    .align 2

    .global _vectors
_vectors:
    | --- Exception vectors 0-1: Reset ---
    .long __stack_top              | 0: Initial SSP
    .long Reset_Handler            | 1: Initial PC (reset entry point)

    | --- Exception vectors 2-7 ---
    .long m68k_bus_handler          | 2:  Bus Error      (group 0, 14-byte frame)
    .long m68k_addr_handler         | 3:  Address Error   (group 0, 14-byte frame)
    .long m68k_illegal_handler     | 4:  Illegal Instruction (group 1, 6-byte frame)
    .long m68k_zerodiv_handler     | 5:  Zero Divide     (group 1, 6-byte frame)
    .long m68k_chk_handler         | 6:  CHK             (group 1, 6-byte frame)
    .long m68k_chk_handler         | 7:  TRAPV           (group 1, 6-byte frame)

    | --- Exception vectors 8-11 ---
    .long m68k_priv_handler        | 8:  Privilege Violation (group 1, 6-byte frame)
    .long m68k_trace_handler       | 9:  Trace
    .long Default_Handler          | 10: A-line emulator
    .long m68k_fline_handler       | 11: F-line emulator (Human68k DOS calls)

    | --- Exception vectors 12-23: Reserved ---
    .rept 12
    .long Default_Handler
    .endr

    | --- Exception vectors 24-31: Interrupts ---
    .long Default_Handler          | 24: Spurious Interrupt
    .long Default_Handler          | 25: Level 1 autovector
    .long Default_Handler          | 26: Level 2 autovector
    .long Default_Handler          | 27: Level 3 autovector
    .long Default_Handler          | 28: Level 4 autovector
    .long Default_Handler          | 29: Level 5 autovector
    .long m68k_timer_isr           | 30: Level 6 autovector (Goldfish RTC)
    .long Default_Handler          | 31: Level 7 autovector (NMI)

    | --- Exception vectors 32-47: TRAP #0-#15 ---
    .long m68k_trap0_handler       | 32: TRAP #0 (PPAP syscall)
    .long m68k_trap1_handler       | 33: TRAP #1 (cooperative yield)
    .rept 13
    .long Default_Handler          | 34-46: TRAP #2 - TRAP #14
    .endr
    .long m68k_trap15_handler      | 47: TRAP #15 (Human68k IOCS)

    | --- Exception vectors 48-63: Reserved ---
    .rept 16
    .long Default_Handler
    .endr

    | --- Exception vectors 64-255: User interrupt vectors ---
    | Used by Goldfish PIC on QEMU virt machine
    .rept 192
    .long Default_Handler
    .endr


| =============================================================================
| Reset_Handler
|
| The CPU jumps here on power-on/reset.  SSP is already set to __stack_top
| by the hardware (loaded from vector 0) before this runs.
|
| Sequence:
|   1. Copy .data image from ROM/load region to RAM
|   2. Zero the .bss region
|   3. Call kmain() — must never return
| =============================================================================

    .section .text.reset_handler, "ax"
    .align 2
    .global Reset_Handler
Reset_Handler:
    | -----------------------------------------------------------------
    | Set SSP explicitly — QEMU's -kernel may not load initial SSP
    | from vector 0 the way real hardware does.
    | -----------------------------------------------------------------
    lea.l    __stack_top,%sp

    | -----------------------------------------------------------------
    | Copy .data: load region → RAM
    |   a0 = source (load address)
    |   a1 = destination start
    |   a2 = destination end
    | -----------------------------------------------------------------
    lea.l    __data_load,%a0
    lea.l    __data_start,%a1
    lea.l    __data_end,%a2
    bra.s    .Lcopy_check
.Lcopy_loop:
    move.l   (%a0)+,(%a1)+
.Lcopy_check:
    cmp.l    %a2,%a1
    blo.s    .Lcopy_loop

    | -----------------------------------------------------------------
    | Zero .bss
    |   a0 = start
    |   a1 = end
    | -----------------------------------------------------------------
    lea.l    __bss_start,%a0
    lea.l    __bss_end,%a1
    bra.s    .Lzero_check
.Lzero_loop:
    clr.l    (%a0)+
.Lzero_check:
    cmp.l    %a1,%a0
    blo.s    .Lzero_loop

    | -----------------------------------------------------------------
    | Enter kernel — kmain() must never return
    | -----------------------------------------------------------------
    jsr      kmain

    | Halt if kmain() returns unexpectedly
.Lhalt:
    stop     #0x2700             | halt with all IRQs masked
    bra.s    .Lhalt


| =============================================================================
| Default_Handler
|
| Catches all unhandled exceptions.
| Halts in a tight loop — attach a debugger to inspect PC/SR.
| =============================================================================

    .section .text, "ax"
    .align 2

| PCB field offsets — must match struct pcb_t in proc.h
    .equ PCB_SP_OFFSET,  44          | byte offset of sp  field in pcb_t
    .equ PCB_USP_OFFSET, 48          | byte offset of usp field in pcb_t

| =============================================================================
| Fault handlers
|
| Bus Error / Address Error (group 0) push a 14-byte frame:
|   [SP+0]  Function code (word)
|   [SP+2]  Access address (long)
|   [SP+6]  Instruction register (word)
|   [SP+8]  SR (word)
|   [SP+10] PC (long)
|
| Illegal Instruction / Zero Divide / etc. (group 1/2) push a 6-byte frame:
|   [SP+0]  SR (word)
|   [SP+2]  PC (long)
|
| All fault handlers save full register state, call the C handler which
| prints a crash report.  If the faulting process is a user process, the
| C handler kills it and we reschedule.  If it's the kernel (pid 0), we halt.
| =============================================================================

| --- Bus Error (group 0 — 14-byte exception frame) ---
    .global m68k_bus_handler
m68k_bus_handler:
    movem.l %d0-%d7/%a0-%a6,%sp@-   | save all regs (60 bytes)
    move.l  %sp,%sp@-               | arg2: register frame
    pea.l   2                        | arg1: fault_type = FAULT_BUS (vector 2)
    jsr     m68k_crash_handler
    addq.l  #8,%sp
    tst.l   %d0
    beq.s   .Lfault_halt
    lea.l   %sp@(74),%sp             | discard regs + group-0 frame (60+14)
    jmp     m68k_schedule_next

| --- Address Error (group 0 — 14-byte exception frame) ---
    .global m68k_addr_handler
m68k_addr_handler:
    movem.l %d0-%d7/%a0-%a6,%sp@-
    move.l  %sp,%sp@-               | arg2: register frame
    pea.l   3                        | arg1: fault_type = FAULT_ADDR (vector 3)
    jsr     m68k_crash_handler
    addq.l  #8,%sp
    tst.l   %d0
    beq.s   .Lfault_halt
    lea.l   %sp@(74),%sp
    jmp     m68k_schedule_next

.Lfault_halt:
    stop    #0x2700
    bra.s   .Lfault_halt

| --- Illegal Instruction (group 1 — 6-byte exception frame) ---
    .global m68k_illegal_handler
m68k_illegal_handler:
    movem.l %d0-%d7/%a0-%a6,%sp@-
    move.l  %sp,%sp@-               | arg2: register frame
    pea.l   4                        | arg1: fault_type = FAULT_ILLEGAL
    jsr     m68k_crash_handler
    addq.l  #8,%sp
    tst.l   %d0
    beq.s   .Lillegal_halt
    lea.l   %sp@(66),%sp             | discard regs + group-1 frame (60+6)
    jmp     m68k_schedule_next
.Lillegal_halt:
    stop    #0x2700
    bra.s   .Lillegal_halt

| --- Zero Divide (group 1 — 6-byte exception frame) ---
    .global m68k_zerodiv_handler
m68k_zerodiv_handler:
    movem.l %d0-%d7/%a0-%a6,%sp@-
    move.l  %sp,%sp@-
    pea.l   5                        | fault_type = FAULT_ZERODIV
    jsr     m68k_crash_handler
    addq.l  #8,%sp
    tst.l   %d0
    beq.s   .Lzerodiv_halt
    lea.l   %sp@(66),%sp
    jmp     m68k_schedule_next
.Lzerodiv_halt:
    stop    #0x2700
    bra.s   .Lzerodiv_halt

| --- CHK / TRAPV (group 1 — 6-byte exception frame) ---
    .global m68k_chk_handler
m68k_chk_handler:
    movem.l %d0-%d7/%a0-%a6,%sp@-
    move.l  %sp,%sp@-
    pea.l   6                        | fault_type = FAULT_CHK
    jsr     m68k_crash_handler
    addq.l  #8,%sp
    tst.l   %d0
    beq.s   .Lchk_halt
    lea.l   %sp@(66),%sp
    jmp     m68k_schedule_next
.Lchk_halt:
    stop    #0x2700
    bra.s   .Lchk_halt

| --- Privilege Violation (group 1 — 6-byte exception frame) ---
    .global m68k_priv_handler
m68k_priv_handler:
    movem.l %d0-%d7/%a0-%a6,%sp@-
    move.l  %sp,%sp@-
    pea.l   8                        | fault_type = FAULT_PRIV
    jsr     m68k_crash_handler
    addq.l  #8,%sp
    tst.l   %d0
    beq.s   .Lpriv_halt
    lea.l   %sp@(66),%sp
    jmp     m68k_schedule_next
.Lpriv_halt:
    stop    #0x2700
    bra.s   .Lpriv_halt

| --- F-line emulator (group 1 — 6-byte exception frame) ---
| Human68k DOS calls are encoded as F-line instructions ($FFxx).
| Saves all registers, then calls m68k_fline_dispatch(regs, usp) in C.
|
| Returns: 0 = kernel fault (halt)
|          1 = process killed (schedule next)
|          2 = DOS call handled (restore regs, advance PC, rte)
    .global m68k_fline_handler
m68k_fline_handler:
    movem.l %d0-%d7/%a0-%a6,%sp@-   | save all regs (60 bytes)
    | Get USP (only accessible via move usp in supervisor mode)
    move.l  %usp,%a0
    move.l  %a0,%sp@-               | arg2: user stack pointer
    move.l  %sp,%d0
    addq.l  #4,%d0
    move.l  %d0,%sp@-               | arg1: register frame pointer
    jsr     m68k_fline_dispatch
    addq.l  #8,%sp                  | clean up 2 args
    cmp.l   #2,%d0
    beq.s   .Lfline_return
    tst.l   %d0
    beq.s   .Lfline_halt
    lea.l   %sp@(66),%sp            | discard regs + group-1 frame (60+6)
    jmp     m68k_schedule_next
.Lfline_return:
    | Check for a pending context switch (e.g. ptrace subsys trace stop set
    | by arch_yield() inside trace_before_subsys).  Mirror the logic in trap.S.
    tst.l   switch_pending
    beq.s   .Lfline_rte
    clr.l   switch_pending
    move.l  current_core,%a0
    move.l  %sp,%a0@(PCB_SP_OFFSET)  | save current kernel SP
    move.l  %usp,%a1
    move.l  %a1,%a0@(PCB_USP_OFFSET) | save user SP
    jsr     sched_next
    move.l  %d0,current_core
    move.l  %d0,%a0
    move.l  %a0@(PCB_SP_OFFSET),%sp  | restore kernel SP
    move.l  %a0@(PCB_USP_OFFSET),%a1
    move.l  %a1,%usp                  | restore user SP
.Lfline_rte:
    jsr     m68k_vfork_restore_frame
    movem.l %sp@+,%d0-%d7/%a0-%a6   | restore all regs (60 bytes)
    rte                              | return (SR+PC on stack)
.Lfline_halt:
    stop    #0x2700
    bra.s   .Lfline_halt

| --- TRAP #15 — Human68k IOCS call (group 1 — 6-byte exception frame) ---
| IOCS call number is in d0.  Saves all registers, calls
| human68k_iocs_dispatch(regs) in C.  Always returns 2 (handled).
    .global m68k_trap15_handler
m68k_trap15_handler:
    movem.l %d0-%d7/%a0-%a6,%sp@-   | save all regs (60 bytes)
    move.l  %sp,%d0
    move.l  %d0,%sp@-               | arg1: register frame pointer
    jsr     human68k_iocs_dispatch
    addq.l  #4,%sp                  | clean up 1 arg
    | Check for a pending context switch (same reason as .Lfline_return above)
    tst.l   switch_pending
    beq.s   .Ltrap15_rte
    clr.l   switch_pending
    move.l  current_core,%a0
    move.l  %sp,%a0@(PCB_SP_OFFSET)  | save current kernel SP
    move.l  %usp,%a1
    move.l  %a1,%a0@(PCB_USP_OFFSET) | save user SP
    jsr     sched_next
    move.l  %d0,current_core
    move.l  %d0,%a0
    move.l  %a0@(PCB_SP_OFFSET),%sp  | restore kernel SP
    move.l  %a0@(PCB_USP_OFFSET),%a1
    move.l  %a1,%usp                  | restore user SP
.Ltrap15_rte:
    jsr     m68k_vfork_restore_frame
    movem.l %sp@+,%d0-%d7/%a0-%a6   | restore all regs (60 bytes)
    rte                              | return (SR+PC on stack)

| --- Schedule next process after killing a faulting process ---
| The old process is dead.  Switch to the kernel stack, pick the next
| runnable process via sched_next(), and restore its full context.
| Same restore sequence as m68k_trap1_handler / m68k_timer_isr.
    .global m68k_schedule_next
m68k_schedule_next:
    lea.l   __stack_top,%sp
    jsr     sched_next
    | d0 = next pcb_t*; update current_core[0]
    move.l  %d0,current_core
    | Restore SP (SSP) and USP from next PCB
    move.l  %d0,%a0
    move.l  44(%a0),%sp              | PCB_SP_OFFSET = 44
    move.l  48(%a0),%a1              | PCB_USP_OFFSET = 48
    move.l  %a1,%usp
    jsr     m68k_vfork_restore_frame
    | Restore all registers (d0-d7/a0-a6) and return via RTE
    movem.l %sp@+,%d0-%d7/%a0-%a6
    rte

| --- Benign IRQ handler: acknowledge and return ---
| Used by x68k target to silence hardware IRQs (VSYNC, OPM, FDC, DMA)
| that have no kernel handler yet.  Simply returns from the exception.
    .global m68k_irq_ignore
m68k_irq_ignore:
    rte

    .global Default_Handler
Default_Handler:
    stop     #0x2700             | halt with all IRQs masked
    bra.w    Default_Handler
