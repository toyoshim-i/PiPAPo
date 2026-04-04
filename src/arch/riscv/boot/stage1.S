# =============================================================================
# stage1.S — Stage 1 Bootloader Extension (RISC-V)
# =============================================================================
#
# Placed in the FLASH_BOOT region (first 4 KB of flash) alongside the
# PICOBIN block.  The RP2350 boot ROM reads the PICOBIN IMAGE_DEF,
# selects the Hazard3 RISC-V cores, and jumps to the entry point
# specified in the PICOBIN ENTRY_POINT item — which is stage1_entry.
#
# stage1_entry then redirects execution to the kernel:
#   1. Set mtvec = __kernel_entry (linker-provided, FLASH_KERNEL start)
#   2. Load sp = __stack_top
#   3. Jump to _start (kernel reset handler in boot.S)
#
# This code must be position-independent.  It runs with XIP already
# active (the ROM or boot2 enabled QSPI).
#
# Unlike ARM (where stage1 mimics a Cortex-M vector table at +0x100),
# RISC-V stage1 is a simple trampoline — the ROM finds the entry point
# via the PICOBIN block, not via a vector table offset.
# =============================================================================

.option norelax

.section .stage1, "ax"
.align 2
.global stage1_entry
stage1_entry:
    # ── 1. Set mtvec to kernel trap vector ───────────────────────────────
    # __kernel_entry is defined by the linker script at ORIGIN(FLASH_KERNEL),
    # pointing to _trap_vector in boot.S.
    la      t0, __kernel_entry
    csrw    mtvec, t0

    # ── 2. Set stack pointer ─────────────────────────────────────────────
    la      sp, __stack_top

    # ── 3. Jump to kernel _start ─────────────────────────────────────────
    # _start (boot.S) handles .data copy, .bss clear, and calls kmain().
    la      t0, _start
    jr      t0
