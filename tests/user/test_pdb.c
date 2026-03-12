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
static char arg_show_sp[] = "show sp";
static char arg_show_surface[] = "show surface";
static char arg_where[] = "where";
static char arg_x[] = "x/2h 0x0100";
static char arg_disas[] = "disas 0x0100 3";
static char arg_break[] = "break 0x0101";
static char arg_disable[] = "disable 0";
static char arg_enable[] = "enable 0";
static char arg_info_break[] = "info break";
static char arg_delete[] = "delete 0";
static char arg_setreg[] = "set reg wz 0x1234";
static char arg_setmem[] = "set mem 0x0100 0x00000000";
static char arg_next[] = "next";
static char arg_step[] = "step";
static char arg_detach[] = "detach";
static char arg_target[] = "/tmp/pdb_smoke.com";

#endif

int main(void)
{
#if !defined(__m68k__)
    UT_ASSERT(1, "pdb smoke is currently enabled on m68k only");
    UT_SUMMARY("test_pdb");
#else
    char out[3072];
    int status = 0;
    int n = 0;
    char *argv[33];
    int a = 0;

    argv[a++] = arg_prog;
    argv[a++] = arg_opt;
    argv[a++] = arg_show_sp;
    argv[a++] = arg_opt;
    argv[a++] = arg_show_surface;
    argv[a++] = arg_opt;
    argv[a++] = arg_where;
    argv[a++] = arg_opt;
    argv[a++] = arg_x;
    argv[a++] = arg_opt;
    argv[a++] = arg_disas;
    argv[a++] = arg_opt;
    argv[a++] = arg_break;
    argv[a++] = arg_opt;
    argv[a++] = arg_disable;
    argv[a++] = arg_opt;
    argv[a++] = arg_enable;
    argv[a++] = arg_opt;
    argv[a++] = arg_info_break;
    argv[a++] = arg_opt;
    argv[a++] = arg_delete;
    argv[a++] = arg_opt;
    argv[a++] = arg_setreg;
    argv[a++] = arg_opt;
    argv[a++] = arg_setmem;
    argv[a++] = arg_opt;
    argv[a++] = arg_next;
    argv[a++] = arg_opt;
    argv[a++] = arg_step;
    argv[a++] = arg_opt;
    argv[a++] = arg_detach;
    argv[a++] = arg_target;
    argv[a] = (char *)0;

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
    UT_ASSERT(str_contains(out, "reason=step"),
              "output should include decoded debug-stop reason");
    UT_ASSERT(str_contains(out, "pdb> show sp"),
              "output should include scripted show sp command");
    UT_ASSERT(str_contains(out, "sp=0x"),
              "output should include show sp output");
    UT_ASSERT(str_contains(out, "pdb> show surface"),
              "output should include scripted show surface command");
    UT_ASSERT(str_contains(out, "surface=ecpu"),
              "output should include show surface output");
    UT_ASSERT(str_contains(out, "pdb> where"),
              "output should include scripted where command");
    UT_ASSERT(str_contains(out, "pc=0x") && str_contains(out, " sp=0x"),
              "output should include where output");
    UT_ASSERT(str_contains(out, "pdb> x/2h 0x0100"),
              "output should include scripted x/<n><fmt> command");
    UT_ASSERT(str_contains(out, "0x00000100:"),
              "output should include memory examine result");
    UT_ASSERT(str_contains(out, "0x00000102: 0x000e"),
              "output should include halfword-form memory output");
    UT_ASSERT(str_contains(out, "pdb> disas 0x0100 3"),
              "output should include scripted disassembly command");
    UT_ASSERT(str_contains(out, "0x00000100: nop"),
              "output should include Z80 disassembly");
    UT_ASSERT(str_contains(out, "pdb> break 0x0101"),
              "output should include scripted break command");
    UT_ASSERT(str_contains(out, "bp 0 @ 0x00000101"),
              "output should include breakpoint creation result");
    UT_ASSERT(str_contains(out, "pdb> disable 0"),
              "output should include scripted disable command");
    UT_ASSERT(str_contains(out, "bp 0 disabled"),
              "output should include breakpoint disable result");
    UT_ASSERT(str_contains(out, "pdb> enable 0"),
              "output should include scripted enable command");
    UT_ASSERT(str_contains(out, "bp 0 enabled"),
              "output should include breakpoint enable result");
    UT_ASSERT(str_contains(out, "pdb> info break"),
              "output should include scripted info break command");
    UT_ASSERT(str_contains(out, "bp 0 @ 0x00000101 enabled"),
              "output should include enabled breakpoint info");
    UT_ASSERT(str_contains(out, "pdb> delete 0"),
              "output should include scripted delete command");
    UT_ASSERT(str_contains(out, "bp 0 cleared"),
              "output should include breakpoint clear result");
    UT_ASSERT(str_contains(out, "pdb> set reg wz 0x1234"),
              "output should include scripted register write command");
    UT_ASSERT(str_contains(out, "reg wz=0x1234"),
              "output should include register write result");
    UT_ASSERT(str_contains(out, "pdb> set mem 0x0100 0x00000000"),
              "output should include scripted memory write command");
    UT_ASSERT(str_contains(out, "mem 0x00000100=0x00000000"),
              "output should include memory write result");
    UT_ASSERT(str_contains(out, "pdb> next"),
              "output should include scripted next command");
    UT_ASSERT(str_contains(out, "pdb> detach"),
              "output should include scripted detach command");
    UT_ASSERT(str_contains(out, "detached"),
              "output should include detach result");
    UT_ASSERT(!str_contains(out, "unknown command"),
              "output should not include unknown command error");
    UT_ASSERT(!str_contains(out, "usage: show <abi|event|caps|regset|pc|sp|surface>"),
              "show command should not print usage error");

    UT_SUMMARY("test_pdb");
#endif
}
