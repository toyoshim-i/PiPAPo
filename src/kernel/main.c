/*
 * main.c — Unified kernel entry point
 *
 * Called from Reset_Handler (startup.S) after .data copy and .bss zero.
 * All target-specific init is delegated to target_early_init(),
 * target_late_init(), target_post_mount(), and target_init_path()
 * — see src/target/target.h.
 */

#include "target/target.h"
#include "klog.h"
#include "mm/page.h"
#include "proc/proc.h"
#include "proc/sched.h"
#include "fd/fd.h"
#include "fd/file.h"
#include "fd/tty.h"
#include "vfs/vfs.h"
#include "fs/romfs.h"
#include "fs/fstab.h"
#ifdef PPAP_HAS_BLKDEV
#include "blkdev/blkdev.h"
#include "blkdev/loopback.h"
#endif
#include "exec/exec.h"
#include "smp.h"
#include "spinlock.h"
#include "errno.h"

/* Linker-provided romfs image location in flash */
extern const uint8_t __romfs_start[];
extern const uint8_t __romfs_end[];

/* ── Kernel entry point ──────────────────────────────────────────────────── */

void kmain(void)
{
    /* Release any stale hardware spinlocks left over from a previous
     * session (e.g. GDB reload).  Must happen before any spinlock use. */
    spin_locks_reset();

    /* Target-specific early init: UART console, clock PLL, SPI bus */
    target_early_init();

    /* Memory manager + boot-time memory map */
    mm_init();

    /* Process table init */
    proc_init();

    /* VFS layer + file pool for sys_open */
    vfs_init();
    file_pool_init();

#ifdef PPAP_HAS_BLKDEV
    /* Block device registry + loopback subsystem */
    blkdev_init();
    loopback_init();
#endif

    /* Target-specific late init: SD/ramblk, IRQ UART, MPU, Core 1 */
    target_late_init();

    /* Bootstrap: mount romfs at / (needed to read /etc/fstab) */
    if (vfs_mount("/", &romfs_ops, MNT_RDONLY, __romfs_start) == 0)
        klog("VFS: romfs mounted at /\n");
    else
        klog("VFS: romfs mount FAILED\n");

    /* Parse /etc/fstab and mount all entries */
    {
        fstab_entry_t fstab[FSTAB_MAX_ENTRIES];
        int nfstab = fstab_parse(fstab, FSTAB_MAX_ENTRIES);
        if (nfstab > 0) {
            klogf("fstab: %u entries parsed\n", (uint32_t)nfstab);
            fstab_mount_all(fstab, nfstab);
        } else {
            klog("fstab: no entries (fallback not implemented)\n");
        }
    }

    /* Kernel integration tests (no-op unless PPAP_TESTS=ON) */
    target_post_mount();

    /* Wire fd 0/1/2 to the UART tty driver */
    fd_stdio_init(&proc_table[0]);
    klog("FD: fd 0/1/2 wired to UART tty\n");

    /* Give the kernel init thread (thread 0) its own PSP stack page */
    proc_table[0].stack_page = page_alloc();
    if (!proc_table[0].stack_page) {
        klog("PANIC: no page for thread 0 stack\n");
        for (;;) __asm__ volatile ("wfi");
    }

    /* Launch init as PID 1 */
    {
        const char *init_path = target_init_path();
        pcb_t *init = proc_alloc();
        init->pgid = init->pid;
        init->sid  = init->pid;

        int exec_err = do_execve(init, init_path, NULL);
        if (exec_err < 0) {
            klogf("INIT: %s failed, trying /bin/sh\n", init_path);
            exec_err = do_execve(init, "/bin/sh", NULL);
        }
        if (exec_err == 0) {
            fd_stdio_init(init);
            init->state = PROC_RUNNABLE;
            /* tty_fg_pgrp stays 0: without CONFIG_HUSH_JOB the shell
             * never calls tcsetpgrp(), so tty_send_signal uses the
             * fallback of signaling all non-init processes. */
            klogf("INIT: pid=%u loaded\n", init->pid);
        } else {
            klogf("PANIC: no init or shell (err=%u)\n",
                  (uint32_t)(-(int)exec_err));
            proc_free(init);
            for (;;) __asm__ volatile ("wfi");
        }
    }

    /* Launch Core 1 after init has PID 1 — core1_sched_entry() calls
     * proc_alloc() which would steal PID 1 if called earlier.
     * Self-stubs on QEMU (no SIO). */
    core1_launch(core1_sched_entry);

    klog("SCHED: starting scheduler\n");
    sched_start();

    /* Idle thread — wake on every interrupt, flush LCD if needed, sleep. */
    for (;;) {
        sched_display_poll();
        __asm__ volatile ("wfi");
    }
}
