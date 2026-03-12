/*
 * test_pdb_arm_disas.c — ARM-only smoke test for /bin/pdb disassembly
 */

#include "utest.h"

#if !defined(__m68k__)

static int str_contains(const char *hay, const char *needle)
{
    int i = 0;
    int j = 0;

    if (!needle[0])
        return 1;

    for (i = 0; hay[i]; i++) {
        for (j = 0; needle[j] && hay[i + j] == needle[j]; j++)
            ;
        if (!needle[j])
            return 1;
    }
    return 0;
}

static int run_capture(char *const argv[], char *buf, int buf_size, int *status)
{
    int pipefd[2];
    int total = 0;
    pid_t pid;

    if (pipe(pipefd) < 0)
        return -1;

    pid = vfork();
    if (pid == 0) {
        int nullfd = open("/dev/null", O_RDONLY, 0);
        if (nullfd >= 0) {
            dup2(nullfd, 0);
            close(nullfd);
        } else {
            close(0);
        }
        close(pipefd[0]);
        dup2(pipefd[1], 1);
        dup2(pipefd[1], 2);
        close(pipefd[1]);
        execve(argv[0], argv, (void *)0);
        _exit(127);
    }

    close(pipefd[1]);
    while (total < buf_size - 1) {
        int n = read(pipefd[0], buf + total, (size_t)(buf_size - 1 - total));
        if (n <= 0)
            break;
        total += n;
    }
    buf[total] = '\0';
    close(pipefd[0]);

    waitpid(pid, status, 0);
    return total;
}

int main(void)
{
    char *argv[10];
    char out[768];
    int status = 0;
    int n = 0;
    int a = 0;

    argv[a++] = "/bin/pdb";
    argv[a++] = "-q";
    argv[a++] = "-c";
    argv[a++] = "show regset";
    argv[a++] = "-c";
    argv[a++] = "disas";
    argv[a++] = "-c";
    argv[a++] = "cont";
    argv[a++] = "/bin/hello";
    argv[a] = (char *)0;

    n = run_capture(argv, out, sizeof(out), &status);
    UT_ASSERT(n > 0, "pdb arm disas smoke should produce output");
    UT_ASSERT(WIFEXITED(status), "pdb arm disas smoke should exit normally");
    UT_ASSERT_EQ(WEXITSTATUS(status), 0);
    UT_ASSERT(str_contains(out, "regset=arm"),
              "output should include ARM regset");
    UT_ASSERT(str_contains(out, "0x"),
              "output should include disassembly address output");
    UT_ASSERT(!str_contains(out, "disas currently supports"),
              "disas should be supported for ARM regset");

    UT_SUMMARY("test_pdb_arm_disas");
}

#else

int main(void)
{
    UT_ASSERT(1, "test_pdb_arm_disas is ARM-only");
    UT_SUMMARY("test_pdb_arm_disas");
}

#endif
