# Memory Management Refactoring Plan

This document describes the current process-memory tracking architecture,
its problems (especially on i16), and a step-by-step migration plan to
unify it around page indices.

---

## 1. Current Architecture

### 1.1 Two Allocation APIs

The kernel has two allocation layers that partially overlap:

| API | Returns | Backend | Used by |
|-----|---------|---------|---------|
| `page_alloc()` / `page_alloc_contiguous()` | `void *` | Free-stack in `page.c` | `mem_region.c` (non-Xtensa) |
| `mm_page_alloc()` / `mm_page_alloc_contiguous()` | `page_id_t` (uint16_t index) | Same free-stack, thin wrapper | `elf16_loader.c` (i16) |
| `mem_region_alloc()` | `proc_image_segment_t` (contains `void *base`) | `page_alloc` on most targets; ESP-IDF heap arenas on Xtensa | All loaders except elf16 |

On non-Xtensa targets, `mem_region_alloc` is just `page_alloc` wrapped in
a `proc_image_segment_t`.  On Xtensa, some memory classes (RAM_TEXT,
RAM_DATA, EXT_TEXT, EXT_RODATA) use ESP-IDF `heap_caps_malloc` arenas
instead of the page pool.

The `mm_page_*` API was added for i16 where `void *` is 16 bits and
cannot represent page-pool addresses above 64 KB.  It returns a
`page_id_t` index and provides `mm_page_read()`/`mm_page_write()` for
cross-segment data access.

### 1.2 Three Per-Process Memory Tracking Structures

Each process (pcb_t) tracks its allocated memory in three overlapping
structures:

#### (a) `user_pages[USER_PAGES_MAX]` (`void *`)

Per-page tracking array.  Each slot holds a pointer to one 4 KB page
in the page pool (or NULL if unused).

- Set by loaders via `proc_track_page()` / `proc_track_page_range()`.
- Freed on exit via `proc_release_tracked_pages()` which calls
  `mem_region_free_tracked_page()` -> `page_free()`.
- Used by `sys_brk` to grow/shrink the data region.
- Copied on `vfork` via `proc_copy_page_tracking()`.
- 16+ helper functions in `proc.c` (first/last base, count,
  contains, copy, release, etc.).

**Problem on i16**: `void *` is 16-bit.  Pages in the pool (above kernel
BSS) have linear addresses > 0xFFFF.  Storing them in `void *` truncates
the address, and `page_free()` receives a wrong pointer -> double-free
or memory corruption.

#### (b) `mmap_regions[MMAP_REGIONS_MAX]`

```c
struct {
    void *addr;       /* base address */
    uint32_t pages;   /* number of pages */
} mmap_regions[MMAP_REGIONS_MAX];
```

Tracks anonymous mmap allocations separately from `user_pages[]`.

- Set by `sys_mmap2()`.
- Freed by `sys_munmap()` or on exit.
- `void *addr` has the same i16 truncation problem.

#### (c) `proc_image_t image`

```c
typedef struct {
    proc_image_segment_t text;
    proc_image_segment_t staged_text;
    proc_image_segment_t staged_rodata;
    proc_image_segment_t literal;
    proc_image_segment_t rodata;
    proc_image_segment_t data;
    proc_image_segment_t stack;
    uintptr_t entry;
    uint32_t flags;
} proc_image_t;
```

Each `proc_image_segment_t` contains:
```c
typedef struct {
    void *base;             /* base address (void * -> broken on i16) */
    uint32_t size;
    uint32_t vaddr;
    ppap_mem_class_t mem_class;
    uint32_t flags;         /* includes PROC_IMAGE_SEG_OWNED */
} proc_image_segment_t;
```

Used for:
- **OWNED segment freeing**: On exit, `image_release_owned_segments()`
  calls `mem_region_free()` for segments marked `PROC_IMAGE_SEG_OWNED`,
  but only if the segment's base is NOT found in `user_pages[]`.
- **Metadata**: entry point, XIP flags, memory class, size.
- **Procfs**: `/proc/[pid]/status` memory reporting.

### 1.3 The Overlap Problem

The same physical pages can be referenced by both `user_pages[]` and
`proc_image_t`.  The exit path must carefully avoid double-free:

```
sys_exit:
  1. image_release_owned_segments()
       -> for each OWNED segment not in user_pages[]:
            mem_region_free() -> page_free()
  2. proc_release_tracked_pages()
       -> for each user_pages[i]:
            mem_region_free_tracked_page() -> page_free()
  3. for each mmap_regions[i]:
       proc_release_mmap_region() -> mem_region_free()
```

Loaders must set the OWNED flag correctly to avoid double-free.  This
is error-prone:
- `elf_loader.c`: data region is OWNED + page-tracked.  The
  `image_segment_is_page_tracked()` check prevents double-free.
- `flat_loader.c`: data region is OWNED + page-tracked.  Same check.
- `elf16_loader.c`: data region is NOT OWNED (to avoid double-free).
  This works but is a special case that diverges from other loaders.

### 1.4 Summary of Problems

1. **`void *` truncation on i16**: All three structures use `void *`
   which is 16-bit on i16.  Page-pool addresses above 64 KB are
   silently corrupted.

2. **Dual free path**: OWNED segments and page-tracked pages are two
   separate ownership mechanisms for the same memory.  The overlap
   check (`image_segment_is_page_tracked`) is a workaround, not a
   design.

3. **mm_page leaks into loaders**: The module-boundary plan said
   `mm_page_*` should be internal to `mm/`, but `elf16_loader.c` calls
   `mm_page_alloc_contiguous()` and `mm_page_write()` directly because
   `mem_region_alloc()` returns `void *` which doesn't work on i16.

4. **mem_region returns a pointer**: `proc_image_segment_t.base` is
   `void *`, making the API fundamentally 32-bit-oriented.

---

## 2. Target Architecture

### 2.1 Single Allocation API: `mem_region`

`mem_region` becomes the sole public allocation API.  It returns
`page_id_t` for page-backed allocations.  The `mm_page_*` functions
become internal to `src/kernel/mm/` and are not called by loaders or
syscall code.

```c
/* New mem_region API (page-backed classes) */
int mem_region_alloc_pages(page_id_t *out_base, ppap_mem_class_t class,
                           uint32_t n_pages);

/* Cross-segment data access (wraps mm_page_read/write internally) */
void mem_region_page_read(page_id_t id, uint16_t off,
                          void *buf, uint16_t len);
void mem_region_page_write(page_id_t id, uint16_t off,
                           const void *buf, uint16_t len);

/* Free pages by index */
void mem_region_free_page(page_id_t id);
```

On Xtensa, non-page-pool classes (ESP-IDF arenas) keep their current
`void *`-based path.  These classes are never used on i16.

### 2.2 Single Page-Tracking Array: `user_pages[]` with `page_id_t`

```c
page_id_t user_pages[USER_PAGES_MAX];  /* PAGE_ID_INVALID = empty */
```

This is the single source of truth for all page-backed process memory
(data region, brk growth, user stack on RISC-V).

`mmap_regions[]` also switches to `page_id_t`:

```c
struct {
    page_id_t base_page;   /* PAGE_ID_INVALID = empty */
    uint32_t pages;
} mmap_regions[MMAP_REGIONS_MAX];
```

### 2.3 `proc_image_t` Becomes Metadata-Only

`proc_image_t` no longer owns memory.  It stores layout metadata for
procfs reporting and runtime queries (entry point, flags, sizes,
memory class).  The `PROC_IMAGE_SEG_OWNED` flag and
`image_release_owned_segments()` are removed.

```c
typedef struct {
    uint32_t size;
    uint32_t vaddr;
    ppap_mem_class_t mem_class;
    uint32_t flags;           /* EXECUTABLE, WRITABLE, XIP — no OWNED */
    page_id_t base_page;      /* page index (page-backed), or: */
    void *base_ptr;            /* direct pointer (XIP ROM, Xtensa arena) */
} proc_image_segment_t;
```

Alternatively, if XIP/arena segments are rare and well-isolated:

```c
typedef struct {
    uint32_t size;
    uint32_t vaddr;
    ppap_mem_class_t mem_class;
    uint32_t flags;
    page_id_t base_page;      /* PAGE_ID_INVALID for non-page-backed */
} proc_image_segment_t;
```

Non-page-backed segments (XIP ROM text, Xtensa arena text) set
`base_page = PAGE_ID_INVALID` and are freed via their own path
(`mem_region_free` for arenas; nothing for XIP ROM).

### 2.4 Exit Path (After Refactoring)

```
sys_exit:
  1. proc_release_tracked_pages()       <- frees user_pages[] via page index
  2. for each mmap_regions[i]:          <- frees mmap pages via page index
       mem_region_free_page(base_page)
  3. free stack_page_id                 <- already uses page_id_t
  4. image segments with OWNED arenas:  <- Xtensa only, via mem_region_free
       (XIP segments: nothing to free)
```

No duplicate-tracking, no overlap check.  Each page has exactly one
owner.

---

## 3. Migration Steps

### Step M-1: Convert `user_pages[]` from `void *` to `page_id_t`

**Scope**: `proc.h`, `proc.c`, all callers in `exec/`, `syscall/`,
`subsys/`.

1. Change `void *user_pages[USER_PAGES_MAX]` to
   `page_id_t user_pages[USER_PAGES_MAX]`.
2. Initialize slots to `PAGE_ID_INVALID` instead of `NULL`.
3. `proc_track_page(p, slot, page_id)` takes `page_id_t`.
4. `proc_track_page_range(p, start, base_page_id, n_pages)` takes
   `page_id_t base` and stores `base + i` for each slot.
5. `proc_release_tracked_pages()` calls `mm_page_free(id)` instead of
   `page_free(ptr)`.
6. All helper functions (`proc_page_backed_base`,
   `proc_page_backed_contains`, etc.) return `page_id_t` or derive
   pointers via `mm_page_to_ptr()` on 32-bit.
7. Callers in `elf_loader.c`, `flat_loader.c`, `x_loader.c`,
   `com_loader.c`, `sos_loader.c`, `r_loader.c` convert
   `void *base` -> `mm_ptr_to_page(base)` before calling
   `proc_track_page`.
8. `elf16_loader.c` already has `page_id_t` — no conversion needed.
9. `sys_brk`, `sys_vfork`, `sys_execve` updated for `page_id_t` slots.

**Validation**: All existing tests (ARM, m68k, RISC-V) must pass
unchanged.  i16 double-free must be resolved.

### Step M-2: Convert `mmap_regions[]` to `page_id_t`

**Scope**: `proc.h`, `sys_mem.c`, `sys_proc.c` (exit), `procfs.c`.

1. Change `void *addr` to `page_id_t base_page`.
2. `sys_mmap2()`: after `mem_region_alloc`, convert
   `region.base` -> `mm_ptr_to_page(region.base)`.
3. `sys_munmap()`: convert `base_page` -> linear address for the
   return value; free via `mm_page_free()`.
4. Exit cleanup: iterate `mmap_regions[]` and free each via page index.
5. `procfs.c`: compute sizes from `base_page` and `pages` count.

### Step M-3: Remove `PROC_IMAGE_SEG_OWNED` and dual-free path

**Scope**: `mem_layout.h`, `sys_proc.c`, all loaders.

1. Remove `PROC_IMAGE_SEG_OWNED` enum value.
2. Remove `image_segment_is_page_tracked()` and
   `image_release_owned_segments()`.
3. Remove `image_segment_release_owned()`.
4. Exit path: only `proc_release_tracked_pages()` + mmap cleanup +
   stack page free.
5. Loaders: stop setting `PROC_IMAGE_SEG_OWNED` flag on data regions.
6. Xtensa arena segments need a new non-OWNED free path if they are
   not page-tracked.  Add explicit `proc_image_release_arenas()` for
   Xtensa only (replaces the generic OWNED check with a
   target-specific call).

### Step M-4: Add `page_id_t` to `proc_image_segment_t`

**Scope**: `mem_layout.h`, all loaders, `procfs.c`.

1. Add `page_id_t base_page` field to `proc_image_segment_t`.
2. Loaders set `base_page` alongside (or instead of) `base` pointer.
3. On i16: `base` (void *) is not used; `base_page` is authoritative.
4. On 32-bit: `base` is derived from `mm_page_to_ptr(base_page)` or
   set directly for XIP/arena.
5. `proc_image_segment_make()` updated to accept `page_id_t`.
6. Eventually remove `void *base` once all consumers use `base_page`.

### Step M-5: Make `mm_page_*` internal to `mm/`

**Scope**: `page.h`, `mem_region.h`, `elf16_loader.c`.

1. Move `mm_page_read()`, `mm_page_write()`, `mm_page_alloc()`,
   `mm_page_alloc_contiguous()`, `mm_page_free()` declarations out of
   `page.h` into an internal header (`mm/page_internal.h`) or keep in
   `page.h` but mark with a comment.
2. Add wrapper functions in `mem_region.h`:
   ```c
   int mem_region_alloc_pages(page_id_t *out, ppap_mem_class_t class,
                              uint32_t n_pages);
   void mem_region_page_read(page_id_t id, uint16_t off,
                             void *buf, uint16_t len);
   void mem_region_page_write(page_id_t id, uint16_t off,
                              const void *buf, uint16_t len);
   void mem_region_free_page(page_id_t id);
   ```
3. `elf16_loader.c` switches from `mm_page_*` to `mem_region_*`
   wrappers.
4. On 32-bit targets, `mem_region_page_read/write` are just `memcpy`
   from `mm_page_to_ptr()`.

### Step M-6 (optional): Merge `mmap_regions` into `user_pages`

If `USER_PAGES_MAX` has sufficient slots, mmap allocations can reuse
the same array with a flag or range convention (e.g. mmap regions use
high slots).  This eliminates the separate mmap tracking entirely.
Evaluate after Steps M-1 through M-5.

---

## 4. Architecture-Specific Notes

### i16 (IBM PC)

- `void *` is 16-bit.  Page pool starts above kernel BSS (~0x4000+),
  well within 16-bit range for the first few pages, but grows above
  0xFFFF as more pages are allocated.
- All memory access to user-process pages must go through
  `mm_page_read/write` (or `mem_region_page_read/write` after M-5).
- `SS=0` means SP is a 20-bit linear address.  Stack pages must be
  allocated at low addresses (< 64 KB) for SP to fit in 16 bits.
- `proc_image_segment_t.base` pointer is meaningless on i16 for
  page-backed segments; only `base_page` matters.

### Xtensa (ESP32-S3)

- Some memory classes use ESP-IDF heap arenas, not the page pool.
- Arena-backed segments cannot have a `page_id_t`; they remain
  pointer-based.
- The OWNED-segment free path for arenas is replaced by an explicit
  Xtensa-specific cleanup in Step M-3.

### ARM / m68k / RISC-V

- `void *` is 32-bit; pointer-based APIs work fine.
- Migration is mostly mechanical: wrap existing pointers in
  `mm_ptr_to_page()` at the tracking boundary.
- No functional change expected; all existing tests must keep passing.

---

## 5. Dependency and Ordering

```
M-1 (user_pages page_id_t)  -- prerequisite for all others
  |
  +-> M-2 (mmap_regions page_id_t)  -- independent of M-3
  |
  +-> M-3 (remove OWNED dual-free)  -- independent of M-2
       |
       +-> M-4 (proc_image_segment base_page)
             |
             +-> M-5 (mm_page internal)
                   |
                   +-> M-6 (optional: merge mmap into user_pages)
```

M-1 is the critical fix for i16.  M-2 and M-3 can proceed in parallel
after M-1.  M-4 and M-5 are cleanup that can wait until the i16 port
is further along.

---

## 6. Page-Index Conversion Rules

Two functions convert a `page_id_t` back to an address.  They have
different portability constraints:

| Function | Returns | i16-safe? | Use when |
|---|---|---|---|
| `mm_page_linear(id)` | `uint32_t` | **Yes** | Address arithmetic, comparisons, reporting — safe on all targets |
| `mm_page_to_ptr(id)` | `void *` | **No** (`#if !defined(__ia16__)`) | Dereferencing: `memcpy`, `memset`, direct read/write — 32-bit only |

### When to use each

- **Default to `mm_page_linear()`.**  It returns a 32-bit linear address
  that works on every architecture including i16.  Use it for:
  - Computing offsets and sizes (e.g. `brk` arithmetic).
  - Address-range containment checks (e.g. `proc_page_backed_contains`).
  - Returning addresses to userspace or subsystem bridges.

- **Use `mm_page_to_ptr()` only when you need a dereferenceable pointer**
  (e.g. to pass to `memcpy`, `memset`, or to cast to a typed pointer for
  direct access).  This function is unavailable on i16.

- **Use `mm_page_read()` / `mm_page_write()` to access page payloads on
  i16.**  On i16, `void *` is 16-bit and cannot address pages above
  64 KB, so there is no dereferenceable pointer.  These functions handle
  segment register setup internally and work on all platforms (on 32-bit
  they reduce to `memcpy` from `mm_page_to_ptr()`).  Any code that must
  compile for i16 and needs to read or write page contents must use
  these instead of `mm_page_to_ptr()`.
  After Step M-5, all `mm_page_*` names become internal to
  `src/kernel/mm/`.  Public wrappers in `mem_region.h`:

  | Current (`mm_page_*`) | After M-5 (`mem_region_*`) |
  |---|---|
  | `mm_page_linear(id)` | `mem_region_page_linear(id)` |
  | `mm_page_to_ptr(id)` | `mem_region_page_to_ptr(id)` |
  | `mm_page_read(id, off, buf, len)` | `mem_region_page_read(id, off, buf, len)` |
  | `mm_page_write(id, off, buf, len)` | `mem_region_page_write(id, off, buf, len)` |
  | `mm_ptr_to_page(ptr)` | `mem_region_ptr_to_page(ptr)` |

### Reverse lookup

| Function | Returns | i16-safe? | Use when |
|---|---|---|---|
| `mm_ptr_to_page(ptr)` | `page_id_t` | **Yes** | Converting a `void *` from `mem_region_alloc()` to a page index for tracking |

Returns `PAGE_ID_INVALID` if the pointer is not in the page pool.

---

## 7. Related Documentation

- [Kernel Module System](../kernel/kernel_modules.md) -- module boundary
  and `mem_region` as the public allocation API
- [IBM PC Port Plan](pc_port.md) -- i16-specific memory model (S3.4)
- [Design Specification](../spec_v07.md) -- overall memory management
  design
