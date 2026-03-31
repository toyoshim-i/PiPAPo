# Memory Management Architecture

This document describes the process-memory tracking architecture
after the page-index refactoring (M-1 through M-6).

---

## 1. Allocation Layers

### 1.1 Public API: `mem_region`

All code outside `src/kernel/mm/` allocates through `mem_region_*`.

| API | Returns | Used by |
|-----|---------|---------|
| `mem_region_alloc()` | `proc_image_segment_t` (with `base_page`) | All loaders, syscalls |
| `mem_region_alloc_at()` | `proc_image_segment_t` | `sys_brk`, `sys_mmap2` (MAP_FIXED) |
| `mem_region_free()` | — | OWNED segment cleanup |
| `mem_region_free_tracked_page_id()` | — | `proc_release_tracked_pages` |

### 1.2 Page-Index Wrappers

Code outside `mm/` accesses pages by index through these wrappers
(declared in `mem_region.h`, implemented in `mem_region.c`):

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

### 1.3 Internal Backend: `mm_page_*`

The `mm_page_*` functions in `page.h` are internal to `src/kernel/mm/`.
They are the implementation behind the `mem_region_page_*` wrappers.
Code outside `mm/` must not call them directly.

On Xtensa, some memory classes (RAM_TEXT, RAM_DATA, EXT_TEXT,
EXT_RODATA) use ESP-IDF `heap_caps_malloc` arenas instead of the
page pool.  These classes are never used on i16.

---

## 2. Per-Process Memory Tracking

### 2.1 Single Page-Tracking Array: `user_pages[]`

```c
page_id_t user_pages[USER_PAGES_MAX];  /* PAGE_ID_INVALID = empty */
```

This is the single source of truth for all page-backed process memory:
data region, brk growth, mmap allocations, and user stack (RISC-V).

- Set by loaders via `proc_track_page()` / `proc_track_page_range()`.
- Freed on exit via `proc_release_tracked_pages()`.
- Used by `sys_brk` to grow/shrink the data region.
- Used by `sys_mmap2` to track anonymous mappings (top-down slot
  allocation avoids collision with brk growth from the bottom).
- Used by `sys_munmap` to find and free mapped pages by address.
- Copied on `vfork` via `proc_copy_page_tracking()`.

**Initialization**: slots are set to `PAGE_ID_INVALID` (0xFFFF)
after `memset` in `proc_init()` and `proc_alloc()`.  This is
critical because 0 is a valid page index.

### 2.2 `proc_image_t` — Layout Metadata

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

`proc_image_t` stores layout metadata for procfs reporting and
runtime queries (entry point, XIP flags, memory class, size).

Segments with `PROC_IMAGE_SEG_OWNED` are independently allocated
and freed by `image_release_owned_segments()` on exit.  Only
text, staged, and emulator-state segments carry this flag.  Data
regions are NOT OWNED — they are freed via `user_pages[]`.

Non-page-backed segments (XIP ROM text, Xtensa arenas) have
`base_page = PAGE_ID_INVALID`.

### 2.3 Exit Path

```
sys_exit:
  1. image_release_owned_segments()  <- frees OWNED text/staged segments
  2. proc_release_tracked_pages()    <- frees user_pages[] (data, brk, mmap)
  3. free stack_page_id              <- kernel stack
```

No duplicate-tracking, no overlap check.  Each page has exactly one
owner.

---

## 3. Page-Index Conversion Rules

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

---

## 4. Architecture-Specific Notes

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

## 5. Module Interface

The `mod_core` vtable exposes page operations as `mem_region_page_*`:

| vtable field | Implementation |
|---|---|
| `mem_region_page_alloc` | `mem_region_page_alloc()` → `mm_page_alloc()` |
| `mem_region_page_free` | `mem_region_page_free()` → `mm_page_free()` |
| `mem_region_page_read` | `mem_region_page_read()` → `mm_page_read()` |
| `mem_region_page_write` | `mem_region_page_write()` → `mm_page_write()` |

The i16 module loader patches these into cross-segment far-call
entries so modules can allocate and access pages without linking
directly against `page.c`.

---

## 6. Related Documentation

- [Kernel Module System](../kernel/kernel_modules.md) -- module boundary
  and `mem_region` as the public allocation API
- [IBM PC Port Plan](pc_port.md) -- i16-specific memory model (S3.4)
- [Design Specification](../spec_v07.md) -- overall memory management
  design
