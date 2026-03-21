/*
 * smp.c — Core 1 startup and SIO FIFO IPC (RP2040/RP2350)
 *
 * RP2040/RP2350 SIO inter-core FIFO (base 0xD0000000, §2.3.1.7):
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

#include "cpu/smp.h"

#include "proc/proc.h"        /* pcb_t, proc_alloc, current_core */
#include "proc/sched.h"       /* SYSTICK_RELOAD */
#include "mm/page.h"
#include "mm/mpu.h"           /* mpu_init */
#include "../drivers/uart.h"
#include "klog.h"
#include "arch/arch.h"
#include "arch/ioregs.h"
#include "config.h"
#include <stdint.h>

/* ── SIO register file ───────────────────────────────────────────────────── */

#define SIO_FIFO_ST   (*(volatile uint32_t *)0xD0000050u)
#define SIO_FIFO_WR   (*(volatile uint32_t *)0xD0000054u)
#define SIO_FIFO_RD   (*(volatile uint32_t *)0xD0000058u)

#define SIO_FIFO_VLD  (1u << 0)   /* RX FIFO has data      */
#define SIO_FIFO_RDY  (1u << 1)   /* TX FIFO has free slot */

/* PSM (Power-on State Machine) — force Core 1 off/on (§2.12)
 * Base address and proc1 bit differ between RP2040 and RP2350. */
#ifdef PICO_RP2350
#define PSM_FRCE_OFF      (*(volatile uint32_t *)0x40018004u)
#define PSM_FRCE_OFF_SET  (*(volatile uint32_t *)0x4001A004u)  /* atomic SET alias */
#define PSM_FRCE_OFF_CLR  (*(volatile uint32_t *)0x4001B004u)  /* atomic CLR alias */
#define PSM_PROC1         (1u << 24)   /* bit 24 = proc1 on RP2350 */
#else
#define PSM_FRCE_OFF      (*(volatile uint32_t *)0x40010004u)
#define PSM_FRCE_OFF_SET  (*(volatile uint32_t *)0x40012004u)  /* atomic SET alias */
#define PSM_FRCE_OFF_CLR  (*(volatile uint32_t *)0x40013004u)  /* atomic CLR alias */
#define PSM_PROC1         (1u << 16)   /* bit 16 = proc1 on RP2040 */
#endif

/* SCB.VTOR — vector table base currently used by Core 0 */
#define SCB_VTOR      (*(volatile uint32_t *)0xE000ED08u)

/* SCB.CPUID — processor identification (always accessible, never faults) */
#define SCB_CPUID     (*(volatile uint32_t *)0xE000ED00u)
#define CPUID_PARTNO_MASK  0x0000FFF0u   /* bits [15:4] = Part Number */
#define CPUID_PARTNO_M0P   0x0000C600u   /* Cortex-M0+ (RP2040)      */
#define CPUID_PARTNO_M33   0x0000D210u   /* Cortex-M33 (RP2350)      */

/* ── sio_fifo_push ───────────────────────────────────────────────────────── */

void sio_fifo_push(uint32_t value)
{
    while (!(SIO_FIFO_ST & SIO_FIFO_RDY))
        ;
    SIO_FIFO_WR = value;
    arch_sev();   /* wake the other core if it is in WFE */
}

/* ── sio_fifo_pop ────────────────────────────────────────────────────────── */

uint32_t sio_fifo_pop(void)
{
    while (!(SIO_FIFO_ST & SIO_FIFO_VLD))
        arch_wfe();   /* sleep until other core signals SEV */
    return SIO_FIFO_RD;
}

/* ── core1_sched_entry ───────────────────────────────────────────────────── */
/*
 * Core 1 entry point for dual-core scheduling.
 *
 * Mirrors sched_start() on Core 0:
 *   1. Program Core 1's MPU (each core has its own on Cortex-M0+)
 *   2. Set PendSV/SVCall priorities (per-core PPB registers)
 *   3. Allocate an idle PCB so PendSV_Handler has a valid current_core[1]
 *   4. Switch from MSP (set by core1_launch) to PSP
 *   5. Configure Core 1's SysTick (per-core timer)
 *   6. Enable interrupts and enter WFI idle loop
 *
 * PendSV_Handler on Core 1 picks up RUNNABLE processes; when none are
 * available, sched_next() returns the idle PCB and we resume WFI.
 */
void core1_sched_entry(void)
{
    /* Disable interrupts until everything is configured */
    arch_irq_disable();

    /* 0. Enable FPU on Core 1 (Cortex-M33 only) — each core has its own CPACR */
#if __ARM_ARCH >= 8
    {
        volatile uint32_t *cpacr = (volatile uint32_t *)0xE000ED88u;
        *cpacr |= (0xFu << 20);  /* CP10+CP11 = full access */
        __asm__ volatile("dsb\nisb" ::: "memory");

        /* Enable lazy stacking — same as boot.S for Core 0 */
        volatile uint32_t *fpccr = (volatile uint32_t *)0xE000EF34u;
        *fpccr |= (3u << 30);  /* ASPEN + LSPEN */
    }
#endif

    /* 1. Program Core 1's MPU (regions 0, 1, 3) */
    mpu_init();

    /* 2. Set exception priorities — same as sched_start() on Core 0.
     * SHPR2/SHPR3 are per-core (Private Peripheral Bus). */
    SCB_SHPR3 = (SCB_SHPR3 & ~PENDSV_PRIO_MASK) | PENDSV_PRIO_LOWEST;
    SCB_SHPR2 = (SCB_SHPR2 & ~SVCALL_PRIO_MASK) | (0x80u << SVCALL_PRIO_SHIFT);

    /* 3. Allocate an idle PCB for Core 1.
     * PendSV_Handler requires current_core[1] to be valid. */
    pcb_t *idle = proc_alloc();
    if (!idle) {
        klog("SMP: Core 1 idle alloc FAILED\n");
        for (;;) arch_wfi();
    }
    idle->stack_page = page_alloc();
    if (!idle->stack_page) {
        klog("SMP: Core 1 stack alloc FAILED\n");
        for (;;) arch_wfi();
    }
    idle->state = PROC_RUNNABLE;
    idle->ticks_remaining = PROC_DEFAULT_TICKS;
    idle->running_on_core = 1;
    idle->is_idle = 1;
    __builtin_memcpy(idle->comm, "idle1", 6);
    current_core[1] = idle;

    /* 4. Switch Thread mode to PSP using idle's stack page.
     * MSP (set by core1_launch) remains the exception stack. */
    uint32_t psp_top = (uint32_t)(uintptr_t)idle->stack_page + PAGE_SIZE;
    __asm__ volatile(
        "msr  psp, %0      \n"   /* PSP = top of idle's stack page */
        "movs r0, #2       \n"   /* CONTROL.SPSEL = 1 */
        "msr  control, r0  \n"
        "isb               \n"
        :: "r"(psp_top) : "r0"
    );

    /* 5. Configure Core 1's SysTick (per-core timer) */
    SYST_RVR = SYSTICK_RELOAD;
    SYST_CVR = 0u;
    SYST_CSR = SYST_CSR_ENABLE | SYST_CSR_TICKINT | SYST_CSR_CLKSOURCE;

    klog("SMP: Core 1 scheduler started\n");

    /* 6. Enable interrupts and enter idle loop.
     * PendSV switches to a RUNNABLE process when one becomes available. */
    arch_irq_enable();

    for (;;)
        arch_wfi();
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
 *   4. Wait for Core 1's boot ROM to push 0 (confirms it is ready)
 */
static void core1_reset(void)
{
    /* Force Core 1 off */
    PSM_FRCE_OFF_SET = PSM_PROC1;
    while (!(PSM_FRCE_OFF & PSM_PROC1))
        ;

    /* Let Core 1 restart — it will re-enter the boot ROM */
    PSM_FRCE_OFF_CLR = PSM_PROC1;

    /* Wait for Core 1's boot ROM to signal readiness by pushing 0.
     * The SDK does multicore_fifo_pop_blocking() here — same idea. */
    while (!(SIO_FIFO_ST & SIO_FIFO_VLD))
        arch_wfe();
    (void)SIO_FIFO_RD;  /* consume the 0 */
}

/* ── core1_launch ────────────────────────────────────────────────────────── */

void core1_launch(void (*entry)(void))
{
    /*
     * QEMU self-stub: check CPUID to verify we're on RP2040/RP2350 hardware.
     * On QEMU mps2-an500 (Cortex-M3), the SIO region at 0xD0000000 is
     * unmapped — any read from it triggers a BusFault → HardFault.
     * SCB.CPUID is in the System Control Space and is always accessible.
     */
    uint32_t partno = SCB_CPUID & CPUID_PARTNO_MASK;
    if (partno != CPUID_PARTNO_M0P && partno != CPUID_PARTNO_M33) {
        klog("SMP: unknown CPU (QEMU?): skipping Core 1 launch\n");
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
     * RP2040/RP2350 boot handshake (§2.8.2) — 6-word sequence:
     *   [0]  0        → drain + sync
     *   [1]  0        → drain + sync (second flush)
     *   [2]  1        → "I am about to send the boot params" marker
     *   [3]  vtor     → vector table base address
     *   [4]  sp       → Core 1 initial stack pointer
     *   [5]  pc       → Core 1 entry address (Thumb bit set by function ptr)
     *
     * For each word (including zeros): send, then wait for Core 1 to echo
     * back the exact value.  If the echo doesn't match, restart the entire
     * sequence from word 0.  This matches pico-sdk multicore_launch_core1_raw.
     */
    const uint32_t seq[6] = { 0u, 0u, 1u, vtor, sp, pc };

    int i = 0;
    while (i < 6) {
        uint32_t word = seq[i];

        if (word == 0u) {
            /* Drain Core 0's RX FIFO to remove any stale response words */
            while (SIO_FIFO_ST & SIO_FIFO_VLD)
                (void)SIO_FIFO_RD;
            arch_sev();
        }

        /* Wait for TX FIFO to have space, then send the word */
        sio_fifo_push(word);

        /* Wait for Core 1's echo — must match to advance */
        uint32_t resp = sio_fifo_pop();
        if (resp == word)
            i++;
        else
            i = 0;   /* mismatch → restart handshake */
    }

    klog("SMP: Core 1 launched\n");
}
