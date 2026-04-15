# Loader Interface Revamp — vnode-based streaming

## Problem

`loader_t.load(file_buf, file_size, …)` passes a `const uint8_t *file_buf`
that each loader dereferences to read the binary.  `exec.c` produces
that buffer by `mem_region_alloc(PPAP_MEM_RAM_DATA, file_size)` and
copies the file into it via `vnode_read`.

On i16 the staging buffer lives in the page pool (above the first
64 KB).  `mem_region_alloc` returns `seg.base = (void *)(uintptr_t)
mm_page_linear(pid)` — a 32-bit linear address cast to a 16-bit near
pointer.  Any loader that dereferences `file_buf`, directly or through
`mem_region_page_write(dst, off, src=file_buf, n)`, reads kernel memory
at the truncated address instead of the actual file, and copies random
core `.text`/`.data` into the new process image.  The process then
wanders into a warm-reset on its first user-mode instruction.

The existing escape hatch — `elf16_load_vnode` with a `#ifdef __ia16__`
short-circuit in [exec.c](../../src/kernel/core/exec/exec.c) — is a
per-loader hack that violates `docs/getting_started/coding_rules.md`
("No arch `#ifdef` in shared code", "No code duplication", "Fix root
causes").  Every other loader that has to read file bytes
(`flat_loader`, `com_loader`, `cpm_loader`, `x_loader`, `r_loader`,
`sos_loader`, `m68k_emu_loader`) has the same latent bug on i16, just
untriggered because nobody has exercised their file-content paths on
a segmented target yet.

## Proposal

Change the loader contract so file content is always read via
`mod_vfs.vnode_read(vn, page, off, n, src_off)`.  Remove the staging
buffer from `exec.c`.  Remove the `elf16_load_vnode` escape hatch.

### New `detect`

```c
int (*detect)(const uint8_t *header, uint32_t header_len,
              uint32_t file_size, const char *path);
```

`exec.c` reads `min(file_size, LOADER_HEADER_MAX)` bytes of the file
into a kernel-stack buffer and passes it.  `LOADER_HEADER_MAX = 64`
covers every current magic check (ELF32 ehdr = 52 bytes).  Loaders
that match on extension ignore `header`.

### New `load`

```c
int (*load)(pcb_t *p, vnode_t *vn, uint32_t file_size,
            const cpu_ops_t *cpu_ops, void *cpu_state,
            const char *const *argv, uint32_t flags);
```

Loaders stream content from `vn` directly into the destination page(s)
via `mod_vfs.vnode_read`.  Loaders that want a flat pointer for XIP
(ARM ELF-in-flash) read `vn->xip_addr` themselves — this path was
already i16-safe because XIP sits in low ROM.

The `loader_t.xip` flag becomes informational only (it used to tell
`exec.c` whether to free the staging buffer); it may be retired later.

## Migration strategy

Atomic swap is too big to review.  Instead, run the old and new
`load` entrypoints in parallel so each loader can be migrated
independently, then retire the old one.

### Phase 0 — plan (this document)

Land this proposal; agree on interface shape and verification gates.

### Phase 1 — `detect` swap (single commit)

`detect` has no allocation or streaming concern.  Change its signature
and update all loaders in one commit.

**Commit:** `exec/loader: switch detect() to header-buffer input`

- Add `LOADER_HEADER_MAX` and update the `detect` field in
  [loader.h](../../src/kernel/core/exec/loader.h).
- In [exec.c](../../src/kernel/core/exec/exec.c), read the first
  `min(file_size, LOADER_HEADER_MAX)` bytes into a kstack buffer
  before iterating the registry.  Pass the buffer to each `detect`.
  (Staging buffer path and XIP `file_base` logic stay otherwise
  unchanged — `load` is still old-style in this commit.)
- Update every loader's `detect` to take `(header, header_len,
  file_size, path)`.  Extension-only detects ignore the buffer.
- Remove the ia16-specific ELF detect shortcut in `exec.c` that
  calls `exec_vnode_read_near` — the header is now always available.

**Verification:** all on-target suites.  No runtime behaviour change
because `load` still receives the same `file_buf` as before.

### Phase 2 — add parallel vnode-based `load` (single commit)

Introduce a second entry point on `loader_t`:

```c
int (*load_vn)(pcb_t *p, vnode_t *vn, uint32_t file_size,
               const cpu_ops_t *cpu_ops, void *cpu_state,
               const char *const *argv, uint32_t flags);
```

`exec.c` calls `load_vn` if non-NULL; otherwise falls back to `load`
with the staging buffer.  Loaders that have not been migrated yet
leave `load_vn = NULL` and keep working as before.

**Commit:** `exec/loader: add vnode-based load_vn entry point`

- Add `load_vn` field; no loader sets it yet.
- `exec.c` iterates: try `load_vn` first; if NULL, continue with the
  existing staging + `load` path.
- No behaviour change.  Build + test all targets.

### Phase 3 — migrate loaders one at a time

Each commit retargets a single loader from `load` to `load_vn` and
clears its `load` pointer.  i16-visible loaders go first so the i16
warm-reset is cleared as early as possible.

| # | Commit scope | Loader | Notes |
|---|---|---|---|
| 3.1 | `msdos/com: stream .COM via vnode_read` | [com_loader.c](../../src/kernel/core/subsys/msdos/com_loader.c) | unblocks MSDOS D-1 |
| 3.2 | `exec/elf16: fold elf16_load_vnode into load_vn` | [elf16_loader.c](../../src/kernel/core/exec/elf16_loader.c) | delete the parallel entrypoint and the `#ifdef __ia16__` ELF shortcut in `exec.c` |
| 3.3 | `exec/flat: stream via vnode_read` | [flat_loader.c](../../src/kernel/core/exec/flat_loader.c) | i16 fallback path |
| 3.4 | `exec/elf: stream via vnode_read` | [elf_loader.c](../../src/kernel/core/exec/elf_loader.c) | ARM/m68k/RISC-V/Xtensa; keep XIP via `vn->xip_addr` |
| 3.5 | `subsys/cpm: stream .COM via vnode_read` | [cpm_loader.c](../../src/kernel/core/subsys/cpm/cpm_loader.c) | Z80 memory destination |
| 3.6 | `subsys/human68k: stream .x via vnode_read` | [x_loader.c](../../src/kernel/core/subsys/human68k/x_loader.c) | |
| 3.7 | `subsys/human68k: stream .r via vnode_read` | [r_loader.c](../../src/kernel/core/subsys/human68k/r_loader.c) | |
| 3.8 | `subsys/sos: stream via vnode_read` | [sos_loader.c](../../src/kernel/core/subsys/sos/sos_loader.c) | |
| 3.9 | `subsys/ppap: stream m68k_emu via vnode_read` | [m68k_emu_loader.c](../../src/kernel/core/subsys/ppap/m68k_emu_loader.c) | |

Each commit:
- Rewrites exactly one `<loader>_load` body to take `vnode_t *vn` and
  read via `mod_vfs.vnode_read` instead of `file_buf`.
- Renames / re-signs the C function, re-points the `loader_t`
  registration to set `.load_vn = …, .load = NULL`.
- Touches at most one other file (the `.h` export, if any).

### Phase 4 — retire the old contract (single commit)

Once every loader in `loader_registry[]` has `load_vn` set and `load`
= NULL:

**Commit:** `exec/loader: remove staging buffer and legacy load()`

- Delete the `load` field from `loader_t`; rename `load_vn` → `load`.
- Delete the staging `mem_region_alloc` + `file_buf`/`file_region`
  code path from `exec.c`.
- Delete the remaining `#ifdef __ia16__` scaffolding.
- Delete `elf16_load_vnode` declaration (already unused after 3.2).
- Demote `loader_t.xip` to removed or documented-only — decide at
  commit time based on whether any loader still needs to tell
  `exec.c` about XIP semantics.

## Scope of the interface change

Files edited by phase:

| Phase | Files | Nature |
|---|---|---|
| 1 | `loader.h`, `exec.c`, every `*_loader.c` `detect` | mechanical |
| 2 | `loader.h`, `exec.c` | mechanical, no loader touched |
| 3.x | one loader per commit | per-loader rewrite |
| 4 | `loader.h`, `exec.c`, possibly `elf16_loader.c` | cleanup |

## Verification

Every commit exercises `./scripts/run.sh --test <target>` with a
hard `timeout`, per [coding_rules.md](../getting_started/coding_rules.md):

```
timeout 180 ./scripts/run.sh --test qemu_arm
timeout 180 ./scripts/run.sh --test qemu_m68k
timeout 180 ./scripts/run.sh --test qemu_rv32
timeout 300 ./scripts/run.sh --test pcxt
./scripts/build.sh pico1calc            # build-only (hardware unavailable)
```

Commit bodies record pass/fail per target.  Phase 1 and Phase 2
should be pure no-ops at runtime; Phase 3.x each unblock exactly one
loader's correctness on i16 (or confirm no regression on flat archs).

## Non-goals

- Changing what any loader does once the file is in memory (argv
  layout, PSP setup, relocation logic, page ownership, etc.) — the
  only refactor is the source of file bytes.
- Reworking `loader_registry[]` ordering.
- Adding new binary formats.

## Risks

- A loader I haven't looked at closely does multi-pass random-access
  indexing on `file_buf` (e.g. reads the header, jumps to an offset,
  reads another structure, jumps back).  `vnode_read` supports random
  access via the `src_off` argument, so the transform is always
  possible; the risk is code volume per loader, not feasibility.
- Flat-arch regressions from per-loader rewrites.  The per-loader
  phasing keeps each commit small enough to bisect.
- Kernel-stack usage in loaders that read into local buffers.  i16
  stacks are tight (1 KB/slot); use sub-page scratch (e.g. 64 bytes)
  and loop, same pattern tmpfs now uses for page-to-page copies.

## Status

Phase 0 (this doc) pending approval.  After approval, work resumes
with Phase 1.  MSDOS D-1 remains suspended until Phase 3.1 lands.
