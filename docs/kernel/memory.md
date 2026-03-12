# Memory Management

This document describes how PPAP detects, lays out, and manages physical memory
across the supported targets.

## 1. Detecting Installed Memory

### ARM (RP2040 / QEMU mps2-an500)

Memory size is fixed at compile time.  The RP2040 has 264 KB of on-chip SRAM
at `0x20000000`; no runtime probe is needed.  The exact split is target-specific:

- `pico1` keeps the original 20 KB kernel region and 51-page pool
- `pico1calc` reserves 24 KB for the kernel and uses a 50-page pool

The remaining SRAM is reserved for kernel data, I/O buffers, and DMA.

### m68k (QEMU virt / X68000)

Memory size is detected at boot by `m68k_probe_ram()` (`src/arch/m68k/probe_ram.S`).
The probe runs in two passes:

1. **Coarse pass** — walk upward from `PAGE_POOL_BASE` in 1 MB steps, writing a
   test pattern and reading it back.  A bus error (vector 2) means the address
   is unmapped; a temporary bus error handler catches this and terminates the
   pass.
2. **Fine pass** — from the last valid 1 MB boundary, walk in `PAGE_SIZE` (4 KB)
   steps to find the exact end of RAM.

The result (bytes of accessible RAM above the page pool) is divided by
`PAGE_SIZE` and capped at `PAGE_COUNT_MAX`.  Targets can set `RAM_END` via
CMake (`-DRAM_END=0xC00000` for X68000, whose VRAM starts at 0xC00000) to
prevent the probe from touching memory-mapped I/O.

`PAGE_COUNT_MAX` is overridable per target via CMake `-DPAGE_COUNT_MAX=...`.
The QEMU m68k target sets it to 3072 (12 MB).

## 2. Memory Layout

### 2.1 ARM (RP2040)

264 KB total SRAM at `0x20000000 – 0x20041FFF`:

`pico1`:

```
0x20000000  ┌────────────────────────────┐
            │ Kernel region (20 KB)      │  .data, .bss, kernel globals
            │                            │  Kernel stack (4 KB, MSP)
0x20005000  ├────────────────────────────┤
            │ Page pool (204 KB)         │  51 pages x 4 KB
            │ User process data, heap,   │  Managed by page_alloc()
            │ stacks                     │
0x20038000  ├────────────────────────────┤
            │ I/O buffer (24 KB)         │  UART, SD card DMA buffers
0x2003E000  ├────────────────────────────┤
            │ DMA / Core 1 (16 KB)       │  Reserved for hardware use
0x20042000  └────────────────────────────┘
```

`pico1calc`:

```
0x20000000  ┌────────────────────────────┐
            │ Kernel region (24 KB)      │  .data, .bss, kernel globals
            │                            │  Kernel stack (4 KB, MSP)
0x20006000  ├────────────────────────────┤
            │ Page pool (200 KB)         │  50 pages x 4 KB
            │ User process data, heap,   │  Managed by page_alloc()
            │ stacks                     │
0x20038000  ├────────────────────────────┤
            │ I/O buffer (24 KB)         │  UART, SD card DMA buffers
0x2003E000  ├────────────────────────────┤
            │ DMA / Core 1 (16 KB)       │  Reserved for hardware use
0x20042000  └────────────────────────────┘
```

Default constants (from `page.h`, as used by `pico1` and `qemu_arm`):

| Symbol             | Value        | Size    |
|--------------------|--------------|---------|
| `SRAM_KERNEL_BASE` | `0x20000000` | 20 KB   |
| `PAGE_POOL_BASE`   | `0x20005000` | 204 KB  |
| `SRAM_IOBUF_BASE`  | `0x20038000` | 24 KB   |
| `SRAM_DMA_BASE`    | `0x2003E000` | 16 KB   |

`pico1calc` overrides the SRAM split in its target CMake configuration:

| Symbol             | Value         | Size    |
|--------------------|---------------|---------|
| `SRAM_KERNEL_BASE` | `0x20000000`  | 24 KB   |
| `PAGE_POOL_BASE`   | `0x20006000`  | 200 KB  |
| `SRAM_IOBUF_BASE`  | `0x20038000`  | 24 KB   |
| `SRAM_DMA_BASE`    | `0x2003E000`  | 16 KB   |

### 2.2 m68k (QEMU virt)

Flat RAM starting at 0, size auto-detected (up to 16 MB):

```
0x00000000  ┌────────────────────────────┐
            │ Vector table (1 KB)        │  256 vectors x 4 bytes
0x00000400  ├────────────────────────────┤
            │ Kernel .text, .rodata      │
            ├────────────────────────────┤
            │ Kernel .data, .bss         │
            ├────────────────────────────┤
            │ Supervisor stack (16 KB)   │  SSP, grows downward
            │ __stack_bottom → __stack_top│
            ├────────────────────────────┤  ← __page_pool_start (linker symbol)
            │ Page pool (remainder)      │  Auto-detected by m68k_probe_ram()
            │ User process data, heap,   │
            │ stacks                     │
            └────────────────────────────┘  ← detected RAM end
```

The linker script places `__page_pool_start` immediately after the supervisor
stack.  Everything from there to the detected RAM end is the page pool.

### 2.3 Per-Process Memory (when multiple processes run)

Each process owns:

- **Data segment pages** — `user_pages[0..N-1]`, contiguous, allocated by
  `do_execve()`.  Contains `.data`, `.bss`, GOT (Global Offset Table).
- **Heap pages** — `user_pages[N..]`, contiguous with data pages, allocated
  on demand by `sys_brk()`.
- **Kernel stack page** — `stack_page`, one 4 KB page.  On ARM this is the
  PSP stack; on m68k this is the per-process SSP stack.
- **User stack page** (m68k only) — `user_stack_page`, one 4 KB page for the
  USP (User Stack Pointer).

All pages come from the same global page pool.  With `PROC_MAX = 8` processes,
a typical ARM layout might look like:

```
Page pool:
  [page 0..2]   Process 1 data + heap
  [page 3]      Process 1 stack (PSP)
  [page 4..5]   Process 2 data + heap
  [page 6]      Process 2 stack (PSP)
  ...
  [page 50]     Free
```

On m68k, each process uses an additional page for the user stack (USP).

## 3. Initial Memory Allocation for a Process

When `do_execve()` loads an ELF binary (`src/kernel/exec/exec.c`):

1. **Pre-allocate the kernel stack page** — `page_alloc()` returns one page.
   This is done first to prevent the LIFO free-stack from interfering with
   contiguous allocation below.  On m68k, a separate user stack page is also
   allocated here.

2. **Allocate contiguous data pages** — `alloc_contiguous(N)` scans the page
   pool from the bottom and allocates N adjacent pages for the data segment
   (`.data` + `.bss` + GOT).  Contiguity is required so that `brk()` can
   later extend the heap by adding the next adjacent page.

3. **Copy and zero segments** — `.data` is copied from the binary image;
   `.bss` is zeroed.

4. **Apply relocations** — GOT entries and relocation tables are patched to
   reflect the actual SRAM addresses.

5. **Set up brk** — The initial program break is set to the end of the data
   segment, 16-byte aligned:
   ```c
   p->brk_base    = ALIGN_UP(data_end, 16);
   p->brk_current = p->brk_base;
   ```

6. **Build the initial stack frame** — `proc_setup_stack()` writes a synthetic
   exception frame onto the kernel stack page so that the first context switch
   enters the process at the ELF entry point.

## 4. How brk Changes Memory Allocation

`sys_brk()` (`src/kernel/syscall/sys_mem.c`) adjusts the program break:

- **Query**: `brk(0)` returns the current break address without changes.
- **Expand**: When `brk(addr)` requests a higher address, the kernel calculates
  how many new pages are needed and allocates them via `page_alloc_at()` at
  the exact addresses following the existing data/heap pages.  Each new page
  is zeroed and stored in `user_pages[]`.
- **Shrink**: When `brk(addr)` requests a lower address (but not below
  `brk_base`), excess pages are freed via `page_free()`.
- **Failure**: If `page_alloc_at()` fails (the target page is already in use
  or the pool is exhausted), or if the request exceeds `USER_PAGES_MAX`
  pages, the break is left unchanged.  The return value is always the
  current break (never a negative errno), matching Linux semantics.  musl
  libc relies on this to detect failure and fall back to `mmap()`.

Limits:

| Target | `USER_PAGES_MAX` | Max data + heap |
|--------|-------------------|-----------------|
| ARM    | 64                | 256 KB          |
| m68k   | 512               | 2 MB            |

## 5. How the Heap Grows

User programs call `malloc()` (provided by musl libc), which internally
calls `brk()` to expand the heap.  The heap grows **upward** from `brk_base`:

```
user_pages[0]  ┌─────────────────┐
               │ .data           │
               │ .bss            │
               │ GOT             │
               ├─────────────────┤ ← brk_base (initial break)
               │ heap (malloc'd) │   grows upward
               │                 │
               ├─────────────────┤ ← brk_current
               │ (unallocated)   │
               └─────────────────┘
```

When `malloc()` needs more memory than available between `brk_current` and the
end of the last allocated page, musl calls `brk()` to extend.  The kernel
allocates the next contiguous page, zeroes it, and advances `brk_current`.

For large allocations, musl falls back to `mmap()`.  PPAP supports
anonymous-only `mmap()` via `sys_mmap2()`, which allocates pages from the
same pool and tracks them in `mmap_regions[]` (up to `MMAP_REGIONS_MAX = 8`
concurrent regions per process).

## 6. How the Stack Grows (or Cannot Grow)

**Stacks are fixed at 4 KB (one page) and cannot grow.**

- On ARM, the process stack (PSP) is a single pre-allocated page.
- On m68k, the user stack (USP) is a single pre-allocated page, separate
  from the kernel stack.

There are no guard pages and no automatic stack expansion.  If a process
overflows its stack:

- On ARM, the MPU may catch the access if it falls outside the configured
  region (region 2 covers the process stack page), triggering a MemManage
  fault.
- On m68k, the overflow silently corrupts adjacent memory.

The kernel stack (MSP on ARM, SSP on m68k) is also fixed-size: 4 KB on ARM,
16 KB on m68k.  It is shared by all interrupt and exception handlers.

## 7. Stack Pointer Usage on Interrupts and Syscalls

### 7.1 ARM Cortex-M

The ARM Cortex-M has two stack pointers with hardware-managed switching:

| Context                | Stack Pointer | Notes                              |
|------------------------|---------------|------------------------------------|
| User process (Thread)  | PSP           | Per-process, loaded from PCB       |
| Exception entry        | MSP           | Hardware auto-switches to MSP      |
| SVC (syscall)          | MSP           | Hardware frame saved on PSP first  |
| SysTick (timer)        | MSP           | Runs on kernel stack               |
| PendSV (context switch)| MSP           | Saves/restores PSP to/from PCB     |

On exception entry, the hardware automatically:
1. Saves 8 registers (r0-r3, r12, lr, pc, xpsr) onto the **current** stack
   (PSP if the process was in Thread mode).
2. Switches to MSP for the handler.
3. On return (`EXC_RETURN = 0xFFFFFFFD`), restores from PSP and switches
   back to Thread mode.

The context switch (PendSV) additionally saves/restores the software frame
(r4-r11) and swaps the PSP value via the PCB's `sp` field.

### 7.2 m68k (Motorola 68000)

The m68k has two stack pointers selected by the supervisor bit in the SR:

| Context                | Stack Pointer | Notes                              |
|------------------------|---------------|------------------------------------|
| User process           | USP           | Per-process, stored in PCB `usp`   |
| Supervisor / kernel    | SSP (A7)      | Per-process kernel stack           |
| TRAP #0 (syscall)      | SSP           | Hardware saves SR+PC (6B) on SSP   |
| TRAP #1 (yield)        | SSP           | Software saves all regs on SSP     |
| Timer ISR (autovector) | SSP           | Hardware saves SR+PC on SSP        |
| Bus/address error      | SSP           | Extended 14B frame on SSP          |

On exception entry, the hardware:
1. Switches to supervisor mode (sets S bit in SR).
2. Saves SR (2 bytes) + PC (4 bytes) = 6 bytes onto SSP.
3. Does **not** automatically save general registers — the software handler
   must save them explicitly.

The context switch saves all 15 general registers (d0-d7, a0-a6) plus the
USP onto the SSP, then stores both SSP and USP in the PCB.  On restore,
both pointers are loaded from the next process's PCB, and `rte` pops
SR+PC to return to user mode.

### 7.3 Summary

```
ARM:  User code ──SVC──→ [hw saves on PSP] ──→ handler on MSP ──→ [hw restores PSP]
      User code ──IRQ──→ [hw saves on PSP] ──→ handler on MSP ──→ [hw restores PSP]

m68k: User code ──TRAP──→ [hw saves SR+PC on SSP] ──→ handler on SSP ──→ rte
      User code ──IRQ───→ [hw saves SR+PC on SSP] ──→ handler on SSP ──→ rte
```

The key difference: ARM separates user and kernel stacks in hardware (PSP vs
MSP), while m68k uses a single supervisor stack for all exception handling,
with USP only accessible in user mode.
