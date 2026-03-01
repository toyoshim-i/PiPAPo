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
 *   On QEMU mps2-an500 (Cortex-M3), the SIO address range (0xD0000000)
 *   is unmapped — any read triggers a BusFault → HardFault.  We detect
 *   QEMU by checking SCB.CPUID: Cortex-M0+ returns PARTNO = 0xC60,
 *   Cortex-M3 returns 0xC23.  Only proceed with SIO if we're on M0+.
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

/* PSM (Power-on State Machine) — force Core 1 off/on (§2.12) */
#define PSM_FRCE_OFF      (*(volatile uint32_t *)0x40010004u)
#define PSM_FRCE_OFF_SET  (*(volatile uint32_t *)0x40012004u)  /* atomic SET alias */
#define PSM_FRCE_OFF_CLR  (*(volatile uint32_t *)0x40013004u)  /* atomic CLR alias */
#define PSM_PROC1         (1u << 16)   /* bit 16 = proc1 (Core 1 processor) */

/* SCB.VTOR — vector table base currently used by Core 0 */
#define SCB_VTOR      (*(volatile uint32_t *)0xE000ED08u)

/* SCB.CPUID — processor identification (always accessible, never faults) */
#define SCB_CPUID     (*(volatile uint32_t *)0xE000ED00u)
#define CPUID_PARTNO_MASK  0x0000FFF0u   /* bits [15:4] = Part Number */
#define CPUID_PARTNO_M0P   0x0000C600u   /* Cortex-M0+ part number   */

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

/* ── core1_reset ─────────────────────────────────────────────────────────── */
/*
 * Force Core 1 back into its boot ROM via the PSM (Power-on State Machine).
 *
 * After a GDB load + reset-halt, only Core 0 is reset — Core 1 may still
 * be executing stale user code from a previous session.  In that state the
 * boot ROM handshake will never receive a response and core1_launch hangs.
 *
 * The fix (same approach as pico-sdk multicore_reset_core1):
 *   1. Assert PSM_FRCE_OFF.PROC1 → power off Core 1
 *   2. Wait until the bit reads back 1 (Core 1 is off)
 *   3. Deassert PSM_FRCE_OFF.PROC1 → Core 1 restarts in boot ROM
 *   4. Wait until the bit reads back 0 (Core 1 is alive)
 *   5. Drain the SIO RX FIFO to discard any stale data
 */
static void core1_reset(void)
{
    /* Force Core 1 off */
    PSM_FRCE_OFF_SET = PSM_PROC1;
    while (!(PSM_FRCE_OFF & PSM_PROC1))
        ;

    /* Let Core 1 restart — it will re-enter the boot ROM */
    PSM_FRCE_OFF_CLR = PSM_PROC1;
    while (PSM_FRCE_OFF & PSM_PROC1)
        ;

    /* Drain any stale words from the SIO RX FIFO */
    while (SIO_FIFO_ST & SIO_FIFO_VLD)
        (void)SIO_FIFO_RD;
}

/* ── core1_launch ────────────────────────────────────────────────────────── */

void core1_launch(void (*entry)(void))
{
    /*
     * QEMU self-stub: check CPUID to verify we're on Cortex-M0+ (RP2040).
     * On QEMU mps2-an500 (Cortex-M3), the SIO region at 0xD0000000 is
     * unmapped — any read from it triggers a BusFault → HardFault.
     * SCB.CPUID is in the System Control Space and is always accessible.
     */
    if ((SCB_CPUID & CPUID_PARTNO_MASK) != CPUID_PARTNO_M0P) {
        uart_puts("SMP: not Cortex-M0+ — skipping Core 1 launch (QEMU)\n");
        return;
    }

    /* Reset Core 1 via PSM so it re-enters the boot ROM handshake.
     * Without this, a GDB reload without power-cycle leaves Core 1
     * running stale code — the handshake below would hang forever. */
    core1_reset();

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
