# Reliable klog and Blocking TTY Write for PicoCalc

Observation: boot messages on pico1calc drop characters heavily.
Root cause: `tty_write` calls `uart_putc` per character; when the 255-byte
TX ring fills, `uart_putc` busy-spins draining to the HW FIFO.  At
115200 baud (~11.5 KB/s) a burst of boot output exceeds drain rate,
and the busy-spin blocks the calling process while other processes
continue generating output — characters are never dropped per se, but
the system stalls and interleaves output from multiple writers, and
the SysTick preemption during the busy loop can cause partial writes
to be lost.

---

## 1. Definitions

### Critical Section

A **critical section** is a region of code with two guarantees:

1. **Mutual exclusion** — at most one thread (or processor) can execute
   the region at any given time.
2. **No interruption** — while a thread is inside the region, it cannot
   be preempted by SysTick or interrupted by other IRQs.

On RP2040 this is implemented by `spin_lock_irqsave(lock)` /
`spin_unlock_irqrestore(lock, saved)`:

- `arch_irq_save()` saves PRIMASK and disables local interrupts
  (`cpsid i`), satisfying guarantee (2).
- The RP2040 hardware spinlock satisfies guarantee (1) across both
  cores — the caller busy-waits until the lock register returns
  non-zero.
- `spin_unlock_irqrestore()` releases the hardware lock and restores
  PRIMASK to its saved value.

On single-core targets (QEMU, m68k) disabling interrupts alone is
sufficient — there is no second processor to race against.

A critical section must be kept **short** — it disables all interrupts,
so long-running work inside it increases interrupt latency and can
starve time-sensitive peripherals.

---

## 2. Current Architecture

### tty_write (tty.c:251-268)

```
for each byte in user buffer:
    backend->putc(c)     ← uart_putc for ttyS0, fbcon_putc for tty1
return n                 ← always returns full count
```

- Never blocks, never returns short count.
- Calls `uart_putc` one character at a time.
- No flow control: writer runs as fast as the CPU.

### uart_putc (uart.c:213-272)

Polling mode (early boot):
- Spins on `UART0_FR & TXFF` then writes `UART0_DR`.

IRQ mode (after `uart_init_irq`):
- Enqueues to 255-byte ring, arms TXIM.
- **Ring full**: manually drains ring → FIFO in a busy loop.
- **FIFO also full**: spins polling `UART0_FR` without spinlock.
- No sleep, no wakeup — burns CPU until space appears.

### UART ISR (uart.c:323-363)

- TX: drains ring → FIFO under `SPIN_TXRING`, disarms TXIM when empty.
- RX: drains FIFO → RX ring, calls `tty_rx_notify()`.
- **No TX-space wakeup**: ISR has no way to unblock a sleeping writer.

### fbcon_putc (fbcon.c)

- Writes to in-memory cell grid (fast, never blocks).
- Deferred flush via `fbcon_flush_deferred()` → idle loop poll.
- No flow-control issue here — cell grid is the "buffer".

---

## 3. Proposed Design

The design is split into two layers:

1. **klog** — a reliable kernel logging facility that guarantees every
   character reaches the output buffer before returning, even from ISR
   or spinlock context.
2. **Userland TTY write** — a Unix-style blocking write that enqueues to
   a per-TTY output buffer and sleeps when the buffer is full.

Both layers share a **common stream interface** for the underlying
output devices (UART, fbcon).

### 3.1. Common Stream Interface

Each output backend (UART, fbcon) exposes a uniform interface:

```c
/* Try to enqueue data into the backend's internal buffer.
 * Returns the number of bytes actually accepted (0..n).
 * Never blocks — returns immediately even if no space. */
size_t stream_write(const void *buf, size_t n);

/* Consume/drain: move data from the internal buffer toward the
 * hardware (e.g. ring → UART FIFO).  Called to make room when
 * the buffer is full.  Returns the number of bytes drained. */
size_t stream_drain(void);
```

For **UART**:
- `stream_write()` copies bytes into the UART TX ring buffer. Returns
  the number of bytes accepted (may be less than `n` if the ring is
  full).
- `stream_drain()` moves bytes from the ring into the UART HW FIFO.
  Returns how many bytes were moved.
- Both functions must be called inside a critical section
  (`SPIN_TXRING`) because they modify the ring's head/tail indices.
  Without the critical section, concurrent access from the other core
  or an ISR could corrupt the indices or cause a deadlock (if the
  lock-owning thread is interrupted by an ISR that also tries to
  acquire the same lock).

For **fbcon**:
- `stream_write()` calls `fbcon_putc()` for each byte and always
  accepts all data (the cell grid is the buffer, and it never fills up).
- `stream_drain()` is a no-op (returns 0) — fbcon does not have a
  hardware output queue.

### 3.2. Reliable klog

`klog` / `klogf` must guarantee that every character of the message is
enqueued into the underlying stream's buffer before returning.  This is
critical for kernel diagnostics — a crash immediately after a klog call
must not lose the message.

Design:

```
klog(message):
    saved = spin_lock_irqsave(SPIN_UART)

    for each character c in message:
        while stream_write(&c, 1) == 0:
            stream_drain()          ← make room, then retry
        mirror_putc(c)              ← also send to fbcon if registered

    spin_unlock_irqrestore(SPIN_UART, saved)
    mirror_flush()                  ← flush fbcon outside critical section
```

Key properties:

- The entire message is emitted inside a **single critical section**.
  No other thread or ISR can interleave output or preempt the caller.
- If the stream's buffer is full, klog calls `stream_drain()` to push
  data toward the hardware, then retries.  This loop runs with
  interrupts disabled, so the drain must be a non-blocking poll
  (move ring → FIFO whenever the FIFO has room).
- klog does **not** need to wait for the hardware to finish
  transmitting all bytes.  It only needs to ensure every character is
  in the ring buffer.  The ISR (or a later drain call) will finish the
  actual transmission.
- The critical section is bounded: for a message of length L, the worst
  case is L × (ring-full drain cycles).  At 115200 baud this is fast
  enough for typical kernel messages (< 200 bytes).

This replaces the current `uart_putc`-per-character approach inside
klog.  The current klog already holds `SPIN_UART` for the entire call,
so the change is replacing per-character `uart_putc()` calls with
`stream_write()` + `stream_drain()` retry loop.

### 3.3. Userland TTY Write — Blocking I/O

Userland processes write to TTY devices via the `write()` syscall.
This follows the standard Unix I/O model:

```
tty_write(buf, n):
    written = 0
    while written < n:
        accepted = stream_write(buf + written, n - written)
        written += accepted

        if written < n:
            kick backend drain (arm TXIM for UART)
            sleep until stream signals "room available"
            ← process is marked PROC_BLOCKED
            ← woken by ISR/drain when buffer has space
            ← process is marked PROC_RUNNABLE, retries

    return written
```

Key properties:

- `stream_write()` returns the number of bytes accepted immediately.
  It does **not** block.
- If not all data was accepted (buffer full), the caller sleeps.  The
  backend raises an event when it has drained enough data to make room.
  The scheduler marks the process as PROC_RUNNABLE, and on its next
  scheduled timeslice the process retries the remaining data.
- This is the standard Unix blocking I/O pattern: write what you can,
  sleep when full, retry on wakeup.
- The `stream_write()` call from process context also uses a critical
  section (`SPIN_TXRING`) to safely modify the ring indices, just like
  the klog path.

For tty1 (fbcon), `stream_write()` always accepts all data, so the
process never sleeps.

### 3.4. UART Ring Buffer Concurrency

The UART TX ring buffer is accessed from three contexts:

1. **klog** (any core, interrupts disabled, holds `SPIN_UART` +
   `SPIN_TXRING`) — enqueues characters and drains to FIFO.
2. **tty_write** (any core, process context) — enqueues characters.
3. **UART ISR** (Core 0 only) — drains ring to FIFO, wakes blocked
   writers.

All three must use a critical section (`SPIN_TXRING`) when modifying
the ring's head/tail indices.  Without this:

- **Index corruption**: two concurrent writers could both read the same
  head value, write to the same slot, and advance head by only 1
  instead of 2.
- **Deadlock**: if a thread holds the ring lock and is interrupted by
  an ISR that also tries to acquire the same lock, the system hangs.
  The critical section prevents this by disabling interrupts before
  acquiring the lock — the ISR cannot fire while the lock is held.
- **IMSC race**: the UART0_IMSC register is modified by both process
  context (`|= TXIM` to arm) and ISR (`&= ~TXIM` to disarm).  Without
  the lock, a non-atomic read-modify-write on one core can clobber the
  other's update → TXIM stays cleared with data in the ring → TX stall.

### 3.5. SVC Restart and Write Resume

`svc_restart` replays the *original* syscall arguments, so a woken
write re-enters `tty_write` with the same `buf` and `n`.  We track
progress inside `tty_dev_t`:

```c
typedef struct {
    /* ... */
    const char *tx_user_buf;   /* current write source */
    size_t      tx_user_pos;   /* bytes already enqueued */
    size_t      tx_user_len;   /* total write length */
} tty_dev_t;
```

When SVC restarts:
1. Check `tx_user_buf == buf && tx_user_len == n` → resume from
   `tx_user_pos`.
2. Otherwise → new write, start from 0.

This matches how real Unix TTY drivers track offset across sleep/wake
cycles.

### 3.6. UART ISR — TX Drain + Wakeup

The ISR drains the ring → FIFO and wakes blocked writers:

```c
void UART0_IRQ_Handler(void)
{
    /* TX: drain tty TX ring → FIFO */
    if (UART0_IMSC & UART_IMSC_TXIM) {
        tty_dev_t *t = &tty_devs[TTY_SERIAL];
        uint32_t saved = spin_lock_irqsave(SPIN_TXRING);
        while (t->tx_head != t->tx_tail && !(UART0_FR & UART_FR_TXFF)) {
            UART0_DR = (uint8_t)t->tx_buf[t->tx_tail];
            t->tx_tail++;
        }
        if (t->tx_head == t->tx_tail)
            UART0_IMSC &= ~UART_IMSC_TXIM;
        spin_unlock_irqrestore(SPIN_TXRING, saved);
        /* Ring has space — wake any blocked writer */
        sched_wakeup(&t->tx_buf);
    }
    /* RX path unchanged */
}
```

### 3.7. tty1 (fbcon) — No Change

`fbcon_putc` writes to a cell grid in SRAM.  It never blocks and never
needs flow control.  tty1 continues using the direct-call path through
the stream interface (which always accepts all data).

---

## 4. Implementation Steps

### Step 1: Define the stream interface

- Add a `stream_ops` structure (or extend `tty_backend_t`) with
  `write(buf, n) → accepted` and `drain() → drained` callbacks.
- Implement UART stream ops:
  - `uart_stream_write()`: copy bytes into UART TX ring under
    `SPIN_TXRING`, return count accepted.
  - `uart_stream_drain()`: move bytes from ring to HW FIFO under
    `SPIN_TXRING`, return count drained.
- Implement fbcon stream ops:
  - `fbcon_stream_write()`: call `fbcon_putc()` per character, always
    return n.
  - `fbcon_stream_drain()`: no-op, return 0.
- The UART TX ring stays in uart.c (not moved to tty_dev_t) — it is
  internal to the UART stream implementation.

### Step 2: Make klog reliable

- Replace the per-character `uart_putc()` calls inside `klog()` and
  `klogf()` with the stream interface.
- Inside the existing `SPIN_UART` critical section, use
  `stream_write()` + `stream_drain()` retry loop to guarantee all
  characters are enqueued before returning.
- `klog_putc()` becomes: try `stream_write(&c, 1)`; if 0, call
  `stream_drain()` and retry.
- The mirror (fbcon) path remains the same: `mirror_putc(c)` inside
  the critical section, `mirror_flush()` outside.
- Verify: a klog message followed by an immediate crash must not lose
  characters (they are all in the ring buffer).

### Step 3: Implement blocking tty_write

- For TTY instances backed by UART (ttyS0):
  - Call `stream_write(buf, n)` to enqueue as much as possible.
  - If not all data accepted, arm TXIM and sleep (`PROC_BLOCKED`) on
    a wait channel (e.g. `&t->tx_buf` or a dedicated `tx_waitq`).
  - Track write progress in `tty_dev_t` (`tx_user_buf`, `tx_user_pos`,
    `tx_user_len`) for SVC restart resume.
  - On wakeup, retry remaining data.
- For TTY instances backed by fbcon (tty1):
  - Keep the current direct-call path — `stream_write()` always
    accepts all data, so the process never sleeps.
- OPOST expansion (\n → \r\n) happens in tty_write before calling
  `stream_write()`.

### Step 4: UART ISR drain + wakeup

- Modify the UART ISR to drain the TX ring → FIFO under `SPIN_TXRING`.
- After draining, if the ring now has space, call
  `sched_wakeup(wait_channel)` to wake any blocked writer.
- `sched_wakeup` only sets process state to PROC_RUNNABLE — it is safe
  from ISR context.
- Remove uart.c's internal `uart_putc` busy-spin-on-ring-full path for
  the tty_write code path.  `uart_putc` itself remains for klog (which
  uses the stream interface now) and echo.

### Step 5: Wire up and test

- Verify boot messages appear without drops on pico1calc.
- Verify `cat /proc/stat` and other long-output commands work.
- Verify klog still works from ISR context (e.g., trigger a klog from
  a spinlock-held path and confirm no deadlock, no lost characters).
- Verify tty1 (fbcon) is unaffected.
- Verify echo (tty_read_canon → putc) still works for single
  characters.
- Run qemu_arm and qemu_m68k test suites (they use polling UART,
  should be unaffected — stream_write for polling mode can fall back
  to direct HW register writes).

---

## 5. Considerations

**QEMU and m68k**: These targets use polling UART drivers.  The stream
interface for polling mode implements `stream_write()` as direct writes
to the HW FIFO (spin until FIFO has room, write byte), same as current
`uart_putc` polling mode.  No ring buffer, no ISR, no sleep.

**Dual-core scheduling**: Both cores run user processes symmetrically
via `sched_next()`.  Any process doing `write(fd, ...)` on ttyS0 can
be running on either Core 0 or Core 1.  This is safe:

- The TX ring is accessed under `SPIN_TXRING`, which serialises updates
  from both cores.
- When a process on Core 1 blocks (`PROC_BLOCKED`), the UART ISR on
  Core 0 calls `sched_wakeup()` → sets the process to `PROC_RUNNABLE`.
  Core 1's PendSV picks it up on the next context switch opportunity.
- `sched_wakeup` acquires `SPIN_PROC` and calls `arch_yield()` (pends
  PendSV), which is safe from ISR context.

What *is* Core-0-specific (and stays that way):
- UART0 IRQ is routed to Core 0 only (RP2040 NVIC is per-core).
- Global tick counter and sleep wakeups run on Core 0.
- Input polling (`input_poll_due`) is deferred to Core 0's idle loop.

**klog vs tty_write lock ordering**: klog holds `SPIN_UART` and
internally acquires `SPIN_TXRING` (via `stream_write` / `stream_drain`).
tty_write only acquires `SPIN_TXRING`.  The ISR acquires `SPIN_TXRING`.
Lock ordering is: `SPIN_UART` → `SPIN_TXRING` (never reversed).  This
prevents deadlock.

**Echo path**: `tty_read_canon` calls `t->out(c)` for echo.  This goes
through `uart_putc` (busy-spin), not the blocking stream.  This is fine
— echo is single characters, not bulk output.

**Signal handling**: If the writer is blocked on TX and receives SIGINT,
the `dev_send_signal` path already wakes PROC_BLOCKED processes and
the interrupted write returns EINTR.

**Ring size**: 256 bytes matches the current uart.c ring.  Could be
increased to 512 or 1024 for better throughput, but 256 is sufficient
for the 115200 baud rate and keeps BSS small.
