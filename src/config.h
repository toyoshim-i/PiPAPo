/*
 * config.h — Compile-time kernel configuration
 *
 * Central location for all tunable constants.  Include this header (directly
 * or transitively through proc.h / sched.h / page.h) rather than scattering
 * magic numbers across driver and kernel files.
 *
 * Each constant has a brief note explaining what it affects so that changing
 * one value makes the implications obvious at a glance.
 */

#ifndef PPAP_CONFIG_H
#define PPAP_CONFIG_H

/* ── System clock ──────────────────────────────────────────────────────────
 * PPAP_SYS_HZ   CPU frequency after clock_init_pll() completes.
 *               Drives SYSTICK_RELOAD (sched.h) and the UART baud divisors
 *               (uart.c).  Must match the PLL configuration in clock.c.
 *
 * PPAP_TICK_HZ  SysTick interrupt rate.  Divides PPAP_SYS_HZ to produce
 *               SYSTICK_RELOAD.  One tick = one unit of ticks_remaining.
 *               Default: 100 Hz → 10 ms time slices.
 * ────────────────────────────────────────────────────────────────────────── */
#define PPAP_SYS_HZ    133000000u   /* CPU frequency after PLL init (Hz)   */
#define PPAP_TICK_HZ         100u   /* SysTick ticks per second            */

/* ── Scheduler ─────────────────────────────────────────────────────────────
 * PROC_MAX           Maximum concurrent processes.  Each PCB lives in the
 *                    static proc_table[] array in BSS — no heap needed.
 *
 * FD_MAX             File descriptors per process (fd_table[] in PCB).
 *
 * PROC_DEFAULT_TICKS Time-slice length in SysTick ticks for new processes.
 *                    With PPAP_TICK_HZ=100: 10 ticks = 100 ms.
 * ────────────────────────────────────────────────────────────────────────── */
#define PROC_MAX              8     /* maximum concurrent processes         */
#define FD_MAX               16     /* file descriptors per process         */
#define PROC_DEFAULT_TICKS   10     /* time-slice length in SysTick ticks   */

/* ── UART ring buffers ─────────────────────────────────────────────────────
 * Sizes must be powers of two.
 *
 * UART_TX_SIZE  TX ring capacity.  uint8_t head/tail → max 256 bytes.
 *               One slot is reserved, giving an effective capacity of
 *               UART_TX_SIZE - 1 bytes.
 *
 * UART_RX_SIZE  RX ring capacity.  Bytes are dropped on overflow.
 *               Must be ≤ 256 (uint8_t count comparison).
 * ────────────────────────────────────────────────────────────────────────── */
#define UART_TX_SIZE        256u    /* TX ring buffer size (bytes)          */
#define UART_RX_SIZE         64u    /* RX ring buffer size (bytes)          */

/* ── Page allocator ────────────────────────────────────────────────────────
 * PAGE_SIZE   Bytes per physical page.  Must match the SRAM layout in
 *             ppap.ld / qemu.ld (the linker script divides the page pool
 *             region by this value).
 *
 * PAGE_COUNT  Number of pages in the free pool.  Must not exceed the size
 *             of the PAGE_POOL region defined in the linker script
 *             (PAGE_POOL_SIZE = PAGE_COUNT × PAGE_SIZE).
 * ────────────────────────────────────────────────────────────────────────── */
#define PAGE_SIZE          4096u    /* bytes per page                       */
#define PAGE_COUNT           52u    /* pages in the pool (208 KB total)     */

#endif /* PPAP_CONFIG_H */
