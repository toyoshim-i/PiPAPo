/*
 * mpu.c — MPU 4-region layout for PPAP
 *
 * Region map:
 *   0  base=0x20000000  16 KB   kernel data   RW priv-only, XN
 *   1  base=0x10000000  16 MB   flash (XIP)   RO all modes, executable
 *   2  base=per-process  4 KB   stack page    RW all modes, XN  (updated each switch)
 *   3  base=0x40000000 512 MB   peripherals   RW priv-only, XN
 *
 * The Cortex-M0+ MPU has 8 regions.  We use only 4.
 * MPU_CTRL.PRIVDEFENA is set so the kernel uses the default background map
 * (full privileged access) while the MPU only restricts user-mode access.
 *
 * QEMU: MPU_TYPE reads as 0 on mps2-an500, so mpu_init() exits early and
 * mpu_switch() becomes a no-op via the mpu_present flag.
 */

#include "mpu.h"
#include "../proc/proc.h"
#include "../../drivers/uart.h"
#include <stdint.h>

/* ── MPU register addresses (ARMv6-M §B3.5) ─────────────────────────────── */

#define MPU_TYPE    (*(volatile uint32_t *)0xE000ED90u)
#define MPU_CTRL    (*(volatile uint32_t *)0xE000ED94u)
#define MPU_RNR     (*(volatile uint32_t *)0xE000ED98u)
#define MPU_RBAR    (*(volatile uint32_t *)0xE000ED9Cu)
#define MPU_RASR    (*(volatile uint32_t *)0xE000EDA0u)

/* MPU_CTRL bits */
#define MPU_CTRL_ENABLE      (1u << 0)   /* enable the MPU */
#define MPU_CTRL_PRIVDEFENA  (1u << 2)   /* privileged code uses default map */

/* ── RASR field helpers ──────────────────────────────────────────────────── */

/*
 * SIZE field [5:1]: SIZE = log2(region_bytes) − 1
 *   4 KB  → SIZE = 11  (2^12)
 *  16 KB  → SIZE = 13  (2^14)
 *  16 MB  → SIZE = 23  (2^24)
 * 512 MB  → SIZE = 28  (2^29)
 */
#define RASR_ENABLE         (1u       << 0)
#define RASR_SIZE(s)        ((uint32_t)(s) << 1)
#define RASR_B              (1u       << 16)   /* bufferable */
#define RASR_C              (1u       << 17)   /* cacheable  */
#define RASR_AP(a)          ((uint32_t)(a) << 24)
#define RASR_XN             (1u       << 28)   /* execute never */

/* Access-permission codes (AP field [26:24]) */
#define AP_RW_PRIV   1u   /* RW privileged, no user access */
#define AP_RW_ALL    3u   /* RW user + privileged           */
#define AP_RO_ALL    6u   /* RO user + privileged           */

/* ── Per-region RASR constants ───────────────────────────────────────────── */

/*
 * Region 0 — Kernel data (0x20000000, 16 KB)
 *   Normal SRAM: C=1, B=1 (write-back).  Privileged RW only.  XN.
 */
#define RASR_R0  (RASR_XN | RASR_AP(AP_RW_PRIV) | RASR_C | RASR_B | \
                  RASR_SIZE(13u) | RASR_ENABLE)

/*
 * Region 1 — Flash XIP (0x10000000, 16 MB)
 *   Normal flash: C=1, B=0 (write-through).  RO all modes.  Executable.
 */
#define RASR_R1  (RASR_AP(AP_RO_ALL) | RASR_C | \
                  RASR_SIZE(23u) | RASR_ENABLE)

/*
 * Region 2 — Process stack (per-process, 4 KB)
 *   Normal SRAM: C=1, B=1.  RW all modes.  XN.
 *   Base address is updated on every context switch by mpu_switch().
 */
#define RASR_R2  (RASR_XN | RASR_AP(AP_RW_ALL) | RASR_C | RASR_B | \
                  RASR_SIZE(11u) | RASR_ENABLE)

/*
 * Region 3 — Peripheral I/O (0x40000000, 512 MB)
 *   Device memory: C=0, B=1 (strongly ordered for MMIO).  Privileged RW.  XN.
 */
#define RASR_R3  (RASR_XN | RASR_AP(AP_RW_PRIV) | RASR_B | \
                  RASR_SIZE(28u) | RASR_ENABLE)

/* ── Module state ────────────────────────────────────────────────────────── */

static int mpu_present = 0;   /* set to 1 by mpu_init() when MPU is found */

/* ── Internal helper ─────────────────────────────────────────────────────── */

static void mpu_set_region(uint32_t region, uint32_t base, uint32_t rasr)
{
    MPU_RNR  = region;
    MPU_RBAR = base;
    MPU_RASR = rasr;
}

/* ── mpu_init ────────────────────────────────────────────────────────────── */

void mpu_init(void)
{
    if (MPU_TYPE == 0u) {
        uart_puts("MPU: not present — skipping (QEMU)\n");
        return;
    }

    /* Disable MPU while programming to avoid faults during region setup */
    MPU_CTRL = 0u;

    /* Region 0: Kernel data */
    mpu_set_region(0u, 0x20000000u, RASR_R0);

    /* Region 1: Flash XIP — read-only, executable */
    mpu_set_region(1u, 0x10000000u, RASR_R1);

    /* Region 2: Process stack — disabled until first mpu_switch() */
    mpu_set_region(2u, 0x00000000u, 0u);

    /* Region 3: Peripheral I/O */
    mpu_set_region(3u, 0x40000000u, RASR_R3);

    /*
     * Enable MPU with PRIVDEFENA:
     *   - ENABLE     (bit 0): MPU is active
     *   - PRIVDEFENA (bit 2): privileged code falls through to the default
     *                         background map — kernel still sees all SRAM
     */
    MPU_CTRL = MPU_CTRL_PRIVDEFENA | MPU_CTRL_ENABLE;

    /* Data and instruction synchronisation barriers required after enabling
     * the MPU to ensure subsequent memory accesses use the new configuration */
    __asm__ volatile ("dsb\n isb" ::: "memory");

    mpu_present = 1;
    uart_puts("MPU: 4 regions active (kernel/flash/stack/periph)\n");
}

/* ── mpu_switch ──────────────────────────────────────────────────────────── */

void mpu_switch(pcb_t *next)
{
    if (!mpu_present)
        return;

    if (!next->stack_page) {
        /* No stack page allocated yet — disable region 2 */
        MPU_RNR  = 2u;
        MPU_RASR = 0u;
        return;
    }

    /* Reprogram region 2 for the incoming process's 4 KB stack page */
    mpu_set_region(2u, (uint32_t)(uintptr_t)next->stack_page, RASR_R2);

    __asm__ volatile ("dsb\n isb" ::: "memory");
}
