# Refactoring Proposals

## R-1: Subsystem Bridge Common Library

**Files:** `human68k_bridge.c` (2,258 lines), `cpm_bridge.c` (1,626 lines), `sos_bridge.c` (1,198 lines)

**Problem:** All three bridges duplicate identical helper functions: `fd_desc()`, `page_ref()`, `fd_read()`, `fd_write()`, `fd_poll()`, `fd_ioctl()`, `putc()`, `print()`, and file operation wrappers. The implementations are literal copy-paste with only the function name prefix changed.

**Proposal:** Extract a shared `src/kernel/subsys/subsys_common.h` (or `.c`) containing the common FD helpers, console I/O, and file operation wrappers. Each bridge calls the shared functions directly.

**Impact:** ~400 lines eliminated; changes to FD handling only need one edit.

---

## R-2: VFS Filesystem Helpers

**Files:** `tmpfs.c`, `romfs.c`, `devfs.c`, `procfs.c`, `ufs.c`

**Problem:** Four patterns are hand-rolled in every filesystem:

1. **Name comparison** -- five filesystems each write `while (*a && *a == *b)` loops.
2. **Vnode alloc-and-fill on lookup** -- alloc, set ino/type/mode/size/mount, assign result. ~6 lines repeated at 5+ call sites.
3. **Page-chunked copy loop** -- `while (remaining > 0) { chunk_len(); page_write(); advance(); }` appears in 4 filesystems.
4. **Readdir entry filling** -- name copy with `VFS_NAME_MAX` truncation repeated in 4 filesystems.

**Proposal:** Add `src/kernel/vfs/vfs_util.h` with:

- `vfs_name_eq(a, b)` -- string comparison for directory names.
- `vfs_lookup_result(result, dir, ino, type, mode, size)` -- vnode allocation helper.
- `vfs_copy_to_pages(page, off, src, n)` / `vfs_copy_from_pages(dst, page, off, n)` -- page-chunked transfer.
- `vfs_fill_dirent(entry, ino, type, name)` -- readdir entry builder.

**Impact:** ~200 lines eliminated; consistent behavior across all filesystems.

---

## R-3: Module System Single Source of Truth

**Files:** `mod_vfs.h`, `mod_vfs.inc`, `vfs.c` (same pattern for `mod_core`)

**Problem:** The module interface is defined in three places: `.h` (struct fields via `MOD_FUNC` macros), `.inc` (function list with manual indices), and the implementing `.c` (struct initializer via `#include .inc`). Adding a module function requires editing all three files and keeping indices in sync.

**Proposal:** Replace with a single X-macro list per module:

```c
#define MOD_VFS_FUNCTIONS \
  X(fd_open,    int, const char *, int, int) \
  X(fd_read,    long, int, page_id_t, uint16_t, size_t) \
  ...
```

Redefining `X` generates the struct, the initializer, and assembly stubs from one definition. The `.inc` files are eliminated.

**Impact:** One edit to add/remove a module function; no manual index management.

---

## R-4: Syscall Dispatch Table

**File:** `syscall.c` (lines 77-368)

**Problem:** A 290-line `switch(nr)` with ~60 cases, each manually casting arguments from the trap frame array. Adding a syscall means inserting a new case with hand-written casts.

**Proposal:** Replace with a function pointer table:

```c
typedef long (*syscall_fn_t)(long a0, long a1, long a2, long a3, long a4, long a5);

static const syscall_fn_t syscall_table[SYS_MAX] = {
  [SYS_EXIT]  = (syscall_fn_t)sys_exit,
  [SYS_READ]  = (syscall_fn_t)sys_read,
  ...
};
```

Dispatch becomes a table lookup + indirect call. Special cases (CLONE/FORK aliasing, OPENAT) become small pre-processing wrappers.

**Impact:** Syscalls become data; adding one is a single table entry.

---

## R-5: VFS Default Ops (Null Object Pattern)

**File:** `vfs.c`

**Problem:** VFS dispatch wrappers chain 4 NULL checks per call (`vn`, `mount`, `ops`, `ops->read`). Error codes are inconsistent (magic `-2` vs `-ENOENT`).

**Proposal:** Define a default `vfs_ops_t` with stub functions returning `-ENOENT`. Assign it at mount time when ops are NULL. This eliminates all function-pointer NULL checks from the dispatch path.

**Impact:** Simpler dispatch; consistent error codes; easy to add tracing later.

---

## R-6: Signal Delivery Arch Hooks

**File:** `signal.c`

**Problem:** The signal delivery sequence (save frame, block signal, call handler, restore frame, unblock) is duplicated per architecture. Only the frame layout and trampoline mechanism differ.

**Proposal:** Define an arch hook interface:

```c
typedef struct {
  size_t frame_size;
  void (*save_frame)(uint32_t *regs, void *buf);
  void (*restore_frame)(void *buf, uint32_t *regs);
  void (*call_handler)(uint32_t *regs, uintptr_t handler, int sig);
} arch_signal_ops_t;
```

Core signal logic in `signal_check()` calls these hooks. Each architecture provides one static instance.

**Impact:** Signal delivery logic lives once; new arch port needs 3-4 hook functions.

---

## R-7: eCPU Register Access Tables

**Files:** `ecpu_z80.c`, `ecpu_m68k.c`

**Problem:** Paired `get_reg()`/`set_reg()` functions contain 20-30 case switches that mirror each other. Adding a register means editing both switches identically.

**Proposal:** Use a register descriptor table:

```c
typedef struct { uint16_t offset; uint8_t size; } reg_desc_t;

static const reg_desc_t z80_regs[Z80_REG_MAX] = {
  [Z80_REG_A] = { offsetof(z80_state_t, a), 1 },
  ...
};
```

Generic `get_reg`/`set_reg` index into the table via `offsetof`. Synthetic registers (e.g. AF = A<<8|F) handled by a small fallback switch.

**Impact:** One table entry per register; get/set stay in sync automatically.

---

## R-8: Process Page Tracking Iterator

**File:** `proc.c` (lines 138-286)

**Problem:** ~9 functions loop over `user_pages[USER_PAGES_MAX]` with nearly identical structure (count valid pages, clear all, release range, find free slot, etc.). Each re-implements bounds checking and the `PAGE_ID_INVALID` sentinel test.

**Proposal:** A generic iterator:

```c
typedef int (*page_iter_fn)(page_id_t *page, uint32_t index, void *ctx);

void proc_pages_iter(pcb_t *p, uint32_t start, uint32_t end,
                     page_iter_fn fn, void *ctx);
```

Each operation becomes a small callback. The iterator centralizes NULL checks, bounds clamping, and the loop.

**Impact:** Centralized loop logic; new page operations are one callback function.

---

## Priority

| ID   | Effort  | Code Reduction | Sync Burden Removed |
|------|---------|----------------|---------------------|
| R-1  | Small   | ~400 lines     | 3-way bridge sync   |
| R-2  | Small   | ~200 lines     | 5-way FS sync       |
| R-3  | Medium  | ~100 lines     | 3-file module sync  |
| R-4  | Medium  | ~200 lines     | Manual cast sync    |
| R-5  | Small   | ~50 lines      | NULL check pattern  |
| R-6  | Medium  | ~100 lines     | Per-arch signal dup |
| R-7  | Small   | ~80 lines      | get/set reg sync    |
| R-8  | Small   | ~60 lines      | Page loop pattern   |

R-1 through R-3 offer the best ROI: they eliminate real duplicated logic that must stay in sync today. R-4 and R-5 simplify core dispatch paths. R-6 through R-8 are smaller wins that improve consistency.
