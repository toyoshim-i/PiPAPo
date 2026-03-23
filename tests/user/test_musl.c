/*
 * test_musl.c — Harness for musl libc integration tests
 *
 * This test is a bare-metal binary (linked with crt0.o + syscall.o like
 * all other user tests).  It exercises musl libc by vfork+execve'ing
 * musl-linked child binaries, then checks exit codes.
 *
 * Each child is a separate small binary to minimize page usage on
 * memory-constrained RISC-V targets (107 pages total on qemu_rv32).
 *
 * Primary purpose: detect double-free and other process lifecycle bugs
 * that only manifest with musl-linked binaries.
 */

#include "utest.h"

/* RISC-V: musl children live on UFS (/mnt/ufs/bin/) because the
 * unstripped ELFs (with relocations) are too large for romfs.
 * ARM/m68k: children are in romfs (/bin/) as usual. */
#if defined(__riscv)
#define MUSL_BIN "/mnt/ufs/bin/"
#else
#define MUSL_BIN "/bin/"
#endif

static void run_child(const char *bin, const char *name)
{
    pid_t pid = vfork();
    if (pid == 0) {
        char *argv[] = { (char *)bin, (char *)0 };
        execve(bin, argv, (void *)0);
        _exit(127);  /* execve failed */
    }

    int status = 0;
    waitpid(pid, &status, 0);
    int code = (status >> 8) & 0xff;

    if (code == 127) {
        UT_PRINT("  SKIP  ");
        const char *p = name;
        while (*p) { write(1, p, 1); p++; }
        UT_PRINT(" not found\n");
        return;
    }

    ut_total++;
    if (code != 0) {
        ut_fail++;
        UT_PRINT("  FAIL  ");
        const char *p = name;
        while (*p) { write(1, p, 1); p++; }
        UT_PRINT(" exited with ");
        ut_print_int(code);
        UT_PRINT("\n");
    }
}

/* run_subtest: backward-compat for test_musl_child with argv[1] */
#define MUSL_CHILD MUSL_BIN "test_musl_child"

static void run_subtest(const char *name)
{
    pid_t pid = vfork();
    if (pid == 0) {
        char *argv[] = { MUSL_CHILD, (char *)name, (char *)0 };
        execve(MUSL_CHILD, argv, (void *)0);
        _exit(127);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    int code = (status >> 8) & 0xff;

    if (code == 127) {
        UT_PRINT("  SKIP  test_musl_child:");
        const char *p = name;
        while (*p) { write(1, p, 1); p++; }
        UT_PRINT(" not found\n");
        return;
    }

    ut_total++;
    if (code != 0) {
        ut_fail++;
        UT_PRINT("  FAIL  musl_child:");
        const char *p = name;
        while (*p) { write(1, p, 1); p++; }
        UT_PRINT(" exit ");
        ut_print_int(code);
        UT_PRINT("\n");
    }
}

int main(void)
{
    /* Basic musl tests (small child binary with sub-test dispatch) */
    run_subtest("exit");
    run_subtest("printf");
    run_subtest("malloc");
    run_subtest("string");

    /* Complex musl tests (separate binaries, like busybox commands) */
    run_child(MUSL_BIN "test_musl_fileio", "fileio");
    run_child(MUSL_BIN "test_musl_dir",    "dir");
    run_child(MUSL_BIN "test_musl_fmt",    "fmt");

    UT_SUMMARY("test_musl");
}
