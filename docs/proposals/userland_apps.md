# Proposal: PPAP Userland Apps & Micro C Library (uclib)

## Problem

PPAP's native apps (push, trace, pdb) each duplicate low-level helpers:
`put_str`, `put_u32`, `put_hex32`, `streq`, etc. — the same code
copy-pasted as `static` functions in every program. Adding new apps (ls,
cat, ps, df) would multiply the duplication further.

Meanwhile, busybox provides ls/cat/ps/df but pulls in musl libc, adding
significant binary size and RAM overhead. For a micro-OS targeting
constrained targets (RP2040, m68k, i8086), we want native replacements
that stay within a single RAM page per process.

## Goal

1. **uclib** — a micro C library that provides syscall wrappers and
   common utilities (string ops, formatted output, numeric parsing).
   Linked with `--gc-sections` so unused functions cost zero.

2. **New native apps** — ls, cat, ps, df — that replace their busybox
   equivalents with tiny, single-page-RAM binaries.

3. **Retire busybox equivalents** — remove busybox symlinks for apps
   that uclib-based replacements cover. Busybox itself stays for the
   remaining applets (vi, grep, sed, etc.) until further replacement.

## uclib Design

### Principles

- **No heap, no malloc**: all state on stack or caller-provided buffers.
- **No global state**: every function is reentrant.
- **`-ffunction-sections -fdata-sections` + `--gc-sections`**: unused
  functions are stripped at link time — zero cost for what you don't use.
- **No binary size regression**: existing apps (push, pdb, trace) must
  not grow when switching from inline statics to uclib.

### API Surface

```c
/* uclib.h — micro C library for PPAP user space */
#ifndef PPAP_UCLIB_H
#define PPAP_UCLIB_H

#include "syscall.h"

/* --- formatted output (fd-based) --- */
void uc_puts(const char *s);          /* write NUL-terminated string to stdout */
void uc_eputs(const char *s);         /* same, to stderr */
void uc_putc(char c);                 /* single char to stdout */
void uc_putu(uint32_t v);            /* decimal unsigned */
void uc_puti(int32_t v);             /* decimal signed */
void uc_putx32(uint32_t v);          /* "0x" + 8 hex digits */
void uc_putx16(uint32_t v);          /* "0x" + 4 hex digits */
void uc_putx8(uint32_t v);           /* "0x" + 2 hex digits */

/* snprintf-like: returns bytes written (excl. NUL), truncates safely */
int uc_snprintf(char *buf, int size, const char *fmt, ...);
/* supported: %s %d %u %x %c %% , width/zero-pad for %d/%u/%x */

/* --- string operations --- */
int  uc_strlen(const char *s);
int  uc_strcmp(const char *a, const char *b);
int  uc_strncmp(const char *a, const char *b, int n);
char *uc_strcpy(char *dst, const char *src);
char *uc_strncpy(char *dst, const char *src, int n);
char *uc_strchr(const char *s, int c);
char *uc_strrchr(const char *s, int c);

/* --- memory operations --- */
void *uc_memcpy(void *dst, const void *src, int n);
void *uc_memset(void *dst, int c, int n);
int   uc_memcmp(const void *a, const void *b, int n);

/* --- numeric parsing --- */
int uc_atoi(const char *s);                    /* decimal */
int uc_parse_u32(const char *s, uint32_t *out); /* 0 on success */

/* --- path helpers --- */
const char *uc_basename(const char *path);     /* last component */

#endif /* PPAP_UCLIB_H */
```

### Implementation

Single file `src/user/lib/uclib.c` (estimated ~300 lines). Each function
in its own section via `-ffunction-sections`. The `uc_snprintf` is a
minimal printf engine (~80 lines) supporting `%s %d %u %x %c %%` with
optional width and zero-pad — no float, no `%n`, no `*` width.

## New Apps

### ls

```
ls [-l] [path]
```

- Default: list names, one per line.
- `-l`: permissions (rwx), size, name.
- Uses `getdents()` + `stat()` syscalls directly.
- Static buffer for directory entries (256 bytes).

### cat

```
cat [file ...]
```

- Read stdin if no arguments.
- 256-byte copy buffer, straight `read()`/`write()` loop.
- No options needed.

### ps

```
ps
```

- Reads `/proc/*/stat` (procfs already provides pid, name, state).
- Formats: PID, STATE, NAME columns.
- Static buffer, iterates `/proc` via `getdents()`.

### df

```
df
```

- Reads `/proc/meminfo` (or a new `/proc/mounts` + per-fs statvfs).
- Shows: filesystem, total, used, available, mount point.
- If statvfs is not yet available, start with `/proc/meminfo` for RAM
  and hard-code romfs size from the romfs header.

## Source & CMake Organization

### Directory Layout

New apps go directly in `src/user/`, consistent with existing apps:

```
src/user/
├── lib/
│   ├── uclib.h           # public API header
│   └── uclib.c           # implementation (one function per section)
├── pdb/
│   ├── pdb.c             # main dispatcher
│   ├── pdb_internal.h    # shared structures & constants
│   ├── pdb_cmd.c         # command handling
│   ├── pdb_regs.c        # register helpers
│   ├── pdb_target.c      # tracee management
│   ├── pdb_inspect.c     # register/memory inspection
│   ├── pdb_break.c       # breakpoint management
│   ├── pdb_util.c        # pdb-specific helpers (after uclib migration)
│   ├── pdb_util.h
│   ├── pdb_trace_util.c  # trace event formatting
│   └── pdb_trace_util.h
├── cat.c                 # new
├── ls.c                  # new
├── ps.c                  # new
├── df.c                  # new
├── init.c                # stays (PID 1, special)
├── getty.c               # stays
├── push.c                # stays, optional uclib adoption
├── push_line.c
├── push.h
├── trace.c               # stays, switch to uclib
├── ttyctl.c              # stays
├── hello.c               # stays (minimal example, no uclib)
├── syscall.h             # stays (arch-independent declarations)
└── arch/                 # stays (crt0, syscall stubs, linker scripts)
```

`lib/` holds uclib (shared library, not an app). `pdb/` gets its own
subdirectory since it has 10+ source files — the only multi-file app
large enough to warrant it. Everything else stays flat in `src/user/`.

### CMake Changes

```cmake
# --- uclib static library (object library for gc-sections) ---
set(PPAP_UCLIB_SRC ${PPAP_ROOT}/src/user/lib/uclib.c)
set(PPAP_UCLIB_OBJ ${PPAP_SHARED_BUILD}/uclib.o)
add_custom_command(
    OUTPUT ${PPAP_UCLIB_OBJ}
    COMMAND ${PPAP_CC} ${PPAP_USER_CFLAGS}
            -ffunction-sections -fdata-sections
            -c -o ${PPAP_UCLIB_OBJ} ${PPAP_UCLIB_SRC}
    DEPENDS ${PPAP_UCLIB_SRC} ${PPAP_ROOT}/src/user/lib/uclib.h
    COMMENT "Compiling uclib.o (${PPAP_ARCH})"
)

# Add uclib.o to CRT objects so all apps get it (gc-sections strips unused)
set(PPAP_CRT_OBJS ${PPAP_SHARED_BUILD}/crt0.o
                   ${PPAP_SHARED_BUILD}/syscall.o
                   ${PPAP_UCLIB_OBJ})

# New apps — same pattern as existing ones
list(APPEND USER_APPS cat ls ps df)

# pdb moves to subdirectory — override main source path
set(PPAP_USER_MAIN_SOURCE_pdb ${PPAP_ROOT}/src/user/pdb/pdb.c)
set(PPAP_USER_EXTRA_SOURCES_pdb
    ${PPAP_ROOT}/src/user/pdb/pdb_util.c
    ${PPAP_ROOT}/src/user/pdb/pdb_trace_util.c
    ${PPAP_ROOT}/src/user/pdb/pdb_cmd.c
    ${PPAP_ROOT}/src/user/pdb/pdb_regs.c
    ${PPAP_ROOT}/src/user/pdb/pdb_target.c
    ${PPAP_ROOT}/src/user/pdb/pdb_inspect.c
    ${PPAP_ROOT}/src/user/pdb/pdb_break.c)
```

The foreach loop needs a small tweak: check for
`PPAP_USER_MAIN_SOURCE_${app}` first, fall back to
`${PPAP_ROOT}/src/user/${app}.c`. This keeps the convention simple —
only apps with a subdirectory need the override.

### uclib Migration of Existing Apps

Every existing PPAP app currently has its own copy of `put_str`,
`put_u32`, `put_hex32`, `streq`, etc. — either as `static` functions
or in `pdb_util.c`. Migration replaces these with uclib calls.

#### pdb (7 extra source files)

`pdb_util.c` is the largest duplication source. Migration:

| pdb_util.c function | uclib replacement |
|---|---|
| `put_str(s)` | `uc_puts(s)` |
| `put_err(s)` | `uc_eputs(s)` |
| `put_chr(c)` | `uc_putc(c)` |
| `put_u32(v)` | `uc_putu(v)` |
| `put_i32(v)` | `uc_puti(v)` |
| `put_hex32(v)` | `uc_putx32(v)` |
| `put_hex16(v)` | `uc_putx16(v)` |
| `put_hex8(v)` | `uc_putx8(v)` |
| `streq(a, b)` | `!uc_strcmp(a, b)` |

After migration, `pdb_util.c` retains only pdb-specific helpers:
`readline`, `split_tokens`, `parse_u32`, `parse_x_spec`,
`select_bp_flag_from_caps`, `load_script_file`. The `pdb_util.h`
header drops the output/string declarations and includes `lib/uclib.h`
instead.

#### trace

`trace.c` has `static` copies of `put_str`, `put_chr`, `put_u32`,
`put_hex32`, `put_nl`, `streq`. All are direct 1:1 replacements with
uclib functions. Remove the static definitions, add
`#include "lib/uclib.h"`.

#### push

`push.c` is self-contained with its own string/env machinery. It does
**not** duplicate the same helpers — it has purpose-built variants
(e.g. `push_env_get`, output via `write()` directly). Migration is
optional. If `uc_snprintf` is useful for prompt or error formatting,
adopt selectively; otherwise leave push as-is.

#### init, getty, hello, ttyctl

These are minimal and use `write()` directly. No migration needed —
they don't duplicate uclib-equivalent code. They will still benefit
from uclib being available if they grow features later.

## Busybox Symlink Removal

Once native cat, ls, ps, df are verified, remove the corresponding
busybox symlinks and disable the applets in `busybox_ppap.fragment`:

- Remove `CONFIG_LS=y`, `CONFIG_CAT=y`, `CONFIG_PS=y`, `CONFIG_DF=y`.
- Remove `/bin/ls`, `/bin/cat`, `/bin/ps`, `/bin/df` symlinks from
  romfs generation.
- Native ELFs installed at `/bin/<app>` by the normal USER_APPS path.

Busybox remains for all other applets (vi, grep, sed, etc.).

## Implementation Order

| Step | Task | Depends |
|------|------|---------|
| 1 | Create `src/user/lib/uclib.{h,c}` with string + output functions | — |
| 2 | CMake: build uclib.o, link into CRT objects | Step 1 |
| 3 | Move pdb sources to `src/user/pdb/`, update CMake paths | Step 2 |
| 4 | Migrate pdb to uclib (verify no size regression) | Step 3 |
| 5 | Migrate trace to uclib | Step 2 |
| 6 | Implement `cat` (simplest new app) | Step 2 |
| 7 | Implement `ls` | Step 2 |
| 8 | Implement `ps` | Step 2 |
| 9 | Implement `df` | Step 8 (may need procfs additions) |
| 10 | Remove busybox symlinks for cat, ls, ps, df | Steps 6–9 |

## Size Budget

Target: each new app ≤ 2 KB text + rodata, ≤ 1 page (4 KB) RAM at
runtime (stack + BSS + data). uclib itself adds 0 bytes to apps that
don't call it (gc-sections).

Reference: `hello.elf` is ~400 bytes text. `trace.elf` is ~1.5 KB.
The new apps should land in the same range.
