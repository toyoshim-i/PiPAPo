# Memory Management API Cleanup

Untangle the page/page_alloc/mem_region/mem_layout naming and layering so
that the code matches what [`docs/kernel/memory.md`](../kernel/memory.md)
and [`docs/kernel/modules.md`](../kernel/modules.md) already describe:
one mm layer with one set of names, one proc-image layer that owns
segment metadata, and no pass-through wrappers between them.

## Current shape

Three files, four conceptual layers smeared across them:

```
page_alloc.c/h    pure free-block list core (host-testable)
                    page_alloc_reset / _add_range / _n / _largest /
                    _at_id / _free_range / _free_total / _max_contiguous /
                    _is_free / _block_count[_get]

page.c/h          locked + traced wrapper, plus mm_init + RAM probe.
                  Exposes TWO redundant public APIs:
                    (a) void*-style  page_alloc / _free / _at /
                                     _contiguous / _pool_base /
                                     _free_count / _max_contiguous
                    (b) page_id-style mm_page_alloc / _free / _contiguous /
                                     _largest_contiguous / _linear /
                                     _to_ptr / _read / _write / _zero +
                                     mm_ptr_to_page

mem_region.c/h    proc_image_segment_t-shaped allocator +
                  identical pass-throughs for the page_id-style API:
                    mem_region_alloc / _at / _free / _*_bytes /
                    mem_region_page_alloc / _free / _contiguous /
                    _largest_contiguous / _linear / _to_ptr / _read /
                    _write / _zero / mem_region_ptr_to_page

mem_layout.h      Mixes mm-layer enum (ppap_mem_class_t) with
                  proc/exec-layer types (proc_image_segment_t,
                  proc_image_t, PROC_IMAGE_SEG_*, PROC_IMAGE_FLAG_*,
                  proc_image_segment_make[_vaddr]).
```

Observed problems:

1. **Two prefixes for the same operation.** `mm_page_*` and
   `mem_region_page_*` are pure pass-throughs (210 outside-mm/ calls
   use `mem_region_page_*`, but the implementation lives in `page.c`
   under `mm_page_*`).  The split was supposed to mean "internal vs
   public" but they're declared side-by-side in `page.h`.
2. **Two return shapes for the same alloc.**  `page_alloc()` returns
   `void *`; `mm_page_alloc()` returns `page_id_t`.  ~45 legacy call
   sites still use the void* form.  The void* form is unsafe on i16
   (pointers truncate to 16 bits for pages above 64 KB).
3. **Layer leak in `mem_region_alloc`.**  Its signature takes
   `proc_image_segment_t *` — a proc/exec-layer type — and fills
   `vaddr` / `flags` / `mem_class` fields the allocator has no
   business knowing.  This forces every transitive include of
   `mem_region.h` to also pull in proc-image types.
4. **`mem_layout.h` is misnamed and mixed-layer.**  The name suggests
   physical-memory constants (those actually live in `config.h`).
   The contents are a class enum (mm) plus a process-image descriptor
   bundle (proc/exec).
5. **`ppap_mem_class_t` is the only `ppap_`-prefixed type in the
   tree.**  Everything else uses bare prefixes (`pcb_t`, `vnode_t`,
   `page_id_t`, …).

## Target shape

Four layers, one prefix each, no redundancy:

```
common/core/mem_class.h               mem_class_t enum (the mm-side type)
common/core/proc_image.h              proc_image_segment_t, proc_image_t,
                                      PROC_IMAGE_SEG_*, PROC_IMAGE_FLAG_*,
                                      proc_image_segment_make[_vaddr]

mm/page_pool.c/h                      pool_*    (pure, host-testable)
mm/page.c/h                           page_*    (raw page-id API,
                                                locked + logged)
mm/region.c/h                         region_*  (typed class-dispatched
                                                alloc returning a small
                                                {base, base_page} pair)
mm/mem_helper.h/.c                    arch hooks for region_alloc
                                      (unchanged role, lighter signature)

proc/exec call sites                  build proc_image_segment_t locally
                                      from region_alloc results
```

Concrete public API:

```c
/* ---- mm/page.h ---- */

/* Raw pages — handle is page_id_t, no class, always from page pool. */
page_id_t page_alloc(void);
page_id_t page_alloc_n(uint32_t n_pages);
page_id_t page_alloc_largest(uint32_t min, uint32_t max, uint32_t *got);
void      page_free(page_id_t id);
uint32_t  page_linear(page_id_t id);
void *    page_to_ptr(page_id_t id);            /* 32-bit only */
page_id_t page_from_ptr(void *p);
void      page_read (page_id_t id, uint16_t off, void *buf, uint16_t len);
void      page_write(page_id_t id, uint16_t off, const void *buf, uint16_t len);
void      page_zero (page_id_t id, uint16_t off, uint16_t len);
uint32_t  page_pool_base(void);
uint32_t  page_free_count(void);
uint32_t  page_max_contiguous(void);

void mm_init(void);   /* boot entry, keeps the legacy name */

/* ---- mm/region.h ---- */

/* Typed class-dispatched alloc — runs mem_helper hook first, falls back
 * to page-backed.  Returns the minimum information the allocator owns.
 * Caller wraps in proc_image_segment_t if needed. */
typedef struct {
    void *base;            /* dereferenceable on 32-bit; truncated on i16 */
    page_id_t base_page;   /* PAGE_ID_INVALID for arena-only Xtensa classes */
} region_t;

int  region_alloc(mem_class_t class, uint32_t size, uint32_t flags,
                  region_t *out);
int  region_alloc_at(mem_class_t class, void *base, uint32_t size,
                     uint32_t flags, region_t *out);
void region_free(mem_class_t class, uint32_t size, const region_t *r);

uint32_t region_total_bytes(mem_class_t class);
uint32_t region_free_bytes(mem_class_t class);
uint32_t region_largest_free_bytes(mem_class_t class);
```

mod_core vtable becomes:

```
region_alloc, region_free, region_free_bytes, region_total_bytes,
page_read, page_write, page_zero
```

(No more `mem_region_*` entries; no more `proc_image_segment_t` crossing
the module boundary.)

## Why split rather than merge

A previous round considered folding `page_*` into `mem_region_*` (one
prefix for everything).  Rejected: raw-page callers (~80% of sites)
don't carry a memory class, and forcing them through a
`proc_image_segment_t`-shaped descriptor adds ~20 bytes of stack /
struct for every scratch allocation.  The two APIs describe different
things — a class-dispatched typed allocation vs. an untyped page handle
— and the integration win is in removing the **pass-throughs**, not in
collapsing the abstractions.

## Phased plan

Each phase is independently buildable and testable.  Land in order.

### Phase M-1 — split `mem_layout.h`

**Goal:** untangle the mm enum from the proc-image descriptors so the
mm-layer cleanup in M-2 isn't blocked on proc-layer headers.

1. New file `src/kernel/common/core/mem_class.h`:
   - `mem_class_t` enum (renamed from `ppap_mem_class_t`).
   - Values renamed `MEM_RAM_TEXT` / `MEM_RAM_DATA` / etc. (drop
     `PPAP_` prefix to match the rest of the tree).
2. New file `src/kernel/common/core/proc_image.h`:
   - `PROC_IMAGE_SEG_*`, `PROC_IMAGE_FLAG_*` enums.
   - `proc_image_segment_t`, `proc_image_t`.
   - `proc_image_segment_make`, `proc_image_segment_make_vaddr`.
   - Includes `mem_class.h`.
3. Delete `mem_layout.h`.  Update its current includers:
   - `mod_core.h` — still needs `proc_image_segment_t` until M-3 lands,
     so include `proc_image.h` for now.
   - `mem_region.h` / `mem_helper.h` — include `mem_class.h` only.
   - `proc_info.h`, `human68k_bridge.h` — include `proc_image.h`.
   - `target/pcxt/kernel/core/core.c` — include `proc_image.h`.
4. Update every `ppap_mem_class_t` / `PPAP_MEM_*` reference in the tree.
   (Bulk `sed` + build.)

**Commits:** one for the header split, one for the rename.

**Verification:** full build sweep (qemu_arm, qemu_m68k, qemu_rv32,
pcxt, x68k, xtensa) + test lanes that already pass.

### Phase M-2 — collapse `mm_page_*` and `mem_region_page_*` into `page_*`

**Goal:** one name for one operation; delete the pass-throughs.

1. In `page.h`, rename the `mm_page_*` set to `page_*`:
   - `mm_page_alloc` → `page_alloc`
   - `mm_page_alloc_contiguous(n)` → `page_alloc_n(n)`
   - `mm_page_alloc_largest_contiguous` → `page_alloc_largest`
   - `mm_page_free` → `page_free`
   - `mm_page_linear` → `page_linear`
   - `mm_page_to_ptr` → `page_to_ptr`
   - `mm_ptr_to_page` → `page_from_ptr`
   - `mm_page_read/_write/_zero` → `page_read/_write/_zero`
2. Delete the old void*-style API from `page.h`:
   - `page_alloc(void)` returning `void *`, `page_free(void *)`,
     `page_alloc_at(void *)`, `page_alloc_contiguous(uint32_t)`.
   - The names get reused with new `page_id_t`-returning signatures.
3. Inside `page.c`, collapse the duplicated lock/trace/OOM logging
   that today exists in both `page_alloc()` and `mm_page_alloc()`.
   Single `page_alloc_locked(n)` helper underneath.
4. Make `page_alloc_at(addr)` mm-internal; `mem_region_alloc_at`'s
   one caller switches to a new `page_alloc_at_id(page_id_t)` (the
   pool core already exports this).
5. In `mem_region.c`, delete all `mem_region_page_*` definitions.
6. Bulk-rename outside callers:
   - `mem_region_page_alloc(` → `page_alloc(` (~strict word match)
   - `mem_region_page_alloc_contiguous(` → `page_alloc_n(`
   - `mem_region_page_alloc_largest_contiguous(` → `page_alloc_largest(`
   - `mem_region_page_free(` → `page_free(`
   - `mem_region_page_linear(` → `page_linear(`
   - `mem_region_page_to_ptr(` → `page_to_ptr(`
   - `mem_region_ptr_to_page(` → `page_from_ptr(`
   - `mem_region_page_read/_write/_zero(` → `page_read/_write/_zero(`
   - `mem_region_free_tracked_page_id(` → `page_free(`
7. Migrate the ~45 legacy void* callers:
   - Read the call.  If it stored the result as `void *`, change the
     declaration to `page_id_t` and replace later uses with
     `page_linear(id)` / `page_to_ptr(id)` as appropriate.
   - If the caller called `page_free(void *)`, find the matching
     allocation and route the id through instead.
   - Touched files (from grep): `proc.c`, `sys_proc.c`,
     `cpu_native.c`, ELF/flat loaders, subsys hosts, vfs/pipe.c,
     vfs/tmpfs.c — most are 1-3 lines each.
8. Update `mod_core.inc` / `mod_core.h`:
   - Rename `mem_region_page_read/_write/_zero` entries to
     `page_read/_write/_zero`.
   - Re-run pcxt five-file stub sync (see
     [`reference_mod_core_inc`](../../README.md) in memory).
9. Update `docs/kernel/memory.md` §3.2/§3.3 tables and
   `docs/kernel/modules.md` §mod_core listing to match.

**Commits:** can be one big rename commit (mechanical) plus a
follow-up that retires the legacy void* form (more thinking per
site).  Splitting is fine; keeping bisect-clean matters more than
commit size here.

**Verification:** full build sweep + run all PPAP_TESTS lanes.  The
i16 module-boundary check (`check_module_boundaries.sh`) and the
pcxt linker's strict cross-module resolution catch any missed call.

### Phase M-3 — split the segment shape out of `mem_region_alloc`

**Goal:** stop the proc-image-layer leak; the new `region.c` becomes
the class-dispatched alloc layer and nothing more.

1. Replace `mem_region_alloc(seg, class, size, flags)` with:

   ```c
   int region_alloc(mem_class_t class, uint32_t size, uint32_t flags,
                    region_t *out);
   int region_alloc_at(mem_class_t class, void *base, uint32_t size,
                       uint32_t flags, region_t *out);
   void region_free(mem_class_t class, uint32_t size, const region_t *r);
   uint32_t region_{total,free,largest_free}_bytes(mem_class_t class);
   ```

   `region_t` is the small `{base, base_page}` struct.

2. Move `mem_helper_*` hooks to take `region_t` instead of
   `proc_image_segment_t`.  Xtensa's mem_helper override loses the
   `vaddr` / `flags` / `mem_class` plumbing — it never needed them.

3. Rename file: `mem_region.{c,h}` → `region.{c,h}`.  Drop the
   `mem_region` prefix from every name within (the directory and
   filename already carry the namespace).

4. Migrate ~30 callers of `mem_region_alloc` (loaders, exec.c,
   sys_mem.c, subsys hosts).  Pattern:

   ```c
   /* before */
   proc_image_segment_t seg;
   if (mem_region_alloc(&seg, PPAP_MEM_RAM_DATA, size, OWNED) < 0)
       return -ENOMEM;
   /* ...use seg.base / seg.base_page... */

   /* after */
   region_t r;
   if (region_alloc(MEM_RAM_DATA, size, 0, &r) < 0) return -ENOMEM;
   proc_image_segment_t seg = proc_image_segment_make(
       r.base, size, MEM_RAM_DATA, PROC_IMAGE_SEG_OWNED);
   seg.base_page = r.base_page;
   /* ...use seg... */
   ```

   For non-image callers (sys_mmap2, brk extensions) the wrapping
   step disappears entirely — those sites just use `page_alloc_n`
   directly with no segment.

5. `mem_region_free(seg)` → `region_free(seg.mem_class, seg.size,
   &(region_t){seg.base, seg.base_page})`.  Trivial inline.

6. Update `mod_core.inc` exports:
   `mem_region_alloc/_free/_free_bytes/_total_bytes` →
   `region_alloc/_free/_free_bytes/_total_bytes`.  Re-run the pcxt
   five-file sync.

7. `mod_core.h` no longer needs `proc_image.h`.  Drop the include.

8. Update `docs/kernel/memory.md` §3.1 and `modules.md` §mod_core
   to reflect the new boundary.

**Commits:** one for the signature change + helper update, one for
the bulk rename and call-site migration, one for the doc update.

**Verification:** full sweep.  Xtensa is the highest-risk target
because its mem_helper override is the most intricate.

### Phase M-4 — rename `page_alloc.{c,h}` → `page_pool.{c,h}`

**Goal:** distinguish the pure host-testable core from the locked
wrapper in name as well as in role.

1. Rename file and `page_alloc_*` symbols to `pool_*`:
   - `page_alloc_reset` → `pool_reset`
   - `page_alloc_add_range` → `pool_add`
   - `page_alloc_n` → `pool_take` (or `pool_take_n`)
   - `page_alloc_largest` → `pool_take_largest`
   - `page_alloc_at_id` → `pool_take_at`
   - `page_alloc_free_range` → `pool_release`
   - `page_alloc_free_total` → `pool_free_total`
   - `page_alloc_max_contiguous` → `pool_max_contig`
   - `page_alloc_is_free` → `pool_is_free`
   - `page_alloc_block_count/_get` → `pool_block_count/_get`
     (host-test only)
2. Only callers are `page.c` and `tests/host/test_page_alloc.c`.
3. Rename the host test file to match: `test_page_pool.c`.

**Commits:** one.

**Verification:** build + host test lane.

## Non-goals

- **No allocator algorithm changes.**  The address-sorted free-block
  list stays; this is purely a naming/layering cleanup.
- **No new memory classes.**  Existing `MEM_*` set is unchanged in
  meaning.
- **No mem_helper API expansion.**  Xtensa keeps its current arena
  set; the hooks just take a lighter struct.
- **No `kmem` / slab changes.**  Out of scope — separate allocator.

## Risks

1. **pcxt five-file stub sync** (mod_core.inc renames in M-2 and
   M-3).  Stale indices route VFS→core calls to the wrong entry
   silently — see [`reference_mod_core_inc`](../../README.md) in
   memory.  Mitigation: do the rename in two stages within a single
   commit (rename .inc, rebuild, fix the matching `PATCH_CORE` lines,
   then rename callers) and run the pcxt build between.
2. **i16 pointer truncation in legacy void* migration** (M-2 step 7).
   Some legacy callers may have been working only because they used
   pages below 64 KB.  Mitigation: every `page_alloc()` → `page_id_t`
   conversion either stays as id or goes through `page_to_ptr` —
   never reinterpret a `void *` from the old API.  i16 lane catches
   any regression because `page_to_ptr` doesn't exist there.
3. **Xtensa mem_helper** (M-3).  The signature change touches the
   most non-trivial arch override.  Mitigation: keep the migration
   atomic per arch (don't split shared and Xtensa-specific changes
   across commits).

## Documentation work

- `docs/kernel/memory.md` — rewrite §3 (Allocation Layers) after M-3.
- `docs/kernel/modules.md` — update mod_core listing after M-3.
- Add a short "naming convention" note in `memory.md`:
  `pool_*` (private core), `page_*` (raw page handles),
  `region_*` (typed class-dispatched alloc), and the proc/exec
  layer's `proc_image_segment_t` is built **at the caller** from a
  `region_t` — segment construction is not an mm concern.

## Status

Not started.  Land phases in M-1 → M-2 → M-3 → M-4 order.  No phase
can be merged out of order without re-introducing the layering smell
the next phase removes.
