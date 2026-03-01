/*
 * test_fd.c — Unit tests for src/kernel/fd/fd.c
 *
 * Tests the fd table bookkeeping: fd_stdio_init, fd_alloc, fd_free, fd_get.
 * The UART tty objects (tty_stdin/stdout/stderr) are provided by
 * stubs/tty_stub.c with NULL ops pointers so no actual I/O occurs.
 *
 * pcb_t is a pure C struct (no hardware deps); we allocate one on the stack
 * and zero-initialise it before each test.
 */

#include "test_framework.h"
#include "kernel/fd/fd.h"
#include "kernel/fd/file.h"
#include "kernel/errno.h"
#include <string.h>
#include <stdint.h>

/* Stubs declared in tty_stub.c */
extern struct file tty_stdin;
extern struct file tty_stdout;
extern struct file tty_stderr;

/* A trivial non-tty file for fd_alloc tests */
static struct file extra_file = { NULL, NULL, O_RDWR, 0u, NULL, 0 };

/* Helpers */
static pcb_t make_pcb(void)
{
    pcb_t p;
    memset(&p, 0, sizeof(p));
    return p;
}

/* ── fd_stdio_init tests ─────────────────────────────────────────────────── */

static void test_stdio_init_fd0_is_stdin(void)
{
    pcb_t p = make_pcb();
    fd_stdio_init(&p);
    ASSERT(p.fd_table[0] == &tty_stdin, "fd 0 should be tty_stdin");
}

static void test_stdio_init_fd1_is_stdout(void)
{
    pcb_t p = make_pcb();
    fd_stdio_init(&p);
    ASSERT(p.fd_table[1] == &tty_stdout, "fd 1 should be tty_stdout");
}

static void test_stdio_init_fd2_is_stderr(void)
{
    pcb_t p = make_pcb();
    fd_stdio_init(&p);
    ASSERT(p.fd_table[2] == &tty_stderr, "fd 2 should be tty_stderr");
}

static void test_stdio_init_higher_fds_null(void)
{
    pcb_t p = make_pcb();
    fd_stdio_init(&p);
    for (int i = 3; i < FD_MAX; i++)
        ASSERT(p.fd_table[i] == NULL, "fd > 2 should be NULL after stdio_init");
}

/* ── fd_get tests ────────────────────────────────────────────────────────── */

static void test_get_valid_fd(void)
{
    pcb_t p = make_pcb();
    fd_stdio_init(&p);
    ASSERT(fd_get(&p, 0) == &tty_stdin,  "fd_get(0) should return tty_stdin");
    ASSERT(fd_get(&p, 1) == &tty_stdout, "fd_get(1) should return tty_stdout");
    ASSERT(fd_get(&p, 2) == &tty_stderr, "fd_get(2) should return tty_stderr");
}

static void test_get_unbound_fd_returns_null(void)
{
    pcb_t p = make_pcb();
    fd_stdio_init(&p);
    ASSERT_NULL(fd_get(&p, 3));   /* not yet allocated */
}

static void test_get_negative_fd_returns_null(void)
{
    pcb_t p = make_pcb();
    fd_stdio_init(&p);
    ASSERT_NULL(fd_get(&p, -1));
}

static void test_get_fd_max_returns_null(void)
{
    pcb_t p = make_pcb();
    fd_stdio_init(&p);
    ASSERT_NULL(fd_get(&p, FD_MAX));
}

/* ── fd_alloc tests ──────────────────────────────────────────────────────── */

static void test_alloc_returns_first_free_slot(void)
{
    pcb_t p = make_pcb();
    fd_stdio_init(&p);
    /* Slots 0/1/2 taken → first free is 3 */
    int fd = fd_alloc(&p, &extra_file);
    ASSERT_EQ(fd, 3);
}

static void test_alloc_installs_file(void)
{
    pcb_t p = make_pcb();
    fd_stdio_init(&p);
    int fd = fd_alloc(&p, &extra_file);
    ASSERT(p.fd_table[fd] == &extra_file, "fd_alloc should install file pointer");
}

static void test_alloc_increments_refcnt(void)
{
    pcb_t p = make_pcb();
    extra_file.refcnt = 0u;
    int fd = fd_alloc(&p, &extra_file);
    (void)fd;
    ASSERT_EQ((int)extra_file.refcnt, 1);
    extra_file.refcnt = 0u;   /* restore for other tests */
}

static void test_alloc_second_slot(void)
{
    pcb_t p = make_pcb();
    fd_stdio_init(&p);
    int fd1 = fd_alloc(&p, &extra_file);
    int fd2 = fd_alloc(&p, &extra_file);
    ASSERT(fd2 == fd1 + 1, "second alloc should use the next free slot");
}

static void test_alloc_emfile_when_full(void)
{
    pcb_t p = make_pcb();
    fd_stdio_init(&p);
    /* Fill all remaining slots */
    for (int i = 3; i < FD_MAX; i++)
        fd_alloc(&p, &extra_file);
    /* Now all slots are used */
    int fd = fd_alloc(&p, &extra_file);
    ASSERT_EQ(fd, -(int)EMFILE);
}

/* ── fd_free tests ───────────────────────────────────────────────────────── */

static void test_free_clears_slot(void)
{
    pcb_t p = make_pcb();
    fd_stdio_init(&p);
    int fd = fd_alloc(&p, &extra_file);
    fd_free(&p, fd);
    ASSERT_NULL(p.fd_table[fd]);
}

static void test_free_decrements_refcnt(void)
{
    pcb_t p = make_pcb();
    extra_file.refcnt = 0u;
    int fd = fd_alloc(&p, &extra_file);   /* refcnt → 1 */
    fd_free(&p, fd);                       /* refcnt → 0; ops==NULL so no close */
    ASSERT_EQ((int)extra_file.refcnt, 0);
    extra_file.refcnt = 0u;
}

static void test_free_allows_realloc(void)
{
    pcb_t p = make_pcb();
    fd_stdio_init(&p);
    int fd1 = fd_alloc(&p, &extra_file);
    fd_free(&p, fd1);
    int fd2 = fd_alloc(&p, &extra_file);
    ASSERT(fd2 >= 0, "fd_alloc after fd_free should succeed");
}

static void test_free_invalid_fd_is_safe(void)
{
    pcb_t p = make_pcb();
    fd_stdio_init(&p);
    /* Must not crash */
    fd_free(&p, -1);
    fd_free(&p, FD_MAX);
    fd_free(&p, FD_MAX + 100);
    /* stdio fds should be undisturbed */
    ASSERT(fd_get(&p, 0) == &tty_stdin, "stdin should survive invalid fd_free");
}

static void test_free_null_slot_is_safe(void)
{
    pcb_t p = make_pcb();
    fd_stdio_init(&p);
    /* Slot 5 is NULL — freeing it must not crash */
    fd_free(&p, 5);
    ASSERT_NULL(p.fd_table[5]);
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    printf("=== test_fd ===\n");

    TEST_GROUP("fd_stdio_init");
    RUN_TEST(test_stdio_init_fd0_is_stdin);
    RUN_TEST(test_stdio_init_fd1_is_stdout);
    RUN_TEST(test_stdio_init_fd2_is_stderr);
    RUN_TEST(test_stdio_init_higher_fds_null);

    TEST_GROUP("fd_get");
    RUN_TEST(test_get_valid_fd);
    RUN_TEST(test_get_unbound_fd_returns_null);
    RUN_TEST(test_get_negative_fd_returns_null);
    RUN_TEST(test_get_fd_max_returns_null);

    TEST_GROUP("fd_alloc");
    RUN_TEST(test_alloc_returns_first_free_slot);
    RUN_TEST(test_alloc_installs_file);
    RUN_TEST(test_alloc_increments_refcnt);
    RUN_TEST(test_alloc_second_slot);
    RUN_TEST(test_alloc_emfile_when_full);

    TEST_GROUP("fd_free");
    RUN_TEST(test_free_clears_slot);
    RUN_TEST(test_free_decrements_refcnt);
    RUN_TEST(test_free_allows_realloc);
    RUN_TEST(test_free_invalid_fd_is_safe);
    RUN_TEST(test_free_null_slot_is_safe);

    TEST_SUMMARY();
}
