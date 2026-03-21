# Raspberry Pi Pico 2 RISC-V Target Port Plan

Porting PPAP to the RP2350 Hazard3 RISC-V cores (`pico2rv` target).
The RP2350 can boot as either dual Cortex-M33 (ARM) or dual Hazard3
(RISC-V).  The existing `pico2` target uses the ARM cores; this proposal
covers the RISC-V side — a completely new ISA for PPAP.

---

## 1. Hazard3 RISC-V Cores

### 1.1 ISA Overview

The Hazard3 is a 3-stage, in-order RV32IMAC core designed by Raspberry Pi:

| Feature | Hazard3 (RP2350) |
|---------|-----------------|
| ISA | **RV32IMAC** (32-bit Integer + Multiply + Atomic + Compressed) |
| Pipeline | 3-stage (fetch, decode/execute, writeback) |
| Privilege levels | M-mode + U-mode |
| Interrupts | Standard RISC-V CLINT + platform IRQ (Zcmt extension) |
| Multiply | 1-cycle (M extension) |
| Atomics | AMO instructions (A extension) |
| Compressed | 16-bit C instructions (like Thumb on ARM) |
| FPU | **None** (no F extension) |
| MMU/MPU | **PMP** (Physical Memory Protection, 8 regions) |
| Endianness | Little-endian |
| Counter extensions | Zicntr, Zihpm |
| Custom extensions | Xh3bm (bit manipulation), Xh3power (power management), Xh3irq (fast IRQ) |

### 1.2 Hazard3 vs Cortex-M33 (both in RP2350)

| Aspect | Cortex-M33 (pico2) | Hazard3 (pico2rv) |
|--------|-------------------|-------------------|
| ISA | Thumb-2 (ARMv8-M) | RV32IMAC |
| Registers | r0-r12, sp, lr, pc (16 GPR) | x0-x31 (32 GPR, x0=zero) |
| FPU | VFPv5 single-precision | None |
| Memory protection | ARMv8-M MPU (8 regions) | PMP (8 regions) |
| Exception model | NVIC (vectored, priority-based) | CLINT + Xh3irq (trap-based) |
| Context switch | PendSV hardware exception | Software trap |
| Privilege | Privileged / Unprivileged | M-mode / U-mode |
| Boot selection | PICOBIN `IMAGE_TYPE` CPU field | PICOBIN `IMAGE_TYPE` CPU field |
| SysTick | ARM SysTick (0xE000E010) | RISC-V timer (mtime/mtimecmp) |
| Atomics | LDREX/STREX | AMO (lr/sc, amoswap, etc.) |
| Code density | Thumb-2 (16/32 bit mix) | RV32C (16/32 bit mix) |

### 1.3 What Stays the Same

The Hazard3 cores share the **same SoC** as the Cortex-M33 cores:

- Same SRAM (520 KB, same addresses)
- Same flash (XIP at 0x10000000)
- Same peripherals (UART, SPI, I2C, PIO, DMA, GPIO, ADC, USB)
- Same peripheral addresses (0x40000000+ range)
- Same SIO block (spinlocks, CPUID, inter-core FIFO)
- Same PLL and clock tree
- Same boot ROM (selects ARM or RISC-V via PICOBIN block)

Only the CPU core changes — everything above the arch layer is identical
to `pico2`.

---

## 2. Goals and Scope

### 2.1 Primary Goal

Produce a bootable PPAP system on the Pico 2 in RISC-V mode that:

- Boots from QSPI flash using the existing stage1 mechanism (adapted for RV32).
- Runs preemptive scheduling on a single Hazard3 core.
- Passes the full PPAP test suite (`runtests`).
- Uses the same 520 KB SRAM layout as `pico2`.

### 2.2 Extended Goals

- Dual-core RISC-V (both Hazard3 cores running PPAP).
- Hazard3 custom extensions (Xh3bm for bit manipulation, Xh3irq for
  fast interrupt dispatch).

### 2.3 Out of Scope

- FPU support (Hazard3 has no FPU — no F extension).
- PSRAM (same as pico2, deferred).
- Wi-Fi/Bluetooth, USB host.

---

## 3. Architecture Layer: New `src/arch/riscv/`

This is the core of the port.  PPAP's architecture abstraction requires
these files per architecture:

| File | ARM (`arm_m/`) | m68k (`m68k/`) | RISC-V (`riscv/`) |
|------|----------------|----------------|-------------------|
| `arch.h` | IRQ save/restore, yield, barriers | Same API, 68k impl | Same API, RV32 impl |
| `cpu.h` | SysTick, SCB, NVIC registers | SR bits, trap numbers | CSR definitions, PMP |
| `boot.S` | Reset_Handler, vector table | Vector table, reset | Trap vector, `_start` |
| `switch.S` | PendSV_Handler (context switch) | TRAP #1 + timer ISR | Software trap / timer |
| `trap.S` | HardFault, SVC, exception entry | TRAP #0 (syscall) | `ecall` handler |
| `stage1.S` | VTOR redirect | N/A (boot from floppy) | mtvec redirect |
| common `.c` | `arm_m_common.c` | `m68k_common.c` | `riscv_common.c` |

### 3.1 `arch.h` — Architecture Abstraction

```c
/* arch_irq_save: read mstatus.MIE, then clear it */
static inline uint32_t arch_irq_save(void) {
    uint32_t mstatus;
    __asm__ volatile ("csrrci %0, mstatus, 0x8" : "=r"(mstatus));
    return mstatus & 0x8;  /* MIE bit */
}

/* arch_irq_restore: restore MIE bit */
static inline void arch_irq_restore(uint32_t saved) {
    if (saved)
        __asm__ volatile ("csrsi mstatus, 0x8");
}

/* arch_irq_enable/disable: set/clear mstatus.MIE */
static inline void arch_irq_enable(void)  { __asm__ volatile ("csrsi mstatus, 0x8"); }
static inline void arch_irq_disable(void) { __asm__ volatile ("csrci mstatus, 0x8"); }

/* arch_yield: set software interrupt pending (trigger context switch) */
static inline void arch_yield(void) {
    /* Hazard3 uses SIO-based doorbell or mip.MSIP for software IRQ */
    /* Details TBD — may use ecall or Xh3irq mechanism */
}

/* arch_wfi/wfe/sev */
static inline void arch_wfi(void) { __asm__ volatile ("wfi"); }
static inline void arch_wfe(void) { __asm__ volatile ("wfi"); } /* no WFE on RV32 */
static inline void arch_sev(void) { /* SIO doorbell or SEV equivalent */ }

/* arch_dsb_isb: fence on RISC-V */
static inline void arch_dsb_isb(void) {
    __asm__ volatile ("fence rw, rw" ::: "memory");
    __asm__ volatile ("fence.i" ::: "memory");
}
```

### 3.2 `cpu.h` — CSR and System Register Definitions

Key RISC-V CSRs needed by PPAP:

| CSR | Address | Purpose |
|-----|---------|---------|
| `mstatus` | 0x300 | Machine status (MIE, MPP, MPIE) |
| `misa` | 0x301 | ISA description |
| `mie` | 0x304 | Machine interrupt enable (MTIE, MSIE, MEIE) |
| `mtvec` | 0x305 | Machine trap vector base |
| `mscratch` | 0x340 | Scratch register for trap handler |
| `mepc` | 0x341 | Machine exception PC |
| `mcause` | 0x342 | Trap cause |
| `mtval` | 0x343 | Trap value (faulting address) |
| `mip` | 0x344 | Machine interrupt pending |
| `pmpcfg0-1` | 0x3A0-0x3A1 | PMP configuration |
| `pmpaddr0-7` | 0x3B0-0x3B7 | PMP address registers |
| `mcycle` | 0xB00 | Cycle counter |

RP2350-specific:
| Register | Address | Purpose |
|----------|---------|---------|
| `mtime` | SIO-mapped | Timer counter (shared with ARM SysTick concept) |
| `mtimecmp` | SIO-mapped | Timer compare (generates MTIP interrupt) |

### 3.3 `boot.S` — Reset and Trap Vector Table

RISC-V uses `mtvec` to define the trap entry point.  Two modes:

- **Direct mode** (`mtvec[1:0] = 00`): all traps go to `mtvec` base.
- **Vectored mode** (`mtvec[1:0] = 01`): interrupts jump to
  `mtvec_base + 4 * cause`.

For simplicity, the initial port uses **direct mode** — a single trap
entry point dispatches based on `mcause`.

```asm
# boot.S — RISC-V entry point
.section .vectors, "ax"
.global _start
_start:
    # Set up stack pointer
    la      sp, __stack_top

    # Set trap vector (direct mode)
    la      t0, _trap_entry
    csrw    mtvec, t0

    # Clear BSS
    la      t0, __bss_start
    la      t1, __bss_end
1:  bge     t0, t1, 2f
    sw      zero, 0(t0)
    addi    t0, t0, 4
    j       1b
2:
    # Jump to C entry
    call    kmain
    # Should not return
1:  wfi
    j       1b
```

### 3.4 `switch.S` — Context Switch

RISC-V has 32 integer registers.  The RISC-V calling convention defines:

| Registers | Role | Saved by |
|-----------|------|----------|
| x0 (zero) | Hardwired zero | — |
| x1 (ra) | Return address | Caller |
| x2 (sp) | Stack pointer | Callee |
| x3 (gp) | Global pointer | — |
| x4 (tp) | Thread pointer | — |
| x5-x7 (t0-t2) | Temporaries | Caller |
| x8 (s0/fp) | Frame pointer | Callee |
| x9 (s1) | Saved | Callee |
| x10-x11 (a0-a1) | Arguments/return | Caller |
| x12-x17 (a2-a7) | Arguments | Caller |
| x18-x27 (s2-s11) | Saved | Callee |
| x28-x31 (t3-t6) | Temporaries | Caller |

On a trap (interrupt or `ecall`), RISC-V does **not** automatically push
any registers — the software trap handler must save everything.  This
is fundamentally different from ARM Cortex-M (which auto-pushes 8 regs).

**Context switch approach** (similar to m68k pattern):

```asm
# _trap_entry — single entry for all traps
# Save all caller-saved + callee-saved registers to kernel stack
# Read mcause:
#   - If interrupt (mcause[31] = 1): dispatch to timer or external ISR
#   - If ecall from U-mode (mcause = 8): syscall handler
#   - If ecall from M-mode (mcause = 11): yield / context switch
#   - Otherwise: fault handler
```

Stack frame for a suspended process:

```
[SP+0 ]  ra          ← pcb_t.sp points here
[SP+4 ]  gp
[SP+8 ]  tp
[SP+12]  t0
[SP+16]  t1
[SP+20]  t2
[SP+24]  s0 (fp)
[SP+28]  s1
[SP+32]  a0
[SP+36]  a1
[SP+40]  a2
[SP+44]  a3
[SP+48]  a4
[SP+52]  a5
[SP+56]  a6
[SP+60]  a7
[SP+64]  s2
[SP+68]  s3
[SP+72]  s4
[SP+76]  s5
[SP+80]  s6
[SP+84]  s7
[SP+88]  s8
[SP+92]  s9
[SP+96]  s10
[SP+100] s11
[SP+104] t3
[SP+108] t4
[SP+112] t5
[SP+116] t6
[SP+120] mepc        (return address)
[SP+124] mstatus     (saved status)
```

Total: 32 words = 128 bytes per context frame (31 GPRs + mepc + mstatus,
minus x0 which is always zero).

**Key difference from ARM**: On ARM Cortex-M, PendSV fires at lowest
priority and the hardware saves 8 registers automatically.  On RISC-V,
we must manually save all registers in the trap handler.  The m68k port
already demonstrates this pattern (`movem.l %d0-%d7/%a0-%a6,%sp@-`).

### 3.5 `trap.S` — Syscall Entry

User processes invoke syscalls via `ecall` (RISC-V equivalent of ARM's
`svc` or m68k's `trap #0`).  The `ecall` from U-mode generates
`mcause = 8` (Environment call from U-mode).

```
User:   ecall                    (syscall number in a7, args in a0-a5)
Kernel: _trap_entry
        → save all registers
        → read mcause
        → mcause == 8: call syscall_dispatch(regs)
        → mepc += 4 (skip past ecall instruction)
        → restore registers
        → mret
```

The syscall calling convention matches the RISC-V Linux ABI:
- `a7` = syscall number
- `a0`-`a5` = arguments
- `a0` = return value

### 3.6 `stage1.S` — Boot Redirect

The ARM `stage1.S` sets `VTOR` to point at the kernel vector table.
On RISC-V, the equivalent is setting `mtvec`:

```asm
# stage1.S — RISC-V boot redirect
.section .stage1, "ax"
.global stage1_entry
stage1_entry:
    la      t0, __kernel_entry     # from linker script
    csrw    mtvec, t0              # set trap vector to kernel
    la      sp, __stack_top
    jr      t0                     # jump to kernel _start
```

### 3.7 Timer and Preemption

ARM Cortex-M uses SysTick for periodic timer interrupts.  RISC-V uses
the standard `mtime`/`mtimecmp` mechanism:

| Aspect | ARM SysTick | RISC-V Timer |
|--------|------------|-------------|
| Counter | SYST_CVR (24-bit, down) | `mtime` (64-bit, up) |
| Compare | SYST_RVR (reload value) | `mtimecmp` (64-bit) |
| Interrupt | SysTick_Handler (exception 15) | Machine timer interrupt (mcause bit 7) |
| Enable | SYST_CSR.TICKINT | mie.MTIE |
| Period | SYSTICK_RELOAD | Set mtimecmp = mtime + TICK_INTERVAL |

On RP2350, the RISC-V timer is mapped through the SIO block.  The timer
runs at the system clock (150 MHz), so:

```c
#define TICK_INTERVAL  (PPAP_SYS_HZ / 100u)  /* 1,500,000 for 10ms @ 150 MHz */
```

The timer ISR:
1. Clear interrupt by writing `mtimecmp = mtime + TICK_INTERVAL`.
2. Call `sched_timer_tick()`.
3. If switch pending, perform context switch inline (like m68k pattern).

---

## 4. Memory Protection: PMP

RISC-V uses PMP (Physical Memory Protection) instead of ARM's MPU.
The Hazard3 supports 8 PMP regions.

### 4.1 PMP vs ARM MPU

| Aspect | ARM MPU (ARMv8-M) | RISC-V PMP |
|--------|-------------------|-----------|
| Regions | 8 | 8 |
| Config registers | MPU_RNR + RBAR + RLAR | pmpcfgN + pmpaddrN |
| Granularity | 32 bytes | 4 bytes (NAPOT) or arbitrary (TOR) |
| Modes | NAPOT only (base+limit) | NAPOT, TOR, NA4 |
| Attributes | MAIR-indexed (RW/RO/XN) | R/W/X bits per region |
| Default | PRIVDEFENA (priv=full access) | M-mode bypasses PMP unless locked |

### 4.2 PMP Region Layout

| Region | Range | Config | Purpose |
|--------|-------|--------|---------|
| 0 | 0x00000000 - 0x0FFFFFFF | None (L=0) | Below flash: no access from U-mode |
| 1 | 0x10000000 - 0x10FFFFFF | R-X (U-mode) | Flash XIP: user code read+execute |
| 2 | 0x20000000 - 0x20005FFF | None | Kernel SRAM: M-mode only |
| 3 | per-process stack page | RW- (U-mode) | Process stack: user read+write |
| 4 | 0x40000000 - 0x5FFFFFFF | None | Peripherals: M-mode only |
| 5-7 | Reserved | — | Future use |

### 4.3 PMP Switch on Context Change

Like ARM's `mpu_switch()`, we need `pmp_switch()` to reprogram the
process stack region (PMP region 3) on every context switch:

```c
void pmp_switch(pcb_t *next) {
    uintptr_t base = (uintptr_t)next->stack_page;
    uintptr_t size = PAGE_SIZE;  /* 4 KB */
    /* NAPOT encoding: pmpaddr = (base | (size/2 - 1)) >> 2 */
    uint32_t napot = (base | (size / 2 - 1)) >> 2;
    csr_write(pmpaddr3, napot);
    /* pmpcfg0 byte 3: A=NAPOT(3), R=1, W=1, X=0 */
    /* Update only byte 3 of pmpcfg0, preserving other regions */
}
```

---

## 5. Build System

### 5.1 Toolchain

RISC-V GCC cross-compiler:

```
riscv32-unknown-elf-gcc   (or riscv64-unknown-elf-gcc with -march=rv32imac)
```

The Pico SDK 2.2.0 supports RISC-V via `PICO_PLATFORM=rp2350-riscv`.
It automatically selects the correct toolchain and compiler flags.

Required flags:
```
-march=rv32imac_zicsr_zifencei
-mabi=ilp32
-mcmodel=medlow
```

### 5.2 CMakeLists.txt

```cmake
# src/target/pico2rv/CMakeLists.txt

set(PICO_PLATFORM rp2350-riscv)   # Key difference from pico2
set(PICO_BOARD pico2)

# ... SDK setup same as pico2 ...

include(${PPAP_ROOT}/cmake/riscv.cmake)   # New: RISC-V build rules

add_executable(ppap_pico2rv
    ${KERNEL_COMMON_SOURCES}
    ${PPAP_ROOT}/src/arch/riscv/boot.S
    ${PPAP_ROOT}/src/arch/riscv/switch.S
    ${PPAP_ROOT}/src/arch/riscv/trap.S
    ${PPAP_ROOT}/src/arch/riscv/stage1.S
    ${PPAP_ROOT}/src/arch/riscv/riscv_common.c
    ${CMAKE_CURRENT_SOURCE_DIR}/picobin_block.S
    ${CMAKE_CURRENT_SOURCE_DIR}/target_pico2rv.c
    ${PPAP_ROOT}/src/drivers/arch/arm_m/uart_rpico.c   # Same UART driver
    ${PPAP_ROOT}/src/drivers/arch/arm_m/clock_rpico.c  # Same clock driver
)

target_compile_definitions(ppap_pico2rv PRIVATE
    PPAP_SYS_HZ=150000000u
    PPAP_PLL_FBDIV=125u
    PPAP_PLL_PD1=5u
    PPAP_PLL_PD2=2u
    PAGE_POOL_BASE=0x20006000u
    PAGE_COUNT_MAX=116u
)
```

**Note on drivers**: The UART and clock drivers access memory-mapped
peripheral registers.  These are at the same addresses regardless of
whether the CPU is ARM or RISC-V, so the same driver source files work
for both.  The drivers currently live under `src/drivers/arch/arm_m/`
but contain no ARM-specific instructions — only volatile register reads
and writes.  We may want to rename to `src/drivers/soc/rpico/` in the
future, but for now linking against the existing files is simplest.

### 5.3 New Build Infrastructure

```
cmake/riscv.cmake         — RISC-V build rules (like arm_m.cmake)
```

This file defines:
- `KERNEL_COMMON_SOURCES` (same kernel C files as arm_m.cmake)
- `ppap_riscv_target_common()` function (compiler flags, include paths)
- RISC-V-specific compiler options (`-march`, `-mabi`)

### 5.4 PICOBIN Block (RISC-V variant)

The pico2 ARM target uses a PICOBIN block declaring `CPU=ARM`.
The RISC-V target needs `CPU=RISCV`:

```asm
# IMAGE_TYPE: EXE | Security=S | CPU=RISCV(1) | Chip=RP2350
.equ IMAGE_TYPE_VALUE, 0x1121    # bit 8 set for RISC-V (was 0x1021 for ARM)
```

Everything else in the PICOBIN block is the same structure.

### 5.5 Linker Script

The linker script is almost identical to `pico2.ld` — same flash and
SRAM layout.  Differences:

- RISC-V uses `ENTRY(_start)` instead of `ENTRY(Reset_Handler)`
- No `.ARM.exidx` section (ARM-specific unwind tables)
- Vector table section is `.vectors` instead of `.isr_vector`
- Global pointer (`__global_pointer$`) symbol for the `gp` register

### 5.6 User-Space Cross Compilation

User-space programs (and musl libc, if used) must be compiled for
RV32IMAC.  This means:

- New `cmake/user_riscv.cmake` for building user programs
- ELF binaries in romfs are RV32 format (not ARM)
- `runtests` and other test programs recompiled for RISC-V

---

## 6. Files to Create and Modify

### New files

```
src/arch/riscv/
    arch.h              — IRQ save/restore, yield, barriers (RV32)
    cpu.h               — CSR definitions, PMP registers
    boot.S              — _start, trap vector setup, BSS clear
    stage1.S            — mtvec redirect to kernel
    switch.S            — Context switch (timer ISR + yield)
    trap.S              — Trap entry, syscall dispatch, fault handler
    riscv_common.c      — Signal return, proc_setup_stack, etc.

src/target/pico2rv/
    CMakeLists.txt      — Build rules (PICO_PLATFORM=rp2350-riscv)
    pico2rv.ld          — Linker script (same memory layout as pico2)
    pico2rv.h           — Target constants (same as pico2.h)
    target_pico2rv.c    — Target hooks
    picobin_block.S     — PICOBIN with CPU=RISCV
    romfs/              — Romfs overlay (RV32 binaries)

cmake/
    riscv.cmake         — RISC-V build rules
```

### Existing files to modify

| File | Change | Phase |
|------|--------|-------|
| `src/kernel/proc.c` | `PCB_SP_OFFSET` static assert for RISC-V | RV-1 |
| `src/kernel/proc.c` | `proc_setup_stack()` for RISC-V trap frame | RV-1 |
| `src/kernel/mm/mpu.c` | Add `#if defined(__riscv)` path for PMP | RV-2 |
| `scripts/run.sh` | Add `pico2rv` target | RV-1 |

### Files that need NO changes

| File | Reason |
|------|--------|
| All kernel C code | Architecture-independent (uses `arch.h` API) |
| `src/target/rpico.h` | Peripheral addresses are CPU-independent |
| UART/clock/SPI drivers | Memory-mapped I/O, no CPU-specific instructions |
| VFS, filesystems | Architecture-independent |
| Scheduler | Architecture-independent |

---

## 7. Implementation Phases

### Phase RV-1: Architecture Layer and Boot

**Goal**: "PiPAPo booting... [pico2rv]" on UART at 115200 bps.

#### Step 1a: Create `src/arch/riscv/` skeleton

1. `arch.h` — interrupt save/restore, yield, barriers (see 3.1).
2. `cpu.h` — CSR definitions, PMP register addresses (see 3.2).
3. `boot.S` — `_start` entry point: set SP, clear BSS, set `mtvec`,
   call `kmain()` (see 3.3).
4. `stage1.S` — redirect `mtvec` to kernel, jump to `_start` (see 3.6).

#### Step 1b: Create `src/target/pico2rv/`

1. `picobin_block.S` — PICOBIN with `CPU=RISCV` (see 5.4).
2. `pico2rv.ld` — copy from `pico2.ld`, adapt for RISC-V (see 5.5).
3. `target_pico2rv.c` — copy from `target_pico2.c`, change target name.
4. `CMakeLists.txt` — `PICO_PLATFORM=rp2350-riscv` (see 5.2).

#### Step 1c: Minimal trap handler

1. `trap.S` — trap entry that handles timer interrupt only (no syscalls
   yet).  Just clear the timer and return (`mret`).
2. Timer setup in `riscv_common.c`: configure `mtimecmp` for 10ms ticks.

#### Step 1d: Smoke test

1. Build: `cmake -S src/target/pico2rv -B build/pico2rv`
2. Flash via picoprobe or UF2.
3. Verify UART output: `PiPAPo booting... [pico2rv]`

**Deliverables**: Kernel boots, UART works, timer ticks.

---

### Phase RV-2: Context Switch and Scheduling

**Goal**: Preemptive multitasking on single Hazard3 core.

#### Step 2a: Full trap handler (`trap.S` + `switch.S`)

1. Save all 31 GPRs + `mepc` + `mstatus` on trap entry.
2. Dispatch by `mcause`:
   - Timer interrupt → `sched_timer_tick()` → context switch if pending.
   - `ecall` from U-mode → `syscall_dispatch()`.
   - Faults → `panic()` or signal delivery.
3. Context switch: save SP to outgoing PCB, load SP from incoming PCB,
   restore registers, `mret`.

#### Step 2b: `proc_setup_stack()` for RISC-V

Set up the initial stack frame so that when the scheduler first switches
to a new process, `mret` jumps to the process entry point in U-mode:

```c
/* Build initial trap frame on the process stack */
frame->mepc = entry_point;
frame->mstatus = MSTATUS_MPP_U | MSTATUS_MPIE;  /* return to U-mode with IRQ enabled */
frame->sp = stack_top;
/* a0 = argc, a1 = argv (if applicable) */
```

#### Step 2c: Yield mechanism

Unlike ARM's PendSV (which fires at lowest priority), RISC-V needs a
software mechanism to trigger context switch from C code:

- **Option A**: `ecall` from M-mode (mcause = 11) → treated as yield.
- **Option B**: Software interrupt via `mip.MSIP` → context switch in
  the standard timer interrupt path.
- **Option C**: Set a flag (`riscv_switch_pending`, like m68k) and check
  it on timer ISR exit.

**Recommendation**: Option C (m68k pattern) for initial simplicity.
The m68k port already demonstrates this works well.

#### Step 2d: Test

1. Boot with `init` process.
2. Verify timer-driven preemption switches between processes.
3. Run basic syscall tests (fork, exit, wait).

**Deliverables**: Multiple processes run with preemptive scheduling.

---

### Phase RV-3: PMP (Memory Protection)

**Goal**: User processes cannot access kernel memory.

#### Step 3a: PMP initialization

Configure PMP regions in `pmp_init()`:
- Region 0-2: kernel SRAM (no U-mode access).
- Region 1: flash XIP (U-mode R+X).
- Region 3: per-process stack (U-mode R+W, reprogrammed on switch).
- Region 4: peripherals (no U-mode access).

#### Step 3b: PMP switch

Add `pmp_switch(pcb_t *next)` called from context switch path
(same role as ARM's `mpu_switch()`).

#### Step 3c: Integration with `mpu.c`

The kernel's `mpu.c` already has `#if __ARM_ARCH >= 8` guards.  Add a
`#elif defined(__riscv)` path that calls PMP-specific functions.

**Deliverables**: U-mode process accessing 0x20000000 triggers trap.

---

### Phase RV-4: Syscall and Signal Support

**Goal**: Full syscall set works on RISC-V.

#### Step 4a: Syscall dispatch

Wire `ecall` (mcause = 8) to `syscall_dispatch()`.  The syscall ABI:
- `a7` = syscall number
- `a0`-`a5` = arguments
- Return value in `a0`
- `mepc += 4` before `mret` (skip past `ecall` instruction)

#### Step 4b: Signal delivery

Implement `arch_signal_deliver()` for RISC-V:
- Push signal frame onto user stack.
- Set `mepc` to signal handler address.
- Set `a0` to signal number.
- On `sigreturn`, restore original `mepc` and registers.

#### Step 4c: ELF loader

The ELF loader must recognize `EM_RISCV` (243) in addition to
`EM_ARM` (40) and `EM_68K` (4).  The loader is mostly
architecture-independent (just loading segments), but the ELF machine
type check needs updating.

**Deliverables**: All syscalls work; signals delivered correctly.

---

### Phase RV-5: Full Test Suite

**Goal**: All tests pass on pico2rv.

#### Step 5a: Build user-space for RV32

1. Compile `runtests`, `init`, and all test programs with
   `riscv32-unknown-elf-gcc -march=rv32imac -mabi=ilp32`.
2. Generate romfs with RV32 ELF binaries.

#### Step 5b: Run tests

1. Build: `./scripts/run.sh --test pico2rv` (if QEMU RV32 available)
   or flash to hardware.
2. Expected: 17 tests pass (same ARM subset — h68k_dos and x68k are
   m68k-only).

**Deliverables**: "ALL TESTS PASSED" on UART.

---

### Phase RV-6: Dual-Core (Extended)

**Goal**: Both Hazard3 cores run PPAP.

#### Step 6a: Core 1 launch

The RP2350 uses the same SIO FIFO mechanism for core 1 launch regardless
of ARM or RISC-V mode.  The SIO_CPUID register works the same way.

#### Step 6b: Per-core trap handling

Each core needs its own `mscratch` (for trap handler scratch space) and
`mtimecmp`.  The RP2350 provides per-core timer compare registers.

#### Step 6c: Spinlocks

The 32 hardware spinlocks in SIO work identically for RISC-V — they're
memory-mapped, not CPU-specific.  The RISC-V A extension (AMO) also
provides `lr.w`/`sc.w` for software spinlocks.

**Deliverables**: Both cores run processes; `core_id_reg` works.

---

## 8. QEMU Testing

QEMU does not currently support the RP2350 in RISC-V mode.  Options:

1. **Hardware-only testing** (Pico 2 board) — simplest, most accurate.
2. **Generic RV32 QEMU target** (`qemu_rv32`) — a separate target using
   `qemu-system-riscv32 -machine virt`.  This would validate the RISC-V
   arch layer independently of RP2350 hardware, similar to how
   `qemu_arm` validates the ARM arch layer independently of RP2040.
3. **Spike ISS** — RISC-V reference ISA simulator, but no peripheral
   emulation.

**Recommendation**: Start with hardware-only (Phase RV-1 through RV-5),
then consider a `qemu_rv32` target later for CI testing.

---

## 9. Effort Estimate (Relative)

| Component | Effort | Notes |
|-----------|--------|-------|
| `arch.h` + `cpu.h` | Small | Straightforward CSR wrappers |
| `boot.S` + `stage1.S` | Small | Simple startup, no complex boot |
| `trap.S` + `switch.S` | **Large** | Core of the port; all exception handling |
| PMP (`mpu.c` RISC-V path) | Medium | Different register model from ARM MPU |
| `proc_setup_stack()` | Medium | New trap frame layout |
| Signal delivery | Medium | Architecture-specific frame manipulation |
| PICOBIN block | Small | One constant change (CPU field) |
| Linker script | Small | Minor adaptation from pico2.ld |
| CMake / build system | Medium | New `riscv.cmake`, toolchain setup |
| User-space recompile | Medium | Cross-compilation for RV32 |

**Total**: Comparable to the m68k port.  The m68k port took ~2 phases
(Phase 5 + Phase X), and RISC-V is a similar scope: new ISA, new
exception model, new register set, but identical peripherals and
memory map.

---

## 10. Dependency Graph

```
RV-1a (arch skeleton: arch.h, cpu.h, boot.S, stage1.S)
  |
  v
RV-1b (target skeleton: pico2rv/, CMakeLists, picobin, linker)
  |
  v
RV-1c (minimal trap handler + timer)
  |
  v
RV-1d (smoke test: UART boot message)
  |
  v
RV-2 (context switch + scheduling)
  |
  v
RV-3 (PMP memory protection)
  |
  v
RV-4 (syscalls + signals)
  |
  v
RV-5 (full test suite)
  |
  +---> RV-6 (dual-core)
```

RV-1 through RV-5 are the core port.  RV-6 is an extension.

---

## 11. Risks and Mitigations

### 11.1 No FPU

The Hazard3 has no floating-point hardware.  All float operations use
soft-float library calls.

**Impact**: Slightly larger code than pico2 ARM (which has hardware FPU).
Not a blocker — PPAP doesn't use float in the kernel.

### 11.2 Trap Handler Complexity

RISC-V requires software to save/restore all registers on every trap
(unlike ARM Cortex-M which auto-pushes 8).  This means:
- More assembly code in `trap.S`/`switch.S`.
- Slightly higher interrupt latency.

**Mitigation**: The m68k port already handles full-register save/restore
(`movem.l %d0-%d7/%a0-%a6`), so we have a proven pattern.  The Hazard3
`Xh3irq` extension may provide faster dispatch for selected interrupts.

### 11.3 Toolchain Availability

RISC-V toolchain (`riscv32-unknown-elf-gcc`) is less commonly
pre-installed than ARM toolchains.

**Mitigation**: Add to `scripts/setup-toolchain.sh`.  Ubuntu/Debian
provide `gcc-riscv64-unknown-elf` which supports `-march=rv32imac`.

### 11.4 PICOBIN CPU Selection

The RP2350 boot ROM uses the PICOBIN block to select ARM or RISC-V
cores.  If the PICOBIN block is malformed, the ROM won't boot.

**Mitigation**: The SDK examples include working RISC-V PICOBIN blocks.
Easy to diagnose — no boot at all means PICOBIN issue.

### 11.5 No QEMU Support

QEMU doesn't emulate RP2350 RISC-V, so all testing must be on hardware
until a `qemu_rv32` target is created.

**Mitigation**: Create `qemu_rv32` target using QEMU's `virt` machine
for CI.  Hardware testing with picoprobe for RP2350-specific validation.

### 11.6 User-Space ABI

RISC-V uses a different syscall ABI (ecall + a7) from ARM (svc + r7)
and m68k (trap #0 + d0).  All user-space code that makes syscalls
needs architecture-conditional assembly.

**Mitigation**: PPAP's syscall wrappers in musl/libc are already
architecture-conditional.  Adding `#elif defined(__riscv)` sections
follows the existing pattern.

---

## 12. Related Documentation

- [docs/proposals/pico2_port.md](pico2_port.md) — Pico 2 ARM port (predecessor)
- [docs/kernel/overview.md](../kernel/overview.md) — PPAP kernel architecture
- [RP2350 Datasheet, Chapter 3.8](https://datasheets.raspberrypi.com/rp2350/rp2350-datasheet.pdf) — Hazard3 RISC-V cores
- [Hazard3 GitHub](https://github.com/Wren6991/Hazard3) — Hazard3 RTL and documentation
- [RISC-V Privileged Spec](https://riscv.org/specifications/privileged-isa/) — M-mode, PMP, trap handling
