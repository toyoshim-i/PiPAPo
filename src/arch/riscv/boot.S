# =============================================================================
# boot.S — RISC-V Reset Handler
# =============================================================================
#
# Provides:
#   _start            → copies .data, zeros .bss, calls kmain()
#   Default_Handler   → infinite loop for unhandled traps
#
# The RP2350 boot ROM selects RISC-V cores via the PICOBIN block, then
# stage1.S redirects mtvec to __kernel_entry and jumps here.
#
# The trap vector (_trap_vector) and dispatch logic are in trap.S.
# =============================================================================

.option norelax   # Prevent linker relaxation of gp-relative accesses

# =============================================================================
# _start — kernel entry point
#
# Called by stage1.S after setting mtvec.  Stack pointer is already set
# to __stack_top by stage1 (or by the ROM for direct-entry boot).
#
# Sequence:
#   1. Set sp = __stack_top (defensive, in case stage1 didn't)
#   2. Set gp = __global_pointer$ (for linker relaxation)
#   3. Set mtvec to _trap_vector (direct mode)
#   4. Disable interrupts (clear mstatus.MIE)
#   5. Copy .data image from flash (LMA) to SRAM (VMA)
#   6. Zero the .bss region
#   7. Call kmain() — must never return
# =============================================================================

.section .text.reset_handler, "ax"
.align 2
.global _start
_start:
    # ── 1. Set stack pointer ─────────────────────────────────────────────
    la      sp, __stack_top

    # ── 2. Set global pointer ────────────────────────────────────────────
    .option push
    .option norelax
    la      gp, __global_pointer$
    .option pop

    # ── 3. Set mtvec (direct mode) ──────────────────────────────────────
    la      t0, _trap_vector
    csrw    mtvec, t0

    # ── 4. Disable interrupts, initialize mscratch ──────────────────────
    csrc    mstatus, 0x8           # clear MIE (bit 3)
    csrw    mscratch, sp           # mscratch = __stack_top (for trap entry)

    # ── 4c. PMP: allow U-mode full memory access ───────────────────────
    li      t0, 0x1FFFFFFF         # NAPOT mask for 4GB
    csrw    pmpaddr0, t0
    li      t0, 0x1F               # A=NAPOT(11), R|W|X, L=0
    csrw    pmpcfg0, t0

#ifndef PPAP_QEMU
    # ── 4b. Disable watchdog (RP2350 only) ─────────────────────────────
    # The RP2350 boot ROM may leave the watchdog armed (especially after
    # BOOTSEL USB reboot).  QEMU virt has no watchdog at this address.
    li      t0, 0x400DB000         # WATCHDOG_CTRL CLR alias
    li      t1, (1 << 30)          # ENABLE bit
    sw      t1, 0(t0)
#endif

    # ── 5. Copy .data: flash (LMA) → SRAM (VMA) ─────────────────────────
    #   a0 = destination start  (__data_start)
    #   a1 = destination end    (__data_end)
    #   a2 = source             (__data_load)
    la      a0, __data_start
    la      a1, __data_end
    la      a2, __data_load
    j       .Lcopy_check
.Lcopy_loop:
    lw      t0, 0(a2)
    sw      t0, 0(a0)
    addi    a0, a0, 4
    addi    a2, a2, 4
.Lcopy_check:
    bltu    a0, a1, .Lcopy_loop

    # ── 5b. Copy .ramfunc: flash → SRAM ─────────────────────────────────
    la      a0, __ramfunc_start
    la      a1, __ramfunc_end
    la      a2, __ramfunc_load
    j       .Lramfunc_check
.Lramfunc_loop:
    lw      t0, 0(a2)
    sw      t0, 0(a0)
    addi    a0, a0, 4
    addi    a2, a2, 4
.Lramfunc_check:
    bltu    a0, a1, .Lramfunc_loop

    # ── 6. Zero .bss ────────────────────────────────────────────────────
    la      a0, __bss_start
    la      a1, __bss_end
    j       .Lzero_check
.Lzero_loop:
    sw      zero, 0(a0)
    addi    a0, a0, 4
.Lzero_check:
    bltu    a0, a1, .Lzero_loop

    # ── 7. Enter kernel ─────────────────────────────────────────────────
    call    kmain

    # Halt if kmain() returns unexpectedly (NOP loop for debuggability)
.Lhalt:
    nop
    j       .Lhalt


# =============================================================================
# Default_Handler
#
# Catches all unhandled traps.
# Spins in a tight NOP loop — attach a debugger to inspect mepc/mcause.
# Using NOP instead of WFI ensures the Hazard3 debug module can always
# halt the core (WFI may gate the core clock on some implementations).
# =============================================================================

.section .text, "ax"
.align 2
.global Default_Handler
Default_Handler:
    nop
    j       Default_Handler
