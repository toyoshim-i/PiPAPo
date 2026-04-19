# Proposal: Non-Blocking I/O as a First-Class Contract

## Summary

PPAP's blocking-I/O contract hides `-EAGAIN`/`-EINTR` behind a trap-level
IP-rewind trick that only works for user-space syscalls. Kernel-side callers
— the DOS, CP/M, and Human68k bridges — call the same VFS read/write
helpers directly and see a spurious `0` (interpreted as EOF) whenever the
underlying device would have blocked. Signal delivery during `read()` is
also fragile: the rewind swallows the signal and re-runs the syscall, so
user wrappers rarely see `-EINTR`.

This proposal makes `-EAGAIN`/`-EINTR` first-class return values at the
syscall boundary, drops the `return 0 + svc_set_restart + trap-IP-rewind`
dance for TTY/pipe read/write, and makes ia16's kernel thread scheduling
work cleanly by (a) giving ia16 a real synchronous cooperative yield and
(b) lifting the "no kernel preemption" restriction, with the two stub-side
preconditions that requires.

Scope: pcxt (ia16) first, since it surfaces both the blocking-contract
issue and the ia16-specific scheduling constraints. Other ports inherit the
VFS-layer changes automatically.

## Motivation

### Today's contract

Blocking sites in [tty.c](../../src/kernel/vfs/tty.c) and
[pipe.c](../../src/kernel/vfs/pipe.c) look like:

```c
current->wait_channel = t;
current->state = PROC_BLOCKED;
mod_core.svc_set_restart();
mod_core.sched_switch();
return 0; /* ignored — SVC restores original args */
```

This depends on the syscall trap handler
([src/arch/i16/kernel/core/trap.S:154-170](../../src/arch/i16/kernel/core/trap.S#L154-L170),
equivalent rewinds on ARM, C-level re-dispatch loop on m68k at
[target_qemu_m68k.c:80-86](../../src/target/qemu_m68k/kernel/core/target_qemu_m68k.c#L80-L86))
noticing `svc_needs_restart` and rewinding user IP by the size of the trap
instruction, so the syscall re-executes from scratch. User space never sees
the literal `0`.

### Where it breaks

1. **Kernel-side bridge callers.** DOS `dos_io_getc`, CP/M `cpm_fd_read`,
   Human68k `h68k_fd_read` all call `mod_vfs.fd_read` / `sys_read` directly.
   The `0` is a plain C return, not a syscall result — there is no trap
   frame to rewind, so the bridge treats it as EOF. Observed in zork1 on
   pcxt: `dos_buffered_input` exits with `len=0` on the first read and
   `len=1` after one character, because every block comes back as EOF
   mid-loop.

2. **Signal delivery during `read`.** If SIGINT arrives while a process is
   blocked in `tty_read_raw`, the wake path just does `return 0` too. The
   trap rewinds IP, `read()` re-executes — `tty_read_raw` now sees
   `sig_pending` *before* blocking and returns `-EINTR`. It works, but only
   because the re-entry path checks `sig_pending` again. User wrappers
   rarely see `-EINTR`; the shell's SIGINT handling works today largely by
   luck (and by its own `sigint_received` flag).

3. **No cooperative scheduling for polling DOS apps.** DOS programs that
   use AH=06h DL=FFh or AH=0Bh hammer the kernel in a tight loop. With the
   current contract the bridge either fakes "no char" constantly or truly
   blocks — neither lets the rest of the system run while the DOS app spins.

### ia16-specific wrinkles surfaced during Phase A implementation

The original proposal was a straight port of the Linux-style "loop inside
the VFS blocking site across `sched_switch`" pattern. It blew up on pcxt in
two ways:

a. **`sched_switch` on ia16 is flag-only, not synchronous.** `arch_yield()`
   sets `switch_pending = 1`; the actual context switch happens at the
   next timer ISR boundary. Looping in `tty_read_raw` and calling
   `sched_switch` never reaches that boundary because…

b. **…the timer ISR refuses to preempt kernel code.**
   Historically `i16_timer_can_preempt` returned 0 when the interrupted
   `SS` was 0 (unless the task was idle), per the "no fully reentrant
   kernel preemption model" comment in `switch.S`. So the loop spun at
   100% CPU forever.  Addressed by §6 below.

c. **Shared state in the core↔VFS far-call stubs.** `core_entries.S` and
   `vfs_entries.S` use static `saved_cs`/`saved_ip` globals to convert the
   compiler's near-call into a far-call ABI. They're non-reentrant by
   design: protected by `cli` against nested ISRs, but **not** against a
   context switch that resumes another process mid-stub. Any form of
   yield (cooperative or preempted) that hops away from a half-executed
   stub corrupts the globals.

The ia16 work below addresses (a), (b), and (c) so the cross-port VFS
change can actually land on pcxt.

## Design

### 1. VFS blocking sites return real status (cross-port)

In [tty.c](../../src/kernel/vfs/tty.c) (`tty_read_raw`, `tty_read_canon`,
`tty_write`) and [pipe.c](../../src/kernel/vfs/pipe.c) (`pipe_read`,
`pipe_write`), replace the `return 0` + `svc_set_restart` pattern with an
internal `while` loop around `sched_switch()`:

```c
for (;;) {
    int c = t->in();
    if (c >= 0) { /* ... process and return 1 ... */ }
    if (nonblock) return -(long)EAGAIN;
    if (current->sig_pending & ~current->sig_blocked) return -(long)EINTR;
    current->wait_channel = t;
    current->state = PROC_BLOCKED;
    mod_core.sched_switch();
    /* loop back and re-check */
}
```

Return values after the change:

- *N* bytes read/written on success,
- `-EAGAIN` when `O_NONBLOCK` is set and the device would block,
- `-EINTR` when a signal became deliverable during the wait.

The `svc_set_restart()` calls are dropped from these sites. Other blocking
syscalls (`sleep`, `waitpid`, `poll`) still use it; the `.Lcheck_restart`
block in `trap.S` is unchanged.

### 2. User-space handles `-EAGAIN` / `-EINTR`

- libc `read`/`write` wrappers surface `-EINTR` to the caller so user code
  can break out of `read` on SIGINT. POSIX-style auto-retry is a wrapper
  concern, not a kernel one.
- `O_NONBLOCK` callers get `-EAGAIN` as today.
- [push_line.c:755](../../src/user/push_line.c#L755) wraps its
  `read(0, &c, 1)` in a retry-on-`EINTR` `continue`, so the top-of-loop
  `sigint_received` check drives Ctrl-C handling deterministically.

### 3. DOS bridge — yield, don't spin

**`dos_io_getc` (AH=01h / 08h / 0Ah)** — blocking `sys_read`; retry on
`-EINTR`. Other processes run during the wait because the VFS-layer loop
actually yields (see §4).

**`dos_direct_console_io` (AH=06h DL=FFh)** — keep the existing
`O_NONBLOCK` toggle dance. On `-EAGAIN`, call `mod_core.sched_switch()` to
let other processes run a slice, then return `ZF=1, AL=0` to the DOS app.
The DOS app re-polls; the yield just prevents the tight poll loop from
starving the rest of the system.

**`dos_check_input_status` (AH=0Bh)** — check `in_avail()` without
consuming. If empty, yield and return "not ready". If ready, return "ready"
without consuming.

### 4. ia16: synchronous cooperative yield

`mod_core.sched_switch()` on ia16 must perform the context switch
*synchronously* from kernel thread context — otherwise §1's loop never
yields. Add [i16_sched_yield](../../src/arch/i16/kernel/core/switch.S) in
`switch.S`: a hand-built cooperative switch that constructs the same frame
layout as `i16_timer_isr` / `i16_syscall_isr` (fake IRET frame via `pushfw`
+ `pushw %cs` + push return-IP, then 9 GP pushes, then `user_SS=0` /
`user_SP = kernel SP`, then the 34-byte vfork reserve), saves `SP` to the
outgoing PCB, calls `sched_next`, loads the new PCB's `SP`, and jumps to
the shared restore tail. Wrapped in `cli`; `IRET` in the restore tail
reloads `FLAGS` so the caller's `IF` state survives.

`sched_switch()` on ia16 becomes:

```c
#elif defined(__ia16__)
  switch_pending = 0;
  i16_sched_yield();
```

and `signal_check` in the shared restore tail is gated on `user_SS != 0`
so kernel-mode frames (from yielded kernel threads) don't get a user-mode
signal-delivery frame planted onto the kernel stack.

### 5. ia16: fix stub reentrancy

Both [core_entries.S](../../src/target/pcxt/kernel/common/stubs/core_entries.S)
and [vfs_entries.S](../../src/target/pcxt/kernel/common/stubs/vfs_entries.S)
save the far-call `CS:IP` to static globals (`saved_cs`/`saved_ip` and
`vfs_saved_cs`/`vfs_saved_ip`) around the near C call. With any form of
yield landing mid-stub, another process's stub invocation overwrites those
globals; when the first process resumes, its `lret` jumps to the second
process's return address. Silent crash or hang.

**Fix (shadow-swap on context switch).** Keep the stub asm byte-for-byte
unchanged — the stubs already pop the far `CS:IP` off the stack before the
near C call, specifically so argument stack offsets match what every C
function expects, and reshuffling that is much more invasive than it looks.
Instead, add four new `uint16_t` fields to the PCB
(`core_stub_saved_cs` / `_ip`, `vfs_stub_saved_cs` / `_ip`) and swap them
against the four globals on every context switch — in both `i16_sched_yield`
(cooperative) and `i16_timer_isr` (preemption). The globals become the
"currently-live" working set; the PCB fields hold the snapshot for
not-currently-running processes. A switch between processes A and B
snapshots A's live values into A's PCB, then loads B's PCB values back into
the globals, so every stub sees only its own caller's return address.

All four globals move into core's `.bss` and are exported through
`core_exports.ld`, so VFS stubs resolve the same memory by fixed address.
Offsets are asserted in C:

```c
_Static_assert(offsetof(pcb_t, core_stub_saved_cs)
    == PCB_CORE_STUB_SAVED_CS_OFFSET, "stub offset must match asm");
/* same for _ip and vfs_stub_saved_* */
```

The `cli` window inside each stub still protects against same-process
nested ISRs. The shadow-swap protects against cross-process yields.

Trade-off: the context-switch paths now carry knowledge of the stub
globals (cross-layer coupling). The alternative — making the stubs
themselves keep `CS:IP` on the stack across the C call — would have
required shuffling argument offsets that every C function depends on,
which was judged a worse cost.

### 6. ia16: enable kernel preemption

With §5 in place, the timer ISR can safely context-switch from any kernel
state that does not hold hardware-level re-entrancy hazards. The stub
globals are no longer a shared-state hazard (§5), and the VFS blocking
sites already use per-process kernel stacks. Delete `i16_timer_can_preempt`
and its call site; the timer ISR checks `switch_pending` unconditionally
and performs the switch if requested, regardless of interrupted SS.

The remaining hazard is BIOS calls — the BIOS is a single pool of shared
state, so a timer-driven switch mid-BIOS-call could strand another process
inside the same call later. That is §7 below.

This gives us two independent ways to yield (cooperative via
`i16_sched_yield`, preemptive via the timer), so a busy kernel path that
forgets to yield doesn't hang the system.

### 7. ia16: BIOS-call serialization

BIOS calls on pcxt need to be atomic with respect to the scheduler to
avoid two processes interleaving inside shared BIOS state. The existing
pattern — mask IRQ 0 at the PIC around the INT — keeps the timer pending
until the BIOS call finishes. Current status:

- **INT 13h** in [bios_blk.c:44-67](../../src/target/pcxt/kernel/vfs/driver/bios_blk.c#L44-L67)
  — already masks IRQ 0. ✓
- **INT 16h** in [bios_con.c:389-415](../../src/target/pcxt/kernel/vfs/driver/bios_con.c#L389-L415)
  (×2) — not protected. Add the same `inb/outb` mask/restore bracket
  around both asm blocks.
- **BIOS INT 08h chain** inside our own timer ISR — called with `IF=0`
  from the ISR, no new re-entrancy concern.
- **VGA text writes** via mem-mapped I/O — no INT, no concern.

Any future BIOS call sites follow the same pattern.

## Phasing

1. **Phase A — VFS blocking loops (cross-port).** Convert `tty_read_raw`,
   `tty_read_canon`, `tty_write`, `pipe_read`, `pipe_write` to internal
   loops. Remove their `svc_set_restart` calls. No runtime effect on ia16
   yet (since `sched_switch` is flag-only and kernel preemption is off) —
   other ports start working immediately.

2. **Phase B+C — ia16 synchronous yield + reentrant stubs.** Landed as
   one commit. Adds `i16_sched_yield` in `switch.S`, routes
   `sched_switch()` through it on `__ia16__`, gates `signal_check` on
   `user_SS != 0`. Stub globals stay, but switch paths shadow-swap them
   against four new per-PCB fields. IRQ-0 mask added to `bios_con.c`'s
   two INT 16h paths at the same time (cheap to bundle; §7).

3. **Phase D — ia16 kernel preemption.** Drop `i16_timer_can_preempt`
   entirely; timer ISR always honours `switch_pending` regardless of
   whether it fired from user or kernel. Safe because §5 made the stubs
   reentrant and §7 keeps BIOS call sites off the preemption path via
   IRQ-0 masking.

4. **Phase E — user-space `-EINTR` handling.** Retry in
   [push_line.c](../../src/user/push_line.c). Audit
   [getty.c](../../src/user/getty.c), [pi_term.c](../../src/user/pi/pi_term.c),
   [pdb_util.c](../../src/user/pdb/pdb_util.c), [top.c](../../src/user/top.c)
   for `n <= 0` patterns that should `continue` on `-EINTR`.

5. **Phase F — DOS bridge.** Update `dos_io_getc`,
   `dos_direct_console_io`, `dos_check_input_status`. Verify zork1
   single-char input works and the DOS polling loop yields cleanly.

6. **Phase G — other bridges.** Same `-EINTR` retry (+ optional yield)
   pattern in `cpm_fd_read` /
   [cpm_bridge.c:36-43](../../src/kernel/core/subsys/cpm/cpm_bridge.c#L36-L43)
   and `h68k_fd_read` /
   [human68k_bridge.c:46-53](../../src/kernel/core/subsys/human68k/human68k_bridge.c#L46-L53).
   Latent same-bug; cheap to fix once Phase A lands.

7. **Phase H — other ports smoke test.** qemu_arm, qemu_m68k, qemu_rv32:
   the VFS change is shared, so these should Just Work. Exercise
   interactive paths and any read-blocking tests.

### Status

- Phase B+C landed: `i16: cooperative kernel-thread yield + reentrant
  entry stubs` (commit 9384d12). Includes the `bios_con.c` IRQ-0 mask
  originally scheduled for Phase D.
- Phase D landed: `i16_timer_can_preempt` removed; kernel code is now
  fully preemptible.
- Phase A + E landed: VFS read/write loops across `sched_switch` and
  returns `-EAGAIN`/`-EINTR`; `push_line` retries on `-EINTR`.  Fixes
  the zork1 single-key-input bug on pcxt.
- Phase F landed: DOS bridge retries on `-EINTR` in `dos_io_getc`,
  yields on `-EAGAIN` in `dos_direct_console_io`, yields in
  `dos_check_input_status`.
- Phases G, H: pending.

## Open questions

- **VFS-side yield once ia16 preemption is on.** §3's
  `dos_direct_console_io` yield on `-EAGAIN` is still useful for fairness
  even with preemption — it hands the CPU off immediately rather than
  waiting for the next tick. Keep it.
- **Other BIOS call sites later.** `INT 10h` video is not used on pcxt
  today (VGA is mem-mapped). If we add one, it gets the IRQ-0-mask
  pattern too.
- **CP/M / Human68k restart behavior.** Phase G's EINTR handling is
  straightforward; the per-bridge `sched_switch` yield-on-EAGAIN pattern
  is optional. Decide per subsystem whether DOS-style polling fairness is
  needed.

## Non-goals

- Not changing `fork` / `vfork` / `execve` or the scheduler data
  structures.
- Not adding full Linux-style signal restart (`SA_RESTART`); surface
  `-EINTR` and let callers decide.
- Not introducing `kqueue` / `epoll` or a new poll API. `sys_poll` keeps
  its current `svc_set_restart` behavior.
- Not touching non-TTY/pipe blocking sites.
- Not changing mod-segment layout or introducing a third module.
