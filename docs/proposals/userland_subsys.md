# Userland Subsystems via vCPU Interface

> Architectural shift: move per-format loaders, hosts, and bridges out of
> the kernel and into userland runner binaries.  Keep the eCPU/native-CPU
> execution backend in the kernel and expose it through a new vCPU
> syscall family (KVM-style).  This unifies software emulation,
> controlled-native execution, and hardware-assisted emulation (V30 8080
> mode) under one interface.

## 1. Motivation

### 1.1 Kernel size on i16

The pcxt kernel is at ~59 KB text in a 64 KB segment (~5 KB headroom).
Adding Z80 + CP/M kernel-side (~24 KB on ARM, more on ia16) is
impossible without a third module.  The msdos subsystem alone is
**16.3 KB** of pcxt kernel today (`dos_bridge` 12 KB, `dos_host` 3 KB,
loaders 1.6 KB) — and pcxt is native 8086, so *every* byte of msdos is
loader+bridge+host with no eCPU coupling.  Moving it to userland
shrinks core to ~43 KB and lifts headroom to ~22 KB.

### 1.2 Modularity

Subsystems today are kernel build flags (`PPAP_ENABLE_MSDOS`,
`PPAP_ENABLE_CPM`, …) wired through five-file mod_core sync points.
Adding a personality means recompiling the kernel.  Under this design,
adding a personality means installing a user binary.

### 1.3 Composition (V30 + CP/M)

A V30-equipped pcxt should be able to run:

```
pcxt (native 8086) → dos-run (userland) → loads CP/M binary → cpm-run
   (userland, nested) → kernel V30-8080 vCPU → BRKEM hardware
```

This composition is only clean if the eCPU/vCPU is a uniform kernel
abstraction.  Format runners stack; the kernel resource (vCPU) doesn't.

### 1.4 Multi-tenant eCPU

Future work may want one eCPU instance shared across multiple guest
applications in the same session (chained CP/M `CCP→TPA→CCP`,
DOS TSRs, MP/M-style co-residence).  A shared multi-tenant eCPU has
to live in the kernel — userland runners with private emulator
instances cannot share TPA/BDOS state without an IPC daemon, which is
heavier than kernel-resident.

### 1.5 Backend selection is a compatibility concern

CP/M-80 binaries are not uniform.  Some are pure 8080.  Some use
documented Z80 extensions.  Some depend on undocumented Z80 quirks
that an 8080 backend will reject.  And some 8080-targeted binaries
implicitly rely on 8080-specific flag-bit layouts that a Z80 backend
gets subtly wrong.

The runner — and through it the user — needs to *pick* the backend
per binary.  A kernel that transparently "upgrades" 8080 to Z80 (or
the reverse) hides bugs and breaks compatibility testing.  Backend
selection therefore needs to be an explicit part of the vCPU API,
not a kernel-internal optimization.

### 1.6 ia16 native userland is single-segment

PPAP-native ia16 user code is small-model COM-style (CS=DS, ≤64 KB).
A DOS guest legitimately needs multiple segments and `INT 21h AH=48h`
(`_ALLOC`).  Today that's hidden in `dos_host.c`.  If `dos-run` is to
be a userland runner, the kernel has to expose page allocation and
guest-physical mapping to userland — i.e. *the eCPU interface is
already needed even for the native-8086 path*, not just for emulation.

---

## 2. Architecture

### 2.1 Kernel responsibilities

- vCPU lifecycle: create / destroy / attach.  A vCPU has a stable
  `vcpu_id_t` and a refcount; processes hold their own per-process
  `vcpu_fd` references to it.  The vCPU is freed when the last fd
  closes.
- Guest address-space management: allocate pages, map at chosen
  guest-physical address, unmap, protect.
- Instance discovery: filter-by-(arch, backend, session, tag) so a
  runner can find an existing vCPU it could reuse instead of creating
  a fresh one (e.g. attaching a new CP/M command to the same TPA, or
  hosting a DOS TSR alongside the foreground program).
- Run + trap reporting: enter guest, return on trap with reason.
- Initial register / segment setup; getregs/setregs.
- ptrace integration: surface the vCPU state to a debugger process
  (single-step, software breakpoints, register access).

### 2.2 Userland responsibilities

- Per-format loader (parse .COM / .EXE / .COM-CP/M / .X / .EXE-Human68k).
- Per-format bridge (translate guest API calls — INT 21h, BDOS, IOCS — to
  PPAP syscalls).
- Per-format host state (PSP, MCB chain, BIOS state, drive-letter map).

### 2.3 Execution backends (kernel-exposed)

A vCPU is identified by a **(arch, backend)** pair.  All backends
present the same interface to userland; the runner names both
explicitly:

| Arch | Backend | Used by | Notes |
|------|---------|---------|-------|
| Z80 | soft-emu | `/subsys/cpm/loader` (Z80 mode) | existing `ecpu_z80.c` |
| 8080 | soft-emu | `/subsys/cpm/loader` (8080 mode) | strict 8080 subset of Z80 emu |
| 8080 | hw-emu | `/subsys/cpm/loader` on V30 | uses V30 `BRKEM`/`RETEM` |
| 8086 | soft-emu | `/subsys/msdos/loader` on non-x86 hosts | new (`docs/proposals/i8086_ecpu.md`) |
| 8086 | native | `/subsys/msdos/loader` on pcxt | kernel-managed real-mode segments |
| m68k | soft-emu | `/subsys/h68k/loader`, `/subsys/ppap-m68k/loader` on non-m68k hosts | existing `ecpu_m68k.c` |
| m68k | native | `/subsys/h68k/loader`, `/subsys/ppap-m68k/loader` on m68k hosts | controlled native execution |

Three concrete backend kinds are exposed:

- `soft-emu` — portable software interpreter.
- `native` — kernel runs the guest on the host CPU directly, with
  whatever mode-specific framing the arch requires (real-mode segments
  for 8086, supervisor-managed user mode elsewhere).  The mechanism
  difference is a kernel detail; userland sees one constant.
- `hw-emu` — hardware-assisted emulation of a foreign ISA on the host
  CPU.  V30 8080 mode (`BRKEM`/`RETEM`) is the first instance; the
  same constant covers any future CPU that grows a hardware emulation
  mode for another ISA.

Backend choice is **the runner's call, not the kernel's** when the
runner cares.  An 8080-only CP/M binary should run on the 8080
backend even when a Z80 backend is available — running it on Z80
hides 8080-specific quirks (PUSH PSW flag layout, undocumented
opcodes, lack of relative jumps) and can mask bugs.  A Z80-extended
CP/M binary needs the Z80 backend.  The runner picks per binary, per
CLI flag, or per manifest.

For runners (or invocations) that *don't* care, an explicit
`VCPU_BACKEND_AUTO` value asks the kernel to pick the best available
backend for the requested arch.  AUTO is a sentinel — it never
appears in the query bitmap; it is only valid as input to
`sys_vcpu_create`.  Typical kernel policy: prefer `hw-emu` →
`native` → `soft-emu`, but the policy is unspecified and may differ
per target.

The kernel exposes a query syscall so a runner that *does* care can
discover what is available on this host (e.g. `hw-emu` for the 8080
arch only exists if the kernel was built for V30 and detected one at
boot).  If the requested (arch, backend) pair is unsupported,
`sys_vcpu_create` returns `-ENOTSUP`; the runner can fall back to
AUTO, fall back to a different specific backend, or fail the user
request.

### 2.4 binfmt dispatch

The userland runner for each subsystem lives under
`/subsys/<name>/loader`, matching the `/subsys/<name>/` directory
convention already documented in
`docs/subsystems/overview.md` §2.4.  `exec()` of a foreign-format
file resolves to its loader through a small kernel-side binfmt
registry:

```
/subsys/msdos/*  (.COM / .EXE) → /subsys/msdos/loader
/subsys/cpm/*    (.COM)        → /subsys/cpm/loader
/subsys/h68k/*   (.X  / .R)    → /subsys/h68k/loader
```

The dispatch key is the **directory** the binary lives in, not magic
bytes.  This is also the disambiguator for formats with overlapping
magic — a `.COM` under `/subsys/cpm/` is CP/M, a `.COM` under
`/subsys/msdos/` is DOS — exactly the convention overview.md §8.2
already calls out.

Source layout mirrors the romfs layout, and **all** userland-resident
subsystem files (loader, bridge, host) live there — not just the
`loader` entry binary:

```
src/user/subsys/msdos/
  loader.c        # entry point: parses .COM/.EXE, drives vCPU run loop
  dos_bridge.c    # INT 21h / INT 10h dispatch (was kernel)
  dos_host.c      # PSP, MCB chain, drive-letter map (was kernel)
src/user/subsys/cpm/
  loader.c
  cpm_bridge.c    # BDOS / BIOS dispatch (was kernel)
  cpm_host.c      # FCB tables, drive map, RAM disk (was kernel)
src/user/subsys/h68k/
  loader.c
  human68k_bridge.c
  human68k_host.c
```

The bridge and host files relocate from
`src/kernel/core/subsys/<name>/` to `src/user/subsys/<name>/` mostly
unchanged in shape — they swap kernel-internal calls (`vfs_*`,
`fd_t`) for the public PPAP syscall ABI plus the new `sys_vcpu_*`
family.

The kernel-side change is small (a table lookup in the exec path
plus a fallback that strips the `/subsys/<name>/` prefix); the rest
is userland.

### 2.5 Session ownership and instance reuse

Each vCPU is tagged at creation time with the **POSIX session id** of
its creator (`getsid(0)`).  The session field is populated by the
kernel, not by the runner — it is not a free-form label.

The standard loader workflow is therefore:

1. Query for an existing vCPU in the current session matching the
   arch / backend / tag the loader needs:

   ```c
   struct vcpu_filter f = {
     .arch    = VCPU_ARCH_8086,
     .backend = VCPU_FILTER_ANY,
     .session = getsid(0),
     .tag     = "msdos-tpa",
   };
   ```

2. If `sys_vcpu_query_instances(&f, ids, 1) > 0`, attach to the
   first match (`sys_vcpu_attach(ids[0])`) and load the new program
   into the existing guest address space.  This is the chained-CP/M
   case: a fresh `cpm/loader` invocation reuses the previous TPA.
3. If no match, `sys_vcpu_create(arch, backend, tag)` and proceed.

The kernel will not surface vCPUs from other sessions in the query
result.  Cross-session sharing requires explicit out-of-band id
passing (e.g. an env var), which is rare and deliberately awkward.

A login session has one shell as session leader, and all foreground
programs inherit its session id.  Background jobs and detached
processes that call `setsid()` create their own sessions, naturally
isolating their vCPUs from the parent shell.

Concurrent execution from multiple attachers in the same session is
serialized by the kernel: `sys_vcpu_run` takes a per-vCPU mutex.
Loaders that need true co-residence (DOS TSR + foreground program)
coordinate cooperatively in userland.

### 2.6 fork / vfork / exec semantics

`vcpu_fd` follows the existing PPAP file-descriptor inheritance
model:

- `vfork()` (the dominant fork variant in PPAP today) — the child
  inherits a duplicated `vcpu_fd` referring to the same vCPU.  The
  vCPU's refcount goes up by one; the *guest* state is not cloned.
  PPAP-native state of the child is cloned the same way as for any
  other vfork.
- `exec()` — the duplicated `vcpu_fd` is preserved across exec
  unless `FD_CLOEXEC` is set.  This is how a chained loader hands
  off a live vCPU to the next command.
- The vCPU itself is never cloned.  Both parent and child see the
  same guest memory and the same registers; coordination is the
  loaders' problem (and the kernel's `sys_vcpu_run` mutex prevents
  simultaneous execution).

### 2.7 ptrace surface

Debugging a guest binary is **runner-mediated**.  `pdb` attaches to
the loader process (`/subsys/<name>/loader`) using the existing
PPAP ptrace mechanism; the loader exposes guest registers, memory,
and trap state through a side channel — most likely a per-vCPU
`/proc/<pid>/vcpu` directory or a debug-protocol pipe.

This keeps the kernel free of subsystem-specific debug knowledge.
Each loader already has full access to its vCPU's state through the
`sys_vcpu_*` API; surfacing that to a debugger is just another
userland API surface.

The exact protocol — file-based vs socket-based, register encoding,
breakpoint installation — is part of the per-loader designs in P-2
and P-4, not the kernel ABI.

### 2.8 Page-mapping granularity

The `sys_vcpu_mem_*` calls operate at the host kernel's `PAGE_SIZE`
granularity — 1 KB on pcxt (i16), 4 KB elsewhere.

Sub-page allocators that DOS and CP/M legitimately need (paragraph-
granular `INT 21h AH=48h` for DOS, FCB extents and BDOS workspace
for CP/M) live entirely inside kernel-allocated pages and are
managed by the userland host (`dos_host.c`, `cpm_host.c`) — not by
the kernel.

---

## 3. New Syscall Surface

Sketch — exact ABI to be pinned in P-1.

```c
/* Arch and backend IDs */
#define VCPU_ARCH_Z80      1
#define VCPU_ARCH_8080     2
#define VCPU_ARCH_8086     3
#define VCPU_ARCH_M68K     4

#define VCPU_BACKEND_AUTO       0   /* sentinel: kernel picks (input only) */
#define VCPU_BACKEND_SOFT_EMU   1   /* portable software interpreter */
#define VCPU_BACKEND_NATIVE     2   /* host CPU runs guest directly */
#define VCPU_BACKEND_HW_EMU     3   /* hardware-assisted foreign-ISA mode */

/* Process-independent vCPU identifier (per-instance, stable until
 * destroy).  Distinct from the per-process vcpu_fd. */
typedef int32_t vcpu_id_t;

/* Capability query: returns a bitmap of supported concrete backends for
 * the given arch.  Bit (1 << VCPU_BACKEND_X) set ⇔ that backend is
 * available on this kernel/host.  AUTO is never reported — it is a
 * create-time sentinel only. */
uint32_t sys_vcpu_query_backends(int arch);

/* vCPU lifecycle */
int  sys_vcpu_create(int arch, int backend, const char *tag);
                                              /* backend may be AUTO;
                                                 tag may be NULL or
                                                 ≤15 bytes; returns
                                                 vcpu_fd or -ENOTSUP */
int  sys_vcpu_destroy(int vcpu_fd);           /* drops a reference; frees
                                                 when last ref goes */
vcpu_id_t sys_vcpu_get_id(int vcpu_fd);       /* fd → global id */
int  sys_vcpu_attach(vcpu_id_t id);           /* global id → new fd in
                                                 current process; bumps
                                                 the vCPU's ref count */

/* Instance discovery — used when a runner wants to decide between
 * reusing an existing vCPU (e.g. attaching a new CP/M command into the
 * same TPA) and creating a fresh one. */
#define VCPU_FILTER_ANY  (-1)

struct vcpu_filter {
  int          arch;       /* VCPU_ARCH_* or VCPU_FILTER_ANY */
  int          backend;    /* VCPU_BACKEND_* or VCPU_FILTER_ANY */
  int32_t      session;    /* session id or VCPU_FILTER_ANY */
  const char  *tag;        /* exact-match string, or NULL = any */
};

/* Returns count of matching vCPUs (may exceed max_ids).  Writes up to
 * max_ids ids into out_ids.  Pass NULL/0 to count without enumerating. */
int  sys_vcpu_query_instances(const struct vcpu_filter *filter,
                              vcpu_id_t *out_ids, int max_ids);

/* Guest memory */
int  sys_vcpu_mem_alloc(int vcpu_fd, uint32_t guest_phys,
                        uint32_t bytes, int prot);
int  sys_vcpu_mem_free(int vcpu_fd, uint32_t guest_phys, uint32_t bytes);
int  sys_vcpu_copy_in(int vcpu_fd, uint32_t guest_phys,
                      const void *src, uint32_t bytes);
int  sys_vcpu_copy_out(int vcpu_fd, void *dst,
                       uint32_t guest_phys, uint32_t bytes);

/* Registers */
int  sys_vcpu_get_regs(int vcpu_fd, void *regs, uint32_t bytes);
int  sys_vcpu_set_regs(int vcpu_fd, const void *regs, uint32_t bytes);

/* Run + trap */
struct vcpu_trap {
  uint16_t reason;     /* CPU_TRAP_SWI / IO_IN / IO_OUT / HALT / ILLEGAL */
  uint16_t pad;
  uint32_t param;      /* INT vector, IO port, ... */
};
int  sys_vcpu_run(int vcpu_fd, struct vcpu_trap *trap);
```

`sys_vcpu_run` returns when the guest hits a trap the kernel cannot
absorb on its own.  The userland runner inspects `trap`, services it
(reading guest memory, calling PPAP syscalls, writing results back via
`copy_in`/`set_regs`), then calls `sys_vcpu_run` again to resume.

---

## 4. Migration Plan

Five phases, each leaving the tree in a working state.  Each phase is
landed as a single multi-commit series; earlier in-kernel paths stay
live until their userland replacement passes the test suite.

### P-1 — 8086 vCPU interface

**Goal**: kernel exposes the 8086 vCPU syscalls.  No userland clients
yet.

- Define vCPU ABI: arch IDs, syscall numbers, `vcpu_trap` struct.
- Implement `sys_vcpu_*` in the kernel core, backed by:
  - Controlled-native backend on pcxt (real-mode 8086 with kernel-managed
    segments).  Reuses the existing `dos_trap.S` machinery for INT
    interception.
  - Software-emu backend (stub initially; filled in by
    `i8086_ecpu.md` work when available).
- Page allocator: the existing free-list (`mem_region_*`) with a new
  guest-physical mapping table per vCPU.
- Tests: `test_vcpu_8086` — create vCPU, copy a tiny INT 20h binary in,
  run, observe `CPU_TRAP_SWI(0x20)`.

**Pcxt size impact**: ~2–3 KB added (vCPU dispatcher, mapping table).
Net positive only after P-2 lands.

### P-2 — Move msdos to userland

**Goal**: `/subsys/msdos/loader` is a userland binary; `dos_bridge.c`,
`dos_host.c`, `com_loader.c`, `exe_loader.c` are deleted from the
kernel.

- New user binary tree `src/user/subsys/msdos/`:
  - `loader.c` — entry point; parses .COM and .EXE (MZ), allocates
    guest pages, copies image in via `sys_vcpu_copy_in`, sets initial
    CS:IP/SS:SP via `sys_vcpu_set_regs`, drives the vCPU run loop.
  - `dos_bridge.c` — moved from `src/kernel/core/subsys/msdos/`; same
    INT 21h handler dispatch, but kernel-internal calls are replaced
    with public syscalls.
  - `dos_host.c` — moved from `src/kernel/core/subsys/msdos/`; PSP /
    MCB chain construction, drive-letter map.
- Links against a small libos that wraps the `sys_vcpu_*` family.
- binfmt registry in kernel exec: files under `/subsys/msdos/` →
  `/subsys/msdos/loader <path>`.
- Test parity: `test_msdos` continues to pass on pcxt with the new
  userland path.
- Drop `PPAP_ENABLE_MSDOS` and the `KERNEL_SUBSYS_MSDOS_SOURCES` list.
- Delete `src/kernel/core/subsys/msdos/`.

**Pcxt size impact**: ~−16 KB core text, +2 KB user space (the
loader sits in user flash/floppy, not the kernel segment).

### P-3 — Z80 vCPU interface

**Goal**: kernel exposes Z80 vCPU.  Same shape as P-1.

- Wrap existing `ecpu_z80.c` / `ecpu_z80_alu.c` behind the vCPU
  syscalls.  Keep the in-kernel trap handler interface for now (CP/M
  in-kernel still uses the old path).
- Tests: `test_vcpu_z80` — minimal RST 0 / OUT trap exercise.
- New: `sys_vcpu_create(VCPU_ARCH_Z80)` works on every host.

### P-4 — Move cpm to userland

**Goal**: `/subsys/cpm/loader` is a userland binary; the cpm subsystem
is deleted from the kernel.

- New user binary tree `src/user/subsys/cpm/`:
  - `loader.c` — entry point; places .COM at `0x0100`, sets up zero
    page and BDOS jump vector, drives the vCPU run loop.
  - `cpm_bridge.c` — moved from `src/kernel/core/subsys/cpm/`; BDOS /
    BIOS trap dispatch.
  - `cpm_host.c` — moved from `src/kernel/core/subsys/cpm/`; FCB
    tables, drive map, RAM disk.
- binfmt: files under `/subsys/cpm/` → `/subsys/cpm/loader`.
  Disambiguation from DOS `.COM` is by directory only — no filename
  suffix, no metadata bit.
- Backend selection knobs (CLI flag / env / per-binary manifest) —
  pinned in this phase, used by P-5.
- Drop `PPAP_ENABLE_CPM` and `KERNEL_SUBSYS_CPM_SOURCES`.
- Delete `src/kernel/core/subsys/cpm/`.

**Verification**: existing CP/M test programs (zexall subset, etc.)
still pass on qemu_arm, pico1calc, x68k.

### P-5 — V30 8080 vCPU backend

**Goal**: on V30-equipped pcxt, `cpm-run` *can* use hardware 8080
emulation when the user asks for it.  Selection is explicit.

- New kernel backend registered as
  `(VCPU_ARCH_8080, VCPU_BACKEND_HW_EMU)`.  Uses `BRKEM`/`RETEM` to
  enter/exit 8080 mode.
- Trap reporting via the V30's INT-on-emulated-RST mechanism (see
  `docs/proposals/v30_support.md`).
- `sys_vcpu_query_backends(VCPU_ARCH_8080)` returns a bitmap that
  includes `HW_EMU` only if the kernel was built for V30 and detected
  one at boot.
- `/subsys/cpm/loader` uses the backend-selection knobs pinned in P-4
  (CLI flag, env var, or per-binary manifest).  Default policy is up
  to the runner; one reasonable default: prefer `hw-emu` when
  available, fall back to soft-emu Z80 for Z80-extended binaries.
- Tests: `test_vcpu_z80` exercises pass on V30 hardware with
  measurably faster execution than the software backend.  A separate
  compatibility suite verifies that an 8080-only binary behaves the
  same on `(8080, soft-emu)` and `(8080, hw-emu)`, and may behave
  differently on `(Z80, soft-emu)` (documenting why explicit backend
  selection matters).

---

## 5. Relationship to in-progress proposals

### `docs/proposals/msdos_subsystem.md`

Phase D-5 (memory functions, AH=48h/49h/4Ah) is the next chunk of msdos
work.  Two options:

a. **Land D-5 in-kernel first**, then migrate the entire msdos surface
   to userland in P-2.  Lower risk, more code to delete later.
b. **Implement D-5 directly in userland** as part of P-2.  One pass
   instead of two.

Recommendation: (b), conditional on P-1 (8086 vCPU) being a small,
contained landing.  If P-1 takes longer than expected, fall back to (a)
to keep msdos progress unblocked.

### `docs/proposals/i8086_ecpu.md`

The 8086 software emulator is orthogonal.  Under this proposal it
becomes a vCPU backend rather than a kernel-resident eCPU with
in-kernel bridges.  No design change needed; the implementation just
plugs into `sys_vcpu_create(VCPU_ARCH_8086)` instead of being called
from `dos_bridge.c`.

### `docs/proposals/v30_support.md`

P-5 of this plan is the implementation vehicle for the V30 8080-mode
work described there.  The two proposals merge at P-5; v30_support.md
can be slimmed to focus on hardware setup (CPU detection, BRKEM
prerequisites) once P-5 design is pinned.

---

## 6. Risks

### 6.1 Performance — context switch on every guest trap

In-kernel bridges call PPAP syscalls directly; userland runners go
kernel↔userland↔kernel.  For DOS programs, INT 21h frequency is
modest (text I/O, file ops); the overhead is one extra kernel↔user
round-trip per call.  Expected to be in the noise.  Compute-bound
guests (compilers, large CP/M programs) are unaffected — the trap is
only entered at API boundaries.

### 6.2 ia16 user binary size

`/subsys/msdos/loader` and `/subsys/cpm/loader` each carry their own
bridge code (~12 KB on ARM each).  On flash-rich targets this is fine.
On x68k floppy budget (~1.4 MB) it costs ~25 KB if both are installed.
Acceptable.

### 6.3 Bridge "cheating"

The current bridges occasionally reach into kernel internals
(direct `vfs_*` calls, `fd_t` access, internal page allocator
helpers).  Auditing those for the userland refactor will likely
surface a handful of cases where new public syscalls are needed
(not yet enumerated — to be done as part of P-2 design review).

### 6.4 Test coverage

`test_msdos` runs entirely in-kernel today.  Migrating it to drive
the userland runner is part of P-2.  Plan: keep the existing
in-kernel test as a regression target until P-2's userland test
reaches parity, then delete.

---

## 7. Cross-References

- `docs/proposals/msdos_subsystem.md` — current msdos design (will
  be reorganized after P-2).
- `docs/proposals/i8086_ecpu.md` — 8086 software emulator (becomes a
  vCPU backend under this plan).
- `docs/proposals/v30_support.md` — V30 8080 mode (implemented by P-5).
- `docs/kernel/modules.md` — current core/VFS module split.
- `docs/subsystems/cpm.md`, `docs/subsystems/sos.md` — current
  subsystem behavior to preserve.
- `src/kernel/core/cpu/cpu.h` — existing `cpu_ops_t` interface that
  the vCPU syscall ABI mirrors.
