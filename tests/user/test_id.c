/*
 * test_id.c — Process identity syscalls: getpid, getppid, setsid, setpgid, getpgid
 */

#include "utest.h"

int main(void)
{
    /* 1. getpid returns positive */
    pid_t pid = getpid();
    UT_ASSERT(pid > 0, "getpid > 0");

    /* 2. getppid returns positive (parent is runtests or init) */
    pid_t ppid = getppid();
    UT_ASSERT(ppid > 0, "getppid > 0");
    UT_ASSERT(ppid != pid, "ppid != pid");

    /* 3. getpgid(0) returns own pgid */
    pid_t pgid = getpgid(0);
    UT_ASSERT(pgid > 0, "getpgid(0) > 0");

    /* 4. setpgid(0,0) — set own pgid to own pid */
    int ret = setpgid(0, 0);
    UT_ASSERT_EQ(ret, 0);
    pid_t new_pgid = getpgid(0);
    UT_ASSERT_EQ(new_pgid, pid);

    /* 5. setsid — creates new session (becomes session + pgrp leader) */
    pid_t sid = setsid();
    UT_ASSERT_EQ(sid, pid);

    /* 6. After setsid, pgid == pid (session leader is also pgrp leader) */
    pid_t pgid_after = getpgid(0);
    UT_ASSERT_EQ(pgid_after, pid);

    /* 7. Parent can update and read a live child's process group */
    {
        char *argv[] = {(char *)"/bin/sleep", (char *)"1", (char *)0};
        pid_t child = vfork();
        if (child == 0) {
            execve(argv[0], argv, (void *)0);
            _exit(127);
        }

        UT_ASSERT(child > 0, "vfork child should start");
        if (child > 0) {
            int status = 0;
            UT_ASSERT_EQ(setpgid(child, child), 0);
            UT_ASSERT_EQ(getpgid(child), child);
            UT_ASSERT_EQ(waitpid(child, &status, 0), child);
            UT_ASSERT(WIFEXITED(status), "sleep child should exit");
            UT_ASSERT_EQ(WEXITSTATUS(status), 0);
        }
    }

    UT_SUMMARY("test_id");
}
