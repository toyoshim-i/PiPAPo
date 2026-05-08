# Memory Management

This document describes how PPAP detects, lays out, allocates, and
tracks physical memory across the supported targets.

---

## 1. Detecting Installed Memory

### ARM (RP2040 / QEMU mps2-an500)

Memory size is fixed at compile time.  The RP2040 has 264 KB of on-chip SRAM
at `0x20000000`; no runtime probe is needed.  The exact split is
target-specific:

- `pico1` uses a 48 KB kernel region and a 44-page pool (176 KB)
- `pico1calc` uses a 48 KB kernel region and a 44-page pool (176 KB)
- `pico2` uses a 48 KB kernel region and a 110-page pool (440 KB)

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

### RISC-V (QEMU virt / RP2350)

Memory size is fixed at compile time per target:

- `qemu_rv32`: 1 MB RAM at `0x80800000`, kernel code in ROM at `0x80000000`
- `pico2rv`: 520 KB SRAM at `0x20000000`, kernel in XIP flash at `0x10000000`

---

## 2. Memory Layout

### 2.1 ARM (RP2040)

264 KB total SRAM at `0x20000000 – 0x20041FFF`:

`pico1` and `pico1calc` share the same SRAM split:

```
0x20000000  ┌────────────────────────────┐
            │ Kernel region (48 KB)      │  .data, .bss, kernel globals
            │                            │  fixed MSP slots + boot stack
0x2000C000  ├────────────────────────────┤
            │ Page pool (176 KB)         │  44 pages x 4 KB
            │ User process data, heap,   │  Managed by page_alloc()
            │ stacks                     │
0x20038000  ├────────────────────────────┤
            │ I/O buffer (24 KB)         │  UART, SD card DMA buffers
0x2003E000  ├────────────────────────────┤
            │ DMA / Core 1 (16 KB)       │  Reserved for hardware use
0x20042000  └────────────────────────────┘
```

ARM RP2040 SRAM constants (from the target linker script, identical for
`pico1` / `pico1calc`):

| Symbol             | Value        | Size    |
|--------------------|--------------|---------|
| `SRAM_KERNEL_BASE` | `0x20000000` | 48 KB   |
| `PAGE_POOL_BASE`   | `0x2000C000` | 176 KB  |
| `SRAM_IOBUF_BASE`  | `0x20038000` | 24 KB   |
| `SRAM_DMA_BASE`    | `0x2003E000` | 16 KB   |

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

---

## 3. Allocation Layers

### 3.1 Public API: `mem_region`

All code outside `src/kernel/mm/` allocates through `mem_region_*`:

| API | Returns | Used by |
|-----|---------|---------|
| `mem_region_alloc()` | `proc_image_segment_t` (with `base_page`) | All loaders, syscalls |
| `mem_region_alloc_at()` | `proc_image_segment_t` | `sys_brk`, `sys_mmap2` (MAP_FIXED) |
| `mem_region_free()` | — | OWNED segment cleanup |
| `mem_region_free_tracked_page_id()` | — | `proc_release_tracked_pages` |

### 3.2 Page-Index Wrappers

Code outside `mm/` accesses pages by index.  Most are exposed via
`mod_core` for cross-module use.  For kernel-owned buffer conversion,
use `mem_region_kbuf_to_page()` from `kernel/common/mem_region_kbuf.h`.
`void *` → `(page, off)` conversion is **not** a general-purpose
utility — only `user_to_page` (at the syscall dispatcher) and
`kbuf_to_page` (for kernel metadata buffers) exist.

| Function | Returns | i16-safe? | Use when |
|---|---|---|---|
| `mem_region_page_alloc()` | `page_id_t` | **Yes** | Allocate one page by index |
| `mem_region_page_alloc_contiguous(n)` | `page_id_t` | **Yes** | Allocate n contiguous pages |
| `mem_region_page_free(id)` | — | **Yes** | Free a page by index |
| `mem_region_page_linear(id)` | `uint32_t` | **Yes** | Linear address for arithmetic, comparisons |
| `mem_region_page_to_ptr(id)` | `void *` | **No** | Dereferenceable pointer (32-bit only) |
| `mem_region_ptr_to_page(ptr)` | `page_id_t` | **Yes** | Reverse lookup from pointer to index |
| `mem_region_page_read(id, off, buf, len)` | — | **Yes** | Read page payload (i16-safe) |
| `mem_region_page_write(id, off, buf, len)` | — | **Yes** | Write page payload (i16-safe) |

### 3.3 Internal Backend: `mm_page_*`

The `mm_page_*` functions in `page.h` are internal to `src/kernel/mm/`.
They are the implementation behind the `mem_region_page_*` wrappers.
Code outside `mm/` must not call them directly.

On Xtensa, some memory classes (RAM_TEXT, RAM_DATA, EXT_TEXT,
EXT_RODATA) use ESP-IDF `heap_caps_malloc` arenas instead of the
page pool.  These classes are never used on i16.

### 3.4 Module Interface

The `mod_core` vtable exposes page and region operations:

| vtable field | Implementation |
|---|---|
| `mem_region_alloc` | `mem_region_alloc()` |
| `mem_region_free` | `mem_region_free()` |
| `mem_region_free_bytes` | `mem_region_free_bytes()` |
| `mem_region_total_bytes` | `mem_region_total_bytes()` |
| `mem_region_page_alloc` | `mem_region_page_alloc()` → `mm_page_alloc()` |
| `mem_region_page_free` | `mem_region_page_free()` → `mm_page_free()` |
| `mem_region_page_linear` | `mem_region_page_linear()` → `mm_page_linear()` |
| `mem_region_page_read` | `mem_region_page_read()` → `mm_page_read()` |
| `mem_region_page_write` | `mem_region_page_write()` → `mm_page_write()` |

`mem_region_page_linear` is the sanctioned API for obtaining the 32-bit
linear address of a page (see §9).  Use it for address arithmetic,
range checks, reporting addresses to userspace, and programming hardware.
Do **not** cast the result to `void *` to feed another API — pass
`(page, off)` to the callee instead.

The i16 module loader patches these into cross-segment far-call
entries so modules can allocate and access pages without linking
directly against `page.c`.

---

## 4. Per-Process Memory Tracking

### 4.1 Single Page-Tracking Array: `user_pages[]`

```c
page_id_t user_pages[USER_PAGES_MAX];  /* PAGE_ID_INVALID = empty */
```

This is the single source of truth for all page-backed process memory:
data region, brk growth, mmap allocations, and user stack (RISC-V).

- Set by loaders via `proc_track_page()` / `proc_track_page_range()`.
- Freed on exit via `proc_release_tracked_pages()`.
- Used by `sys_brk` to grow/shrink the data region (low slots, grows up).
- Used by `sys_mmap2` to track anonymous mappings (high slots, grows down).
- Used by `sys_munmap` to find and free mapped pages by address.
- Copied on `vfork` via `proc_copy_page_tracking()`.

**Initialization**: slots are set to `PAGE_ID_INVALID` (0xFFFF) after
`memset` in `proc_init()` and `proc_alloc()`.  This is critical because
0 is a valid page index.

| Target  | `USER_PAGES_MAX` | Max tracked pages |
|---------|-------------------|-------------------|
| ARM     | 64                | 256 KB            |
| m68k    | 512               | 2 MB              |
| RISC-V  | 64                | 256 KB            |

### 4.2 `proc_image_t` — Layout Metadata

```c
typedef struct {
    void *base;
    uint32_t size;
    uint32_t vaddr;
    ppap_mem_class_t mem_class;
    uint32_t flags;
    page_id_t base_page;  /* PAGE_ID_INVALID for non-page-backed */
} proc_image_segment_t;
```

`proc_image_t` stores layout metadata for procfs reporting and runtime
queries (entry point, XIP flags, memory class, size).

Segments with `PROC_IMAGE_SEG_OWNED` are independently allocated and
freed by `image_release_owned_segments()` on exit.  Only text, staged,
and emulator-state segments carry this flag.  Data regions are NOT
OWNED — they are freed via `user_pages[]`.

Non-page-backed segments (XIP ROM text, Xtensa arenas) have
`base_page = PAGE_ID_INVALID`.

### 4.3 Per-Process Memory Overview

Each process owns:

- **Data segment pages** — `user_pages[0..N-1]`, contiguous, allocated by
  `execve()`.  Contains `.data`, `.bss`, GOT (Global Offset Table).
- **Heap pages** — `user_pages[N..]`, contiguous with data pages, allocated
  on demand by `sys_brk()`.
- **mmap pages** — `user_pages[]` high slots (top-down), allocated by
  `sys_mmap2()` for anonymous mappings.
- **Kernel stack page** — `stack_page_id`, one 4 KB page.  On ARM this is
  the PSP stack; on m68k the per-process SSP stack; on RISC-V the
  mscratch-based kernel stack.
- **User stack page** (m68k and RISC-V) — `user_stack_page` (m68k) or a
  dedicated page in `user_pages[]` (RISC-V), one 4 KB page.

All pages come from the same global page pool.

### 4.4 Exit Path

```
sys_exit:
  1. image_release_owned_segments()  <- frees OWNED text/staged segments
  2. proc_release_tracked_pages()    <- frees user_pages[] (data, brk, mmap)
  3. free stack_page_id              <- kernel stack
```

No duplicate-tracking, no overlap check.  Each page has exactly one
owner.

---

## 5. Initial Memory Allocation for a Process

When `execve()` loads an ELF binary (`src/kernel/exec/exec.c`):

1. **Pre-allocate the kernel stack page** — `mem_region_alloc()` returns one
   page.  This is done first to prevent the LIFO free-stack from interfering
   with contiguous allocation below.  On m68k and RISC-V, a separate user
   stack page is also allocated here.

2. **Allocate contiguous data pages** — `mem_region_alloc()` scans the page
   pool from the bottom and allocates N adjacent pages for the data segment
   (`.data` + `.bss` + GOT).  Contiguity is required so that `brk()` can
   later extend the heap by adding the next adjacent page.

3. **Copy and zero segments** — `.data` is copied from the binary image;
   `.bss` is zeroed.

4. **Apply relocations** — GOT entries and relocation tables are patched to
   reflect the actual SRAM addresses.

5. **Track pages** — Data pages are stored in `user_pages[]` via
   `proc_track_page_range()` using `mem_region_ptr_to_page()` to convert
   the allocation pointer to a page index.

6. **Set up brk** — The initial program break is set to the end of the data
   segment, 16-byte aligned:
   ```c
   p->brk_base    = ALIGN_UP(data_end, 16);
   p->brk_current = p->brk_base;
   ```

7. **Build the initial stack frame** — `proc_setup_stack()` writes a synthetic
   exception frame onto the kernel stack page so that the first context switch
   enters the process at the ELF entry point.

---

## 6. How brk Changes Memory Allocation

`sys_brk()` (`src/kernel/syscall/sys_mem.c`) adjusts the program break:

- **Query**: `brk(0)` returns the current break address without changes.
- **Expand**: When `brk(addr)` requests a higher address, the kernel calculates
  how many new pages are needed and allocates them via `mem_region_alloc_at()`
  at the exact addresses following the existing data/heap pages.  Each new page
  is zeroed and tracked in `user_pages[]`.
- **Shrink**: When `brk(addr)` requests a lower address (but not below
  `brk_base`), excess pages are freed via `proc_release_tracked_pages()`.
- **Failure**: If `mem_region_alloc_at()` fails (the target page is already in
  use or the pool is exhausted), or if the request exceeds `USER_PAGES_MAX`
  pages, the break is left unchanged.  The return value is always the
  current break (never a negative errno), matching Linux semantics.  musl
  libc relies on this to detect failure and fall back to `mmap()`.

---

## 7. How the Heap Grows

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
same pool and tracks them in `user_pages[]` (high slots, allocated
top-down to avoid collision with brk growth).

---

## 8. How the Stack Grows (or Cannot Grow)

**Stacks are fixed at 4 KB (one page) and cannot grow.**

- On ARM, the process stack (PSP) is a single pre-allocated page.
- On m68k, the user stack (USP) is a single pre-allocated page, separate
  from the kernel stack.
- On RISC-V, the user stack is a dedicated page in `user_pages[]`, separate
  from the kernel stack (swapped via `mscratch` on trap entry/exit).

There are no guard pages and no automatic stack expansion.  If a process
overflows its stack:

- On ARM, the MPU may catch the access if it falls outside the configured
  region (region 2 covers the process stack page), triggering a MemManage
  fault.
- On m68k, the overflow silently corrupts adjacent memory.
- On RISC-V, PMP is currently configured for full access, so overflow
  silently corrupts adjacent memory (same as m68k).

The kernel stack (MSP on ARM, SSP on m68k, mscratch-based on RISC-V) is also
fixed-size: 4 KB on ARM, 16 KB on m68k, 4 KB on RISC-V.  It is shared by
all interrupt and exception handlers.

---

## 9. Page-Index Conversion Rules

### When to use each function

- **Default to `mem_region_page_linear()`.**  It returns a 32-bit
  linear address that works on every architecture including i16.
  Use it for:
  - Computing offsets and sizes (e.g. `brk` arithmetic).
  - Address-range containment checks (e.g. `proc_page_backed_contains`).
  - Returning addresses to userspace or subsystem bridges.

- **Use `mem_region_page_to_ptr()` only when you need a dereferenceable
  pointer** (e.g. to pass to `memcpy`, `memset`, or to cast to a typed
  pointer for direct access).  This function is unavailable on i16.

- **Use `mem_region_page_read()` / `mem_region_page_write()` to access
  page payloads on i16.**  On i16, `void *` is 16-bit and cannot
  address pages above 64 KB, so there is no dereferenceable pointer.
  These functions handle segment register setup internally and work on
  all platforms (on 32-bit they reduce to `memcpy`).

### Reverse lookup

`mem_region_ptr_to_page(ptr)` converts a `void *` from
`mem_region_alloc()` to a page index.  Returns `PAGE_ID_INVALID`
if the pointer is not in the page pool.

### Single-page I/O contract

The VFS module never advances a `(page, off)` cursor across a page
boundary.  When a syscall receives a user buffer spanning multiple
pages, the **core syscall dispatcher** (`sys_io.c`) splits the request
into per-page chunks and issues one `mod_vfs.fd_read/fd_write` call per
chunk.  VFS `read`/`write` implementations may assume
`off + n ≤ PAGE_SIZE`.

This keeps page-advance arithmetic out of VFS entirely — file-system
drivers handle at most one page's worth of data per call.

---

## 10. Architecture-Specific Notes

### i16 (IBM PC)

- `void *` is 16-bit.  All memory tracking uses `page_id_t` (uint16_t
  index), not pointers.
- All access to user-process pages goes through
  `mem_region_page_read/write`.
- `SS=0` means SP is a 20-bit linear address.  Stack pages must be
  allocated at low addresses (< 64 KB) for SP to fit in 16 bits.
- `proc_image_segment_t.base` pointer is meaningless on i16 for
  page-backed segments; `base_page` is authoritative.

### Xtensa (ESP32-S3)

- Some memory classes use ESP-IDF heap arenas, not the page pool.
- Arena-backed segments have `base_page = PAGE_ID_INVALID` and are
  freed via `mem_region_free()` based on `mem_class`.
- `PROC_IMAGE_SEG_OWNED` on text/staged segments triggers arena-aware
  freeing through `image_release_owned_segments()`.

### ARM / m68k / RISC-V

- `void *` is 32-bit; `mem_region_page_to_ptr()` is available.
- Page tracking is index-based for consistency with i16, but pointers
  are derived via `mem_region_page_to_ptr()` where needed.

---

## 11. Stack Pointer Usage on Interrupts and Syscalls

### 11.1 ARM Cortex-M

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

### 11.2 m68k (Motorola 68000)

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

### 11.3 RISC-V (mscratch stack split)

RISC-V has a single `sp` register with no hardware stack switching.  PPAP
implements a software kernel/user stack split using the `mscratch` CSR:

| Context                | Stack Pointer  | Notes                              |
|------------------------|----------------|------------------------------------|
| User process (U-mode)  | user sp        | Per-process, in `user_pages[]`     |
| Trap entry             | kernel sp      | `csrrw sp, mscratch, sp` swaps     |
| ecall (syscall)        | kernel sp      | User sp saved in trap frame        |
| Timer interrupt         | kernel sp      | Same swap mechanism                |
| Context switch         | kernel sp      | Swaps PCB `sp` + updates mscratch  |

On trap entry:
1. `csrrw sp, mscratch, sp` — atomically swap user sp and kernel sp.
2. Check `mstatus.MPP`: if 0 (U-mode), the swap was correct; if 3 (M-mode),
   it was a nested trap — undo the swap.
3. Save all 31 registers + mepc + mstatus + user_sp into a 144-byte trap
   frame on the kernel stack.

On trap return:
1. If `TF_USER_SP ≠ 0`: returning to U-mode — restore user sp to mscratch,
   restore all registers, `csrrw sp, mscratch, sp` to swap back, `mret`.
2. If `TF_USER_SP = 0`: returning to M-mode (nested) — restore registers,
   deallocate trap frame, `mret`.

### 11.4 Summary

```
ARM:   User code ──SVC──→ [hw saves on PSP] ──→ handler on MSP ──→ [hw restores PSP]
       User code ──IRQ──→ [hw saves on PSP] ──→ handler on MSP ──→ [hw restores PSP]

m68k:  User code ──TRAP──→ [hw saves SR+PC on SSP] ──→ handler on SSP ──→ rte
       User code ──IRQ───→ [hw saves SR+PC on SSP] ──→ handler on SSP ──→ rte

RISCV: User code ──ecall─→ [sw swap sp↔mscratch] ──→ handler on ksp ──→ [sw swap back]
       User code ──IRQ───→ [sw swap sp↔mscratch] ──→ handler on ksp ──→ [sw swap back]
```

ARM separates user and kernel stacks in hardware (PSP vs MSP).  m68k uses
a single supervisor stack with USP only accessible in user mode.  RISC-V
uses a software swap via `mscratch` to achieve the same separation.

---

## 12. Related Documentation

- [Kernel Module System](modules.md) -- module boundary
  and `mem_region` as the public allocation API
- [Intel 8086 Target](../targets/ia16.md) -- i16-specific memory
  model (S5)
- [Design Specification](../spec_v07.md) -- overall memory management
  design
