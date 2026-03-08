/*
 * target_qemu_m68k.c — Target implementation for QEMU virt m68k
 *
 * Implements the target hook API (target.h) for the QEMU virt m68k machine.
 * QEMU virt m68k: RAM at 0x0, Goldfish TTY at 0xFF008000, Goldfish RTC
 * timer at 0xFF006000.
 *
 * Run with:
 *   qemu-system-m68k -machine virt -cpu m68000 -nographic \
 *       -kernel ppap_qemu_m68k.elf
 */

#include "../target.h"
#include "drivers/uart.h"
#include "mm/page.h"
#include "proc/proc.h"
#include "proc/sched.h"
#include "fs/romfs_format.h"
#include "klog.h"
#include "arch/arch.h"
#include "errno.h"
#include <stdint.h>
#include <stddef.h>

/* ── Timer driver ────────────────────────────────────────────────────── */

/* Defined in drivers/timer_qemu_m68k.c */
extern void timer_init(void);

/* ── In-memory romfs image ───────────────────────────────────────────── *
 *
 * On m68k, the romfs image must be in target-native endian (big-endian).
 * mkromfs builds on the host (little-endian x86), so we cannot use a
 * pre-built image.  Instead, we place a buffer in the .romfs section
 * (so that the linker symbols __romfs_start/__romfs_end bracket it)
 * and fill it at runtime in target_early_init().
 *
 * The image contains:
 *   /              — root directory
 *   /etc/          — directory
 *   /etc/fstab     — mount table (devfs, procfs, tmpfs)
 * ────────────────────────────────────────────────────────────────────── */

static uint8_t m68k_romfs_buf[512]
    __attribute__((section(".romfs"), aligned(4)));

static void build_boot_romfs(void)
{
    __builtin_memset(m68k_romfs_buf, 0, sizeof(m68k_romfs_buf));

    static const char fstab_data[] =
        "none /dev devfs rw\n"
        "none /proc procfs ro\n"
        "none /tmp tmpfs rw\n";

    /* Superblock at offset 0 */
    romfs_super_t *sb = (romfs_super_t *)m68k_romfs_buf;
    sb->magic      = ROMFS_MAGIC;
    sb->file_count = 3;
    sb->root_off   = (uint32_t)sizeof(romfs_super_t);   /* 16 */

    /* ── Root directory "/" (offset 16) ─────────────────────────────── */
    uint32_t root_off = sb->root_off;
    romfs_entry_t *root = (romfs_entry_t *)(m68k_romfs_buf + root_off);
    root->type     = ROMFS_TYPE_DIR;
    root->size     = 0;
    root->name_len = 0;
    m68k_romfs_buf[root_off + ROMFS_NAME_OFF] = '\0';
    uint32_t etc_off = root_off + ROMFS_NAME_OFF + ROMFS_ALIGN4(1);
    root->child_off = etc_off;

    /* ── /etc directory (follows root) ─────────────────────────────── */
    romfs_entry_t *etc = (romfs_entry_t *)(m68k_romfs_buf + etc_off);
    etc->type     = ROMFS_TYPE_DIR;
    etc->size     = 0;
    etc->name_len = 3;   /* "etc" */
    etc->next_off = 0;
    __builtin_memcpy(m68k_romfs_buf + etc_off + ROMFS_NAME_OFF,
                     "etc", 4);   /* "etc\0" */
    uint32_t fstab_off = etc_off + ROMFS_NAME_OFF + ROMFS_ALIGN4(4);
    etc->child_off = fstab_off;

    /* ── /etc/fstab file ───────────────────────────────────────────── */
    uint32_t fstab_len = (uint32_t)(sizeof(fstab_data) - 1);
    romfs_entry_t *fe = (romfs_entry_t *)(m68k_romfs_buf + fstab_off);
    fe->type      = ROMFS_TYPE_FILE;
    fe->size      = fstab_len;
    fe->child_off = 0;
    fe->next_off  = 0;
    fe->name_len  = 5;   /* "fstab" */
    __builtin_memcpy(m68k_romfs_buf + fstab_off + ROMFS_NAME_OFF,
                     "fstab", 6);   /* "fstab\0" */
    uint32_t data_off = fstab_off + ROMFS_DATA_OFF(fe);
    __builtin_memcpy(m68k_romfs_buf + data_off,
                     fstab_data, fstab_len);

    sb->size = data_off + ROMFS_ALIGN4(fstab_len);
}

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
 * Syscall numbers match Linux m68k (same as i386 for common calls).
 * Phase C implements: exit(1), write(4).  Phase D adds the rest.
 * ────────────────────────────────────────────────────────────────────── */

/* Linux m68k syscall numbers (matching musl) */
#define M68K_SYS_EXIT   1
#define M68K_SYS_WRITE  4

void m68k_syscall_entry(uint32_t *regs)
{
    uint32_t nr  = regs[0];
    long a0 = (long)regs[1];
    long a1 = (long)regs[2];
    long a2 = (long)regs[3];
    long ret;

    switch (nr) {

    case M68K_SYS_EXIT:
        /* exit(status) — mark process free and yield forever */
        current->state = PROC_FREE;
        for (;;)
            sched_yield();
        /* not reached */
        ret = 0;
        break;

    case M68K_SYS_WRITE: {
        /* write(fd, buf, count) → d1=fd, d2=buf, d3=count */
        int fd             = (int)a0;
        const char *buf    = (const char *)(uintptr_t)a1;
        uint32_t count     = (uint32_t)a2;
        if (fd == 1 || fd == 2) {
            for (uint32_t i = 0; i < count; i++)
                uart_putc(buf[i]);
            ret = (long)count;
        } else {
            ret = -(long)EBADF;
        }
        break;
    }

    default:
        ret = -(long)ENOSYS;
        break;
    }

    regs[0] = (uint32_t)ret;
}

/* ── Test processes ──────────────────────────────────────────────────── */

/* Process 1: spins and counts.  Preemptive scheduling switches it out. */
static void test_process1(void)
{
    for (;;)
        __asm__ volatile ("nop");
}

/* Process 2: spins and counts.  Preemptive scheduling switches it out. */
static void test_process2(void)
{
    for (;;)
        __asm__ volatile ("nop");
}

/* ── Target hooks ────────────────────────────────────────────────────── */

void target_early_init(void)
{
    uart_init_console();
    klog("PicoPiAndPortable booting... [qemu_m68k]\n");
    klog("UART: Goldfish TTY @ 0xFF008000\n");
    klog("Clock: emulated (no PLL)\n");

    /* Build the in-memory romfs image (big-endian native).
     * Must be done before main.c calls vfs_mount(). */
    build_boot_romfs();
}

void target_late_init(void)
{
    /* Initialize the Goldfish RTC timer (10 ms periodic) */
    timer_init();
    /* No IRQ UART, no MPU, no Core 1 on m68k */
}

void target_post_mount(void)
{
    /* Launch two test processes for preemptive scheduling validation.
     * These will be scheduled by the timer once sched_start() runs.
     * TODO: replace with m68k ELF user-mode init when available. */
    pcb_t *pa = proc_alloc();
    if (pa) {
        pa->stack_page = page_alloc();
        proc_setup_stack(pa, test_process1, 0);
        pa->state = PROC_RUNNABLE;
        __builtin_memcpy(pa->comm, "test1", 6);
    }

    pcb_t *pb = proc_alloc();
    if (pb) {
        pb->stack_page = page_alloc();
        proc_setup_stack(pb, test_process2, 0);
        pb->state = PROC_RUNNABLE;
        __builtin_memcpy(pb->comm, "test2", 6);
    }
}

const char *target_init_path(void)
{
    /* No m68k ELF user-mode binaries yet — skip exec */
    return NULL;
}

uint32_t target_caps(void)
{
    return 0;  /* No SD, no SPI, no Core 1 */
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
