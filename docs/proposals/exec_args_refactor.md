# Exec args refactor

## Why

The `sys_execve` scratch buffer (which captures `path + argv + envp`
from user space for the duration of the call) today violates three
project rules:

1. **Arch `#ifdef` in shared code** — [`sys_proc.c`](../../src/kernel/core/syscall/sys_proc.c)
   branches on `#if defined(__ia16__)` to pick BSS vs. stack storage for
   `execve_args_storage_t`.  `coding_rules.md` forbids arch `#ifdef` in shared
   kernel code.
2. **`static` scratch in a syscall path** — `i16_execve_args_storage` is a
   permanent BSS resident and races under preemption.  Memory feedback
   `feedback_no_static_scratch.md` explicitly forbids this pattern.
3. **Budget fights from BSS pressure** — `EXEC_ARGV_BYTES_MAX` is
   overridden to `128` on pcxt because the args lives in the 64 KB
   core segment.  Any increase (e.g. to support shell glob expansion)
   directly eats core-segment headroom that is reserved for feature
   growth.

The root cause is the ia16 memory model: 1 KB kernel stack is too
small for an 800 B scratch, and BSS competes for the 64 KB core
segment, while the page pool (~600 KB on pcxt) sits above 64 KB and
requires `mem_region_page_read/write` for access.

A uniform, page-pool-backed scratch resolves all three issues: no
`#ifdef`, no `static`, no BSS growth, and `EXEC_ARGV_BYTES_MAX` becomes
a pure policy knob — sized by what a glob or command line needs, not
by what fits under 64 KB minus BSS.

## Non-goals

- No change to user-visible `execve(2)` semantics.
- No change to `EXEC_ARGV_MAX` / `EXEC_ENVP_MAX` (vector slot caps).
- No change to the ELF loader's child-stack layout on success.

## Design

### Lifecycle

```
sys_execve entry
  args_page = mem_region_alloc(DATA, PAGE_SIZE)   ← 4 KB page
  copy path / argv / envp from user memory into args_page
      (via mem_region_page_write — trivial memcpy wrapper on 32-bit,
       real page I/O on ia16)
  hand exec_args_t (page_id + layout) to exec_execve → loader.load
  loader reads argv/envp strings via an accessor (below)
  loader writes strings into child stack via mem_region_page_write
  on success: args_page is freed (exec never returns to us)
  on failure: free args_page, restore old image, return -errno
```

One page suffices because:

- `path` ≤ 128 B
- `argv_copy[65]` and `envp_copy[33]` total ≤ 800 B (stored as
  `uint16_t` offsets, not pointers — see below)
- `argv_buf` + `envp_buf` share the remaining ~3 KB of the page,
  budgets become `EXEC_ARGV_BYTES_MAX = 1024`,
  `EXEC_ENVP_BYTES_MAX = 2048` uniformly

Layout within the args page (packed, deterministic):

```
offset 0                       path[VFS_PATH_MAX]                    128 B
offset VFS_PATH_MAX            argv_off[EXEC_ARGV_MAX+1]  (uint16_t) 130 B
offset VFS_PATH_MAX+130        envp_off[EXEC_ENVP_MAX+1]  (uint16_t)  66 B
offset 324                     argv_buf[EXEC_ARGV_BYTES_MAX]        1024 B
offset 1348                    envp_buf[EXEC_ENVP_BYTES_MAX]        2048 B
(total 3396 B; page = 4096 B — 700 B slack for future growth)
```

### Access contract

New header `src/kernel/core/exec/exec_args.h`:

```c
typedef struct {
  page_id_t page;
  uint16_t argv_off;      /* start of argv_off[] table in the page */
  uint16_t envp_off;      /* start of envp_off[] table */
  uint16_t argv_buf_off;  /* start of argv_buf */
  uint16_t envp_buf_off;  /* start of envp_buf */
  uint8_t argc;
  uint8_t envc;
} exec_args_t;

/* Copy the Nth argv string out of the args page into caller buffer.
 * Returns length copied (excluding NUL), or -1 on bounds error.
 * Works on all arches via mem_region_page_read. */
int exec_args_argv_copy(const exec_args_t *s, int idx,
                           char *out, uint16_t out_size);

int exec_args_envp_copy(const exec_args_t *s, int idx,
                           char *out, uint16_t out_size);

/* Length of Nth argv/envp string (without reading it). */
uint16_t exec_args_argv_len(const exec_args_t *s, int idx);
uint16_t exec_args_envp_len(const exec_args_t *s, int idx);

/* Path getter — copies the (already-validated) path out of the
 * args page.  Loaders typically don't need this (exec.c does the
 * open), but provided for symmetry. */
int exec_args_path(const exec_args_t *s, char *out, uint16_t out_size);
```

No direct pointer exposure.  All reads go through page I/O.  On 32-bit
the helpers are thin wrappers over `memcpy`; on ia16 they use the real
page-based read.

### Loader API change

Current:

```c
int (*load)(pcb_t *p, vnode_t *vn, uint32_t file_size,
            const cpu_ops_t *cpu_ops, void *cpu_state,
            const char *const *argv, const char *const *envp, uint32_t flags);
```

New:

```c
int (*load)(pcb_t *p, vnode_t *vn, uint32_t file_size,
            const cpu_ops_t *cpu_ops, void *cpu_state,
            const exec_args_t *args, uint32_t flags);
```

Each loader iterates via `args->argc` / `args->envc` and copies
strings into the child stack via `exec_args_argv_copy` +
`mem_region_page_write` (or `memcpy` + pointer math on 32-bit, but the
helper is the preferred path).  The default-argv case (when
`args->argc == 0`) — inserting `path` as `argv[0]` — moves from
`exec_execve` into each loader (or stays in `exec_execve`, which
stuffs the `path` string into `argv_buf` + sets `argc=1` before
calling `loader.load`).

### Affected files

Direct:

- `src/kernel/core/syscall/sys_proc.c` — allocate args page, copy
  user data via page helpers, pass `exec_args_t` to `exec_execve`.
- `src/kernel/core/exec/exec.{h,c}` — signature change to
  `exec_execve`; default-argv handling migrates into scratch
  construction.
- `src/kernel/core/exec/loader.h` — `loader_t.load` signature change.
- `src/kernel/core/exec/elf_loader.c` — argv/envp write path now
  copies via `exec_args_argv_copy` + page write.
- `src/kernel/core/exec/elf16_loader.c` — same, plus near-pointer
  dereferences (`src = (const uint8_t *)argv[i]`) replaced by scratch
  helper + page write to the child segment.
- `src/kernel/core/exec/flat_loader.c` — same (small file, minimal
  change).
- `src/kernel/core/exec/exec_args.{h,c}` — new (the accessor
  implementation).

Indirect (verify no behavior change):

- `src/kernel/core/subsys/ppap/ppap_m68k_host.c` — uses `EXEC_ARGV_MAX`,
  not affected.
- `src/target/pcxt/CMakeLists.txt` — drop `EXEC_ARGV_BYTES_MAX=128` /
  `EXEC_ENVP_BYTES_MAX=256` overrides.
- `src/kernel/common/config.h` — update comment block; new defaults
  `1024 / 2048`.

### Failure modes

| Failure | Current behavior | New behavior |
|---|---|---|
| `mem_region_alloc` fails | n/a | return `-ENOMEM` before any user copy |
| argv overflow | `-E2BIG` from vec_copy | same, args page freed on return |
| envp overflow | `-E2BIG` | same |
| loader reject | restore old image, free snapshot | same, plus free args page |
| loader success | never returns | never returns; args page freed as part of unbound pages |

The "loader success" path needs a tweak: today the args lives on
stack or BSS and is discarded implicitly.  With a page, we need to
free it after `exec_execve` returns 0 — either in the
success-finalization block of `sys_execve`, or by handing ownership
to the loader.  Simplest: free in `sys_execve` right after
`exec_execve` returns, since loaders fully consume the args before
returning.

### Per-arch considerations

- **arm / riscv / xtensa (32-bit, MMU-less)**: `mem_region_page_to_ptr`
  yields a valid kernel pointer; page helpers are `memcpy` wrappers.
  Zero behavior change beyond the signature switch.
- **m68k (32-bit, QEMU + X68000)**: same as other 32-bit.  m68k flat
  loader + m68k ELF loader both straight-line code; helper swap is
  mechanical.
- **ia16 (pcxt)**: pool pages live above 64 KB; helpers perform the
  real page-segment-offset compute.  Loader writes to child stack
  segment via `mem_region_page_write` (already supported in
  `elf16_loader.c` for image payloads; extending to argv/envp is
  additive).

## Stages

Each stage is committed separately for reviewability.

1. **exec: introduce `exec_args_t` + accessors**
   - New `exec_args.{h,c}` with the struct and accessor helpers.
   - Accessors are page-backed from day one (always use
     `mem_region_page_read/write`).  No wiring yet — this stage lands
     a self-contained helper module.
   - Compiles as part of the kernel core; linked but unreferenced.

2. **exec: switch sys_execve + loaders to `exec_args_t`**
   - Change `loader_t.load` signature.
   - Update every loader (ELF, ELF16, flat) to read argv/envp via
     accessors.
   - `sys_execve` allocates an `exec_args_t` via `mem_region_alloc`,
     populates it via page helpers, passes it down, frees it on
     every exit path.
   - Remove `execve_args_storage_t`, `i16_execve_args_storage`, and
     the `#if defined(__ia16__)` branch.

3. **exec: unify `EXEC_ARGV_BYTES_MAX` / `EXEC_ENVP_BYTES_MAX`**
   - Drop pcxt `EXEC_ARGV_BYTES_MAX=128` / `EXEC_ENVP_BYTES_MAX=256`
     overrides.
   - Set shared defaults `1024 / 2048`.

4. **tests**: exercise large argv / envp via `push` + `env`; verify
   `test_msdos`, `test_exec_args`, `test_env` on all targets.

## Verification

- `./scripts/run.sh --test qemu_arm` — all existing tests pass.
- `./scripts/run.sh --test qemu_m68k` — same.
- `./scripts/run.sh --test qemu_rv32` — same.
- `./scripts/run.sh --test pcxt` — same; verify `ppap_pcxt.elf` size
  **drops** by ~836 B (scratch removed from BSS).
- Manual: glob expansion in push (`ls *.c`) succeeds with ~40+ matches
  once budget is `1024` everywhere.

## Open questions

- Does ia16 need the args page pinned (i.e., not candidate for page
  reclaim during execve)?  `mem_region_alloc` returns a dedicated
  page; no reclaim path should touch it mid-syscall.  Verify.
- ELF argument-frame layout on ARM/m68k: today the ELF loader reads
  argv strings with `memcpy((void *)str_pos, argv[i], len)` — a direct
  near-pointer copy.  Replacing with
  `exec_args_argv_copy` + write should produce identical results
  on 32-bit; smoke-test with a long argv.

## Follow-up (not in this proposal)

- Push glob expansion (`*` / `?`), bump `TOK_BUF_SIZE` to 1024.
  Independent, but only makes sense on top of this refactor because
  otherwise pcxt's 128 B argv cap still bottlenecks.
