/*
 * smp.c — Core 1 startup and SIO FIFO IPC (RP2040)
 *
 * RP2040 SIO inter-core FIFO (base 0xD0000000, §2.3.1.7):
 *   SIO_FIFO_ST  @ 0xD0000050
 *     bit 0  VLD  — this core's RX FIFO has at least one word
 *     bit 1  RDY  — this core's TX FIFO has at least one free slot
 *   SIO_FIFO_WR  @ 0xD0000054  — write here to push to the OTHER core
 *   SIO_FIFO_RD  @ 0xD0000058  — read here to pop from THIS core's RX FIFO
 *
 * The FIFO is symmetric: the same register addresses are valid on both
 * cores; the hardware routes data to the correct per-core ring buffers.
 *
 * Boot sequence (RP2040 Datasheet §2.8.2):
 *   Core 0 sends the 6-word sequence [0, 0, 1, VTOR, SP, PC].
 *   For each zero word: drain the RX FIFO, SEV, send 0.
 *   For each non-zero word: send, SEV, then wait for Core 1 to echo
 *   the exact word back before sending the next word.
 *
 * QEMU self-stub:
 *   On real RP2040, SIO_FIFO_ST.RDY is set at boot (TX FIFO is empty →
 *   writable).  On QEMU mps2-an500 the SIO address range is not mapped
 *   and reads as 0x00000000, so RDY is clear.  core1_launch() uses this
 *   to detect QEMU and return immediately without touching the FIFO.
 */

#include "smp.h"
#include "mm/page.h"
#include "../drivers/uart.h"
#include "config.h"
#include <stdint.h>

/* ── SIO register file ───────────────────────────────────────────────────── */

#define SIO_FIFO_ST   (*(volatile uint32_t *)0xD0000050u)
#define SIO_FIFO_WR   (*(volatile uint32_t *)0xD0000054u)
#define SIO_FIFO_RD   (*(volatile uint32_t *)0xD0000058u)

#define SIO_FIFO_VLD  (1u << 0)   /* RX FIFO has data      */
#define SIO_FIFO_RDY  (1u << 1)   /* TX FIFO has free slot */

/* SCB.VTOR — vector table base currently used by Core 0 */
#define SCB_VTOR      (*(volatile uint32_t *)0xE000ED08u)

/* ── sio_fifo_push ───────────────────────────────────────────────────────── */

void sio_fifo_push(uint32_t value)
{
    while (!(SIO_FIFO_ST & SIO_FIFO_RDY))
        ;
    SIO_FIFO_WR = value;
    __asm__ volatile ("sev");   /* wake the other core if it is in WFE */
}

/* ── sio_fifo_pop ────────────────────────────────────────────────────────── */

uint32_t sio_fifo_pop(void)
{
    while (!(SIO_FIFO_ST & SIO_FIFO_VLD))
        __asm__ volatile ("wfe");   /* sleep until other core signals SEV */
    return SIO_FIFO_RD;
}

/* ── core1_io_worker ─────────────────────────────────────────────────────── */

void core1_io_worker(void)
{
    for (;;) {
        uint32_t cmd = sio_fifo_pop();
        /* Phase 4: dispatch to SD / block-device handler based on cmd */
        sio_fifo_push(cmd);     /* echo for now — signals Core 0 that cmd was processed */
    }
}

/* ── core1_launch ────────────────────────────────────────────────────────── */

void core1_launch(void (*entry)(void))
{
    /*
     * QEMU self-stub: on real RP2040 at boot, SIO_FIFO_ST.RDY = 1 because
     * the TX FIFO is empty (writable).  On QEMU mps2-an500, the SIO region
     * is unmapped and reads as 0, so RDY = 0 → skip the launch.
     */
    if ((SIO_FIFO_ST & SIO_FIFO_RDY) == 0u) {
        uart_puts("SMP: SIO not present — skipping Core 1 launch (QEMU)\n");
        return;
    }

    /* Allocate a 4 KB stack page for Core 1.
     * Stacks grow downward, so Core 1's initial SP = top of the page. */
    void *stack_page = page_alloc();
    uint32_t sp   = (uint32_t)(uintptr_t)stack_page + PAGE_SIZE;

    /* Core 1 must use the same vector table as Core 0 so that SVC,
     * PendSV, and SysTick handlers are shared. */
    uint32_t vtor = SCB_VTOR;
    uint32_t pc   = (uint32_t)(uintptr_t)entry;

    /*
     * RP2040 boot handshake (§2.8.2) — 6-word sequence:
     *   [0]  0        → drain + sync: clear Core 0's RX FIFO, SEV Core 1
     *   [1]  0        → drain + sync (second flush)
     *   [2]  1        → "I am about to send the boot params" marker
     *   [3]  vtor     → vector table base address
     *   [4]  sp       → Core 1 initial stack pointer
     *   [5]  pc       → Core 1 entry address (Thumb bit set by function ptr)
     *
     * For each zero word: drain this core's RX FIFO, wake Core 1 with SEV,
     * write 0 to the TX FIFO.  No echo expected.
     *
     * For each non-zero word: write the word, SEV, then wait for Core 1 to
     * echo the exact value back before advancing to the next word.  The inner
     * loop discards any stale bytes until the expected echo arrives.
     */
    const uint32_t seq[6] = { 0u, 0u, 1u, vtor, sp, pc };

    for (int i = 0; i < 6; i++) {
        uint32_t word = seq[i];

        if (word == 0u) {
            /* Drain Core 0's RX FIFO to remove any stale response words */
            while (SIO_FIFO_ST & SIO_FIFO_VLD)
                (void)SIO_FIFO_RD;
            /* Wake Core 1 so it re-polls the FIFO */
            __asm__ volatile ("sev");
        }

        /* Wait for TX FIFO to have space, then send the word */
        while (!(SIO_FIFO_ST & SIO_FIFO_RDY))
            ;
        SIO_FIFO_WR = word;
        __asm__ volatile ("sev");

        /* For non-zero words, wait for Core 1's echo acknowledgement */
        if (word != 0u) {
            uint32_t resp;
            do {
                while (!(SIO_FIFO_ST & SIO_FIFO_VLD))
                    __asm__ volatile ("wfe");
                resp = SIO_FIFO_RD;
            } while (resp != word);
        }
    }

    uart_puts("SMP: Core 1 launched\n");
}
