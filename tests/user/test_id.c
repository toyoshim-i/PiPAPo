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

    UT_SUMMARY("test_id");
}
