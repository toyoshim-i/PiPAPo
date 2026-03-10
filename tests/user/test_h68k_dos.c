/*
 * test_h68k_dos.c — Human68k DOS call compatibility tests
 *
 * Runs R-format (.r) test binaries that exercise the Human68k DOS call
 * bridge from userland.  Each .r program exits with 0 on success or
 * a non-zero count of failures.
 *
 * Test binaries live at /subsys/human68k/ in romfs.
 */

#include "utest.h"

#define RUN_R68K(path) do {                              \
    pid_t pid = vfork();                                  \
    if (pid == 0) {                                       \
        execve(path, (void *)0, (void *)0);               \
        _exit(127);                                       \
    }                                                     \
    int _st = 0;                                          \
    waitpid(pid, &_st, 0);                                \
    UT_ASSERT_EQ((_st >> 8) & 0xff, 0);                   \
} while (0)

int main(void)
{
    UT_PRINT("  h68k: basic\n");
    RUN_R68K("/subsys/human68k/test_dos_basic.r");

    UT_PRINT("  h68k: mem\n");
    RUN_R68K("/subsys/human68k/test_dos_mem.r");

    UT_PRINT("  h68k: file\n");
    RUN_R68K("/subsys/human68k/test_dos_file.r");

    UT_PRINT("  h68k: dir\n");
    RUN_R68K("/subsys/human68k/test_dos_dir.r");

    UT_SUMMARY("test_h68k_dos");
}
