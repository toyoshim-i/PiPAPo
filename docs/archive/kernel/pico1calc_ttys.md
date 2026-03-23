# Reliable klog and Blocking TTY Write for PicoCalc

**Status: Implemented** (closes #4)

Observation: boot messages on pico1calc drop characters heavily.
Root cause: `tty_write` called `uart_putc` per character; when the
255-byte TX ring filled, the caller busy-spun without sleeping —
burning CPU while SysTick preemption caused interleaved and lost
output.

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

## 2. Unified Backend Interface

All character output backends conform to the `tty_backend_t` interface
(`src/kernel/fd/tty.h`):

```c
typedef struct {
    int  (*putc)(char c, void (*notify)(void));
    void (*flush)(void);        /* NULL if not needed */
    int  (*getc)(void);
    int  (*rx_avail)(void);
    int  (*get_cols)(void);     /* NULL → default */
    int  (*get_rows)(void);     /* NULL → default */
} tty_backend_t;
```

### putc contract

- Returns **1** on success, **0** if the backend's buffer is full.
- Never blocks — returns immediately.
- If returning 0 and `notify != NULL`, the backend registers `notify`
  **atomically** (inside the same critical section) and calls it from
  ISR context when space becomes available.

### Implementations

| Backend | File | Blocks? | notify used? |
|---------|------|---------|-------------|
| RP2040 UART | `src/drivers/arch/arm_m/uart_rp2040.c` | yes (ring full) | yes — ISR calls it |
| QEMU ARM UART | `src/target/qemu_arm/drivers/uart_qemu.c` | no (direct HW write) | ignored |
| QEMU m68k UART | `src/target/qemu_m68k/drivers/uart_qemu_m68k.c` | no (direct HW write) | ignored |
| x68k TVRAM | `src/target/x68k/drivers/uart_x68k.c` | no (IOCS sync) | ignored |
| x68k RS-232C | `src/target/x68k/drivers/uart_x68k.c` | no (IOCS sync) | ignored |
| fbcon (LCD) | `src/drivers/fbcon.c` | no (SRAM grid) | ignored |

### flush contract

- Triggers deferred output rendering (e.g. `fbcon_flush_deferred()`
  for the LCD backend).  Called after echo and write completion.
- NULL for backends with no deferred rendering (UART, TVRAM).

---

## 3. Reliable klog

`klog` / `klogf` guarantee that every character of the message is
enqueued before returning.  This is critical for kernel diagnostics —
a crash immediately after a klog call must not lose the message.

```
klog(message):
    arch_preempt_disable()      ← disable SysTick only; UART ISR stays active
    spin_lock(SPIN_UART)

    for each character c in message:
        while !uart_putc(c, NULL):
            ;                   ← ISR drains ring → FIFO asynchronously
        mirror_putc(c, NULL)    ← also send to fbcon if registered

    spin_unlock(SPIN_UART)
    arch_preempt_enable()
    mirror_flush()              ← flush fbcon outside critical section
```

Key properties:

- The entire message is emitted inside a **single critical section**
  (`SPIN_UART`).  No other thread can interleave output.
- Only SysTick TICKINT is disabled (`arch_preempt_disable`).  The UART
  ISR remains active so it can drain the TX ring while klog spins on a
  full ring.
- `uart_putc` under the hood acquires `SPIN_TXRING` per character,
  enqueues to the ring, and kicks bytes directly into the HW FIFO.
  When the ring is full, it attempts to drain ring → FIFO inline.  If
  both ring and FIFO are full, it returns 0 and klog retries.
- The critical section is bounded: for a message of length L, the worst
  case is L × (ring-full drain cycles).  At 115200 baud this is fast
  enough for typical kernel messages (< 200 bytes).

---

## 4. Userland TTY Write — Blocking I/O

Userland processes write to TTY devices via the `write()` syscall
(`tty_write` in `src/kernel/fd/tty.c`):

```
tty_write(buf, n):
    resume pos from SVC restart state (or 0 for new write)

    while pos < n:
        if OPOST && buf[pos] == '\n':
            if !t->out('\r', t->tx_wakeup): goto block
        if !t->out(buf[pos], t->tx_wakeup): goto block
        pos++

    clear resume state
    flush backend
    return n

block:
    save (buf, pos, n) in tty_dev_t
    set PROC_BLOCKED, set_svc_restart()
    sched_yield()
    ← ISR fires notify → sched_wakeup → process retries from pos
```

Key properties:

- `putc` returns 1 or 0 immediately — never blocks inside the backend.
- When `putc` returns 0 (ring full), the `notify` callback was already
  registered atomically inside that same `putc` call.  The caller
  saves progress and sleeps.
- The ISR fires the callback (`sched_wakeup`) when ring space opens.
  SVC restart replays the original syscall args; `tty_write` resumes
  from the saved `tx_user_pos`.
- For fbcon backends, `putc` always returns 1 — the process never
  sleeps.
- OPOST expansion (`\n` → `\r\n`) happens in `tty_write` before
  calling the backend.

### SVC Restart and Write Resume

`svc_restart` replays the *original* syscall arguments, so a woken
write re-enters `tty_write` with the same `buf` and `n`.  Progress is
tracked inside `tty_dev_t`:

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

---

## 5. RP2040 UART Driver

Located in `src/drivers/arch/arm_m/uart_rp2040.c`.

### uart_putc

```c
int uart_putc(char c, void (*notify)(void))
{
    saved = spin_lock_irqsave(SPIN_TXRING);

    if ring full:
        drain ring → FIFO inline
        if still full:
            arm TXIM, register notify callback
            unlock; return 0

    enqueue c to ring

    /* Kick: drain ring → FIFO while FIFO has space */
    while ring non-empty && FIFO not full:
        write ring byte to UART0_DR
    if ring still non-empty:
        arm TXIM for ISR continuation

    unlock; return 1
}
```

The "kick" (direct ring → FIFO drain) is essential for PL011's
**transition-based** TX interrupt: TXRIS only asserts when the FIFO
level drops from above the watermark to at-or-below it.  Without
writing bytes directly into the FIFO, the watermark transition never
occurs and the ISR never fires.

### UART ISR

```c
void UART0_IRQ_Handler(void)
{
    /* TX: drain ring → FIFO */
    if (UART0_IMSC & UART_IMSC_TXIM) {
        saved = spin_lock_irqsave(SPIN_TXRING);
        drain ring → FIFO
        if ring empty: disarm TXIM
        unlock;
        if (tx_notify_cb) tx_notify_cb();   /* wake blocked writer */
    }

    /* RX: drain FIFO → ring, notify TTY layer */
    while FIFO not empty:
        read byte; handle Ctrl-C; enqueue to RX ring
    tty_rx_notify(TTY_SERIAL);
}
```

### Boot sequence (12 MHz → 133 MHz PLL)

1. `uart_init()` — UART at 115200 @ 12 MHz XOSC
2. `uart_tx_drain()` — flush TX ring at 12 MHz baud rate
3. `clock_init_pll()` — switch clk_sys to 133 MHz
4. `uart_reinit_133mhz()` — reconfigure baud divisors for 133 MHz

`uart_tx_drain` and `uart_reinit_133mhz` are RP2040-specific,
declared in `src/drivers/arch/arm_m/uart_rp2040.h`.

---

## 6. Ring Buffer Concurrency

The UART TX ring buffer is accessed from three contexts:

1. **klog** (any core, preemption disabled, holds `SPIN_UART` +
   `SPIN_TXRING`) — enqueues characters and drains to FIFO.
2. **tty_write** (any core, process context, holds `SPIN_TXRING` via
   `uart_putc`) — enqueues characters.
3. **UART ISR** (Core 0 only, holds `SPIN_TXRING`) — drains ring to
   FIFO, wakes blocked writers.

All three use `spin_lock_irqsave(SPIN_TXRING)`.  Without this:

- **Index corruption**: two concurrent writers could both read the same
  head value, write to the same slot, and advance head by only 1.
- **Deadlock**: if a thread holds the ring lock and is interrupted by
  an ISR that also tries to acquire the same lock, the system hangs.
- **IMSC race**: UART0_IMSC is modified by both process context
  (`|= TXIM`) and ISR (`&= ~TXIM`).  Without the lock, a non-atomic
  RMW can clobber the other's update.

**Lock ordering**: `SPIN_UART` (4) → `SPIN_TXRING` (5).  Never reversed.

---

## 7. Considerations

**QEMU and m68k**: These targets use polling UART drivers.  `uart_putc`
writes directly to the HW data register (spins until ready).  Always
returns 1.  No ring buffer, no ISR, no sleep.

**Dual-core scheduling**: Both cores run user processes via
`sched_next()`.  The TX ring is accessed under `SPIN_TXRING`, which
serialises updates from both cores.  When a process on Core 1 blocks,
the UART ISR on Core 0 calls the wakeup callback → sets process to
PROC_RUNNABLE.

**Echo path**: `dev_echo` in tty_read calls `t->out(c, NULL)` with
no notify callback.  This busy-spins if the ring is full.  Acceptable
for single-character echo.  `dev_echo_flush` calls the backend's
`flush` callback to trigger deferred rendering (fbcon LCD).

**Signal handling**: If the writer is blocked on TX and receives SIGINT,
`dev_send_signal` wakes PROC_BLOCKED processes and the interrupted
write returns EINTR (or a short count if bytes were already enqueued).

**fbcon flush**: `fbcon_putc` sets `flush_pending = 1` on every
character.  The scheduler's idle loop calls `fbcon_poll_flush()` every
~10 ms to render dirty rows via SPI.  Additionally, `dev_echo_flush`
calls `fbcon_flush_deferred()` for immediate echo visibility.

**Ring size**: 256 bytes (uint8_t index wraparound).  Effective
capacity: 255 bytes.  Sufficient for 115200 baud and keeps BSS small.
