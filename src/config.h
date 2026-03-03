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
#define FILE_MAX             32     /* max concurrent open struct file objs */

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
 *             QEMU has 512 KB RAM so we use a larger pool there.
 * ────────────────────────────────────────────────────────────────────────── */
#define PAGE_SIZE          4096u    /* bytes per page                       */

#ifndef PAGE_COUNT
#define PAGE_COUNT           51u    /* RP2040 default: 204 KB pool          */
#endif

/* ── VFS (Virtual File System) ────────────────────────────────────────────
 * VFS_MOUNT_MAX     Maximum concurrent mount points (romfs /, devfs /dev,
 *                   procfs /proc, tmpfs /tmp, UFS /usr, /home, /var, spare).
 *
 * VFS_VNODE_MAX     Maximum concurrent in-memory vnodes.  Each open file
 *                   or directory holds a vnode; freed on last close.
 *                   64 × 32 B = 2 KB in kernel BSS.
 *
 * VFS_NAME_MAX      Maximum filename component length (excluding NUL).
 *
 * VFS_PATH_MAX      Maximum absolute path length (including NUL).
 *
 * VFS_SYMLOOP_MAX   Maximum symlink resolution depth before returning ELOOP.
 * ────────────────────────────────────────────────────────────────────────── */
#define VFS_MOUNT_MAX        8     /* maximum concurrent mounts             */
#define VFS_VNODE_MAX       64     /* maximum concurrent vnodes             */
#define VFS_NAME_MAX        63     /* max filename component (LFN support)  */
#define VFS_PATH_MAX       128     /* max absolute path length              */
#define VFS_SYMLOOP_MAX      8     /* max symlink resolution depth          */

/* ── Block device layer ───────────────────────────────────────────────────
 * BLKDEV_MAX          Maximum registered block devices (mmcblk0, loop0, …).
 *
 * BLKDEV_SECTOR_SIZE  Bytes per sector.  All blkdev I/O is in these units.
 *                     512 is the universal standard for SD / eMMC / ATA.
 * ────────────────────────────────────────────────────────────────────────── */
#define BLKDEV_MAX            4     /* maximum registered block devices      */
#define BLKDEV_SECTOR_SIZE  512u    /* bytes per sector                      */

/* ── mmap regions ─────────────────────────────────────────────────────────
 * MMAP_REGIONS_MAX  Maximum concurrent anonymous mmap regions per process.
 *                   musl's malloc uses mmap for large allocations; busybox
 *                   applets need 5–6 concurrent regions.
 * ────────────────────────────────────────────────────────────────────────── */
#define MMAP_REGIONS_MAX      8     /* max concurrent mmap regions per proc  */

/* ── ELF loader (exec) ───────────────────────────────────────────────────
 * USER_PAGES_MAX  Maximum data-segment pages per process.  Must match
 *                 the user_pages[] array size in pcb_t (proc.h).
 *                 busybox .data+.bss ≈ 34 KB → 9 pages; 12 gives headroom.
 * ────────────────────────────────────────────────────────────────────────── */
#define USER_PAGES_MAX       12     /* max data-segment pages per process    */

/* ── tmpfs (RAM-backed temporary filesystem) ─────────────────────────────
 * TMPFS_INODE_MAX   Maximum files + directories in tmpfs.
 *
 * TMPFS_NAME_MAX    Maximum filename length in tmpfs (shorter than
 *                   VFS_NAME_MAX to save BSS).  16 inodes × 32 B name
 *                   = 512 B vs 1024 B at 64 B.
 *
 * TMPFS_DATA_MAX    Maximum total file data stored in tmpfs.  Enforced at
 *                   write time (returns -ENOSPC when exceeded).  Data is
 *                   allocated in PAGE_SIZE chunks via page_alloc().
 * ────────────────────────────────────────────────────────────────────────── */
#define TMPFS_INODE_MAX     16     /* maximum files + directories in tmpfs   */
#define TMPFS_NAME_MAX      31     /* max filename in tmpfs (31 chars + NUL) */
#define TMPFS_DATA_MAX    8192u    /* max total file data (8 KB = 2 pages)   */

#endif /* PPAP_CONFIG_H */
