/*
 * test_pdb.c — Smoke test for /bin/pdb scripted mode
 */

#include "utest.h"

#if defined(__m68k__)

static int write_blob(const char *path, const uint8_t *data, int size)
{
    int fd;
    int n;

    unlink(path);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd < 0)
        return -1;
    n = write(fd, data, (size_t)size);
    close(fd);
    if (n != size) {
        unlink(path);
        return -1;
    }
    return 0;
}

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

static const uint8_t pdb_smoke_com[] = {
    0x00,                   /* NOP @ 0100 */
    0x00,                   /* NOP @ 0101 */
    0x0E, 0x00,             /* LD C,0 */
    0xCD, 0x05, 0x00,       /* CALL 0005h */
};

static char arg_prog[] = "/bin/pdb";
static char arg_opt[] = "-c";
static char arg_caps[] = "caps";
static char arg_regs[] = "regs";
static char arg_x[] = "x 0x0100 1";
static char arg_setreg[] = "set reg wz 0x1234";
static char arg_setmem[] = "set mem 0x0100 0x00000000";
static char arg_step[] = "step";
static char arg_event[] = "event";
static char arg_quit[] = "quit";
static char arg_target[] = "/tmp/pdb_smoke.com";

#endif

int main(void)
{
#if !defined(__m68k__)
    UT_ASSERT(1, "pdb smoke is currently enabled on m68k only");
    UT_SUMMARY("test_pdb");
#else
    char out[2048];
    int status = 0;
    int n = 0;
    char *argv[19];

    argv[0] = arg_prog;
    argv[1] = arg_opt;
    argv[2] = arg_caps;
    argv[3] = arg_opt;
    argv[4] = arg_regs;
    argv[5] = arg_opt;
    argv[6] = arg_x;
    argv[7] = arg_opt;
    argv[8] = arg_setreg;
    argv[9] = arg_opt;
    argv[10] = arg_setmem;
    argv[11] = arg_opt;
    argv[12] = arg_step;
    argv[13] = arg_opt;
    argv[14] = arg_event;
    argv[15] = arg_opt;
    argv[16] = arg_quit;
    argv[17] = arg_target;
    argv[18] = (char *)0;

    UT_ASSERT_EQ(write_blob("/tmp/pdb_smoke.com", pdb_smoke_com,
                            (int)sizeof(pdb_smoke_com)), 0);

    n = run_capture(argv, out, sizeof(out), &status);
    unlink("/tmp/pdb_smoke.com");

    UT_ASSERT(n > 0, "pdb should produce output");
    UT_ASSERT(WIFEXITED(status), "pdb should exit normally");
    UT_ASSERT_EQ(WEXITSTATUS(status), 0);

    UT_ASSERT(str_contains(out, "target regset=z80"),
              "output should include target caps");
    UT_ASSERT(str_contains(out, "stop exec"), "output should include exec stop");
    UT_ASSERT(str_contains(out, "debug-stop"),
              "output should include single-step debug stop");
    UT_ASSERT(str_contains(out, "0x00000100:"),
              "output should include memory examine result");
    UT_ASSERT(str_contains(out, "pdb> set reg wz 0x1234"),
              "output should include scripted register write command");
    UT_ASSERT(str_contains(out, "reg wz=0x1234"),
              "output should include register write result");
    UT_ASSERT(str_contains(out, "pdb> set mem 0x0100 0x00000000"),
              "output should include scripted memory write command");
    UT_ASSERT(str_contains(out, "mem 0x00000100=0x00000000"),
              "output should include memory write result");
    UT_ASSERT(str_contains(out, "pdb> quit"),
              "output should include scripted quit command");

    UT_SUMMARY("test_pdb");
#endif
}
