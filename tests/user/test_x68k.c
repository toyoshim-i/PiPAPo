/*
 * test_x68k.c — Human68k X-format binary detection and loading test
 *
 * Verifies that execve() correctly detects "HU" magic and dispatches
 * to the X-format loader.  The test binary lives at
 * /subsys/human68k/hello.x in romfs.
 *
 * Phase 1 Steps 4+5: loader allocates memory, loads segments, applies
 * relocations, and starts execution.  hello.x hits F-line _SETBLOCK
 * before DOS call dispatch is implemented → SIGILL → exit 132.
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

    /* 2. execve of hello.x — loader succeeds, program starts executing,
     *    hits F-line _SETBLOCK ($FF4A) → SIGILL → exit 132 (128+4).
     *    When F-line DOS call dispatch is implemented, this will change. */
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
        /* Program loads and runs, crashes on F-line with SIGILL */
        UT_ASSERT_EQ(code, 132);
    }

    UT_SUMMARY("test_x68k");
}
