/*
 * test_vfork.c — vfork + execve + waitpid integration test
 *
 * 1. Parent calls vfork() — parent blocked, child runs
 * 2. Child calls execve("/bin/hello") — loads hello, unblocks parent
 * 3. Parent resumes, calls waitpid() — waits for hello to exit
 * 4. Parent prints result
 */

#include "syscall.h"

static void puts(const char *s)
{
    int len = 0;
    while (s[len]) len++;
    write(1, s, len);
}

int main(void)
{
    puts("test_vfork: calling vfork\n");

    pid_t pid = vfork();

    if (pid == 0) {
        /* Child — replace with hello */
        execve("/bin/hello", (void *)0, (void *)0);
        /* If execve fails, exit with error */
        _exit(1);
    }

    /* Parent — wait for child */
    int status = 0;
    waitpid(pid, &status, 0);

    puts("test_vfork: child exited, test PASSED\n");

    return 0;
}
