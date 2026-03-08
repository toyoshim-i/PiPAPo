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

/* ── TRAP #0 syscall entry (Phase C Step 3) ──────────────────────────── *
 *
 * Called from m68k_trap0_handler (trap.S) with a pointer to the saved
 * register frame.  Register layout:
 *
 *   regs[0]  = d0  (syscall number → overwritten with return value)
 *   regs[1]  = d1  (arg 1)
 *   regs[2]  = d2  (arg 2)
 *   regs[3]  = d3  (arg 3)
 *   regs[4]  = d4  (arg 4)
 *   regs[5]  = d5  (arg 5)
 *   regs[8]  = a0  (arg 6)
 *
 * For Phase C testing, only SYS_WRITE (4) is handled.  Full dispatch
 * will be wired in Phase C Step 8.
 * ────────────────────────────────────────────────────────────────────── */

#define SYS_WRITE  4
#define ENOSYS     38

void m68k_syscall_entry(uint32_t *regs)
{
    uint32_t nr  = regs[0];
    long ret;

    switch (nr) {
    case SYS_WRITE: {
        /* write(fd, buf, count) → d1=fd, d2=buf, d3=count */
        int fd             = (int)regs[1];
        const char *buf    = (const char *)(uintptr_t)regs[2];
        uint32_t count     = regs[3];
        if (fd == 1 || fd == 2) {
            /* stdout/stderr → UART */
            for (uint32_t i = 0; i < count; i++)
                uart_putc(buf[i]);
            ret = (long)count;
        } else {
            ret = -9;  /* -EBADF */
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

/* ── Syscall test process ────────────────────────────────────────────── */

/* Uses TRAP #0 with the Linux m68k convention to invoke write(). */
static void syscall_test_process(void)
{
    static const char msg[] = "Hello from TRAP #0 syscall!\n";
    long ret;

    /* Inline TRAP #0: d0=SYS_WRITE(4), d1=fd(1), d2=buf, d3=count */
    __asm__ volatile (
        "move.l  %1,%%d0\n"       /* d0 = SYS_WRITE (4) */
        "move.l  %2,%%d1\n"       /* d1 = fd (1 = stdout) */
        "move.l  %3,%%d2\n"       /* d2 = buf */
        "move.l  %4,%%d3\n"       /* d3 = count */
        "trap    #0\n"
        "move.l  %%d0,%0\n"       /* ret = d0 (return value) */
        : "=d"(ret)
        : "i"(SYS_WRITE),
          "i"(1),
          "g"(msg),
          "i"((uint32_t)sizeof(msg) - 1)
        : "d0", "d1", "d2", "d3", "cc", "memory"
    );

    klogf("SYSCALL: write returned %u (expected %u)\n",
          (uint32_t)ret, (uint32_t)(sizeof(msg) - 1));

    if (ret == (long)(sizeof(msg) - 1))
        klog("SYSCALL: TRAP #0 test PASSED\n");
    else
        klog("SYSCALL: TRAP #0 test FAILED\n");

    /* Done — mark free and yield forever */
    current->state = PROC_FREE;
    for (;;)
        sched_yield();
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

    /* ── TRAP #0 syscall test ────────────────────────────────────────── */
    klog("SYSCALL: testing TRAP #0...\n");
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
