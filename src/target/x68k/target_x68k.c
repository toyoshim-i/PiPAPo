/*
 * target_x68k.c — Target implementation for X68000
 *
 * Implements the target hook API (target.h) for the Sharp X68000 personal
 * computer.  Console I/O is provided by the IPL IOCS via TRAP #15.
 *
 * X68000 hardware:
 *   CPU:     Motorola 68000 @ 10 MHz
 *   RAM:     1 MB standard, up to 12 MB expanded
 *   Console: TVRAM text screen via IOCS _B_PUTC / _B_GETC / _B_KEYSNS
 *   Timer:   MFP (MC68901) Timer-C at 100 Hz (Phase X-2)
 *   Storage: 5-inch floppy (Phase X-3 UFS boot floppy)
 *
 * Boot flow:
 *   Stage1 (sector 0) → Stage2 (sectors 1–3) → kernel at 0x006000
 *   Stage2 copies .vectors to 0x000000, restores TRAP #15 to IPL IOCS,
 *   then jumps to Reset_Handler at 0x006400.
 *
 * NOTE (Phase X-3):
 *   - Preemptive scheduling via MFP Timer-C at 100 Hz
 *   - Rootfs is a UFS image (boot/rootfs.ufs) loaded into RAM by stage2
 *   - Kernel mounts it via flatblk ("ram0") as the initial root filesystem
 */

#include "../target.h"
#include "drivers/uart.h"
#include "mm/page.h"
#include "proc/proc.h"
#include "proc/sched.h"
#include "fd/tty.h"
#include "klog.h"
#include "arch/arch.h"
#include "errno.h"
#ifdef PPAP_HAS_BLKDEV
#include "blkdev/blkdev.h"
#include "blkdev/flatblk.h"
#include "vfs/vfs.h"
#include "fs/ufs.h"
#endif
#include <stdint.h>
#include <stddef.h>

/* ── Stage2 handoff record (written by stage2.c, read by target_mount_rootfs) */

#define STAGE2_MAGIC_ADDR  ((volatile uint32_t *)0x002FF4u)
#define STAGE2_ROOTFS_ADDR ((volatile uint32_t *)0x002FF8u)
#define STAGE2_ROOTFS_SIZE ((volatile uint32_t *)0x002FFCu)
#define STAGE2_RAMD_MAGIC  0x52414D44u  /* 'RAMD' */

/* Saved copies read in target_early_init() before any IOCS call.
 * The IOCS _B_CLR_ST (screen clear) reuses the 0x002000-0x003FFF region
 * (the stage1/stage2 load area) as a temporary work buffer, corrupting
 * the stage2 handoff at 0x002FF4-0x002FFC.  We capture the values first. */
static uint32_t s_ramdisk_magic;
static uint32_t s_ramdisk_addr;
static uint32_t s_ramdisk_size;

/* ── Timer driver ────────────────────────────────────────────────────── */

/* Defined in drivers/timer_x68k.c */
extern void timer_init(void);

/* Benign IRQ handler — silences unhandled hardware IRQs with a bare rte.
 * Defined in arch/m68k/irq.S (or similar). */
extern void m68k_irq_ignore(void);

/* ── TRAP #0 syscall dispatch ────────────────────────────────────────── *
 *
 * Called from m68k_trap0_handler (trap.S) with a pointer to the saved
 * register frame.  Register layout (Linux m68k / musl convention):
 *
 *   regs[0]  = d0  (syscall number → overwritten with return value)
 *   regs[1]  = d1  (arg 1)
 *   regs[2]  = d2  (arg 2)
 *   regs[3]  = d3  (arg 3)
 *   regs[4]  = d4  (arg 4)
 *   regs[5]  = d5  (arg 5)
 *   regs[8]  = a0  (arg 6)
 *
 * Maps the m68k register frame onto the shared syscall_dispatch() API.
 * ────────────────────────────────────────────────────────────────────── */

#include "syscall/syscall.h"
#include "signal/signal.h"

void m68k_syscall_entry(uint32_t *regs)
{
    uint32_t nr   = regs[0];       /* d0 = syscall number */
    uint32_t a4   = regs[5];       /* d5 = arg 5 */
    uint32_t a5   = regs[8];       /* a0 = arg 6 */

    uint32_t saved_d1 = regs[1];

    current->svc_needs_restart = 0;

    syscall_dispatch(&regs[1], nr, a4, a5);

    regs[0] = regs[1];

    while (current->svc_needs_restart) {
        current->svc_needs_restart = 0;
        regs[0] = nr;
        regs[1] = saved_d1;
        syscall_dispatch(&regs[1], nr, a4, a5);
        regs[0] = regs[1];
    }

    signal_check(regs);
}

/* ── Target hooks ────────────────────────────────────────────────────── */

void target_early_init(void)
{
    /* Capture stage2 handoff BEFORE any IOCS call.
     * The IOCS _B_CLR_ST reuses the 0x002000-0x003FFF area (stage1/stage2
     * load region) as a temporary work buffer, overwriting 0x002FF4-0x002FFC.
     * Saving here ensures target_mount_rootfs() gets the correct values. */
    s_ramdisk_magic = *STAGE2_MAGIC_ADDR;
    s_ramdisk_addr  = *STAGE2_ROOTFS_ADDR;
    s_ramdisk_size  = *STAGE2_ROOTFS_SIZE;

    /* Patch hardware interrupt vectors BEFORE the first IOCS call.
     *
     * Stage2 copies the kernel's .vectors to 0x000000 before jumping here,
     * so autovectors 25-30 (OPM, MFP, FDC, VSYNC, DMA …) and MFP vectored
     * interrupts 64-79 all point to Default_Handler (stop #0x2700) at this
     * point.  Some IOCS functions (e.g. _B_CLR_ST) temporarily lower the CPU
     * interrupt priority level to allow VSYNC-sync; if an interrupt fires
     * before the vectors are overridden the CPU would halt inside the IOCS.
     *
     * NOTE: timer_init() is called later in target_late_init() and installs
     * m68k_timer_isr at vt[69].  It must come AFTER this loop; the loop
     * filling vt[64-79] with m68k_irq_ignore runs here so that call is safe.
     */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
    volatile uint32_t *vt = (volatile uint32_t *)0x0;
    uint32_t ignore = (uint32_t)(uintptr_t)m68k_irq_ignore;
    vt[25] = ignore;  /* Level 1: OPM (YM2151) */
    vt[26] = ignore;  /* Level 2: MFP autovector (fallback) */
    vt[27] = ignore;  /* Level 3: reserved */
    vt[28] = ignore;  /* Level 4: SCC / VSYNC */
    vt[29] = ignore;  /* Level 5: FDC */
    vt[30] = ignore;  /* Level 6: DMA */
    /* Level 7 (NMI, vector 31): keep Default_Handler */
    for (uint32_t v = 64u; v < 80u; v++)
        vt[v] = ignore;
#pragma GCC diagnostic pop

    /* Diagnostic: "Po" — kernel reached (TRAP #15 = IPL IOCS, restored by stage2) */
    {
        register uint32_t d0 asm("d0") = 0x20u;
        register uint32_t d1 asm("d1") = 'P';
        asm volatile("trap #15" : "+r"(d0) : "r"(d1) : "a0", "a1", "memory");
        d0 = 0x20u; d1 = (uint32_t)'o';
        asm volatile("trap #15" : "+r"(d0) : "r"(d1) : "a0", "a1", "memory");
    }
    uart_init_console();
    klog("PiPAPo booting... [x68k]\n");
    klog("Console: X68000 IOCS (TVRAM)\n");
    klog("Phase X-2: preemptive scheduling (MFP Timer-C), embedded romfs\n");
}

void target_late_init(void)
{
    /* Vector patching (autovectors 25-30, MFP vectors 64-79 → m68k_irq_ignore)
     * was done in target_early_init() before the first IOCS call.
     * Here we only need to start the MFP Timer-C for preemptive scheduling.
     * timer_init() installs m68k_timer_isr at vector 69 (MFP VR_base+5 = 0x45);
     * vt[64-79] were already filled with m68k_irq_ignore so this is safe. */
    timer_init();

    /* Set up dual-TTY: TTY_DISPLAY = TVRAM (primary), TTY_SERIAL = RS-232C */
    extern void uart_serial_putc(char c);
    static const tty_backend_t tvram_be = {
        .putc     = uart_putc,
        .flush    = NULL,
        .getc     = uart_getc,
        .rx_avail = uart_rx_avail,
        .get_cols = NULL,
        .get_rows = NULL,
    };
    static const tty_backend_t serial_be = {
        .putc     = uart_serial_putc,
        .flush    = NULL,
        .getc     = NULL,
        .rx_avail = NULL,
        .get_cols = NULL,
        .get_rows = NULL,
    };
    tty_set_backend(TTY_DISPLAY, &tvram_be);
    tty_set_backend(TTY_SERIAL,  &serial_be);
    tty_set_console(TTY_DISPLAY);
    klog_set_mirror(uart_serial_putc, NULL);

    /* Register keyboard polling so blocked TTY reads get woken up */
    sched_set_input_poll(uart_rx_avail, TTY_DISPLAY);
}

int target_mount_rootfs(void)
{
#ifdef PPAP_HAS_BLKDEV
    /* Use values captured in target_early_init() before IOCS could corrupt
     * the stage2 handoff area at 0x002FF4-0x002FFC. */
    if (s_ramdisk_magic != STAGE2_RAMD_MAGIC) {
        klog("x68k: no stage2 ramdisk handoff\n");
        return -1;
    }
    uint32_t addr = s_ramdisk_addr;
    uint32_t size = s_ramdisk_size;
    klogf("x68k: ramdisk at 0x%lx, %lu bytes\n",
          (unsigned long)addr, (unsigned long)size);

    /* Reserve all page-pool pages that fall inside the rootfs region so the
     * allocator never hands them out and overwrites the live UFS image.
     * page_alloc_at() is a no-op for addresses outside the pool bounds, so
     * it is safe to call even for the sub-pool portion of the rootfs. */
    {
        uint32_t rfs_end = (addr + size + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);
        for (uint32_t p = addr & ~(PAGE_SIZE - 1u); p < rfs_end; p += PAGE_SIZE)
            page_alloc_at((void *)(uintptr_t)p);
    }

    flatblk_init("ram0", (const void *)(uintptr_t)addr, size);
    blkdev_t *bd = blkdev_find("ram0");
    if (!bd)
        return -1;
    return vfs_mount("/", &ufs_ops, MNT_RDONLY, bd);
#else
    return -1;
#endif
}

void target_post_mount(void)
{
    /* User-space init (/sbin/init) is launched by main.c via do_execve() */
}

const char *target_init_path(void)
{
#ifdef PPAP_TESTS
#ifdef PPAP_TESTS_EXTENDED
    return "/bin/runtests_ext";
#else
    return "/bin/runtests";
#endif
#else
    return "/sbin/init";
#endif
}

const char *target_name(void)
{
    return "x68k";
}

uint32_t target_caps(void)
{
    return 0;  /* Phase X-3: no SD, no SPI, no Core 1 */
}

/* Yield-test process — runs on its own stack, yields back to thread 0 */
void yield_test_process(void)
{
    for (int i = 0; i < 4; i++)
        sched_yield();

    current->state = PROC_FREE;
    for (;;)
        sched_yield();
}
