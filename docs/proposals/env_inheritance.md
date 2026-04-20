# Proposal: Environment-Variable Inheritance

## Summary

PPAP has user-space infrastructure for environment variables (`push`
has an env pool, `-l` login-shell flag, `/etc/profile` support) and a
host-side libc model that understands `environ` / `getenv`.  But env
never actually reaches child processes: `sys_execve` drops `envp`, the
ELF / ELF16 / flat loaders hard-code `envp = NULL` on the initial user
stack, and the MS-DOS subsystem has to choose between faking
environment values (wrong) or presenting an empty env (the current
honest state, landed in commit `b387348`).

This proposal closes the loop: thread `envp` from the shell through
`sys_execve` and the loaders onto the child's initial user stack (POSIX
layout) and into the MS-DOS env MCB (DOS format, converted).

## Motivation

- Running real DOS programs like FreeCOM cleanly — they look up
  `COMSPEC` and fall through to sensible defaults only when it's
  unset by the parent.  Faking values in the loader is a workaround.
- Letting PPAP shells `export FOO=bar` and have children see it is
  standard POSIX behavior that the libc stubs already expect.
- Removing the special case in the MS-DOS bridge that hides the
  missing feature.  Once env inheritance works, `dos_build_env`
  iterates the parent's env and writes entries directly — no
  synthesized values, no TODO.

## Non-Goals

- Per-process env *storage* semantics beyond the initial inheritance
  (i.e. `setenv` in a child updates only its own stack-placed env,
  not the parent's).
- A kernel-managed environment database.  The kernel's job is to
  copy bytes from parent to child at exec time.
- Any change to vfork semantics.

## Architecture

POSIX model applies as-is:

```
                              sys_execve(path, argv, envp)
                                         │
              parent user memory         │   kernel scratch
              ┌────────────────┐         │   ┌─────────────────┐
              │ argv[]  │ env[]│─────────┼──▶│ argv_copy[]     │
              └────────────────┘         │   │ argv_buf        │
                                         │   │ envp_copy[]     │
                                         │   │ envp_buf        │
                                         │   └─────────────────┘
                                         ▼
                                   loader.load_vn(p, argv, envp, …)
                                         │
                                         ▼
                  user stack (ELF / ELF16 / flat):
                      argc
                      argv[0..n] NULL
                      envp[0..m] NULL
                      auxv[]     AT_NULL
                  or  MS-DOS env MCB (env strings in DOS format)
```

## Phases

Each phase lands as its own commit.  Early phases are ABI-only and
leave runtime behaviour unchanged (envp propagated but discarded by
loaders); later phases consume it.

### Status (2026-04-21)

All phases landed.  End-to-end: parent (push or test_env) calls
`execve(path, argv, envp)` → kernel copies envp into `execve_scratch_t`
→ loaders place envp on the initial user stack (POSIX layout, or
convert to DOS env MCB / Human68k env block for personality guests)
→ uclib crt0 sets `environ` → user code reads via `uc_getenv()`.

| Phase | Commit | Summary |
|---|---|---|
| E-1 | `2e466d0` | `sys_execve` / `exec_execve` / `loader.load` gain `envp`; all arches pass-through |
| E-2 | `8f59df8` | ELF loader emits envp on the initial user stack |
| E-3 | `6024736` | ELF16 loader emits envp on the initial user stack |
| E-4 | `5ae46f7` | `test_env.c` verifies env inheritance end-to-end |
| E-5a | `a22f855` | MS-DOS env MCB populated from parent envp |
| E-5b | `52149ac` | Human68k PMB[0x10] / A3 populated from parent envp |
| E-6 | `814c7a4` | uclib `environ` / `uc_getenv`; every crt0 initialises it |
| E-7 | this commit | expanded test_env sub-tests + syscall.md ABI note |

Out of scope (deferred): `setenv` / `putenv` (need a growable env
pool), `init` env seeding from `/etc/environment`, DOS extensions
for Human68k program-path trailer.

### Phase E-1: Syscall ABI

- `sys_execve` grows `envp_ptr` (user pointer to a NULL-terminated
  array).  Accept `NULL` for "no env" — preserves today's behaviour.
- User-space `execve()` stub in each arch's `syscall.S` passes the
  third arg.
- Kernel copies the envp array and backing strings into
  `execve_scratch_t` alongside argv.  Bounds match argv's limits
  (`ENV_BUF_SIZE`, `ENV_COUNT_MAX`).  Exceeds → `E2BIG`.
- `exec_execve(p, path, argv)` grows `envp` param.  Loader call-sites
  (where argv is already plumbed) pass NULL for now.

*Observable change:* none (no loader consumes `envp` yet).

### Phase E-2: ELF / flat loader stack emission

- ELF loader (32-bit targets) and flat loader write env strings and
  the envp pointer array before the auxv block (replacing today's
  `*sp = 0 /* envp terminator */`).
- Stack layout follows the standard:
    `argc, argv[0..], NULL, envp[0..], NULL, auxv, AT_NULL`.

*Observable change:* children see inherited env once a caller passes
a real envp.

### Phase E-3: ELF16 loader stack emission (ia16)

- Same for `elf16_loader.c`, with 16-bit pointer widths.
- Watch kernel-stack budget on ia16 (1 KB slots); reuse
  `i16_execve_scratch` for the envp copy.

### Phase E-4: Shell (`push`) propagates env

- `push` already maintains an internal env pool for its own builtins.
- On `exec`/spawn of an external command, flatten the pool into a
  `char **envp` and pass to `execve`.
- Existing shell tests for `export`/`unset` gain an "is child's env
  what we exported?" check using a new `test_env` user program.

### Phase E-5: Personality subsystems consume parent env

Each subsystem handles env in its native format.  The loader already
hands `envp` to the subsystem's host-setup function; each subsystem
decides what to do with it.

- **MS-DOS** (`dos_host.c`): `dos_build_env` takes `envp` alongside
  `argv` and writes real `NAME=VALUE\0` entries into the env MCB
  payload in addition to the DOS 3+ program-path trailer.  Sizing
  stays exact: env-para = ceil(content_bytes / 16), main-para fills
  the rest.  MCB `'Z'` signature for the env block is unchanged.
  Existing TODO in `dos_host.c` /
  `docs/proposals/msdos_subsystem.md` is removed.
- **Human68k** (`human68k_host.c`): today sets `PMB[0x10]` env pointer
  to `0xFFFFFFFF` ("none") and initialises `A3 = -1` — the same lazy
  pattern.  Allocate an env region (from the process's page-backed
  memory, Human68k-style), populate with `NAME=VALUE\0…\0` entries,
  store the 32-bit address in `PMB[0x10]`, and load `A3 = env_addr`
  at user-frame setup time.
- **CP/M** (`cpm_host.c`) and **SOS** (`sos_host.c`): the underlying
  personalities have no environment-variable concept, so `envp` is
  accepted by the signature and ignored.  No behaviour change.

Each subsystem change lands as its own sub-phase commit
(E-5a MS-DOS, E-5b Human68k) so the human68k tests don't drift into
the msdos changes and vice versa.

### Phase E-6: User-space libc glue

- Wire `environ` / `getenv` / `setenv` / `putenv` to the stack
  `envp` placed by the loader.  Most of this is already present in
  `src/user/lib/*`; audit and plug any gap.

### Phase E-7: Tests and docs

- `tests/user/test_env.c` covers:
    - parent sets, child reads
    - unset erases it in the child
    - empty env (`envp = NULL`) still works
- `docs/kernel/syscall.md` documents the new `sys_execve` ABI.
- `docs/getting_started/coding_rules.md` reference is unchanged —
  this proposal becomes the normative env-inheritance design.

## Compatibility

`sys_execve` today takes `(path, argv)`.  Extending to `(path, argv,
envp)` is an ABI change, but:

- All in-tree callers will be updated in lockstep (Phase E-1 includes
  the user-space stub + `exec_execve` kernel wrapper).
- No third-party binaries depend on the two-arg form.
- Accepting `envp = NULL` as "no env" keeps today's behavior bit-for-bit
  through Phase E-1.

## Risks

- **Stack budget on ia16.**  Each envp string adds to the initial user
  stack.  Cap at `ENV_BUF_SIZE` (same bound as argv) and fail with
  `E2BIG` beyond that.
- **Kernel-stack in execve.**  Copying env into
  `execve_scratch_t` must reuse the same pattern as argv — which is
  already a static per-arch scratch on ia16.  No new kernel-stack
  pressure.
- **Partial rollout.**  Between phases, children get `envp = NULL`
  even from shells that "set" an env.  That's no worse than today.

## Open Questions

- Initial env content in `init`.  Should `init` read `/etc/environment`
  (or similar) at boot and seed its env?  Out of scope for this
  proposal — land plumbing first, decide on init env seeding later.
- DOS env size bound.  Real DOS caps env at 32 KB by default, with
  `/E:size` to override.  PPAP should pick a reasonable default;
  proposed: inherit `env_para = ceil(content_bytes / 16)` as we do
  today, but refuse env whose total bytes won't fit in the run.
