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

#ifndef PPAP_KERNEL_COMMON_CONFIG_H
#define PPAP_KERNEL_COMMON_CONFIG_H

#include <stdint.h>

/* ── System clock ──────────────────────────────────────────────────────────
 * PPAP_SYS_HZ   CPU frequency after clock_init_pll() completes.
 *               Drives SYSTICK_RELOAD (sched.h) and the UART baud divisors
 *               (uart.c).  Must match the PLL configuration in clock.c.
 *
 * PPAP_TICK_HZ  SysTick interrupt rate.  Divides PPAP_SYS_HZ to produce
 *               SYSTICK_RELOAD.  One tick = one unit of ticks_remaining.
 *               Default: 100 Hz → 10 ms time slices.
 * ────────────────────────────────────────────────────────────────────────── */
#ifndef PPAP_SYS_HZ
#define PPAP_SYS_HZ 133000000u /* CPU frequency after PLL init (Hz)   */
#endif
#ifndef PPAP_TICK_HZ
#define PPAP_TICK_HZ 100u /* SysTick ticks per second            */
#endif

/* ── Scheduler ─────────────────────────────────────────────────────────────
 * PROC_MAX           Maximum concurrent processes.  Each PCB lives in the
 *                    static proc_table[] array in BSS — no heap needed.
 *
 * FD_MAX             File descriptors per process (fd_table[] in PCB).
 *
 * PROC_DEFAULT_TICKS Time-slice length in SysTick ticks for new processes.
 *                    With PPAP_TICK_HZ=100: 10 ticks = 100 ms.
 * ────────────────────────────────────────────────────────────────────────── */
#define PROC_MAX 8            /* maximum concurrent processes         */
#define FD_MAX 16             /* file descriptors per process         */
#define PROC_DEFAULT_TICKS 10 /* time-slice length in SysTick ticks   */
#define FILE_MAX 32           /* max concurrent open struct file objs */
#define FD_DESC_NONE (-1)     /* empty fd_map slot (no descriptor)    */

/* ── Per-process kernel stack ─────────────────────────────────────────────
 * Arches that own a per-process kernel stack carve a fixed region into
 * one slot per PCB.  Slot 0 is the idle thread (small budget); slots
 * 1..PROC_MAX-1 are user procs (full budget).  Total carved bytes are
 * PROC_KSTACK_IDLE_SIZE + (PROC_MAX-1) * PROC_KSTACK_SIZE.
 *
 * Fixed-region kstacks are the default kernel model.  Xtensa currently
 * reserves and initializes this region as migration staging, while its live
 * switch frames are still moved in a later cleanup step.  Defaults below
 * match ia16's mature numbers; targets may override them.
 * ────────────────────────────────────────────────────────────────────────── */
#ifndef PROC_KSTACK_SIZE
#define PROC_KSTACK_SIZE 1024u /* per-proc kernel stack (slots 1+)     */
#endif
#ifndef PROC_KSTACK_IDLE_SIZE
#define PROC_KSTACK_IDLE_SIZE 128u /* slot 0 (idle loop only)          */
#endif
/* Targets must reserve __kstack_region_base and provide a kernel_sp field in
 * the arch's PCB.  The kstack.c functions are weak; per-arch overlays in
 * src/arch/<arch>/kernel/core/kstack.c may strong-override any individual
 * function without touching the others. */

#ifndef PROC_KSTACK_GUARD_BYTES
/* Reserved bytes at the top of every slot for the overflow sentinel
 * planted by proc_kstack_plant_canary().  Subtracted from the slot's
 * true top when initialising kernel_sp so the active stack never
 * overwrites the guard.  4 B (one uint32_t) is enough to detect
 * adjacent-slot underflow. */
#define PROC_KSTACK_GUARD_BYTES 4u
#endif

/* ── execve args byte budgets ──────────────────────────────────────────────
 * Bytes reserved for argv and envp string payloads in the per-execve
 * args payload (`exec_args_t` — one 4 KB page from the data region).
 * Layout overhead is ~324 bytes (path + offset tables), so the sum of
 * argv+envp budgets must leave ≤ ~3772 bytes.
 *
 * argv and envp are sized independently because argv is a handful of
 * short tokens while envp carries more numerous / longer NAME=VALUE
 * strings (see feedback memory on constant reuse).  Targets may
 * override either via CMake `-DEXEC_ARGV_BYTES_MAX=…
 * -DEXEC_ENVP_BYTES_MAX=…` as long as the one-page invariant holds
 * (enforced by a _Static_assert in exec_args.h).
 * ────────────────────────────────────────────────────────────────────────── */
#ifndef EXEC_ARGV_BYTES_MAX
#define EXEC_ARGV_BYTES_MAX 1024u
#endif
#ifndef EXEC_ENVP_BYTES_MAX
#define EXEC_ENVP_BYTES_MAX 2048u
#endif

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
#define UART_TX_SIZE 256u /* TX ring buffer size (bytes)          */
#define UART_RX_SIZE 64u  /* RX ring buffer size (bytes)          */

/* ── Page allocator ────────────────────────────────────────────────────────
 * PAGE_SIZE   Bytes per physical page.  Must match the SRAM layout in
 *             ppap.ld / qemu.ld (the linker script divides the page pool
 *             region by this value).
 *
 * PAGE_COUNT_MAX  Compile-time upper bound on pages.  Sizes the static
 *                 free_stack[] array.  The actual page count is detected
 *                 at boot and stored in the runtime variable `page_count`
 *                 (see page.h).
 * ────────────────────────────────────────────────────────────────────────── */
#define PAGE_SIZE 4096u /* bytes per page                       */

#ifndef PAGE_COUNT_MAX
#define PAGE_COUNT_MAX 50u /* RP2040 default: 200 KB pool          */
#endif

/* ── Memory map (must match the target linker script) ────────────────────
 * SRAM_KERNEL_BASE/SIZE   Kernel data region (.data/.bss).
 * PAGE_POOL_BASE/SIZE     Physical page pool for user pages.
 * SRAM_IOBUF_BASE/SIZE    Block device write overlay (ARM/RP2040 only).
 * SRAM_DMA_BASE/SIZE      DMA-capable buffer region (ARM/RP2040 only).
 * RAM_END                 Hard ceiling for RAM probing.
 *
 * Targets override via -D flags in CMake (e.g. PAGE_POOL_BASE, RAM_END).
 * ────────────────────────────────────────────────────────────────────────── */

#if defined(__m68k__)
/* M68K: RAM at 0x0, page pool placed by linker after stack.
 * No IOBUF/DMA regions — those are RP2040-specific.
 *
 * RAM_END is the hard ceiling for the RAM probe — addresses at or above
 * this value are never probed (e.g. X68000 VRAM starts at 0xC00000).
 * Targets override via -DRAM_END=... in CMake; default = PAGE_POOL_BASE
 * + PAGE_COUNT_MAX * PAGE_SIZE (set after PAGE_POOL_BASE is known). */
#define SRAM_KERNEL_BASE 0x00000000u
#define SRAM_KERNEL_SIZE (20u * 1024u)
extern char __page_pool_start[];
#define PAGE_POOL_BASE ((uintptr_t)__page_pool_start)
#define PAGE_POOL_SIZE (PAGE_COUNT_MAX * PAGE_SIZE)
#ifndef RAM_END
#define RAM_END (PAGE_POOL_BASE + PAGE_POOL_SIZE)
#endif
#elif defined(__ia16__)
/* i8086 real mode: code is in a far CS segment; data at DS=0.
 * Page pool starts after the last code segment (computed by stage2,
 * passed via mod_info, set in target_pcxt.c before mm_init). */
#define SRAM_KERNEL_BASE 0x0600u
#define SRAM_KERNEL_SIZE (4u * 1024u)
extern uint32_t i16_page_pool_base;
#define PAGE_POOL_BASE i16_page_pool_base
#define PAGE_POOL_SIZE (PAGE_COUNT_MAX * PAGE_SIZE)
#ifndef RAM_END
#define RAM_END 0x9FC00ul /* 640 KB conventional - EBDA */
#endif
#elif defined(__xtensa__)
/* Xtensa / ESP32-S3: ESP-IDF manages the linker script.
 * PAGE_POOL_BASE and PAGE_COUNT_MAX are defined via CMake -D flags.
 * No IOBUF/DMA regions — ESP-IDF manages DMA buffers. */
#define SRAM_KERNEL_BASE 0x3FC90000u /* ESP32-S3 DRAM region start      */
#define SRAM_KERNEL_SIZE (32u * 1024u)
#define PAGE_POOL_SIZE (PAGE_COUNT_MAX * PAGE_SIZE)
/* Stub IOBUF/DMA to zero-size regions at end of page pool */
#define SRAM_IOBUF_BASE (PAGE_POOL_BASE + PAGE_POOL_SIZE)
#define SRAM_IOBUF_SIZE 0u
#define SRAM_DMA_BASE SRAM_IOBUF_BASE
#define SRAM_DMA_SIZE 0u
#else
/* ARM / RP2040: SRAM layout defaults match pico1 / qemu_arm.
 * Targets with a different split (for example PicoCalc) override these via
 * target_compile_definitions(). */
#define SRAM_KERNEL_BASE 0x20000000u /* kernel data region start        */
#ifndef SRAM_KERNEL_SIZE
#define SRAM_KERNEL_SIZE (24u * 1024u) /* reserved for kernel .data/.bss  */
#endif
#ifndef PAGE_POOL_BASE
#define PAGE_POOL_BASE 0x20006000u /* first page in the pool          */
#endif
#define PAGE_POOL_SIZE (PAGE_COUNT_MAX * PAGE_SIZE)
#define SRAM_IOBUF_BASE (PAGE_POOL_BASE + PAGE_POOL_SIZE) /* after pool    */
#define SRAM_IOBUF_SIZE (24u * 1024u) /* 24 KB                           */
#define SRAM_DMA_BASE (SRAM_IOBUF_BASE + SRAM_IOBUF_SIZE)
#define SRAM_DMA_SIZE (16u * 1024u) /* 16 KB                           */
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
#define VFS_MOUNT_MAX 8   /* maximum concurrent mounts             */
#define VFS_VNODE_MAX 64  /* maximum concurrent vnodes             */
#define VFS_NAME_MAX 63   /* max filename component (LFN support)  */
#define VFS_PATH_MAX 128  /* max absolute path length              */
#define VFS_SYMLOOP_MAX 4 /* max symlink resolution depth          */

/* ── Block device layer ───────────────────────────────────────────────────
 * BLKDEV_MAX          Maximum registered block devices (mmcblk0, loop0, …).
 *
 * BLKDEV_SECTOR_SIZE  Bytes per sector.  All blkdev I/O is in these units.
 *                     512 is the universal standard for SD / eMMC / ATA.
 * ────────────────────────────────────────────────────────────────────────── */
#define BLKDEV_MAX 4            /* maximum registered block devices      */
#define BLKDEV_SECTOR_SIZE 512u /* bytes per sector                      */

/* ── ELF loader (exec) ───────────────────────────────────────────────────
 * USER_PAGES_MAX  Maximum tracked pages per process (data, brk, mmap,
 *                 user stack).  Must match user_pages[] in pcb_t (proc.h).
 *                 Costs USER_PAGES_MAX × 2 B per PCB slot (page_id_t).
 *                 Default 64 pages = 256 KB.  m68k targets override to 512
 *                 (2 MB) since they have ample RAM.
 * ────────────────────────────────────────────────────────────────────────── */
#ifndef USER_PAGES_MAX
#define USER_PAGES_MAX 64 /* max data-segment pages per process    */
#endif

/* ── tmpfs (RAM-backed temporary filesystem) ─────────────────────────────
 * TMPFS_INODE_MAX   Maximum files + directories in tmpfs.
 *
 * TMPFS_NAME_MAX    Maximum filename length in tmpfs (shorter than
 *                   VFS_NAME_MAX to save BSS).  16 inodes × 32 B name
 *                   = 512 B vs 1024 B at 64 B.
 *
 * TMPFS_DATA_MAX    Maximum total file data stored in tmpfs.  Enforced at
 *                   write time (returns -ENOSPC when exceeded).  Data is
 *                   allocated in PAGE_SIZE chunks.
 * ────────────────────────────────────────────────────────────────────────── */
#define TMPFS_INODE_MAX 16   /* maximum files + directories in tmpfs   */
#define TMPFS_NAME_MAX 31    /* max filename in tmpfs (31 chars + NUL) */
#define TMPFS_DATA_MAX 8192u /* max total file data (8 KB = 2 pages)   */

#endif /* PPAP_KERNEL_COMMON_CONFIG_H */
