# Proposal: File Timestamps

## Summary

PPAP has no working file timestamps today.  The native `struct stat`
in [src/common/stat.h](/src/common/stat.h) exposes only `st_ino`,
`st_mode`, `st_nlink`, `st_size` — no atime/mtime/ctime fields.  The
Linux-compat `stat64` path leaves the timestamp fields as zero.
`SYS_UTIMES` is stubbed to return 0 without doing anything, with a
comment `"no RTC — timestamp update is no-op"`.

The situation was defensible when PPAP had no time source, but
`sys_clock_gettime32` / `sys_gettimeofday` have been working for a
while now — they derive wall time from `sched_get_ticks()` and a
`time_boot_epoch` that `sys_settimeofday` can seed.  Targets without
RTC start at epoch 0 and report "seconds since boot" until someone
calls `settimeofday`, which is good enough to stamp meaningful
*relative* mtime on writes within a session.

This proposal wires timestamps through the stack — VFS ops → per-FS
inode storage → `stat` exposure → userland consumers — and lifts
`SYS_UTIMES` out of its current stub.

Normative references for implementers:

- [docs/kernel/syscall.md](/docs/kernel/syscall.md) — syscall ABI,
  numbering groups.
- [docs/proposals/more_userland_apps.md](/docs/proposals/more_userland_apps.md)
  — Tier 2 depends on this for a meaningful `touch`.

## Motivation

- **`ls -l` currently lies by omission.**  The native stat struct has
  no time fields, so native `ls` can't even show timestamps.  Busybox
  `ls -l` shows 1970 epoch for everything because the stat64 fill path
  zeroes the timestamp fields.  Both are bad UX.
- **`touch` needs it.**  Tier 2 of the userland plan includes `touch`.
  Without real mtime plumbing `touch file` on an existing path is a
  no-op — functionally identical to `true` — which is not a touch.
- **Build systems care.**  Any future port of `make` (or even just
  push-scripted rebuild logic) assumes mtime comparisons work.
- **UFS on-disk already reserves the bytes.**
  [src/kernel/vfs/ufs_format.h:73-75](/src/kernel/vfs/ufs_format.h#L73-L75)
  stores `i_atime/i_mtime/i_ctime` (+ nsec) per inode.  Every UFS
  image we've shipped since the UFS44 migration has been writing zero
  into these fields; lifting the stub costs nothing on disk.
- **Clock source exists.**  `sys_time_now()` in
  [src/kernel/core/syscall/sys_time.c](/src/kernel/core/syscall/sys_time.c)
  is the canonical boot-epoch + tick-derived clock.  We can reuse it
  directly from VFS ops.

## Non-Goals

- **RTC integration for targets other than pcxt.**  x68k has an RTC,
  pico has none, xtensa varies.  Seeding `time_boot_epoch` from
  target-specific sources is out of scope except for pcxt, which we
  cover in T-5 because CMOS reading is cheap and gives us a real
  wall clock on the tightest target.  `ntpd` / network time sync is
  always out of scope here.
- **Sub-second precision.**  Store `i_mtime` at second granularity.
  The `_nsec` fields are preserved in UFS on-disk but we'll write
  zero into them initially.  Tick-derived fractional seconds can be
  added later without an ABI break.
- **Full POSIX atime semantics.**  atime-on-read is famously
  expensive and most modern systems run `noatime` by default.  Defer
  atime update on read; only stamp atime when an explicit
  `utimensat` sets it.  mtime-on-write and ctime-on-metadata-change
  are the useful ones.
- **Per-FS configurable time policy** (`noatime` / `relatime` mount
  flags).  The atime-never default above is hard-coded; add options
  only when a real use case appears.

## Current State

### What exists

- [src/common/time.h](/src/common/time.h): `struct timespec` with
  `tv_sec` / `tv_nsec`.
- [src/kernel/core/syscall/sys_time.c](/src/kernel/core/syscall/sys_time.c):
  `sys_time_now(sec, frac)`, `sys_clock_gettime{32,64}`,
  `sys_gettimeofday`, `sys_settimeofday`.  Boot epoch starts at 0.
- [src/kernel/vfs/ufs_format.h](/src/kernel/vfs/ufs_format.h):
  on-disk inode with `i_atime/mtime/ctime` (+ nsec).
- [src/common/syscall_nr.h](/src/common/syscall_nr.h): `SYS_UTIMES =
  0x0309` reserved; handler stubbed at
  [syscall.c:412](/src/kernel/core/syscall/syscall.c#L412) to return 0.

### What is missing

- Native `struct stat` has no time fields.
- `fill_stat64` zeroes out `st_{a,m,c}time[_nsec]`.
- No VFS op takes/returns timestamps — there's no kernel-internal
  `struct kstat` analogue with times.
- tmpfs in-memory inode
  ([src/kernel/vfs/tmpfs.c:29-39](/src/kernel/vfs/tmpfs.c#L29-L39))
  has no timestamp field.
- UFS driver never reads or writes `i_mtime` (grep confirms: no
  references to the field name inside ufs.c).
- romfs is read-only and uses only file-size and mode from its
  header — no timestamp source.
- devfs / procfs synthesize stat on the fly; currently return no
  times.
- `SYS_UTIMES` is a no-op return-0 stub.  Busybox `touch` appears
  to succeed but nothing is stored.

## Proposed design

### 1. Grow `struct stat` directly

We considered a kernel-internal `struct kstat` to stage the user
ABI break behind the kernel one.  The split was dropped: every
userland binary in this repo rebuilds from source, so staging buys
nothing.  Instead grow the native `struct stat` directly:

```c
struct stat {
  uint32_t st_ino;
  uint32_t st_mode;
  uint32_t st_nlink;
  uint32_t st_size;
  uint32_t st_mtime;  /* seconds since epoch */
  uint32_t st_ctime;  /* seconds since epoch */
  uint32_t st_atime;  /* seconds since epoch, 0 if atime-never */
};
```

The `.stat` VFS op signature stays `int (*stat)(vnode_t *, struct
stat *)`; each FS driver fills the new fields directly.

Sub-second precision is intentionally omitted; adding `st_mtime_nsec`
later is a second ABI bump but costs nothing to defer.

### 2. Per-FS inode storage

- **tmpfs**: grow `tmpfs_inode_t` by three `uint32_t` (12 B per
  inode).  With `TMPFS_INODE_MAX = 16` that's +192 B of BSS — fine
  even on pcxt.
- **UFS**: use the existing on-disk fields.  No image format change.
  Reading stamps on `.stat`, writing stamps on create / write /
  truncate / metadata change.
- **romfs**: read-only, no per-inode storage.  Return a single
  build-time epoch (passed as a module parameter or fixed to zero)
  for every file.  Stamping the build time requires cooperation with
  the romfs image builder; zero is the lazy default and still no
  worse than today.
- **devfs / procfs**: synthesize.  mtime = boot time (so they look
  "created at boot"), ctime = boot time, atime = 0.  Trivial and
  accurate-enough.
- **vfat**: has on-disk DOS timestamps.  Out of scope for the first
  landing — return zeros, file a follow-up.

### 3. VFS op table additions

`.stat` signature is unchanged (`struct stat *`) — the struct itself
grew.  Add a new op for explicit timestamp writes:

```c
int (*utimes)(vnode_t *vn, uint32_t atime, uint32_t mtime);
```

`NULL` on FS drivers that do not store times (romfs, devfs, procfs)
— the VFS layer returns `-EPERM` or `0` depending on policy.

### 4. Stamping hooks in VFS ops

Writing happens in:

- `.create` / `.mkdir` — set mtime = ctime = now on the new inode,
  and ctime = now on the parent directory.
- `.write` — set mtime = ctime = now on the file.
- `.truncate` / `.ftruncate` — set mtime = ctime = now.
- `.unlink` / `.rmdir` — set ctime = now on the parent directory.
- `.rename` — set ctime = now on both parents (source and dest).
- `.chmod` / `.chown` (future) — set ctime = now on the file.

These hooks live in each FS driver (they touch per-FS inode state).
Call `sys_time_now(&sec, NULL)` once at the top of the op and pass
the stamp down.

### 5. `SYS_UTIMES` / `SYS_UTIMENSAT`

Un-stub `SYS_UTIMES`:

```c
case SYS_UTIMES:
  ret = sys_utimes((uintptr_t)a0, (uintptr_t)a1);
  break;
```

Implementation wraps the new `.utimes` VFS op.  Pass `NULL` to stamp
current time (common `touch` case); otherwise honour the
user-supplied `struct timeval[2]`.

If musl or a future POSIX app wants `utimensat(dirfd, path, times,
flags)`, reserve `SYS_UTIMENSAT = 0x030A` (next free in group 0x03;
0x030A is currently `SYS_GETDENTS64` — double-check) and wire it.
Timekeeping-wise it's the same handler; only the struct shape
differs.

**Syscall number sanity check before landing:** Group 0x03 currently
ends at `SYS_GETDENTS64 = 0x030A`.  `SYS_UTIMENSAT` would land at
0x030B.

### 6. User ABI note

Growing `struct stat` (see §1) is an ABI break for every native
userland binary that calls `stat` / `fstat`.  Audit: `ls`, `cat`,
`push`, `pi`, `mkdir`, `rm`, `top`, `df` under `src/user/`, plus
test code under `tests/user/`.  All rebuild from source; no consumer
serializes the struct.  The break is contained to the T-1a landing
commit which rebuilds everything together.

### 7. `stat64` fill-in

`fill_stat64` currently memsets the buffer and skips the time
fields.  After step 6, it populates `st_mtime / st_mtime_nsec`
(nsec = 0), same for atime and ctime.  This immediately makes
busybox `ls -l` stop showing 1970 for everything.

### 8. Userland consumers

- Native `ls -l`: show mtime.  New column.  Use existing
  `uc_snprintf` and a small `format_time()` helper (`"YYYY-MM-DD
  HH:MM"` if mtime is after some sane threshold, `"MMM DD HH:MM"`
  otherwise — the common `ls` convention).  Lands alongside the
  stat-struct change so the column appears the moment users can see
  non-zero values.
- `touch` (Tier 2): becomes meaningful.  No-flag variant = stamp
  now; `-t` parses a user-supplied stamp.
- `stat` (potential future applet): trivial after this.

### 9. Calendar helpers

`ls -l` / `date` / `touch -t` all need `time_t → broken-down-time`
and the reverse.  PPAP has no `gmtime` / `localtime` today — a small
`uc_gmtime` / `uc_mktime` lands in `src/user/lib/uclib.c` as part of
the same body of work (or slightly ahead, if we want `date` sooner).

### 10. Migration plan (tiers)

The original draft split this into T-1 (kernel-internal `struct
kstat`, no user visibility) and T-2 (grow user `struct stat`).  The
split was dropped: every userland binary in this repo rebuilds from
source, so ABI staging buys nothing.  The collapsed plan grows
`struct stat` directly and lets "times show up as zero" be the
visible state until stamping hooks land.

- **T-1a — foundation.**  Grow native `struct stat` with
  `st_mtime / st_ctime / st_atime` (three `uint32_t`).  Update the
  six FS drivers' `.stat` to fill the new fields with zero.  Teach
  `fill_stat64` to copy them.  Add `uc_gmtime` / `uc_strftime`-ish
  helper to `src/user/lib/uclib`.  No stamping yet — users see
  1970-01-01 everywhere, same as busybox shows today.
- **T-1b — tmpfs stamping.**  Grow `tmpfs_inode_t` by three
  `uint32_t`; stamp on create/mkdir/write/truncate/unlink/rename.
- **T-1c — UFS stamping.**  Read `i_mtime/i_ctime/i_atime` in
  `ufs_stat`; write them in create/mkdir/write/truncate/unlink/
  rename.  No on-disk format change.
- **T-1d — devfs/procfs synthesis.**  Synthesize mtime/ctime as
  boot epoch.  atime = 0.  romfs stays at zero (build-time stamp
  is a separate follow-up).
- **T-1e — `ls -l` column.**  Native `ls -l` grows a mtime column
  using the T-1a helper.  Becomes visibly useful as T-1b/c/d land.
- **T-2 — explicit stamping.**  Un-stub `SYS_UTIMES`.  Add
  `SYS_UTIMENSAT` if needed.  `touch` applet (Tier 2 of the
  userland plan) becomes real.
- **T-3 — `date` applet.**  Wraps `clock_gettime` +
  `settimeofday`.  Uses the T-1a calendar helper.
- **T-4 — pcxt CMOS RTC seeding.**  Read the MC146818-compatible
  CMOS RTC at ports `0x70/0x71` during pcxt target init; convert
  BCD Y/M/D/h/m/s to epoch seconds; seed `time_boot_epoch`.  After
  T-4, pcxt boots with real wall-clock time and every file stamped
  by T-1/T-2 has a meaningful date, not just "seconds since boot".
  See the dedicated section below.

Each tier is independently committable.  T-1a is the load-bearing
foundation; T-1b–e are independent per-FS and userland commits.  T-4
is the one that makes the times *accurate* on the only target where
the hardware actually knows.

## T-4 — pcxt CMOS RTC seeding (detail)

The IBM PC/AT family exposes an MC146818-compatible RTC behind the
CMOS index/data port pair:

- `0x70` — index register (write-only).  Bit 7 masks NMI; keep it
  set while probing to avoid accidental NMI enable/disable side
  effects.
- `0x71` — data register.  Read or write the byte at the selected
  index.

Relevant CMOS indices:

| Idx | Field           |
|----:|-----------------|
| 0x00 | Seconds         |
| 0x02 | Minutes         |
| 0x04 | Hours           |
| 0x07 | Day of month    |
| 0x08 | Month           |
| 0x09 | Year (2 digits) |
| 0x32 | Century (some clones; 0x37 on others — probe both) |
| 0x0A | Status A (UIP)  |
| 0x0B | Status B (24h / BCD flags) |

Algorithm:

1. Read Status B (idx 0x0B) once.  Bit 2 = binary (clear = BCD);
   bit 1 = 24h (clear = 12h + PM bit in hours).  All real PCs and
   all pcxt-targeted emulators (QEMU pcxt, PCem, 86Box) report BCD
   + 24h by default — handle the non-default case only if we ever
   see it in the wild.
2. Wait until Status A (idx 0x0A) UIP bit (0x80) is clear to avoid
   reading a field mid-update.
3. Read sec/min/hr/day/mon/year.  Convert from BCD if Status B
   said so.
4. Read again; if any field differs, loop (UIP may have flipped
   between field reads on a slow bus).  This is the standard
   "read twice, compare" RTC pattern.
5. Pick a century: try idx 0x32, then 0x37, else hard-code 20
   (range 1980–2079 covers everything this OS will ever care
   about).
6. Convert (Y, M, D, h, m, s) → epoch seconds via a proleptic
   Gregorian `mktime`-equivalent — pure arithmetic, no DST, no
   timezone (we treat CMOS as UTC; users can offset themselves
   later).
7. Call into the kernel-internal seed hook (not the userland
   `sys_settimeofday`) to set `time_boot_epoch`.

### Where the code lives

- **RTC driver:** new file
  `src/target/pcxt/kernel/driver/rtc_cmos.c` exposing a single
  `cmos_rtc_read_epoch(uint32_t *out)` function that returns 0 on
  success, negative on failure.  Header next to it.
- **mktime helper:** share between kernel and userland.  The
  kernel side reuses the same arithmetic as `uc_mktime` from T-4,
  but the implementation lives in `src/common/` (header) +
  `src/common/time_conv.c` (or similar) so it is not built twice.
  The kernel links it directly; userland goes through uclib.
- **Boot hook:** `target_pcxt_early_init` (or wherever the target
  currently seeds other subsystems) calls
  `cmos_rtc_read_epoch(&sec)` and, on success, writes
  `time_boot_epoch = sec;`.  On failure (UIP timeout, all-zeros
  probe, impossible date), leave the epoch at 0 so the system
  degrades gracefully to "seconds since boot" — same behaviour as
  every other target.

### Writeback (out of scope for T-5)

`sys_settimeofday` after T-5 still only updates `time_boot_epoch`
in RAM; it does *not* write back to CMOS.  Adding CMOS writeback
is trivial (inverse of the read path) but raises the "who owns
the clock" question: if the user runs pcxt inside an emulator that
is also updating CMOS from the host, racing them is bad.  Defer
until someone actually wants `date -s` to persist across reboot.

### Non-goals for T-5

- **DST / timezones.**  Treat CMOS as UTC.  Adding a
  `TZ`-interpretation layer belongs in a user-space `tzset` /
  `localtime`, not in the kernel.
- **Century-byte autodetect across every clone.**  Emulators we
  target all use 0x32; hard-coding that plus a 20xx fallback is
  fine.
- **Update-in-progress polling with ACPI FADT.**  Too heavy.
  Read-twice-compare catches the same race for roughly zero code.

## Size impact

- **tmpfs**: +192 B BSS (3 × u32 × 16 inodes).
- **UFS**: 0 B on-disk (already reserved); driver grows a handful of
  bytes for the read/write plumbing.
- **romfs**: 0 B.
- **Per-applet**: `ls` grows a column + format_time helper — rough
  estimate +500 B text.  `touch` now does something useful,
  comparable to Tier 1 average (~23 KB on ARM / 7 KB on pcxt).
- **Kernel**: extending `struct kstat` and six FS drivers adds a few
  hundred bytes, most of it in fill_stat64 + the `.utimes` op
  dispatch.  Well within pcxt headroom.
- **T-5 pcxt RTC**: CMOS driver is ~200 B of text; the shared mktime
  helper is ~300 B.  Runs once at boot, no per-call cost.  Fits
  inside the pcxt 64 KB segment by a comfortable margin.

## Open Questions

- **Option (a) vs (b) for the VFS op signature.**  Leaning (a);
  decide at T-1 time.
- **romfs build-time epoch source.**  Pass via module init, or stamp
  into the romfs image header?  Current romfs superblock doesn't
  reserve a field; header extension is the cleaner path if we care
  about it at all.
- **Boot-time seed on other targets.**  T-5 covers pcxt (where CMOS
  is cheap and standard).  x68k already has an RTC driver for the
  Human68k subsystem — folding it into the generic seed hook is
  another small tier's worth of work and left as a follow-up.  Pico
  has no hardware clock and stays at epoch 0 unless someone calls
  `settimeofday` explicitly.
- **vfat timestamp reading.**  Out of first-landing scope — fine to
  return zeros initially, or do we want a small vfat patch in the
  same series?  Separate follow-up.

## Verification

- tmpfs: read mtime immediately after `write()`, assert it equals
  `clock_gettime(CLOCK_REALTIME)` within one tick.  Write the same
  file twice with a yield between, assert mtime strictly increases.
- UFS: same two asserts, run through a mount cycle (unmount /
  remount) to verify the on-disk stamp survives.  Use `mkimg.sh` to
  produce a fresh UFS image and inspect the inode via a small
  host-side tool.
- `touch` (T-3): stamp an existing file, read mtime back via stat,
  check match.  Pass `-t` with an arbitrary timestamp, check it
  lands in the stored mtime.
- Size: `./scripts/build.sh pcxt` romfs output before/after T-1 and
  T-2, reported in the landing commits.
