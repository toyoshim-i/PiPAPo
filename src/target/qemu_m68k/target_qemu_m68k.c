/*
 * target_qemu_m68k.c — Target implementation for QEMU virt m68k
 *
 * Phase C: shared kernel subsystems compiled for m68k.
 * Exercises the memory manager, process table, context switch
 * (cooperative + preemptive), and TRAP #0 syscall entry.
 *
 * QEMU virt m68k: RAM at 0x0, Goldfish TTY at 0xFF008000.
 * Run with:
 *   qemu-system-m68k -machine virt -cpu m68000 -nographic \
 *       -kernel ppap_qemu_m68k.elf
 */

#include "drivers/uart.h"
#include "mm/page.h"
#include "proc/proc.h"
#include "proc/sched.h"
#include "vfs/vfs.h"
#include "fs/romfs.h"
#include "fs/romfs_format.h"
#include "klog.h"
#include "arch/arch.h"
#include <stdint.h>

/* ── Stubs for subsystems not yet ported to m68k ─────────────────────── */

/* tty_rx_notify is called from sched_timer_tick's input polling path.
 * No tty subsystem on m68k yet — stub it out. */
void tty_rx_notify(int idx) { (void)idx; }

/* ── Timer driver ────────────────────────────────────────────────────── */

/* Defined in drivers/timer_qemu_m68k.c */
extern void timer_init(void);

/* ── TRAP #0 syscall dispatch (Phase C Step 8) ───────────────────────── *
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
#define SYS_EXIT   1
#define SYS_WRITE  4
#define ENOSYS     38
#define EBADF       9

void m68k_syscall_entry(uint32_t *regs)
{
    uint32_t nr  = regs[0];
    long a0 = (long)regs[1];
    long a1 = (long)regs[2];
    long a2 = (long)regs[3];
    long ret;

    switch (nr) {

    case SYS_EXIT:
        /* exit(status) — mark process free and yield forever */
        klogf("SYSCALL: exit(%u)\n", (uint32_t)a0);
        current->state = PROC_FREE;
        for (;;)
            sched_yield();
        /* not reached */
        ret = 0;
        break;

    case SYS_WRITE: {
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

/* ── In-memory romfs test ─────────────────────────────────────────────── */

/*
 * Build a tiny romfs image in SRAM and verify mount + lookup + read.
 * The image is target-native endian (big-endian on m68k), which is
 * exactly what we get by assigning struct fields at runtime.
 *
 * Layout:
 *   [0..15]   romfs_super_t
 *   [16..39]  root dir entry (name_len=0, padded name = 4 bytes)
 *   [40..91]  "hello.txt" file entry (9-char name + 18-byte content)
 */
static uint8_t romfs_image[128] __attribute__((aligned(4)));

static void build_test_romfs(void)
{
    __builtin_memset(romfs_image, 0, sizeof(romfs_image));

    static const char file_name[] = "hello.txt";
    static const char file_data[] = "Hello from romfs!\n";

    /* Superblock at offset 0 */
    romfs_super_t *sb = (romfs_super_t *)romfs_image;
    sb->magic      = ROMFS_MAGIC;
    sb->file_count = 2;
    sb->root_off   = (uint32_t)sizeof(romfs_super_t);   /* 16 */

    /* Root directory at offset 16 */
    uint32_t root_off = sb->root_off;
    romfs_entry_t *root = (romfs_entry_t *)(romfs_image + root_off);
    root->next_off  = 0;
    root->type      = ROMFS_TYPE_DIR;
    root->size      = 0;
    root->name_len  = 0;       /* root has empty name */
    /* NUL terminator for name (1 byte, padded to 4) */
    romfs_image[root_off + ROMFS_NAME_OFF] = '\0';

    /* hello.txt entry follows root */
    uint32_t file_off = root_off + ROMFS_NAME_OFF + ROMFS_ALIGN4(1);
    root->child_off = file_off;

    romfs_entry_t *fe = (romfs_entry_t *)(romfs_image + file_off);
    fe->next_off  = 0;
    fe->type      = ROMFS_TYPE_FILE;
    fe->size      = (uint32_t)(sizeof(file_data) - 1);
    fe->child_off = 0;
    fe->name_len  = (uint32_t)(sizeof(file_name) - 1);

    /* Copy name + data */
    __builtin_memcpy(romfs_image + file_off + ROMFS_NAME_OFF,
                     file_name, sizeof(file_name));
    uint32_t data_off = file_off + ROMFS_DATA_OFF(fe);
    __builtin_memcpy(romfs_image + data_off,
                     file_data, sizeof(file_data) - 1);

    sb->size = data_off + ROMFS_ALIGN4(fe->size);
}

static void romfs_test(void)
{
    build_test_romfs();
    vfs_init();

    int rc = vfs_mount("/", &romfs_ops, MNT_RDONLY, romfs_image);
    if (rc != 0) {
        klog("ROMFS: mount FAILED\n");
        return;
    }
    klog("ROMFS: mounted OK\n");

    vnode_t *vn = 0;
    rc = vfs_lookup("/hello.txt", &vn);
    if (rc != 0) {
        klog("ROMFS: lookup /hello.txt FAILED\n");
        return;
    }
    klogf("ROMFS: /hello.txt size=%u\n", vn->size);

    char buf[64];
    long n = vn->mount->ops->read(vn, buf, sizeof(buf) - 1, 0);
    if (n > 0) {
        buf[n] = '\0';
        klogf("ROMFS: read %u bytes: %s", (uint32_t)n, buf);
    } else {
        klog("ROMFS: read FAILED\n");
        return;
    }

    vnode_put(vn);
    klog("ROMFS: test PASSED\n");
}

/* ── Test processes ──────────────────────────────────────────────────── */

static volatile uint32_t counter1 = 0;
static volatile uint32_t counter2 = 0;

/* Process 1: spins and counts.  Preemptive scheduling switches it out. */
void test_process1(void)
{
    for (;;) {
        counter1++;
        if ((counter1 % 100000u) == 0u)
            klogf("P1: count=%u\n", counter1);
    }
}

/* Process 2: spins and counts.  Preemptive scheduling switches it out. */
void test_process2(void)
{
    for (;;) {
        counter2++;
        if ((counter2 % 100000u) == 0u)
            klogf("P2: count=%u\n", counter2);
    }
}

/* ── Syscall dispatch test process ────────────────────────────────────── */

/* Helper: invoke TRAP #0 syscall with up to 3 arguments */
static long m68k_syscall3(long nr, long a0, long a1, long a2)
{
    long ret;
    __asm__ volatile (
        "move.l  %1,%%d0\n"
        "move.l  %2,%%d1\n"
        "move.l  %3,%%d2\n"
        "move.l  %4,%%d3\n"
        "trap    #0\n"
        "move.l  %%d0,%0\n"
        : "=d"(ret)
        : "g"(nr), "g"(a0), "g"(a1), "g"(a2)
        : "d0", "d1", "d2", "d3", "cc", "memory"
    );
    return ret;
}

/*
 * Tests the syscall dispatch (Phase C Step 8):
 *   1. write(1, "hello", 5)  — the canonical test from the plan
 *   2. write(1, "\n", 1)     — newline
 *   3. exit(0)               — clean process termination via syscall
 */
static void syscall_test_process(void)
{
    long ret;

    /* write(1, "hello", 5) — the canonical Step 8 test */
    ret = m68k_syscall3(SYS_WRITE, 1, (long)"hello", 5);
    if (ret != 5) {
        klog("\nSYSCALL: write(1,\"hello\",5) FAILED\n");
        current->state = PROC_FREE;
        for (;;) sched_yield();
    }

    /* newline for clean output */
    m68k_syscall3(SYS_WRITE, 1, (long)"\n", 1);

    klogf("SYSCALL: write returned %u (expected 5)\n", (uint32_t)ret);
    klog("SYSCALL: dispatch test PASSED\n");

    /* exit(0) via syscall */
    m68k_syscall3(SYS_EXIT, 0, 0, 0);

    /* not reached */
    for (;;) sched_yield();
}

/* ── Phase C kernel entry ─────────────────────────────────────────────── */

void kmain(void)
{
    uart_init_console();

    klog("PicoPiAndPortable booting... [qemu_m68k]\n");
    klog("CPU: Motorola 68000\n");

    /* Memory manager */
    mm_init();

    /* Process table */
    proc_init();

    /* ── romfs read-only mount test ────────────────────────────────────── */
    romfs_test();

    /* ── Cooperative context switch test ──────────────────────────────── */
    proc_table[0].stack_page = page_alloc();
    if (!proc_table[0].stack_page) {
        klog("PANIC: no page for thread 0 stack\n");
        for (;;) __asm__ volatile ("nop");
    }

    pcb_t *p1 = proc_alloc();
    p1->stack_page = page_alloc();
    extern void yield_test_process(void);
    proc_setup_stack(p1, yield_test_process, 0);
    p1->state = PROC_RUNNABLE;

    klog("SCHED: cooperative yield test...\n");
    for (int i = 0; i < 4; i++) {
        klogf("Thread 0: iteration %u, yielding...\n", (uint32_t)i);
        sched_yield();
    }
    klog("SCHED: cooperative yield test PASSED\n");
    p1->state = PROC_FREE;

    /* ── Syscall dispatch test (Phase C Step 8) ──────────────────────── */
    klog("SYSCALL: testing dispatch via TRAP #0...\n");
    pcb_t *ps = proc_alloc();
    ps->stack_page = page_alloc();
    proc_setup_stack(ps, syscall_test_process, 0);
    ps->state = PROC_RUNNABLE;

    /* Yield to let the syscall test process run */
    sched_yield();
    /* Back — syscall test process should have completed */

    /* ── Preemptive scheduling test ───────────────────────────────────── */
    klog("SCHED: starting preemptive scheduling test...\n");

    pcb_t *pa = proc_alloc();
    pa->stack_page = page_alloc();
    proc_setup_stack(pa, test_process1, 0);
    pa->state = PROC_RUNNABLE;

    pcb_t *pb = proc_alloc();
    pb->stack_page = page_alloc();
    proc_setup_stack(pb, test_process2, 0);
    pb->state = PROC_RUNNABLE;

    /* Initialize the Goldfish RTC timer (10 ms periodic) */
    timer_init();

    /* Start the scheduler (enables interrupts) */
    sched_start();

    /* Thread 0 (idle): spin and report tick count periodically */
    uint32_t last_report = 0;
    for (;;) {
        uint32_t ticks = sched_get_ticks();
        if (ticks >= last_report + 100u) {
            last_report = ticks;
            klogf("IDLE: ticks=%u  P1=%u  P2=%u\n",
                  ticks, counter1, counter2);

            if (ticks >= 500u) {
                klog("Phase C preemptive scheduling test PASSED\n");
                for (;;) __asm__ volatile ("nop");
            }
        }
    }
}

/* Yield-test process — runs on its own stack, yields back to thread 0 */
void yield_test_process(void)
{
    for (int i = 0; i < 4; i++) {
        klogf("Yield P1: iteration %u, yielding...\n", (uint32_t)i);
        sched_yield();
    }

    current->state = PROC_FREE;
    for (;;)
        sched_yield();
}
