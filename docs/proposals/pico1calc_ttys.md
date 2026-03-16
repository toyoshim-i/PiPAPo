# Blocking TTY Write for PicoCalc Serial Console

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

## 1. Current Architecture

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

## 2. Proposed Design

Follow the traditional Unix model: TTY write enqueues to a kernel-side
TX buffer and sleeps when the buffer is full.  The UART ISR drains the
buffer and wakes the sleeping writer when space becomes available.

### 2.1. TX Output Ring in tty_dev_t

Add a per-TTY output ring buffer to `tty_dev_t`:

```c
#define TTY_TX_SIZE  256u   /* power of 2 */

typedef struct {
    /* existing fields ... */

    /* TX output ring — written by tty_write, drained by backend */
    char     tx_buf[TTY_TX_SIZE];
    volatile uint8_t tx_head;   /* written by writer (process context) */
    volatile uint8_t tx_tail;   /* advanced by drain (ISR or poll)     */
} tty_dev_t;
```

For ttyS0, the existing uart.c ring buffer is replaced by this ring.
For tty1 (fbcon), the ring is not needed — `fbcon_putc` is instant —
so tty1 keeps the current direct-call path.

### 2.2. tty_write — Blocking with Short Writes

```c
static long tty_write(struct file *f, const char *buf, size_t n)
{
    tty_dev_t *t = f->priv;
    if (!t || !t->out)
        return n;   /* discard if no backend */

    /* tty1 (fbcon): direct call, no ring needed */
    if (!t->tx_buf)
        return tty_write_direct(t, buf, n);

    size_t written = 0;
    while (written < n) {
        /* Try to fill the TX ring */
        while (written < n && tx_ring_space(t) > 0) {
            char c = buf[written];
            if (opost && c == '\n' && (t->termios.c_oflag & ONLCR))
                tx_ring_put(t, '\r');
            tx_ring_put(t, c);
            written++;
        }
        /* Kick the backend if it wasn't already draining */
        tty_tx_kick(t);

        if (written < n) {
            /* Ring full — sleep until ISR drains some */
            current->wait_channel = &t->tx_buf;
            current->state = PROC_BLOCKED;
            set_svc_restart();
            sched_yield();
            return (long)written;   /* short write — SVC restarts with remaining */
        }
    }
    return (long)written;
}
```

Key properties:
- Returns **short count** when blocked (matches pipe_write pattern).
- Uses `svc_restart` so the syscall re-executes with original args.
  The `written` count from the first pass is consumed; SVC restarts
  the write with `buf + written`, `n - written`.

Actually, `svc_restart` replays the *original* syscall arguments, so
we must handle this differently. Two options:

**Option A — Restart from beginning, idempotent ring fill.**
When woken, the syscall re-executes from the top. Characters already
in the ring are not duplicated because they've been drained by the ISR.
The process just fills the ring again from `buf[0]`. This means some
bytes are written twice to the ring — wasteful but correct if we
re-enter from position 0.

This doesn't work: the user buffer hasn't changed, but bytes 0..written
would be duplicated in the output stream.

**Option B — Write everything, block in the middle.**
`tty_write` fills the ring as far as it can, then blocks. When woken,
the restart re-executes with original args. We need to track progress
inside `tty_dev_t` so the restart knows where to resume:

```c
typedef struct {
    /* ... */
    const char *tx_user_buf;   /* current write source */
    size_t      tx_user_pos;   /* bytes already enqueued */
    size_t      tx_user_len;   /* total write length */
} tty_dev_t;
```

When SVC restarts:
1. Check `tx_user_buf == buf && tx_user_len == n` → resume from `tx_user_pos`.
2. Otherwise → new write, start from 0.

This is the cleaner approach and matches how real Unix TTY drivers work
(the `struct uio` tracks offset across sleep/wake cycles).

### 2.3. UART ISR — TX Drain + Wakeup

The ISR already drains the ring → FIFO. We add a wakeup call:

```c
void UART0_IRQ_Handler(void)
{
    /* TX: drain tty TX ring → FIFO */
    if (UART0_IMSC & UART_IMSC_TXIM) {
        tty_dev_t *t = &tty_devs[TTY_SERIAL];
        while (t->tx_head != t->tx_tail && !(UART0_FR & UART_FR_TXFF)) {
            UART0_DR = (uint8_t)t->tx_buf[t->tx_tail];
            t->tx_tail++;
        }
        if (t->tx_head == t->tx_tail)
            UART0_IMSC &= ~UART_IMSC_TXIM;
        /* Ring has space — wake any blocked writer */
        sched_wakeup(&t->tx_buf);
    }
    /* RX path unchanged */
}
```

The wakeup is cheap (scans proc_table for matching wait_channel) and
safe from ISR context — `sched_wakeup` only sets state to RUNNABLE.

### 2.4. uart_putc — Dual Role

`uart_putc` is still needed for:
- **klog/klogf**: kernel messages from ISR or spinlock context (cannot sleep).
- **tty_read echo**: echoing input characters back to the terminal.

These callers cannot block, so `uart_putc` keeps its current busy-spin
behaviour. Only `tty_write` (process context, syscall path) uses the
blocking ring.

### 2.5. tty1 (fbcon) — No Change

`fbcon_putc` writes to a cell grid in SRAM. It never blocks and never
needs flow control. tty1 continues using the direct-call path.

---

## 3. Implementation Steps

### Step 1: Add TX ring to tty_dev_t

- Add `tx_buf[256]`, `tx_head`, `tx_tail`, `tx_user_pos`, `tx_user_len`
  to `tty_dev_t`.
- Add a `tx_wakeup` flag or use `&t->tx_buf` as wait channel.
- Add `tty_backend_t.tx_drain` callback (or reuse existing `putc` for
  fbcon, add a new `drain` for UART that moves ring → FIFO).

### Step 2: Implement blocking tty_write

- For TTY instances with a TX ring (ttyS0): fill ring, kick drain,
  sleep if full, resume via `tx_user_pos`.
- For TTY instances without a TX ring (tty1): keep current direct putc loop.
- OPOST expansion happens before ring insertion.

### Step 3: UART ISR drains tty TX ring

- Move TX drain from uart.c's own ring to the tty_dev_t ring.
- Remove `uart.c`'s internal `tx_buf`/`tx_head`/`tx_tail`.
- After draining, call `sched_wakeup(&t->tx_buf)`.
- `uart_putc` keeps its own direct-to-FIFO-or-busy-spin path for klog.

### Step 4: Wire up and test

- Verify boot messages appear without drops on pico1calc.
- Verify `cat /proc/stat` and long output commands work.
- Verify klog still works from ISR context.
- Verify tty1 (fbcon) is unaffected.
- Run qemu_arm and qemu_m68k test suites (they use polling UART,
  should be unaffected).

---

## 4. Considerations

**QEMU and m68k**: These targets use polling UART drivers. `tty_write`
should detect when there's no ISR-driven drain and fall back to the
current direct-putc path. Simplest: only use the TX ring when the
backend has a `drain` callback.

**Dual-core scheduling**: Both cores run user processes symmetrically
via `sched_next()`.  Any process doing `write(fd, ...)` on ttyS0 can
be running on either Core 0 or Core 1.  This is fine:

- The TX ring in `tty_dev_t` is accessed under `SPIN_TXRING`, which
  serialises updates from both cores.
- When a process on Core 1 blocks (`PROC_BLOCKED`), the UART ISR on
  Core 0 calls `sched_wakeup()` → sets the process to `PROC_RUNNABLE`.
  Core 1's PendSV picks it up on the next context switch opportunity.
- `sched_wakeup` acquires `SPIN_PROC` and calls `arch_yield()` (pends
  PendSV), which is safe from ISR context.

What *is* Core-0-specific (and stays that way):
- UART0 IRQ is routed to Core 0 only (RP2040 NVIC is per-core).
- Global tick counter and sleep wakeups run on Core 0.
- Input polling (`input_poll_due`) is deferred to Core 0's idle loop.
- `klog`/`klogf` use `uart_putc` (busy-spin), not the blocking ring.
  These are safe from either core — they hold `SPIN_UART`.

**Echo path**: `tty_read_canon` calls `t->out(c)` for echo. This goes
through `uart_putc` (busy-spin), not the TX ring. This is fine — echo
is single characters, not bulk output.

**Signal handling**: If the writer is blocked on TX and receives SIGINT,
the `dev_send_signal` path already wakes PROC_BLOCKED processes and
the interrupted write returns EINTR.

**Ring size**: 256 bytes matches the current uart.c ring. Could be
increased to 512 or 1024 for better throughput, but 256 is sufficient
for the 115200 baud rate and keeps BSS small.
