/*
 * smp.h — Core 1 startup and SIO FIFO IPC helpers (RP2040)
 *
 * The RP2040 boot ROM starts Core 1 via a 6-word SIO FIFO handshake:
 *   Core 0 sends [0, 0, 1, VTOR, SP, PC]; Core 1 echoes each non-zero word.
 * After the handshake Core 1 executes the supplied entry function with a
 * fresh PSP stack allocated from the kernel page pool.
 *
 * QEMU self-stub: core1_launch() reads SIO_FIFO_ST; if the TX-ready bit is
 * clear (SIO is not mapped on mps2-an500), it prints a message and returns
 * immediately.  Both main.c and main_qemu.c call core1_launch() unconditionally
 * — no #ifdef is needed.
 *
 * IPC after launch: Core 0 and Core 1 communicate via sio_fifo_push() and
 * sio_fifo_pop() (both blocking).  In Phase 1 the worker simply echoes every
 * command word back to Core 0 as a placeholder for Phase 4 SD I/O.
 */

#ifndef PPAP_KERNEL_SMP_H
#define PPAP_KERNEL_SMP_H

#include <stdint.h>

/*
 * core1_launch(entry) — start Core 1 running entry().
 *
 * Allocates a 4 KB stack page for Core 1, reads the current VTOR, then
 * executes the RP2040 §2.8.2 boot handshake over the SIO inter-core FIFO.
 * Must be called from kmain() before sched_start().
 */
void core1_launch(void (*entry)(void));

/*
 * sio_fifo_push(value) — blocking send to the other core's RX FIFO.
 * Waits until the TX FIFO has space, writes value, then signals with SEV.
 */
void sio_fifo_push(uint32_t value);

/*
 * sio_fifo_pop() — blocking receive from this core's RX FIFO.
 * Sleeps with WFE until the RX FIFO has data, then returns the value.
 */
uint32_t sio_fifo_pop(void);

/*
 * core1_io_worker() — Phase 1 Core 1 entry function.
 * Loops forever: pops a command word sent by Core 0 and echoes it back.
 * Placeholder for Phase 4 SD / block-device dispatch.
 */
void core1_io_worker(void);

#endif /* PPAP_KERNEL_SMP_H */
