# Raspberry Pi Zero Port — Target Plan

Porting PPAP to the Raspberry Pi Zero (BCM2835, ARM1176JZF-S). This is a
major architectural leap: the ARM1176 has a **full MMU**, transforming PPAP
from a micro OS into a modern Unix-like OS capable of proper virtual memory,
fork() with copy-on-write, demand paging, and process isolation.

Initial target is QEMU (`raspi0` / `versatilepb`), then real Pi Zero hardware.

---

## 1. Architecture Comparison

| Aspect | Cortex-M0+ (RP2040) | ARM1176JZF-S (Pi Zero) |
|--------|---------------------|------------------------|
| Architecture | ARMv6-M | ARMv6Z (full ARM) |
| ISA | Thumb-1 only (16-bit) | ARM + Thumb + Thumb-2 |
| Word size | 32-bit | 32-bit |
| Clock | 133 MHz | 1 GHz |
| RAM | 264 KB SRAM | 512 MB SDRAM |
| Flash/Storage | 2-16 MB QSPI (XIP) | SD card (no XIP) |
| MMU | **None** (4-region MPU) | **Full MMU** (ARMv6 page tables) |
| FPU | None | VFPv2 (single+double precision) |
| Caches | 16 KB XIP cache | 16 KB I-cache, 16 KB D-cache, L2 cache |
| Exception model | Cortex-M NVIC (handler/thread) | ARM mode exceptions (7 modes) |
| Privilege | Handler/Thread + MSP/PSP | 7 processor modes (USR/SVC/IRQ/FIQ/ABT/UND/SYS) |
| Syscall | `SVC` instruction | `SWI` instruction (same encoding, different name) |
| Context switch | PendSV (deferred, lowest priority) | Software-triggered via IRQ or SWI return path |
| Interrupt controller | NVIC (nested, prioritized) | BCM2835 custom IC (no nesting by default) |
| Timer | SysTick (24-bit) | ARM Timer + System Timer (64-bit) |
| Cores | Dual Cortex-M0+ | Single ARM1176 + VideoCore IV GPU |
| Boot | ROM → boot2 → stage1 → kernel (XIP) | GPU → bootcode.bin → start.elf → kernel.img @ 0x8000 |
| Endianness | Little-endian | Little-endian (configurable, but LE in practice) |
| Address space | 32-bit flat, single space | 32-bit virtual, per-process page tables |
| PIC convention | r9 = GOT base | Standard ARM PIC (GOT via PC-relative) |
| ELF flags | EF_ARM_EABI_VER5, Thumb | EF_ARM_EABI_VER5, ARM/Thumb interwork |

---

## 2. What the MMU Changes

This is the defining feature of the Pi Zero port. The MMU enables capabilities
that were impossible on the RP2040:

### 2.1 Virtual Address Spaces

Each process gets its own virtual address space via per-process page tables.
User processes all see the same virtual addresses (e.g., text at 0x00010000,
stack at 0x7FFF0000) but map to different physical pages.

**Proposed virtual address layout (user/kernel split at 0xC0000000):**

| Virtual address | Size | Contents |
|---|---|---|
| 0x00000000 - 0x00000FFF | 4 KB | Null page (unmapped, catches NULL derefs) |
| 0x00010000 - 0x0FFFFFFF | ~256 MB | User text + data + heap (grows up) |
| 0x70000000 - 0x7FFFFFFF | 256 MB | User stack (grows down) + mmap region |
| 0xC0000000 - 0xCFFFFFFF | 256 MB | Kernel direct-map (phys 0 → virt 0xC0000000) |
| 0xD0000000 - 0xDFFFFFFF | 256 MB | Kernel heap, page tables, etc. |
| 0xF0000000 - 0xFFFFFFFF | 256 MB | Peripheral MMIO (0x20000000 phys) |

The kernel is mapped into every process's address space (high addresses),
so syscalls don't require a page table switch — only a mode change
(USR → SVC). This is the classic Linux 3G/1G split approach.

### 2.2 Real fork() with Copy-on-Write

The RP2040 port uses `vfork()` because fork() without an MMU would require
physically copying the entire address space. With the MMU:

1. `fork()` duplicates the page table, marking all pages **read-only**
2. Both parent and child share the same physical pages
3. On first write, a **data abort** (page fault) fires
4. The fault handler allocates a new physical page, copies the data,
   updates the faulting process's page table entry, and resumes
5. Only modified pages are ever copied — huge memory savings

This is standard COW fork, the same mechanism used by Linux, BSD, etc.

### 2.3 Demand Paging

With 512 MB RAM, PPAP can support many more processes. But more importantly,
the MMU enables **demand paging**: pages can be loaded from storage (SD card)
on first access rather than at exec() time.

- Page fault on unmapped page → check if it belongs to a file mapping
- Load the page from SD, map it, resume the process
- Under memory pressure, evict clean pages (just unmap) or dirty pages
  (write back to swap, then unmap)

### 2.4 Proper mmap()

With virtual memory, `mmap()` becomes a real operation:
- `MAP_ANONYMOUS`: allocate virtual pages, back with physical on demand
- `MAP_PRIVATE` file mapping: map file pages COW
- `MAP_SHARED` file mapping: share physical pages between processes
- `PROT_READ | PROT_WRITE | PROT_EXEC`: per-page permissions via PTE bits

### 2.5 Full Process Isolation

Every process has its own page table. A user process **cannot** access
another process's memory or kernel memory — the MMU enforces this in
hardware. This is a fundamental security improvement over the MPU-based
RP2040 port.

---

## 3. ARM1176 MMU Architecture

### 3.1 Page Table Format (ARMv6)

Two-level page tables:

**Level 1 (L1) — Translation Table:**
- 4096 entries × 4 bytes = **16 KB** per process
- Each entry covers 1 MB of virtual address space
- Entry types: Fault (unmapped), Section (1 MB mapping), Page Table (pointer to L2)

**Level 2 (L2) — Page Table:**
- 256 entries × 4 bytes = **1 KB** per L2 table
- Each entry covers 4 KB (small page) or 64 KB (large page)
- Entry types: Fault, Large Page (64 KB), Small Page (4 KB), Extended Small Page

**Small page (4 KB) descriptor bits:**
```
[31:12] Physical page base address
[11:10] (should be zero)
[9]     AP[2] (access permission extension)
[8:6]   TEX[2:0] (memory type)
[5:4]   AP[1:0] (access permissions)
[3]     C (cacheable)
[2]     B (bufferable)
[1]     1 (small page indicator)
[0]     XN (execute never)
```

Access permissions (AP[2:0]):
- 0b001: SVC R/W, USR no access (kernel-only)
- 0b011: SVC R/W, USR R/W
- 0b101: SVC R/O, USR no access
- 0b111: SVC R/O, USR R/O (read-only, used for COW)

### 3.2 TLB Management

The ARM1176 has separate I-TLB and D-TLB. On context switch:
- Write TTBR0 (Translation Table Base Register 0) with new process's L1 base
- Flush TLB (or use ASID to avoid full flush — ARM1176 supports 256 ASIDs)

ASID (Address Space Identifier):
- Each TLB entry is tagged with an 8-bit ASID
- CONTEXTIDR register holds the current ASID
- On context switch: update CONTEXTIDR + TTBR0, no TLB flush needed
  (unless ASIDs are exhausted)

### 3.3 Domain Access Control

ARMv6 supports 16 domains. Each L1 entry belongs to a domain.
Domain access is checked before page permissions:
- **No access**: any access faults
- **Client**: page permission bits checked
- **Manager**: no permission check (full access)

Simplest approach: use 2 domains:
- Domain 0 = client (user pages — permission bits enforced)
- Domain 1 = manager (kernel pages — unrestricted access)

---

## 4. ARM-Specific Code Changes

### 4.1 Same Architecture Family, Different Profile

The Pi Zero uses the same ARM instruction set family as the RP2040 but a
completely different processor profile:

| Aspect | Cortex-M0+ (M-profile) | ARM1176 (A-profile) |
|---|---|---|
| Exception entry | Auto-push {r0-r3,r12,lr,pc,xpsr} | Save CPSR→SPSR, LR=return addr, mode switch |
| Exception return | `bx lr` with EXC_RETURN | `movs pc, lr` or `subs pc, lr, #4` |
| IRQ disable | `cpsid i` / `cpsie i` | Same (also available: `msr cpsr_c, ...`) |
| Syscall | `svc #N` → SVCall vector | `swi #N` → SWI vector at 0x08 |
| Stack pointer | MSP/PSP via CONTROL reg | r13 banked per mode (USP, SSP, etc.) |
| Vector table | Pointers at VTOR address | Branch instructions at 0x0 or 0xFFFF0000 |
| Context switch | PendSV (deferred exception) | Manual in IRQ handler or SWI return |

### 4.2 Exception Vector Table

ARM1176 uses **branch instructions** (not pointers) at fixed addresses:

```asm
.section .vectors, "ax"
_vectors:
    ldr pc, =reset_handler      @ 0x00: Reset
    ldr pc, =undefined_handler  @ 0x04: Undefined instruction
    ldr pc, =swi_handler        @ 0x08: SWI (syscall)
    ldr pc, =prefetch_handler   @ 0x0C: Prefetch abort
    ldr pc, =data_abort_handler @ 0x10: Data abort (page fault!)
    nop                         @ 0x14: Reserved
    ldr pc, =irq_handler        @ 0x18: IRQ
    ldr pc, =fiq_handler        @ 0x1C: FIQ
```

Key difference: **Data Abort** (0x10) is the page fault handler — this is
the heart of the virtual memory system. Every COW fault, demand page load,
and access violation flows through here.

### 4.3 Processor Modes and Stacks

The ARM1176 has 7 modes, each with banked r13 (SP) and r14 (LR):

| Mode | Purpose | Banked registers |
|---|---|---|
| USR (User) | User processes | r13_usr, r14_usr |
| SVC (Supervisor) | Kernel, SWI handler | r13_svc, r14_svc, SPSR_svc |
| IRQ | Interrupt handler | r13_irq, r14_irq, SPSR_irq |
| FIQ | Fast interrupt | r8_fiq-r12_fiq, r13_fiq, r14_fiq, SPSR_fiq |
| ABT (Abort) | Data/prefetch abort | r13_abt, r14_abt, SPSR_abt |
| UND (Undefined) | Undefined instruction | r13_und, r14_und, SPSR_und |
| SYS (System) | Privileged, shares USR regs | Same as USR (r13_usr, r14_usr) |

Each mode needs its own stack. At boot, set up:
```asm
    msr cpsr_c, #0xD2    @ IRQ mode, IRQs disabled
    ldr sp, =irq_stack_top
    msr cpsr_c, #0xD7    @ ABT mode
    ldr sp, =abt_stack_top
    msr cpsr_c, #0xDB    @ UND mode
    ldr sp, =und_stack_top
    msr cpsr_c, #0xD3    @ SVC mode
    ldr sp, =svc_stack_top
```

### 4.4 Context Switch

No PendSV on ARM1176. Context switch options:

**Recommended: switch on SWI return and IRQ return.**

Timer IRQ handler (preemption):
```asm
irq_handler:
    sub   lr, lr, #4          @ adjust return address
    srsdb sp!, #0x13          @ save {LR_irq, SPSR_irq} to SVC stack
    cps   #0x13               @ switch to SVC mode
    push  {r0-r12, lr}        @ save caller-saved + lr on SVC stack
    bl    timer_handler        @ C handler: acknowledge timer, call sched_tick
    @ check if context switch needed
    bl    sched_should_switch
    cmp   r0, #0
    bne   do_context_switch
    pop   {r0-r12, lr}
    rfeia sp!                  @ return from exception (restore CPSR + PC)

do_context_switch:
    @ save remaining state to current PCB
    @ load next PCB's page table (TTBR0), ASID, stack, registers
    @ restore and rfeia
```

### 4.5 Syscall Entry (SWI)

```asm
swi_handler:
    srsdb sp!, #0x13          @ save {LR_svc, SPSR_svc} to SVC stack
    push  {r0-r12, lr}
    mov   r0, r7              @ syscall number (ARM EABI: same as Cortex-M)
    mov   r1, sp              @ pointer to saved registers (args in r0-r5)
    bl    syscall_dispatch
    str   r0, [sp, #0]        @ store return value in saved r0
    pop   {r0-r12, lr}
    rfeia sp!                  @ return to user mode
```

The syscall ABI is identical to the RP2040 port: r7=syscall number,
r0-r5=arguments, return in r0. musl libc uses the same convention on
all ARM Linux targets.

### 4.6 Page Fault Handler (Data Abort)

This is **new** — the RP2040 has no equivalent:

```c
void data_abort_handler(uint32_t fault_addr, uint32_t fsr, uint32_t pc) {
    uint32_t fault_type = fsr & 0xF;

    if (fault_type == 0x7) {
        /* Translation fault (page not mapped) */
        /* → demand paging: load page from file/swap, map it, resume */
    } else if (fault_type == 0xF) {
        /* Permission fault (write to read-only page) */
        /* → COW: copy page, make writable, resume */
    } else {
        /* Unexpected fault → SIGSEGV to process */
    }
}
```

---

## 5. Boot Sequence

### 5.1 GPU Bootloader (Broadcom, closed-source)

The BCM2835 boot is GPU-first:

1. **GPU ROM** boots from on-chip ROM
2. GPU reads **bootcode.bin** from SD card FAT32 partition
3. bootcode.bin loads **start.elf** (GPU firmware)
4. start.elf reads **config.txt** for configuration
5. start.elf loads **kernel.img** to physical address **0x8000**
6. GPU releases ARM core with PC = 0x8000

PPAP provides `kernel.img` (our kernel binary). The GPU bootloader files
(`bootcode.bin`, `start.elf`, `fixup.dat`) come from the Raspberry Pi
firmware repository.

### 5.2 SD Card FAT32 Partition Layout

```
SD Card (FAT32)
├── bootcode.bin        # GPU stage 1 (from RPi firmware)
├── start.elf           # GPU firmware (from RPi firmware)
├── fixup.dat           # GPU memory split config (from RPi firmware)
├── config.txt          # Boot configuration (arm_freq, kernel name, etc.)
├── kernel.img          # PPAP kernel (our binary, loaded to 0x8000)
├── romfs.img           # romfs image (loaded by kernel at boot)
├── ppap_usr.img        # UFS image → mounted at /usr
├── ppap_home.img       # UFS image → mounted at /home
├── ppap_var.img        # UFS image → mounted at /var
└── cmdline.txt         # Kernel command line (optional)
```

### 5.3 PPAP Kernel Boot (from 0x8000)

```
1. _start (0x8000): set up exception mode stacks, jump to reset_handler
2. reset_handler: copy .data, zero .bss, set up initial page table
3. Enable MMU: identity-map first 16 MB + kernel high map at 0xC0000000
4. Jump to virtual address (0xC000xxxx) — now running in virtual memory
5. kmain():
   a. target_early_init() — UART console (PL011 at 0x20201000)
   b. Page allocator init (thousands of pages from 512 MB)
   c. MMU: set up kernel page tables for all physical memory
   d. Mount romfs (loaded from SD card into RAM, or read on demand)
   e. target_late_init() — SD card, interrupts, timer
   f. Mount remaining filesystems (VFAT on SD, UFS via loopback)
   g. execve("/sbin/init") as PID 1
```

---

## 6. BCM2835 Hardware

### 6.1 Peripheral Base Address

All BCM2835 peripherals are at physical **0x20000000** (bus address
0x7E000000). The BCM2835 datasheet uses bus addresses; subtract
0x5E000000 to get physical addresses.

### 6.2 Key Peripherals

| Peripheral | Physical address | Purpose |
|---|---|---|
| System Timer | 0x20003000 | 64-bit free-running counter + 4 compare channels |
| Interrupt Controller | 0x2000B200 | Custom BCM2835 IC (not GIC) |
| ARM Timer | 0x2000B400 | SP804-style timer (from ARM VIC) |
| GPU Mailbox | 0x2000B880 | ARM↔GPU communication (framebuffer, clock, etc.) |
| UART0 (PL011) | 0x20201000 | Full UART (same IP as RP2040!) |
| UART1 (mini) | 0x20215000 | Mini UART (simpler, often used as default) |
| SPI0 | 0x20204000 | SPI master |
| I2C0/1 | 0x20205000/0x20804000 | I2C/BSC |
| GPIO | 0x20200000 | 54 GPIO pins, alt functions |
| EMMC (SD) | 0x20300000 | SD card controller (SDHCI-compatible) |
| USB | 0x20980000 | DWC2 OTG USB controller |
| DMA | 0x20007000 | 16 DMA channels |

### 6.3 Interrupt Controller

The BCM2835 has a custom interrupt controller (NOT ARM GIC):
- 3 pending registers: `IRQ_basic_pending`, `IRQ_pending_1`, `IRQ_pending_2`
- 3 enable registers + 3 disable registers
- 72 interrupt sources total (GPU + ARM)
- No priority levels, no nesting by default
- IRQ and FIQ supported (FIQ can be routed to one source)

Key IRQ sources:
- System Timer match 1, 3 (channels 0, 2 reserved by GPU)
- UART0
- EMMC (SD card)
- USB
- ARM Timer

### 6.4 PL011 UART

The BCM2835's PL011 UART (0x20201000) is the same PrimeCell IP used in the
RP2040. The existing `uart.c` driver can be adapted with minimal changes:
- Different base address (0x20201000 vs 0x40034000)
- Different clock source (configured via GPU mailbox, typically 48 MHz)
- GPIO alt function setup via BCM2835 GPIO registers (not RP2040 IO_BANK0)

### 6.5 SD Card (EMMC)

The BCM2835 has an SDHCI-compatible EMMC controller at 0x20300000.
This is much faster than SPI-mode SD (used on PicoCalc):
- 4-bit parallel data bus
- DMA capable
- SDHCI standard register interface
- Supports SD, SDHC, SDXC

---

## 7. Memory Architecture (512 MB)

### 7.1 Physical Memory Map

| Physical address | Size | Contents |
|---|---|---|
| 0x00000000 | 0x8000 | ARM vector table + GPU reserved |
| 0x00008000 | ~1 MB | Kernel .text, .rodata, .data, .bss |
| 0x00108000 | ~2 MB | Kernel page tables (L1 + L2 pool) |
| 0x00308000 | ~4 MB | romfs image (loaded from SD at boot) |
| 0x00708000 | ~440 MB | Page pool (user processes, file cache, etc.) |
| 0x1C000000 | 64 MB | GPU memory (configured via config.txt) |
| 0x20000000 | 16 MB | Peripheral MMIO |

GPU memory split is configurable via `gpu_mem=64` in config.txt (default).
ARM gets 512 - 64 = 448 MB.

### 7.2 Page Allocator Redesign

With 448 MB of RAM and 4 KB pages, the page pool has **~114,000 pages**.
The RP2040's 51-page free-stack is insufficient. Options:

- **Bitmap allocator**: 114,000 bits = ~14 KB bitmap. O(n) worst case but
  simple and cache-friendly.
- **Buddy allocator**: standard Linux approach. O(log n) alloc/free,
  supports multi-page allocations efficiently. More complex.
- **Free list with zones**: simple linked list, but add zones for DMA
  vs normal memory.

**Recommendation**: Start with bitmap (simplest), upgrade to buddy if
multi-page allocation patterns demand it.

### 7.3 Page Table Memory

Each process needs a 16 KB L1 table + L2 tables on demand (1 KB each).
A process mapping 16 MB of virtual space needs ~16 L2 tables = 16 KB.
Total per process: ~32 KB. With 64 processes: ~2 MB. Easily fits.

---

## 8. Kernel Subsystem Impact

### 8.1 What Changes Significantly

| Subsystem | RP2040 | Pi Zero | Impact |
|---|---|---|---|
| Memory management | 51-page free-stack, MPU | Bitmap/buddy allocator, MMU page tables | **Major rewrite** |
| Process model | vfork only, 8 max | Real fork + COW, 64+ max | **Major rewrite** |
| exec | XIP from flash, GOT reloc | Load to RAM, standard ELF reloc | Moderate rewrite |
| Context switch | PendSV, PSP/MSP swap | IRQ/SWI return path, TTBR0 swap | **Full rewrite** (asm) |
| Syscall entry | SVC → NVIC SVCall vector | SWI → vector at 0x08 | Full rewrite (asm) |
| Signal delivery | Modify PSP exception frame | Modify user stack directly | Moderate rewrite |
| mmap | Anonymous only, page_alloc | Full mmap (file, anon, shared, COW) | **Major new code** |
| Scheduler | Same | Mostly same (C code portable) | Minor changes |
| Spinlock | RP2040 SIO hardware | IRQ disable only (single core) | Simplify |

### 8.2 What Stays the Same

| Subsystem | Notes |
|---|---|
| VFS layer | Unchanged |
| All filesystem drivers | Unchanged (romfs, VFAT, UFS, devfs, procfs, tmpfs) |
| File descriptor layer | Unchanged |
| TTY subsystem | Unchanged (different UART driver, same TTY discipline) |
| Pipe | Unchanged |
| Syscall dispatch (C) | Unchanged (same syscall numbers, same C dispatcher) |
| Signal handling (C) | Unchanged (delivery mechanism needs arch hooks) |
| Block device layer | Unchanged (different SD driver, same blkdev API) |
| klog | Unchanged |
| Endianness | Same (both little-endian) |

### 8.3 New Subsystems

| Subsystem | Description |
|---|---|
| MMU driver | Page table create/destroy, map/unmap, TLB flush, ASID management |
| Page fault handler | Data abort → COW, demand paging, SIGSEGV delivery |
| Physical page allocator | Bitmap or buddy for ~114,000 pages |
| Kernel virtual memory | vmalloc, ioremap for peripheral mapping |
| File page cache | Cache file data in RAM pages (huge performance win with 512 MB) |
| EMMC/SD driver | SDHCI-compatible, replaces SPI-mode SD driver |
| GPU mailbox driver | ARM↔VideoCore communication (clock config, framebuffer) |
| Framebuffer | HDMI output via GPU mailbox (allocate FB, set resolution) |
| USB (future) | DWC2 OTG for keyboard, storage, networking |

---

## 9. Build Targets

| Target | Board | Description |
|---|---|---|
| `ppap_qemu_a6` | QEMU `raspi0` or `versatilepb` | Emulated ARM1176 for testing |
| `ppap_pizero` | Raspberry Pi Zero / Zero W | Real hardware target |

### 9.1 QEMU Target

QEMU's `raspi0` machine emulates the BCM2835 faithfully:
```sh
qemu-system-arm -M raspi0 -kernel kernel.img -serial stdio -dtb bcm2835-rpi-zero.dtb
```

Alternatively, `versatilepb` with `-cpu arm1176` is simpler (standard
PL011 UART, PL190 VIC) for early bringup, then switch to `raspi0` for
BCM2835 peripheral testing.

### 9.2 Toolchain

```
arm-none-eabi-gcc          # same toolchain as RP2040 (different -mcpu flag)
  -mcpu=arm1176jzf-s       # target the specific core
  -mfpu=vfp                # enable VFP
  -mfloat-abi=hard         # hardware floating point
  -marm                    # generate ARM (not Thumb) code for kernel
```

User-space can use Thumb-2 for code density, but the kernel should use
ARM mode for exception handlers (ARM1176 enters exceptions in ARM mode).

---

## 10. Implementation Plan

### Phase A — Architecture Abstraction

Same as the m68k port — refactor ARM code into `src/arch/arm_m/` (M-profile)
before adding `src/arch/arm_a/` (A-profile). This phase is shared with the
m68k porting effort.

### Phase B — QEMU Bringup (ARM1176)

1. Write `src/arch/arm_a/boot.S` — exception vectors (branch table at 0x0)
2. Write `src/arch/arm_a/start.S` — mode stacks, MMU early init
3. Write `src/arch/arm_a/cpu.h` — CP15 registers, TTBR, DACR, DFSR/IFSR
4. Identity-map first 16 MB + kernel high map, enable MMU
5. Jump to virtual kernel, init PL011 UART
6. Boot to `kmain()` with UART output: "PicoPiAndPortable booting... (armv6)"

### Phase C — MMU and Virtual Memory

1. Implement L1/L2 page table create/destroy/map/unmap
2. Physical page allocator (bitmap, 512 MB)
3. Kernel page table: direct-map all physical RAM at 0xC0000000
4. Per-process page tables: allocate on proc_alloc(), free on proc_free()
5. Context switch: save/restore r4-r14, update TTBR0 + ASID
6. Data abort handler: dispatch to COW or demand page or SIGSEGV

### Phase D — Process Model

1. Real `fork()` with COW (mark PTEs read-only, fault on write)
2. `exec()` — load ELF to user virtual addresses (no XIP, full RAM load)
3. `_exit()` — free all process pages and page tables
4. `waitpid()` — same as RP2040 (pure C, unchanged)
5. `brk()` / `mmap()` — allocate virtual pages, back with physical on fault
6. Preemptive scheduling via ARM Timer IRQ

### Phase E — User Space

1. Port musl libc for armv6 (musl supports armv6 out of the box)
2. Build busybox for armv6 (static, musl)
3. Interactive shell on QEMU
4. Framebuffer console via GPU mailbox (HDMI output)

### Phase F — Pi Zero Hardware

1. EMMC/SD driver (SDHCI) — replaces SPI-mode SD
2. GPU mailbox driver — clock configuration, framebuffer allocation
3. HDMI framebuffer console
4. USB driver (DWC2 OTG) — keyboard input (stretch goal)
5. Boot from SD card on real Pi Zero hardware

---

## 11. Comparison: Micro OS vs Modern OS

| Feature | RP2040 (micro OS) | Pi Zero (modern OS) |
|---|---|---|
| Processes | 8 max, vfork only | 64+, real fork + COW |
| Memory per process | 128 KB max (4 KB stack + data pages) | Up to ~400 MB virtual |
| Memory protection | MPU (4 regions, coarse) | MMU (per-page, full isolation) |
| Address space | Flat physical | Per-process virtual |
| Page faults | HardFault → kill process | Data abort → COW / demand page |
| File cache | None (direct SD I/O) | Page cache in RAM (huge speedup) |
| Code execution | XIP from flash (fast, zero RAM) | Load to RAM (512 MB makes this fine) |
| Swap | Manual page-out to SD | Standard page-out via MMU |
| Dynamic linking | Not supported | Possible (full mmap + GOT/PLT) |
| Multi-user | Single-user (no isolation) | Full multi-user (MMU-enforced) |
| Networking | None | Possible via USB (Wi-Fi on Zero W) |
| Display | PicoCalc LCD (320×320) | HDMI (up to 1080p via GPU) |

---

## 12. Risk Assessment

| Risk | Impact | Mitigation |
|---|---|---|
| MMU complexity | Longest development phase | Incremental: section-mapped first, then 4 KB pages |
| Page table bugs | Kernel crashes, data corruption | Heavy use of QEMU + GDB; test COW with fork-heavy workloads |
| BCM2835 undocumented quirks | Stalled bringup | Reference Linux, Circle OS, and RPi bare-metal projects |
| USB driver complexity | No keyboard on real hardware | Use UART console first; USB is a Phase F stretch goal |
| GPU mailbox protocol | No HDMI output | Reference `raspberrypi/firmware` wiki; UART console as fallback |
| Scope creep (full Linux clone) | Never finishes | Keep PPAP philosophy: POSIX subset, busybox, correctness first |

---

## 13. Open Questions

1. **QEMU machine**: `raspi0` vs `versatilepb -cpu arm1176`? The former is
   more realistic but has BCM2835-specific peripherals; the latter has
   standard ARM peripherals (PL190 VIC, SP804 timer) that are easier
   to start with.

2. **Kernel/user split**: 3G/1G (0xC0000000) is standard Linux, but with
   512 MB physical RAM, a 2G/2G split wastes less virtual space. 3G/1G
   is simpler and battle-tested.

3. **Page table allocator**: where do L2 page tables come from? Slab
   allocator for 1 KB L2 tables, or just allocate full 4 KB pages and
   pack 4 L2 tables per page?

4. **File page cache**: implement as part of the VFS vnode (each vnode
   holds a radix tree of cached pages)? Or a global page cache indexed
   by (device, block)?

5. **Pi Zero 2 W**: uses BCM2710 (quad-core Cortex-A53, ARMv8-A, 64-bit).
   Different enough to be a separate port. Focus on Pi Zero (v1) first
   for the ARMv6 MMU milestone.

6. **Shared arch code with RP2040**: both are ARM, but M-profile vs
   A-profile are so different that sharing assembly is impractical.
   The C-level syscall dispatcher, VFS, and filesystem code are shared.
   The arch split should be `arm_m` (Cortex-M) vs `arm_a` (ARM11/A-class).
