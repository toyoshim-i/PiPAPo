# Proposal: High-Resolution Timer Subsystem

## Summary

`sys_nanosleep`, `sys_clock_nanosleep32`, and `sys_clock_nanosleep64` carry
nanosecond-precision interfaces but deliver only 10 ms resolution.  The
limitation is structural: the wakeup path is the periodic preemption tick
(`sched_tick()` scanning `proc_table` every 10 ms for expired
`sleep_until`), not a deadline-driven event source.

This proposal adds a hardware-backed one-shot timer subsystem that decouples
sleep resolution from the preemption tick.  The continuation-blocking loops
already present in `sys_nanosleep`, `sys_clock_nanosleep32/64`, `sys_ppoll`,
and `sys_waitpid` have the right shape — only the blocking primitive
changes.

## Motivation

- POSIX `nanosleep(0, 1)` rounds up to 10 ms today.  Real-world workloads
  (e.g. retro emulators emitting audio frames, animation frame pacing,
  protocol back-offs in user-space network stacks) lose accuracy.
- `sched_tick()` does O(PROC_MAX) work every tick to find expired sleepers
  and poll timeouts.  This cost scales with PROC_MAX even when nothing is
  sleeping.
- `sys_ppoll` and `sys_wait4` timeouts share the same 10 ms floor for the
  same reason.
- `pcb_t.sleep_until` is per-process and tick-typed; there is no shared
  data structure for "what is the next timed event in the system?", which
  is what every modern kernel uses to drive low-power idle.

A deadline-driven timer subsystem fixes all three issues with one change.

## Design

### Counter source

Each architecture exposes a free-running monotonic counter in microseconds:

| Arch | Source |
|------|--------|
| ARM Cortex-M (RP2040) | TIMER peripheral (1 µs, 64-bit) |
| ARM Cortex-M (mps2-an500) | SysTick CVR or DWT CYCCNT |
| RISC-V (Hazard3 / virt) | `mtime` (already monotonic) |
| Motorola 68000 (virt / X68K) | MFP timer C / virt PIT |
| Intel 8086 | PIT channel 2 in latched mode |
| Xtensa LX7 | CCOUNT |

Common interface:

```c
typedef uint64_t hrtimer_us_t;
hrtimer_us_t hrtimer_now(void);
```

### One-shot wakeup

A second hardware interface programs a one-shot interrupt:

```c
void hrtimer_arm(hrtimer_us_t deadline);  // fire at or after deadline
void hrtimer_disarm(void);
```

The interrupt handler calls `hrtimer_expire()`, which pops all timer-queue
entries whose `deadline <= now()` and wakes their owners.

### Per-process state

`pcb_t` gains:

```c
hrtimer_us_t timer_deadline;   // 0 = no pending wakeup
pcb_t *timer_next;             // sorted-list link
```

A single kernel-wide `timer_head` points at the soonest-expiring waiter.
Insertion and removal are O(N) over current waiters (≤ PROC_MAX, small).
A binary heap can replace the list if PROC_MAX grows.

### Blocking primitive

Replace the current "set `sleep_until` + `PROC_SLEEPING` + spin loop with
proc_table scan" pattern with:

```c
void sched_wait_timeout(hrtimer_us_t deadline);
```

- Inserts `current` into the timer queue.
- Sets `current->state = PROC_SLEEPING`.
- Calls `sched_switch()`.
- On return, removes `current` from the timer queue (already removed if the
  timer fired; idempotent otherwise — woken by signal).

Sleep syscalls then become:

```c
long sys_nanosleep(uintptr_t req_ptr, uintptr_t rem_ptr) {
  /* validate, compute deadline_us once */
  hrtimer_us_t dl = hrtimer_now() + ns_to_us(req->tv_sec, req->tv_nsec);
  for (;;) {
    if (current->sig_pending & ~current->sig_blocked)
      return -EINTR_with_rem(dl, rem_ptr);
    if ((int64_t)(hrtimer_now() - dl) >= 0) return 0;
    sched_wait_timeout(dl);
  }
}
```

The loop shape is identical to the existing sleep / poll syscall bodies —
only the wait primitive changes.

### Preemption tick

`sched_tick()` keeps its time-slice accounting role
(`ticks_remaining`, `cpu_*_ticks[]`) but loses the proc_table sleep scan.
The 100 Hz periodic interrupt becomes purely a quantum boundary.  Tickless
operation (programming the next quantum boundary on the one-shot too) is a
later refinement, not part of this proposal.

## Migration

### Phase A — Counter source

Per-arch `hrtimer_now()` returning microseconds.  No semantic change yet;
just a callable function that other code can use for timing.  Wire it into
`/proc/uptime` and `clock_gettime(CLOCK_MONOTONIC)` for validation.

### Phase B — One-shot timer + queue

Add `hrtimer_arm` / `hrtimer_disarm` per arch.  Add the kernel-wide sorted
queue, `sched_wait_timeout`, and `hrtimer_expire`.  No syscall changes yet
— the new primitive lives alongside the old `sleep_until` mechanism.

### Phase C — Convert sleep and poll/wait timeouts

- `sys_nanosleep`, `sys_clock_nanosleep32/64` switch from
  `sched_switch()` to `sched_wait_timeout()`.
- `sys_ppoll` and `sys_wait4` timeout paths likewise.
- Delete `pcb_t.sleep_until`, the `sched_tick()` proc_table scan, and the
  poll-timeout block that consults it.

### Phase D — Hardware coverage

Backfill any arch that booted on a stubbed `hrtimer_arm` (returning
immediately).  Verify resolution and jitter on real hardware (Pico, Pico 2,
PicoCalc, X68K, CardComputer).

## Risk notes

- **Counter overflow**: 32-bit µs counters wrap at ~71 minutes.  Either
  widen via software (32 → 64 bit accumulator updated each interrupt) or
  use 64-bit hardware counters where available.  `mtime` and RP2040 TIMER
  are already 64-bit; m68k MFP and i8086 PIT need software extension.
- **ISR overhead**: arming a one-shot on every sleep entry costs a few
  hundred cycles.  Acceptable; sleep is already a heavy operation.
- **Race between arm and switch**: `hrtimer_arm` must happen before the
  process is taken off-CPU.  Easiest: arm inside `sched_wait_timeout`
  with interrupts disabled, then unmask on `sched_switch` entry.
- **Power**: a one-shot programmed far in the future allows the idle loop
  to enter deep sleep until either the timer or an external interrupt
  fires.  Future tickless work builds on this.

## Out of scope

- Tickless operation (`NO_HZ`) — possible follow-up.
- Per-CPU timer state for SMP — current cores are dual on RP2040/2350 but
  ppap_sched is single-core for now.
- POSIX timers (`timer_create`, `setitimer`).  Same infrastructure can host
  them later.
- Real-time scheduling guarantees (`SCHED_FIFO`, etc.).  Out of scope.

## Relationship to existing code

The sleep and poll syscall bodies already run a continuation-blocking
loop around `sched_switch()`.  Only that `sched_switch()` call site
needs to become `sched_wait_timeout(dl)` once Phase C of this proposal
lands.  No further rewrite of the syscall bodies is required.
