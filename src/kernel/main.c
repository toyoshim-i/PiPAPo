/*
 * main.c — Unified kernel entry point
 *
 * Called from Reset_Handler (startup.S) after .data copy and .bss zero.
 * All target-specific init is delegated to target_early_init(),
 * target_late_init(), target_post_mount(), and target_init_path()
 * — see src/target/target.h.
 */

#include "fd/fd.h"
#include "fd/file.h"
#include "fd/tty.h"
#include "fs/fstab.h"
#include "fs/romfs.h"
#include "klog.h"
#include "mm/mem_region.h"
#include "mm/page.h"
#include "proc/proc.h"
#include "proc/sched.h"
#include "target/target.h"
#include "common/mod/mod_vfs.h"
#ifdef PPAP_HAS_BLKDEV
#include "blkdev/blkdev.h"
#include "blkdev/loopback.h"
#endif
#include "arch/arch.h"
#include "cpu/cpu.h"
#include "cpu/smp.h"
#include "common/errno.h"
#include "common/mod/mod_exec.h"
#include "common/spinlock.h"
#include "subsys/subsys.h"

/* Linker-provided romfs image location in flash */
extern const uint8_t __romfs_start[];
extern const uint8_t __romfs_end[];

/* ── Kernel entry point ──────────────────────────────────────────────────── */

void kmain(void) {
  /* Release any stale hardware spinlocks left over from a previous
   * session (e.g. GDB reload).  Must happen before any spinlock use. */
  spin_locks_reset();

  /* Target-specific early init: UART console, clock PLL, SPI bus */
  target_early_init();

  /* Memory manager + boot-time memory map */
  mm_init();
  {
    int err = mem_region_init();
    if (err < 0) {
      klogf("PANIC: mem_region_init failed (%d)\n", err);
      for (;;) arch_wfi();
    }
  }

  /* Process table init */
  proc_init();

  /* Register OS personality subsystem names with procfs */
  subsys_init();

  /* Log native + emulated CPU support */
  cpu_init();

  /* VFS layer + file pool for sys_open */
  mod_vfs.init();
  file_pool_init();

#ifdef PPAP_HAS_BLKDEV
  /* Block device registry + loopback subsystem */
  blkdev_init();
  loopback_init();
#endif

  /* Target-specific late init: SD/ramblk, IRQ UART, MPU, Core 1 */
  target_late_init();

  /* Bootstrap: mount root filesystem (needed to read /etc/fstab).
   * If an embedded romfs is present use it; otherwise delegate to the
   * target (e.g. x68k mounts a UFS ramdisk loaded by stage2). */
  if (&__romfs_start[0] != &__romfs_end[0]) {
    if (mod_vfs.mount("/", &romfs_ops, MNT_RDONLY, __romfs_start) == 0)
      klog("VFS: romfs mounted at /\n");
    else
      klog("VFS: romfs mount FAILED\n");
  } else if (target_mount_rootfs() == 0) {
    klog("VFS: rootfs mounted\n");
  } else {
    klog("VFS: rootfs mount FAILED\n");
  }

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

  /* Give the kernel init thread (thread 0) its own PSP stack page */
  {
    proc_image_segment_t stack_region;

    if (mem_region_alloc(&stack_region, PPAP_MEM_RAM_STACK, PAGE_SIZE,
                         PROC_IMAGE_SEG_OWNED | PROC_IMAGE_SEG_WRITABLE) == 0)
      proc_table[0].stack_page = stack_region.base;
    else
      proc_table[0].stack_page = NULL;
  }
  if (!proc_table[0].stack_page) {
    klog("PANIC: no page for thread 0 stack\n");
    for (;;) arch_wfi();
  }

  /* Launch init as PID 1 (skipped when target_init_path() returns NULL,
   * e.g. m68k targets that don't have ELF user-mode binaries yet). */
  {
    const char *init_path = target_init_path();
    if (init_path) {
      pcb_t *init = proc_alloc();
      init->pgid = init->pid;
      init->sid = init->pid;
      fd_stdio_init(init);

      int exec_err = mod_exec.do_execve(init, init_path, NULL);
      if (exec_err < 0) {
        klogf("INIT: %s failed, trying /bin/sh\n", init_path);
        exec_err = mod_exec.do_execve(init, "/bin/sh", NULL);
      }
      if (exec_err == 0) {
        init->state = PROC_RUNNABLE;
        klogf("INIT: pid=%u loaded\n", init->pid);
      } else {
        klogf("PANIC: no init or shell (err=%u)\n", (uint32_t)(-(int)exec_err));
        proc_free(init);
        for (;;) arch_wfi();
      }
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
#if defined(__xtensa__)
    /* Semi-preemptive: timer ISR sets the flag, idle loop performs switch.
     * True preemptive switching (in interrupt return path) deferred to CC-4. */
    {
      extern volatile uint32_t xtensa_switch_pending;
      if (xtensa_switch_pending) {
        xtensa_switch_pending = 0;
        sched_yield();
      }
    }
#endif
    arch_wfi();
  }
}
