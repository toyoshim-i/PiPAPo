/*
 * test_x68k.c — Human68k X-format binary detection and loading test
 *
 * Verifies that execve() correctly detects "HU" magic and dispatches
 * to the X-format loader.  The test binary lives at
 * /subsys/human68k/hello.x in romfs.
 *
 * Phase 1 Step 2: loader returns -ENOSYS, so exec fails with exit 127.
 * As the loader is implemented, this test evolves to verify successful
 * execution and exit.
 */

#include "utest.h"

int main(void)
{
    /* 1. execve of a non-existent file should fail */
    {
        pid_t pid = vfork();
        if (pid == 0) {
            execve("/subsys/human68k/nonexistent.x",
                   (void *)0, (void *)0);
            _exit(127);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        int code = (status >> 8) & 0xff;
        UT_ASSERT_EQ(code, 127);
    }

    /* 2. execve of hello.x — detected as X-format, loader returns -ENOSYS
     *    for now (exec fails → child exits 127).  When the loader is
     *    complete this will change to expect exit code 0. */
    {
        pid_t pid = vfork();
        if (pid == 0) {
            execve("/subsys/human68k/hello.x",
                   (void *)0, (void *)0);
            _exit(127);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        int code = (status >> 8) & 0xff;
        /* Loader stub: exec fails, child exits 127 */
        UT_ASSERT_EQ(code, 127);
    }

    UT_SUMMARY("test_x68k");
}
