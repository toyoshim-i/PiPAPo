/*
 * test_pdb.c — Smoke test for /bin/pdb scripted mode
 */

#include "utest.h"

#if !defined(__m68k__)

static int str_contains_basic(const char *hay, const char *needle)
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

static int run_capture_basic(char *const argv[], char *buf, int buf_size, int *status)
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

#endif

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

static int write_repeat_line(const char *path, const char *line, int repeat)
{
    int fd;
    int len = 0;

    unlink(path);
    while (line[len])
        len++;
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    for (int i = 0; i < repeat; i++) {
        int n = write(fd, line, (size_t)len);
        if (n != len) {
            close(fd);
            unlink(path);
            return -1;
        }
    }
    close(fd);
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

static int str_count(const char *hay, const char *needle)
{
    int i = 0;
    int j = 0;
    int count = 0;

    if (!needle[0])
        return 0;

    for (i = 0; hay[i]; i++) {
        for (j = 0; needle[j] && hay[i + j] == needle[j]; j++)
            ;
        if (!needle[j])
            count++;
    }
    return count;
}

static int str_index(const char *hay, const char *needle)
{
    int i = 0;
    int j = 0;

    if (!needle[0])
        return 0;

    for (i = 0; hay[i]; i++) {
        for (j = 0; needle[j] && hay[i + j] == needle[j]; j++)
            ;
        if (!needle[j])
            return i;
    }
    return -1;
}

static void u32_to_dec(uint32_t value, char *buf, int buf_size)
{
    char tmp[16];
    int n = 0;

    if (buf_size <= 0)
        return;

    if (value == 0) {
        if (buf_size > 1) {
            buf[0] = '0';
            buf[1] = '\0';
        } else {
            buf[0] = '\0';
        }
        return;
    }

    while (value > 0 && n < (int)sizeof(tmp)) {
        tmp[n++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    if (n + 1 > buf_size) {
        buf[0] = '\0';
        return;
    }
    for (int i = 0; i < n; i++)
        buf[i] = tmp[n - 1 - i];
    buf[n] = '\0';
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

static const uint8_t pdb_smoke_com[] = {
    0x00,                   /* NOP @ 0100 */
    0x00,                   /* NOP @ 0101 */
    0x0E, 0x00,             /* LD C,0 */
    0xCD, 0x05, 0x00,       /* CALL 0005h */
};
static const uint8_t pdb_trim_script[] = {
    '#', ' ', 'c', 'o', 'm', 'm', 'e', 'n', 't', '\n',
    ' ', ' ', '#', ' ', 'l', 'e', 'a', 'd', 'i', 'n', 'g', '\n',
    '\n',
    ' ', ' ', 's', 'h', 'o', 'w', ' ', 'r', 'e', 'g', 's', 'e', 't', ' ', ' ', '\n',
    ' ', 's', 'h', 'o', 'w', ' ', 'c', 'a', 'p', 's', '\n',
    ' ', 'q', ' ', '\n',
};
static const uint8_t pdb_crlf_script[] = {
    '#', ' ', 'c', 'r', 'l', 'f', ' ', 'c', 'o', 'm', 'm', 'e', 'n', 't', '\r', '\n',
    ' ', ' ', 's', 'h', 'o', 'w', ' ', 'r', 'e', 'g', 's', 'e', 't', ' ', '\r', '\n',
    's', 'h', 'o', 'w', ' ', 'c', 'a', 'p', 's', '\r', '\n',
    'q', '\r', '\n',
};
static const uint8_t pdb_order_a_script[] = {
    's', 'h', 'o', 'w', ' ', 'r', 'e', 'g', 's', 'e', 't', '\n',
};
static const uint8_t pdb_order_b_script[] = {
    's', 'h', 'o', 'w', ' ', 'c', 'a', 'p', 's', '\n',
};

static char arg_prog[] = "/bin/pdb";
static char arg_help[] = "-h";
static char arg_help_long[] = "--help";
static char arg_quiet[] = "-q";
static char arg_opt[] = "-c";
static char arg_file_opt[] = "-f";
static char arg_attach_opt[] = "--attach";
static char arg_zero[] = "0";
static char arg_bad_pid[] = "abc";
static char arg_big_pid[] = "2147483647";
static char arg_dev_null[] = "/dev/null";
static char arg_long_script[] = "/tmp/pdb_long.script";
static char arg_missing_script[] = "/tmp/pdb_missing.script";
static char arg_many_script[] = "/tmp/pdb_many.script";
static char arg_trim_script[] = "/tmp/pdb_trim.script";
static char arg_crlf_script[] = "/tmp/pdb_crlf.script";
static char arg_order_a_script[] = "/tmp/pdb_order_a.script";
static char arg_order_b_script[] = "/tmp/pdb_order_b.script";
static char arg_blank_cmd[] = "   ";
static char arg_event_short[] = "event";
static char arg_show_event[] = "show event";
static char arg_event_invalid[] = "event nope";
static char arg_show_only[] = "show";
static char arg_show_pc_extra[] = "show pc extra";
static char arg_caps_short[] = "caps";
static char arg_caps_invalid[] = "caps nope";
static char arg_show_caps[] = "show caps";
static char arg_show_pc[] = "show pc";
static char arg_show_regset[] = "show regset";
static char arg_show_sp[] = "show sp";
static char arg_pc_short[] = "pc";
static char arg_sp_short[] = "sp";
static char arg_pc_invalid[] = "pc nope";
static char arg_sp_invalid[] = "sp nope";
static char arg_where_short[] = "w";
static char arg_where_long[] = "where";
static char arg_where_invalid[] = "where nope";
static char arg_help_short_cmd[] = "?";
static char arg_help_invalid[] = "help nope";
static char arg_regs_invalid[] = "regs nope";
static char arg_reg_wz[] = "reg wz";
static char arg_reg_wz_index[] = "reg 15";
static char arg_reg_extra[] = "reg wz extra";
static char arg_show_surface[] = "show surface";
static char arg_surface_real[] = "surface real";
static char arg_surface_ecpu[] = "surface ecpu";
static char arg_surface_invalid[] = "surface nope";
static char arg_surface_extra[] = "surface real nope";
static char arg_x[] = "x/2h 0x0100";
static char arg_x_missing_addr[] = "x";
static char arg_x_invalid_count_plain[] = "x 0x0100 nope";
static char arg_x_extra_count[] = "x 0x0100 1 2";
static char arg_x_spec_invalid_fmt[] = "x/2z 0x0100";
static char arg_x_spec_zero_count[] = "x/0x 0x0100";
static char arg_x_spec_extra[] = "x/1x 0x0100 1";
static char arg_disas_pc[] = "disas";
static char arg_disas[] = "disas 0x0100 3";
static char arg_disas_invalid_addr[] = "disas nope";
static char arg_disas_invalid_count[] = "disas 0x0100 xyz";
static char arg_disas_extra[] = "disas 0x0100 1 2";
static char arg_break[] = "b 0x0101";
static char arg_break_missing_addr[] = "break";
static char arg_break_extra[] = "break 0x0101 extra";
static char arg_break_long[] = "break 0x0101";
static char arg_info_break[] = "info break";
static char arg_info_break_extra[] = "info break extra";
static char arg_info_break_short[] = "info b";
static char arg_info_only[] = "info";
static char arg_disable[] = "disable 0";
static char arg_enable[] = "enable 0";
static char arg_delete[] = "d 0";
static char arg_delete_long[] = "delete 0";
static char arg_setreg[] = "set reg wz 0x1234";
static char arg_setreg_index[] = "set reg 15 0x2222";
static char arg_setreg_extra[] = "set reg wz 0x1234 extra";
static char arg_setmem[] = "set mem 0x0100 0x00000000";
static char arg_setmem_extra[] = "set mem 0x0100 0x12 b extra";
static char arg_setmem_byte[] = "set mem 0x0101 0x34 1";
static char arg_setmem_half[] = "set mem 0x0102 0x5678 2";
static char arg_setmem_word_num[] = "set mem 0x0200 0x89abcdef w";
static char arg_x_byte_after_setmem[] = "x/1b 0x0101";
static char arg_x_half_after_setmem[] = "x/1h 0x0102";
static char arg_x_word_after_setmem[] = "x/1x 0x0200";
static char arg_setreg_missing_value[] = "set reg wz";
static char arg_setmem_missing_value[] = "set mem 0x0100";
static char arg_setreg_invalid_value[] = "set reg wz xyz";
static char arg_reg_unknown[] = "reg no_such_reg";
static char arg_setreg_unknown[] = "set reg no_such_reg 0x1234";
static char arg_setmem_invalid_value[] = "set mem 0x0100 xyz";
static char arg_setmem_invalid_size[] = "set mem 0x0100 0x12 q";
static char arg_setmem_byte_overflow[] = "set mem 0x0101 0x123 b";
static char arg_setmem_half_overflow[] = "set mem 0x0102 0x12345 h";
static char arg_setmem_half_odd[] = "set mem 0x0101 0x1234 h";
static char arg_setmem_half_cross[] = "set mem 0x0103 0x5678 h";
static char arg_break_invalid_addr[] = "break xyz";
static char arg_disable_invalid_id[] = "disable xyz";
static char arg_enable_invalid_id[] = "enable xyz";
static char arg_delete_invalid_id[] = "delete xyz";
static char arg_disable_extra[] = "disable 0 extra";
static char arg_enable_extra[] = "enable 0 extra";
static char arg_delete_extra[] = "delete 0 extra";
static char arg_next[] = "n";
static char arg_next_long[] = "next";
static char arg_next_invalid[] = "next nope";
static char arg_step[] = "s";
static char arg_step_long[] = "step";
static char arg_step_invalid[] = "step nope";
static char arg_continue[] = "cont";
static char arg_continue_long[] = "continue";
static char arg_continue_invalid[] = "cont nope";
static char arg_c_short[] = "c";
static char arg_run[] = "run";
static char arg_unknown_cmd[] = "no_such_cmd";
static char arg_detach[] = "detach";
static char arg_detach_invalid[] = "detach nope";
static char arg_quit_short[] = "q";
static char arg_quit_long[] = "quit";
static char arg_quit_invalid[] = "quit nope";
static char arg_target[] = "/tmp/pdb_smoke.com";
static char arg_native_target[] = "/bin/hello";
static char arg_sleep[] = "/bin/sleep";
static char arg_sleep_1[] = "1";
static char out_buf[3072];
static char out2_buf[1024];
static uint8_t long_script_line_buf[160];
static char long_cmd_buf[129];
static char attach_pid_buf[16];
static char *argv_buf[40];
static char *argv2_buf[5];
static char *argv3_buf[41];

#endif

int main(void)
{
#if !defined(__m68k__)
    char out[2048];
    int status = 0;
    int n = 0;
    char *argv[8];
    int a = 0;

    a = 0;
    argv[a++] = "/bin/pdb";
    argv[a++] = "-h";
    argv[a++] = (char *)0;
    n = run_capture_basic(argv, out, sizeof(out), &status);
    UT_ASSERT(n > 0, "pdb -h should produce output");
    UT_ASSERT(WIFEXITED(status), "pdb -h should exit");
    UT_ASSERT_EQ(WEXITSTATUS(status), 0);
    UT_ASSERT(str_contains_basic(out, "options:"),
              "pdb -h should print help text");
    UT_ASSERT(str_contains_basic(out, "--batch"),
              "pdb -h should include batch-mode option");
    UT_ASSERT(str_contains_basic(out, "pc                show current program counter"),
              "pdb -h should include pc shorthand help");
    UT_ASSERT(str_contains_basic(out, "sp                show current stack pointer"),
              "pdb -h should include sp shorthand help");
    UT_ASSERT(str_contains_basic(out, "info break|b"),
              "pdb -h should include info break alias help");

    a = 0;
    argv[a++] = "/bin/pdb";
    argv[a++] = "--attach";
    argv[a++] = (char *)0;
    n = run_capture_basic(argv, out, sizeof(out), &status);
    UT_ASSERT(n > 0, "pdb --attach missing pid should produce output");
    UT_ASSERT(WIFEXITED(status), "pdb --attach missing pid should exit");
    UT_ASSERT_EQ(WEXITSTATUS(status), 1);
    UT_ASSERT(str_contains_basic(out, "pdb: --attach requires a pid"),
              "pdb --attach missing pid should report usage error");

    a = 0;
    argv[a++] = "/bin/pdb";
    argv[a++] = "--attach";
    argv[a++] = "0";
    argv[a++] = (char *)0;
    n = run_capture_basic(argv, out, sizeof(out), &status);
    UT_ASSERT(n > 0, "pdb --attach 0 should produce output");
    UT_ASSERT(WIFEXITED(status), "pdb --attach 0 should exit");
    UT_ASSERT_EQ(WEXITSTATUS(status), 1);
    UT_ASSERT(str_contains_basic(out, "pdb: --attach requires a valid positive pid"),
              "pdb --attach 0 should report validation error");

    a = 0;
    argv[a++] = "/bin/pdb";
    argv[a++] = "--attach";
    argv[a++] = "abc";
    argv[a++] = (char *)0;
    n = run_capture_basic(argv, out, sizeof(out), &status);
    UT_ASSERT(n > 0, "pdb --attach non-numeric pid should produce output");
    UT_ASSERT(WIFEXITED(status), "pdb --attach non-numeric pid should exit");
    UT_ASSERT_EQ(WEXITSTATUS(status), 1);
    UT_ASSERT(str_contains_basic(out, "pdb: --attach requires a valid positive pid"),
              "pdb --attach non-numeric pid should report validation error");

    a = 0;
    argv[a++] = "/bin/pdb";
    argv[a++] = "-f";
    argv[a++] = (char *)0;
    n = run_capture_basic(argv, out, sizeof(out), &status);
    UT_ASSERT(n > 0, "pdb -f missing path should produce output");
    UT_ASSERT(WIFEXITED(status), "pdb -f missing path should exit");
    UT_ASSERT_EQ(WEXITSTATUS(status), 1);
    UT_ASSERT(str_contains_basic(out, "pdb: -f requires a script path"),
              "pdb -f missing path should report usage error");

    a = 0;
    argv[a++] = "/bin/pdb";
    argv[a++] = "-q";
    argv[a++] = "-c";
    argv[a++] = "show regset";
    argv[a++] = "-c";
    argv[a++] = "q";
    argv[a++] = "/bin/hello";
    argv[a++] = (char *)0;
    n = run_capture_basic(argv, out, sizeof(out), &status);
    UT_ASSERT(n > 0, "pdb scripted launch should produce output");
    UT_ASSERT(WIFEXITED(status), "pdb scripted launch should exit");
    UT_ASSERT_EQ(WEXITSTATUS(status), 0);
    UT_ASSERT(str_contains_basic(out, "regset=arm"),
              "pdb scripted launch should report arm regset");

    a = 0;
    argv[a++] = "/bin/pdb";
    argv[a++] = "--batch";
    argv[a++] = "-c";
    argv[a++] = "show caps";
    argv[a++] = "-c";
    argv[a++] = "q";
    argv[a++] = "/bin/hello";
    argv[a++] = (char *)0;
    n = run_capture_basic(argv, out, sizeof(out), &status);
    UT_ASSERT(n > 0, "pdb --batch scripted launch should produce output");
    UT_ASSERT(WIFEXITED(status), "pdb --batch scripted launch should exit");
    UT_ASSERT_EQ(WEXITSTATUS(status), 0);
    UT_ASSERT(str_contains_basic(out, "caps="),
              "pdb --batch scripted launch should keep command output");
    UT_ASSERT(str_contains_basic(out, "[getregs"),
              "pdb --batch scripted launch should include decoded capability names");
    UT_ASSERT(!str_contains_basic(out, "target "),
              "pdb --batch should suppress target banner");
    UT_ASSERT(!str_contains_basic(out, "stop "),
              "pdb --batch should suppress automatic stop output");
    UT_ASSERT(!str_contains_basic(out, "pdb> "),
              "pdb --batch should suppress command prompt/echo output");

    a = 0;
    argv[a++] = "/bin/pdb";
    argv[a++] = "-q";
    argv[a++] = "-c";
    argv[a++] = "break 0x0100";
    argv[a++] = "-c";
    argv[a++] = "q";
    argv[a++] = "/bin/hello";
    argv[a++] = (char *)0;
    n = run_capture_basic(argv, out, sizeof(out), &status);
    UT_ASSERT(n > 0, "pdb native break capability smoke should produce output");
    UT_ASSERT(WIFEXITED(status), "pdb native break capability smoke should exit");
    UT_ASSERT_EQ(WEXITSTATUS(status), 0);
    UT_ASSERT(str_contains_basic(out, "pdb: break not supported on this target/mapping"),
              "pdb should report mapping-dependent breakpoint capability");

    UT_SUMMARY("test_pdb");
#else
    char *out = out_buf;
    char *out2 = out2_buf;
    uint8_t *long_script_line = long_script_line_buf;
    char *long_cmd = long_cmd_buf;
    int status = 0;
    int status2 = 0;
    int attach_status = 0;
    int n = 0;
    int n2 = 0;
    pid_t attach_target = -1;
    char *sleep_argv[3];
    char *attach_pid_str = attach_pid_buf;
    char *argv4[20];
    char **argv = argv_buf;
    char **argv2 = argv2_buf;
    char **argv3 = argv3_buf;
    int a = 0;

    argv4[0] = arg_prog;
    argv4[1] = arg_quiet;
    argv4[2] = arg_opt;
    argv4[3] = arg_show_regset;
    argv4[4] = arg_opt;
    argv4[5] = arg_disas_pc;
    argv4[6] = arg_opt;
    argv4[7] = arg_help_short_cmd;
    argv4[8] = arg_opt;
    argv4[9] = arg_c_short;
    argv4[10] = arg_native_target;
    argv4[11] = (char *)0;
    n2 = run_capture(argv4, out2, sizeof(out2_buf), &status2);
    UT_ASSERT(n2 > 0, "pdb disas-m68k smoke should produce output");
    UT_ASSERT(WIFEXITED(status2), "pdb disas-m68k smoke should exit normally");
    UT_ASSERT_EQ(WEXITSTATUS(status2), 0);
    UT_ASSERT(str_contains(out2, "regset=m68k"),
              "output should include m68k regset");
    UT_ASSERT(str_contains(out2, "0x"),
              "output should include disassembly address output");
    UT_ASSERT(str_contains(out2, "commands:"),
              "output should include help alias output");
    UT_ASSERT(!str_contains(out2, "disas currently supports"),
              "disas should be supported for m68k regset");

    argv[a++] = arg_prog;
    argv[a++] = arg_opt;
    argv[a++] = arg_reg_wz;
    argv[a++] = arg_opt;
    argv[a++] = arg_show_surface;
    argv[a++] = arg_opt;
    argv[a++] = arg_surface_real;
    argv[a++] = arg_opt;
    argv[a++] = arg_surface_ecpu;
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
    argv[a++] = arg_delete;
    argv[a++] = arg_opt;
    argv[a++] = arg_setreg;
    argv[a++] = arg_opt;
    argv[a++] = arg_reg_wz_index;
    argv[a++] = arg_opt;
    argv[a++] = arg_setreg_index;
    argv[a++] = arg_opt;
    argv[a++] = arg_reg_wz_index;
    argv[a++] = arg_opt;
    argv[a++] = arg_setmem;
    argv[a++] = arg_opt;
    argv[a++] = arg_next;
    argv[a++] = arg_opt;
    argv[a++] = arg_step;
    argv[a++] = arg_opt;
    argv[a++] = arg_quit_short;
    argv[a++] = arg_target;
    argv[a] = (char *)0;

    UT_ASSERT_EQ(write_blob("/tmp/pdb_smoke.com", pdb_smoke_com,
                            (int)sizeof(pdb_smoke_com)), 0);

    argv3[0] = arg_prog;
    argv3[1] = arg_quiet;
    argv3[2] = arg_opt;
    argv3[3] = arg_show_sp;
    argv3[4] = arg_opt;
    argv3[5] = arg_show_caps;
    argv3[6] = arg_opt;
    argv3[7] = arg_caps_short;
    argv3[8] = arg_opt;
    argv3[9] = arg_show_pc;
    argv3[10] = arg_opt;
    argv3[11] = arg_where_short;
    argv3[12] = arg_opt;
    argv3[13] = arg_where_long;
    argv3[14] = arg_opt;
    argv3[15] = arg_sp_short;
    argv3[16] = arg_opt;
    argv3[17] = arg_pc_short;
    argv3[18] = arg_opt;
    argv3[19] = arg_run;
    argv3[20] = arg_target;
    argv3[21] = (char *)0;
    argv3[22] = (char *)0;
    n2 = run_capture(argv3, out2, sizeof(out2_buf), &status2);
    UT_ASSERT(n2 > 0, "pdb -q should produce output");
    UT_ASSERT(WIFEXITED(status2), "pdb -q should exit normally");
    UT_ASSERT_EQ(WEXITSTATUS(status2), 0);
    UT_ASSERT(str_count(out2, "sp=0x") >= 4,
              "pdb -q should include show sp, sp, w, and where outputs");
    UT_ASSERT(str_count(out2, "caps=") >= 2,
              "pdb -q should include show caps and caps outputs");
    UT_ASSERT(str_count(out2, "pc=0x") >= 4,
              "pdb -q should include show pc, pc, w, and where outputs");
    UT_ASSERT(str_contains(out2, "pc=0x") && str_contains(out2, " sp=0x"),
              "pdb -q should include where output");
    UT_ASSERT(!str_contains(out2, "pdb> "),
              "pdb -q should suppress command prompt/echo output");

    sleep_argv[0] = arg_sleep;
    sleep_argv[1] = arg_sleep_1;
    sleep_argv[2] = (char *)0;
    attach_target = vfork();
    if (attach_target == 0) {
        execve(arg_sleep, sleep_argv, (void *)0);
        _exit(127);
    }
    UT_ASSERT(attach_target > 0, "attach target process should launch");
    u32_to_dec((uint32_t)attach_target, attach_pid_str,
               (int)sizeof(attach_pid_buf));
    argv4[0] = arg_prog;
    argv4[1] = arg_quiet;
    argv4[2] = arg_opt;
    argv4[3] = arg_event_short;
    argv4[4] = arg_opt;
    argv4[5] = arg_show_event;
    argv4[6] = arg_opt;
    argv4[7] = arg_quit_long;
    argv4[8] = arg_attach_opt;
    argv4[9] = attach_pid_str;
    argv4[10] = (char *)0;
    argv4[11] = (char *)0;
    n2 = run_capture(argv4, out2, sizeof(out2_buf), &status2);
    UT_ASSERT(n2 > 0, "pdb --attach should produce output");
    UT_ASSERT(WIFEXITED(status2), "pdb --attach should exit normally");
    UT_ASSERT_EQ(WEXITSTATUS(status2), 0);
    UT_ASSERT(str_contains(out2, "stop debug-stop"),
              "pdb --attach should report initial debug stop");
    UT_ASSERT(str_contains(out2, "detached"),
              "pdb --attach should detach cleanly");
    UT_ASSERT_EQ(waitpid(attach_target, &attach_status, 0), attach_target);
    UT_ASSERT(WIFEXITED(attach_status),
              "attached target should exit after detach");
    UT_ASSERT_EQ(WEXITSTATUS(attach_status), 0);

    attach_target = vfork();
    if (attach_target == 0) {
        execve(arg_sleep, sleep_argv, (void *)0);
        _exit(127);
    }
    UT_ASSERT(attach_target > 0, "attach+show-regs target process should launch");
    u32_to_dec((uint32_t)attach_target, attach_pid_str,
               (int)sizeof(attach_pid_buf));
    argv4[0] = arg_prog;
    argv4[1] = arg_quiet;
    argv4[2] = arg_opt;
    argv4[3] = arg_show_regset;
    argv4[4] = arg_opt;
    argv4[5] = arg_show_pc;
    argv4[6] = arg_opt;
    argv4[7] = arg_show_sp;
    argv4[8] = arg_opt;
    argv4[9] = arg_detach;
    argv4[10] = arg_attach_opt;
    argv4[11] = attach_pid_str;
    argv4[12] = (char *)0;
    argv4[13] = (char *)0;
    n2 = run_capture(argv4, out2, sizeof(out2_buf), &status2);
    UT_ASSERT(n2 > 0, "pdb --attach show-regs smoke should produce output");
    UT_ASSERT(WIFEXITED(status2), "pdb --attach show-regs smoke should exit normally");
    UT_ASSERT_EQ(WEXITSTATUS(status2), 0);
    UT_ASSERT(str_contains(out2, "regset=arm"),
              "pdb --attach show-regs smoke should report native regset");
    UT_ASSERT(str_contains(out2, "pc=0x"),
              "pdb --attach show-regs smoke should report pc");
    UT_ASSERT(str_contains(out2, "sp=0x"),
              "pdb --attach show-regs smoke should report sp");
    UT_ASSERT(str_contains(out2, "detached"),
              "pdb --attach show-regs smoke should detach cleanly");
    UT_ASSERT_EQ(waitpid(attach_target, &attach_status, 0), attach_target);
    UT_ASSERT(WIFEXITED(attach_status),
              "attach+show-regs target should still be reapable by parent");
    UT_ASSERT_EQ(WEXITSTATUS(attach_status), 0);

    attach_target = vfork();
    if (attach_target == 0) {
        execve(arg_sleep, sleep_argv, (void *)0);
        _exit(127);
    }
    UT_ASSERT(attach_target > 0, "attach+pcsp target process should launch");
    u32_to_dec((uint32_t)attach_target, attach_pid_str,
               (int)sizeof(attach_pid_buf));
    argv4[0] = arg_prog;
    argv4[1] = arg_quiet;
    argv4[2] = arg_opt;
    argv4[3] = arg_pc_short;
    argv4[4] = arg_opt;
    argv4[5] = arg_sp_short;
    argv4[6] = arg_opt;
    argv4[7] = arg_detach;
    argv4[8] = arg_attach_opt;
    argv4[9] = attach_pid_str;
    argv4[10] = (char *)0;
    argv4[11] = (char *)0;
    n2 = run_capture(argv4, out2, sizeof(out2_buf), &status2);
    UT_ASSERT(n2 > 0, "pdb --attach pc/sp shorthand smoke should produce output");
    UT_ASSERT(WIFEXITED(status2), "pdb --attach pc/sp shorthand smoke should exit normally");
    UT_ASSERT_EQ(WEXITSTATUS(status2), 0);
    UT_ASSERT(str_contains(out2, "pc=0x"),
              "pdb --attach pc/sp shorthand smoke should report pc");
    UT_ASSERT(str_contains(out2, "sp=0x"),
              "pdb --attach pc/sp shorthand smoke should report sp");
    UT_ASSERT(str_contains(out2, "detached"),
              "pdb --attach pc/sp shorthand smoke should detach cleanly");
    UT_ASSERT_EQ(waitpid(attach_target, &attach_status, 0), attach_target);
    UT_ASSERT(WIFEXITED(attach_status),
              "attach+pcsp target should still be reapable by parent");
    UT_ASSERT_EQ(WEXITSTATUS(attach_status), 0);

    attach_target = vfork();
    if (attach_target == 0) {
        execve(arg_sleep, sleep_argv, (void *)0);
        _exit(127);
    }
    UT_ASSERT(attach_target > 0, "attach+surface target process should launch");
    u32_to_dec((uint32_t)attach_target, attach_pid_str,
               (int)sizeof(attach_pid_buf));
    argv4[0] = arg_prog;
    argv4[1] = arg_quiet;
    argv4[2] = arg_opt;
    argv4[3] = arg_show_surface;
    argv4[4] = arg_opt;
    argv4[5] = arg_surface_ecpu;
    argv4[6] = arg_opt;
    argv4[7] = arg_show_surface;
    argv4[8] = arg_opt;
    argv4[9] = arg_detach;
    argv4[10] = arg_attach_opt;
    argv4[11] = attach_pid_str;
    argv4[12] = (char *)0;
    argv4[13] = (char *)0;
    n2 = run_capture(argv4, out2, sizeof(out2_buf), &status2);
    UT_ASSERT(n2 > 0, "pdb --attach surface smoke should produce output");
    UT_ASSERT(WIFEXITED(status2), "pdb --attach surface smoke should exit normally");
    UT_ASSERT_EQ(WEXITSTATUS(status2), 0);
    UT_ASSERT(str_count(out2, "surface=real") >= 2,
              "pdb --attach surface smoke should keep real surface");
    UT_ASSERT(str_contains(out2, "pdb: SETSURFACE failed rc="),
              "pdb --attach surface smoke should report unsupported ecpu surface");
    UT_ASSERT(str_contains(out2, "target may not support requested surface"),
              "pdb --attach surface smoke should provide actionable surface failure hint");
    UT_ASSERT(str_contains(out2, "detached"),
              "pdb --attach surface smoke should detach cleanly");
    UT_ASSERT_EQ(waitpid(attach_target, &attach_status, 0), attach_target);
    UT_ASSERT(WIFEXITED(attach_status),
              "attach+surface target should still be reapable by parent");
    UT_ASSERT_EQ(WEXITSTATUS(attach_status), 0);

    attach_target = vfork();
    if (attach_target == 0) {
        execve(arg_sleep, sleep_argv, (void *)0);
        _exit(127);
    }
    UT_ASSERT(attach_target > 0, "attach+cont target process should launch");
    u32_to_dec((uint32_t)attach_target, attach_pid_str,
               (int)sizeof(attach_pid_buf));
    argv4[0] = arg_prog;
    argv4[1] = arg_quiet;
    argv4[2] = arg_opt;
    argv4[3] = arg_continue;
    argv4[4] = arg_attach_opt;
    argv4[5] = attach_pid_str;
    argv4[6] = (char *)0;
    argv4[7] = (char *)0;
    argv4[8] = (char *)0;
    argv4[9] = (char *)0;
    n2 = run_capture(argv4, out2, sizeof(out2_buf), &status2);
    UT_ASSERT(n2 > 0, "pdb --attach cont should produce output");
    UT_ASSERT(WIFEXITED(status2), "pdb --attach cont should exit normally");
    UT_ASSERT_EQ(WEXITSTATUS(status2), 0);
    UT_ASSERT(str_contains(out2, "child exited 0"),
              "pdb --attach cont should report child exit");
    UT_ASSERT_EQ(waitpid(attach_target, &attach_status, 0), attach_target);
    UT_ASSERT(WIFEXITED(attach_status),
              "attach+cont target should still be reapable by parent");
    UT_ASSERT_EQ(WEXITSTATUS(attach_status), 0);

    attach_target = vfork();
    if (attach_target == 0) {
        execve(arg_sleep, sleep_argv, (void *)0);
        _exit(127);
    }
    UT_ASSERT(attach_target > 0, "attach+detach target process should launch");
    u32_to_dec((uint32_t)attach_target, attach_pid_str,
               (int)sizeof(attach_pid_buf));
    argv4[0] = arg_prog;
    argv4[1] = arg_quiet;
    argv4[2] = arg_opt;
    argv4[3] = arg_detach;
    argv4[4] = arg_attach_opt;
    argv4[5] = attach_pid_str;
    argv4[6] = (char *)0;
    argv4[7] = (char *)0;
    argv4[8] = (char *)0;
    argv4[9] = (char *)0;
    n2 = run_capture(argv4, out2, sizeof(out2_buf), &status2);
    UT_ASSERT(n2 > 0, "pdb --attach detach should produce output");
    UT_ASSERT(WIFEXITED(status2), "pdb --attach detach should exit normally");
    UT_ASSERT_EQ(WEXITSTATUS(status2), 0);
    UT_ASSERT(str_contains(out2, "detached"),
              "pdb --attach detach should report detach result");
    UT_ASSERT_EQ(waitpid(attach_target, &attach_status, 0), attach_target);
    UT_ASSERT(WIFEXITED(attach_status),
              "attach+detach target should still be reapable by parent");
    UT_ASSERT_EQ(WEXITSTATUS(attach_status), 0);

    attach_target = vfork();
    if (attach_target == 0) {
        execve(arg_sleep, sleep_argv, (void *)0);
        _exit(127);
    }
    UT_ASSERT(attach_target > 0, "attach+unknown target process should launch");
    u32_to_dec((uint32_t)attach_target, attach_pid_str,
               (int)sizeof(attach_pid_buf));
    argv4[0] = arg_prog;
    argv4[1] = arg_quiet;
    argv4[2] = arg_opt;
    argv4[3] = arg_unknown_cmd;
    argv4[4] = arg_opt;
    argv4[5] = arg_detach;
    argv4[6] = arg_attach_opt;
    argv4[7] = attach_pid_str;
    argv4[8] = (char *)0;
    argv4[9] = (char *)0;
    argv4[10] = (char *)0;
    n2 = run_capture(argv4, out2, sizeof(out2_buf), &status2);
    UT_ASSERT(n2 > 0, "pdb --attach unknown-command smoke should produce output");
    UT_ASSERT(WIFEXITED(status2), "pdb --attach unknown-command smoke should exit normally");
    UT_ASSERT_EQ(WEXITSTATUS(status2), 0);
    UT_ASSERT(str_contains(out2, "pdb: unknown command"),
              "pdb --attach unknown-command smoke should report command error");
    UT_ASSERT(str_contains(out2, "detached"),
              "pdb --attach unknown-command smoke should still detach cleanly");
    UT_ASSERT_EQ(waitpid(attach_target, &attach_status, 0), attach_target);
    UT_ASSERT(WIFEXITED(attach_status),
              "attach+unknown target should still be reapable by parent");
    UT_ASSERT_EQ(WEXITSTATUS(attach_status), 0);

    UT_ASSERT_EQ(write_blob("/tmp/pdb_smoke.com", pdb_smoke_com,
                            (int)sizeof(pdb_smoke_com)), 0);
    n = run_capture(argv, out, sizeof(out_buf), &status);

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
    UT_ASSERT(str_contains(out, "pdb> reg wz"),
              "output should include scripted reg command");
    UT_ASSERT(str_contains(out, "wz=0x"),
              "output should include single-register read output");
    UT_ASSERT(str_contains(out, "pdb> show surface"),
              "output should include scripted show surface command");
    UT_ASSERT(str_contains(out, "surface=ecpu"),
              "output should include show surface output");
    UT_ASSERT(str_contains(out, "surfaces=real|ecpu"),
              "output should include available surfaces in caps");
    UT_ASSERT(str_contains(out, "pdb> surface real"),
              "output should include scripted surface real command");
    UT_ASSERT(str_contains(out, "surface=real"),
              "output should include switched real surface output");
    UT_ASSERT(str_contains(out, "pdb> surface ecpu"),
              "output should include scripted surface ecpu command");
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
    UT_ASSERT(str_contains(out, "pdb> b 0x0101"),
              "output should include scripted break alias");
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
    UT_ASSERT(str_contains(out, "pdb> d 0"),
              "output should include scripted delete alias");
    UT_ASSERT(str_contains(out, "bp 0 cleared"),
              "output should include breakpoint clear result");
    UT_ASSERT(str_contains(out, "pdb> set reg wz 0x1234"),
              "output should include scripted register write command");
    UT_ASSERT(str_contains(out, "reg wz=0x1234"),
              "output should include register write result");
    UT_ASSERT(str_contains(out, "pdb> reg 15"),
              "output should include numeric register read command");
    UT_ASSERT(str_contains(out, "pdb> set reg 15 0x2222"),
              "output should include numeric register write command");
    UT_ASSERT(str_contains(out, "reg wz=0x2222"),
              "output should include numeric-index register write result");
    UT_ASSERT(str_contains(out, "pdb> set mem 0x0100 0x00000000"),
              "output should include scripted memory write command");
    UT_ASSERT(str_contains(out, "mem 0x00000100=0x00000000"),
              "output should include memory write result");
    UT_ASSERT(str_contains(out, "pdb> n"),
              "output should include scripted next alias");
    UT_ASSERT(str_contains(out, "pdb> s"),
              "output should include scripted step alias");
    UT_ASSERT(str_contains(out, "pdb> q"),
              "output should include scripted quit alias");
    UT_ASSERT(str_contains(out, "detached"),
              "output should include detach result");
    UT_ASSERT(!str_contains(out, "unknown command"),
              "output should not include unknown command error");
    UT_ASSERT(!str_contains(out, "usage: show <abi|event|caps|regset|pc|sp|surface>"),
              "show command should not print usage error");

    argv4[0] = arg_prog;
    argv4[1] = arg_quiet;
    argv4[2] = arg_opt;
    argv4[3] = arg_setmem_byte;
    argv4[4] = arg_opt;
    argv4[5] = arg_setmem_half;
    argv4[6] = arg_opt;
    argv4[7] = arg_x_byte_after_setmem;
    argv4[8] = arg_opt;
    argv4[9] = arg_x_half_after_setmem;
    argv4[10] = arg_opt;
    argv4[11] = arg_setmem_word_num;
    argv4[12] = arg_opt;
    argv4[13] = arg_x_word_after_setmem;
    argv4[14] = arg_opt;
    argv4[15] = arg_continue;
    argv4[16] = arg_target;
    argv4[17] = (char *)0;
    n2 = run_capture(argv4, out2, sizeof(out2_buf), &status2);
    UT_ASSERT(n2 > 0, "pdb set mem width smoke should produce output");
    UT_ASSERT(WIFEXITED(status2), "pdb set mem width smoke should exit normally");
    UT_ASSERT_EQ(WEXITSTATUS(status2), 0);
    UT_ASSERT(str_contains(out2, "mem 0x00000101=0x34"),
              "pdb set mem width smoke should report byte write result");
    UT_ASSERT(str_contains(out2, "mem 0x00000102=0x5678"),
              "pdb set mem width smoke should report halfword write result");
    UT_ASSERT(str_contains(out2, "mem 0x00000200=0x89abcdef"),
              "pdb set mem width smoke should report word write result");
    UT_ASSERT(str_contains(out2, "0x00000101: 0x34"),
              "pdb set mem width smoke should verify byte memory content");
    UT_ASSERT(str_contains(out2, "0x00000102: 0x5678"),
              "pdb set mem width smoke should verify halfword memory content");
    UT_ASSERT(str_contains(out2, "0x00000200: 0x89abcdef"),
              "pdb set mem width smoke should verify word memory content");
    UT_ASSERT(str_contains(out2, "child exited 0"),
              "pdb set mem width smoke should allow target to exit");

    argv4[0] = arg_prog;
    argv4[1] = arg_quiet;
    argv4[2] = arg_opt;
    argv4[3] = arg_break;
    argv4[4] = arg_opt;
    argv4[5] = arg_info_break;
    argv4[6] = arg_opt;
    argv4[7] = arg_quit_short;
    argv4[8] = arg_target;
    argv4[9] = (char *)0;
    n2 = run_capture(argv4, out2, sizeof(out2_buf), &status2);
    UT_ASSERT(n2 > 0, "pdb info break smoke should produce output");
    UT_ASSERT(WIFEXITED(status2), "pdb info break smoke should exit normally");
    UT_ASSERT_EQ(WEXITSTATUS(status2), 0);
    UT_ASSERT(str_contains(out2, "bp 0 @ 0x00000101 enabled"),
              "output should include info break table output");

    argv4[0] = arg_prog;
    argv4[1] = arg_quiet;
    argv4[2] = arg_opt;
    argv4[3] = arg_break;
    argv4[4] = arg_opt;
    argv4[5] = arg_disable;
    argv4[6] = arg_opt;
    argv4[7] = arg_disable;
    argv4[8] = arg_opt;
    argv4[9] = arg_info_break;
    argv4[10] = arg_opt;
    argv4[11] = arg_enable;
    argv4[12] = arg_opt;
    argv4[13] = arg_enable;
    argv4[14] = arg_opt;
    argv4[15] = arg_quit_short;
    argv4[16] = arg_target;
    argv4[17] = (char *)0;
    n2 = run_capture(argv4, out2, sizeof(out2_buf), &status2);
    UT_ASSERT(n2 > 0, "pdb info break disabled smoke should produce output");
    UT_ASSERT(WIFEXITED(status2), "pdb info break disabled smoke should exit normally");
    UT_ASSERT_EQ(WEXITSTATUS(status2), 0);
    UT_ASSERT(str_contains(out2, "bp 0 disabled"),
              "output should include disable command result");
    UT_ASSERT(str_contains(out2, "bp 0 already disabled"),
              "output should include already-disabled command result");
    UT_ASSERT(str_contains(out2, "bp 0 @ 0x00000101 disabled"),
              "output should include disabled info break table output");
    UT_ASSERT(str_contains(out2, "bp 0 enabled"),
              "output should include enable command result");
    UT_ASSERT(str_contains(out2, "bp 0 already enabled"),
              "output should include already-enabled command result");

    argv4[0] = arg_prog;
    argv4[1] = arg_quiet;
    argv4[2] = arg_opt;
    argv4[3] = arg_break_long;
    argv4[4] = arg_opt;
    argv4[5] = arg_delete_long;
    argv4[6] = arg_opt;
    argv4[7] = arg_continue;
    argv4[8] = arg_target;
    argv4[9] = (char *)0;
    n2 = run_capture(argv4, out2, sizeof(out2_buf), &status2);
    UT_ASSERT(n2 > 0, "pdb long break/delete alias smoke should produce output");
    UT_ASSERT(WIFEXITED(status2), "pdb long break/delete alias smoke should exit normally");
    UT_ASSERT_EQ(WEXITSTATUS(status2), 0);
    UT_ASSERT(str_contains(out2, "bp 0 @ 0x00000101"),
              "pdb long break alias should create a breakpoint");
    UT_ASSERT(str_contains(out2, "bp 0 cleared"),
              "pdb long delete alias should clear a breakpoint");
    UT_ASSERT(str_contains(out2, "child exited 0"),
              "pdb long break/delete smoke should allow child to exit");

    argv4[0] = arg_prog;
    argv4[1] = arg_opt;
    argv4[2] = arg_next_long;
    argv4[3] = arg_opt;
    argv4[4] = arg_step_long;
    argv4[5] = arg_opt;
    argv4[6] = arg_continue_long;
    argv4[7] = arg_target;
    argv4[8] = (char *)0;
    n = run_capture(argv4, out, sizeof(out_buf), &status);
    UT_ASSERT(n > 0, "pdb long next/step/continue alias smoke should produce output");
    UT_ASSERT(WIFEXITED(status), "pdb long next/step/continue alias smoke should exit normally");
    UT_ASSERT_EQ(WEXITSTATUS(status), 0);
    UT_ASSERT(str_contains(out, "pdb> next"),
              "pdb long next alias should be accepted");
    UT_ASSERT(str_contains(out, "pdb> step"),
              "pdb long step alias should be accepted");
    UT_ASSERT(str_contains(out, "pdb> continue"),
              "pdb long continue alias should be accepted");
    UT_ASSERT(str_contains(out, "reason=step"),
              "pdb long next/step smoke should produce step-stop reason");
    UT_ASSERT(str_contains(out, "child exited 0"),
              "pdb long continue alias should allow child to exit");
    UT_ASSERT(!str_contains(out, "pdb: unknown command"),
              "pdb long aliases should not trigger unknown-command errors");

    argv3[0] = arg_prog;
    argv3[1] = arg_quiet;
    argv3[2] = arg_opt;
    argv3[3] = arg_disable;
    argv3[4] = arg_opt;
    argv3[5] = arg_enable;
    argv3[6] = arg_opt;
    argv3[7] = arg_delete_long;
    argv3[8] = arg_opt;
    argv3[9] = arg_break_missing_addr;
    argv3[10] = arg_opt;
    argv3[11] = arg_continue;
    argv3[12] = arg_target;
    argv3[13] = (char *)0;
    n2 = run_capture(argv3, out, sizeof(out_buf), &status2);
    UT_ASSERT(n2 > 0, "pdb breakpoint error-path smoke should produce output");
    UT_ASSERT(WIFEXITED(status2), "pdb breakpoint error-path smoke should exit normally");
    UT_ASSERT_EQ(WEXITSTATUS(status2), 0);
    UT_ASSERT(str_count(out, "pdb: unknown breakpoint id") >= 3,
              "pdb breakpoint error-path smoke should report unknown id for disable/enable/delete");
    UT_ASSERT(str_contains(out, "pdb: usage: break <addr>"),
              "pdb breakpoint error-path smoke should report break usage");
    UT_ASSERT(str_contains(out, "child exited 0"),
              "pdb breakpoint error-path smoke should allow target to exit");

    argv3[0] = arg_prog;
    argv3[1] = arg_quiet;
    argv3[2] = arg_opt;
    argv3[3] = arg_show_only;
    argv3[4] = arg_opt;
    argv3[5] = arg_show_pc_extra;
    argv3[6] = arg_opt;
    argv3[7] = arg_event_invalid;
    argv3[8] = arg_opt;
    argv3[9] = arg_caps_invalid;
    argv3[10] = arg_opt;
    argv3[11] = arg_surface_invalid;
    argv3[12] = arg_opt;
    argv3[13] = arg_x_missing_addr;
    argv3[14] = arg_opt;
    argv3[15] = arg_x_invalid_count_plain;
    argv3[16] = arg_opt;
    argv3[17] = arg_x_spec_invalid_fmt;
    argv3[18] = arg_opt;
    argv3[19] = arg_x_spec_zero_count;
    argv3[20] = arg_opt;
    argv3[21] = arg_disas_invalid_addr;
    argv3[22] = arg_opt;
    argv3[23] = arg_disas_invalid_count;
    argv3[24] = arg_opt;
    argv3[25] = arg_setreg_missing_value;
    argv3[26] = arg_opt;
    argv3[27] = arg_setmem_missing_value;
    argv3[28] = arg_opt;
    argv3[29] = arg_continue;
    argv3[30] = arg_target;
    argv3[31] = (char *)0;
    argv3[32] = (char *)0;
    n2 = run_capture(argv3, out, sizeof(out_buf), &status2);
    UT_ASSERT(n2 > 0, "pdb usage-diagnostics smoke should produce output");
    UT_ASSERT(WIFEXITED(status2), "pdb usage-diagnostics smoke should exit normally");
    UT_ASSERT_EQ(WEXITSTATUS(status2), 0);
    UT_ASSERT(str_contains(out, "pdb: usage: show <abi|event|caps|regset|pc|sp|surface>"),
              "pdb usage-diagnostics smoke should report show usage");
    UT_ASSERT(str_contains(out, "pdb: usage: event"),
              "pdb usage-diagnostics smoke should report event usage");
    UT_ASSERT(str_contains(out, "pdb: usage: caps"),
              "pdb usage-diagnostics smoke should report caps usage");
    UT_ASSERT(str_contains(out, "pdb: usage: surface <real|ecpu>"),
              "pdb usage-diagnostics smoke should report surface usage");
    UT_ASSERT(str_contains(out, "pdb: usage: x <addr> [count]"),
              "pdb usage-diagnostics smoke should report x usage");
    UT_ASSERT(str_contains(out, "pdb: invalid count"),
              "pdb usage-diagnostics smoke should report invalid count");
    UT_ASSERT(str_contains(out, "pdb: usage: disas [addr] [count]"),
              "pdb usage-diagnostics smoke should report disas usage");
    UT_ASSERT(str_contains(out, "pdb: usage: set reg <name|index> <value>"),
              "pdb usage-diagnostics smoke should report set reg usage");
    UT_ASSERT(str_contains(out, "pdb:    or: set mem <addr> <value> [size]"),
              "pdb usage-diagnostics smoke should report set mem usage");
    UT_ASSERT(str_contains(out, "child exited 0"),
              "pdb usage-diagnostics smoke should allow target to exit");

    argv4[0] = arg_prog;
    argv4[1] = arg_quiet;
    argv4[2] = arg_opt;
    argv4[3] = arg_reg_extra;
    argv4[4] = arg_opt;
    argv4[5] = arg_surface_extra;
    argv4[6] = arg_opt;
    argv4[7] = arg_x_extra_count;
    argv4[8] = arg_opt;
    argv4[9] = arg_x_spec_extra;
    argv4[10] = arg_opt;
    argv4[11] = arg_disas_extra;
    argv4[12] = arg_opt;
    argv4[13] = arg_continue;
    argv4[14] = arg_target;
    argv4[15] = (char *)0;
    n2 = run_capture(argv4, out2, sizeof(out2_buf), &status2);
    UT_ASSERT(n2 > 0, "pdb inspect-command usage smoke should produce output");
    UT_ASSERT(WIFEXITED(status2), "pdb inspect-command usage smoke should exit normally");
    UT_ASSERT_EQ(WEXITSTATUS(status2), 0);
    UT_ASSERT(str_contains(out2, "pdb: usage: reg <name|index>"),
              "pdb inspect-command usage smoke should report reg usage");
    UT_ASSERT(str_contains(out2, "pdb: usage: surface <real|ecpu>"),
              "pdb inspect-command usage smoke should report surface usage");
    UT_ASSERT(str_contains(out2, "pdb: usage: x <addr> [count]"),
              "pdb inspect-command usage smoke should report x usage");
    UT_ASSERT(str_contains(out2, "pdb: usage: disas [addr] [count]"),
              "pdb inspect-command usage smoke should report disas usage");
    UT_ASSERT(str_contains(out2, "child exited 0"),
              "pdb inspect-command usage smoke should allow target to exit");

    argv4[0] = arg_prog;
    argv4[1] = arg_quiet;
    argv4[2] = arg_opt;
    argv4[3] = arg_setreg_extra;
    argv4[4] = arg_opt;
    argv4[5] = arg_setmem_extra;
    argv4[6] = arg_opt;
    argv4[7] = arg_break_extra;
    argv4[8] = arg_opt;
    argv4[9] = arg_disable_extra;
    argv4[10] = arg_opt;
    argv4[11] = arg_enable_extra;
    argv4[12] = arg_opt;
    argv4[13] = arg_delete_extra;
    argv4[14] = arg_opt;
    argv4[15] = arg_continue;
    argv4[16] = arg_target;
    argv4[17] = (char *)0;
    n2 = run_capture(argv4, out2, sizeof(out2_buf), &status2);
    UT_ASSERT(n2 > 0, "pdb mutation-command usage smoke should produce output");
    UT_ASSERT(WIFEXITED(status2), "pdb mutation-command usage smoke should exit normally");
    UT_ASSERT_EQ(WEXITSTATUS(status2), 0);
    UT_ASSERT(str_contains(out2, "pdb: usage: set reg <name|index> <value>"),
              "pdb mutation-command usage smoke should report set reg usage");
    UT_ASSERT(str_contains(out2, "pdb:    or: set mem <addr> <value> [size]"),
              "pdb mutation-command usage smoke should report set mem usage");
    UT_ASSERT(str_contains(out2, "pdb: usage: break <addr>"),
              "pdb mutation-command usage smoke should report break usage");
    UT_ASSERT(str_contains(out2, "pdb: usage: disable <id>"),
              "pdb mutation-command usage smoke should report disable usage");
    UT_ASSERT(str_contains(out2, "pdb: usage: enable <id>"),
              "pdb mutation-command usage smoke should report enable usage");
    UT_ASSERT(str_contains(out2, "pdb: usage: delete <id>"),
              "pdb mutation-command usage smoke should report delete usage");
    UT_ASSERT(str_contains(out2, "child exited 0"),
              "pdb mutation-command usage smoke should allow target to exit");

    argv3[0] = arg_prog;
    argv3[1] = arg_quiet;
    argv3[2] = arg_opt;
    argv3[3] = arg_help_invalid;
    argv3[4] = arg_opt;
    argv3[5] = arg_regs_invalid;
    argv3[6] = arg_opt;
    argv3[7] = arg_where_invalid;
    argv3[8] = arg_opt;
    argv3[9] = arg_next_invalid;
    argv3[10] = arg_opt;
    argv3[11] = arg_step_invalid;
    argv3[12] = arg_opt;
    argv3[13] = arg_continue_invalid;
    argv3[14] = arg_opt;
    argv3[15] = arg_detach_invalid;
    argv3[16] = arg_opt;
    argv3[17] = arg_quit_invalid;
    argv3[18] = arg_opt;
    argv3[19] = arg_continue;
    argv3[20] = arg_target;
    argv3[21] = (char *)0;
    argv3[22] = (char *)0;
    n2 = run_capture(argv3, out, sizeof(out_buf), &status2);
    UT_ASSERT(n2 > 0, "pdb control-command usage smoke should produce output");
    UT_ASSERT(WIFEXITED(status2), "pdb control-command usage smoke should exit normally");
    UT_ASSERT_EQ(WEXITSTATUS(status2), 0);
    UT_ASSERT(str_contains(out, "pdb: usage: help"),
              "pdb control-command usage smoke should report help usage");
    UT_ASSERT(str_contains(out, "pdb: usage: regs"),
              "pdb control-command usage smoke should report regs usage");
    UT_ASSERT(str_contains(out, "pdb: usage: where"),
              "pdb control-command usage smoke should report where usage");
    UT_ASSERT(str_contains(out, "pdb: usage: next"),
              "pdb control-command usage smoke should report next usage");
    UT_ASSERT(str_contains(out, "pdb: usage: step"),
              "pdb control-command usage smoke should report step usage");
    UT_ASSERT(str_contains(out, "pdb: usage: cont"),
              "pdb control-command usage smoke should report cont usage");
    UT_ASSERT(str_contains(out, "pdb: usage: detach"),
              "pdb control-command usage smoke should report detach usage");
    UT_ASSERT(str_contains(out, "pdb: usage: quit"),
              "pdb control-command usage smoke should report quit usage");
    UT_ASSERT(str_contains(out, "child exited 0"),
              "pdb control-command usage smoke should allow target to exit");

    argv4[0] = arg_prog;
    argv4[1] = arg_quiet;
    argv4[2] = arg_opt;
    argv4[3] = arg_info_only;
    argv4[4] = arg_opt;
    argv4[5] = arg_continue;
    argv4[6] = arg_target;
    argv4[7] = (char *)0;
    n2 = run_capture(argv4, out2, sizeof(out2_buf), &status2);
    UT_ASSERT(n2 > 0, "pdb info usage smoke should produce output");
    UT_ASSERT(WIFEXITED(status2), "pdb info usage smoke should exit normally");
    UT_ASSERT_EQ(WEXITSTATUS(status2), 0);
    UT_ASSERT(str_contains(out2, "pdb: usage: info break"),
              "pdb info usage smoke should report info usage");
    UT_ASSERT(str_contains(out2, "child exited 0"),
              "pdb info usage smoke should allow target to exit");

    argv4[0] = arg_prog;
    argv4[1] = arg_quiet;
    argv4[2] = arg_opt;
    argv4[3] = arg_info_break;
    argv4[4] = arg_opt;
    argv4[5] = arg_continue;
    argv4[6] = arg_target;
    argv4[7] = (char *)0;
    n2 = run_capture(argv4, out2, sizeof(out2_buf), &status2);
    UT_ASSERT(n2 > 0, "pdb info break empty-table smoke should produce output");
    UT_ASSERT(WIFEXITED(status2), "pdb info break empty-table smoke should exit normally");
    UT_ASSERT_EQ(WEXITSTATUS(status2), 0);
    UT_ASSERT(str_contains(out2, "no breakpoints"),
              "pdb info break empty-table smoke should report empty table");
    UT_ASSERT(str_contains(out2, "child exited 0"),
              "pdb info break empty-table smoke should allow target to exit");

    argv4[0] = arg_prog;
    argv4[1] = arg_quiet;
    argv4[2] = arg_opt;
    argv4[3] = arg_info_break_short;
    argv4[4] = arg_opt;
    argv4[5] = arg_info_break_extra;
    argv4[6] = arg_opt;
    argv4[7] = arg_continue;
    argv4[8] = arg_target;
    argv4[9] = (char *)0;
    n2 = run_capture(argv4, out2, sizeof(out2_buf), &status2);
    UT_ASSERT(n2 > 0, "pdb info break alias/usage smoke should produce output");
    UT_ASSERT(WIFEXITED(status2), "pdb info break alias/usage smoke should exit normally");
    UT_ASSERT_EQ(WEXITSTATUS(status2), 0);
    UT_ASSERT(str_contains(out2, "no breakpoints"),
              "pdb info break alias/usage smoke should accept short info b form");
    UT_ASSERT(str_contains(out2, "pdb: usage: info break"),
              "pdb info break alias/usage smoke should reject extra args");
    UT_ASSERT(str_contains(out2, "child exited 0"),
              "pdb info break alias/usage smoke should allow target to exit");

    argv3[0] = arg_prog;
    argv3[1] = arg_quiet;
    argv3[2] = arg_opt;
    argv3[3] = arg_setreg_invalid_value;
    argv3[4] = arg_opt;
    argv3[5] = arg_reg_unknown;
    argv3[6] = arg_opt;
    argv3[7] = arg_setreg_unknown;
    argv3[8] = arg_opt;
    argv3[9] = arg_setmem_invalid_value;
    argv3[10] = arg_opt;
    argv3[11] = arg_setmem_invalid_size;
    argv3[12] = arg_opt;
    argv3[13] = arg_setmem_byte_overflow;
    argv3[14] = arg_opt;
    argv3[15] = arg_setmem_half_overflow;
    argv3[16] = arg_opt;
    argv3[17] = arg_setmem_half_odd;
    argv3[18] = arg_opt;
    argv3[19] = arg_setmem_half_cross;
    argv3[20] = arg_opt;
    argv3[21] = arg_break_invalid_addr;
    argv3[22] = arg_opt;
    argv3[23] = arg_disable_invalid_id;
    argv3[24] = arg_opt;
    argv3[25] = arg_enable_invalid_id;
    argv3[26] = arg_opt;
    argv3[27] = arg_delete_invalid_id;
    argv3[28] = arg_opt;
    argv3[29] = arg_pc_invalid;
    argv3[30] = arg_opt;
    argv3[31] = arg_sp_invalid;
    argv3[32] = arg_opt;
    argv3[33] = arg_continue;
    argv3[34] = arg_target;
    argv3[35] = (char *)0;
    argv3[36] = (char *)0;
    n2 = run_capture(argv3, out, sizeof(out_buf), &status2);
    UT_ASSERT(n2 > 0, "pdb invalid-number diagnostics smoke should produce output");
    UT_ASSERT(WIFEXITED(status2), "pdb invalid-number diagnostics smoke should exit normally");
    UT_ASSERT_EQ(WEXITSTATUS(status2), 0);
    UT_ASSERT(str_contains(out, "pdb: invalid register value"),
              "pdb invalid-number diagnostics smoke should report set reg value parse failure");
    UT_ASSERT(str_count(out, "pdb: unknown register") >= 2,
              "pdb invalid-number diagnostics smoke should report unknown register for reg/set reg");
    UT_ASSERT(str_contains(out, "pdb: usage: set mem <addr> <value> [size]"),
              "pdb invalid-number diagnostics smoke should report set mem value parse failure");
    UT_ASSERT(str_contains(out, "pdb:        size: b|h|w (or 1|2|4)"),
              "pdb invalid-number diagnostics smoke should report set mem size usage");
    UT_ASSERT(str_contains(out, "pdb: set mem halfword requires even address"),
              "pdb invalid-number diagnostics smoke should report halfword odd-address rejection");
    UT_ASSERT(str_contains(out, "pdb: set mem halfword must not cross word boundary"),
              "pdb invalid-number diagnostics smoke should report halfword cross-word rejection");
    UT_ASSERT(str_contains(out, "pdb: set mem value out of range for size"),
              "pdb invalid-number diagnostics smoke should report size-range rejection");
    UT_ASSERT(str_contains(out, "pdb: usage: break <addr>"),
              "pdb invalid-number diagnostics smoke should report break address parse failure");
    UT_ASSERT(str_contains(out, "pdb: usage: disable <id>"),
              "pdb invalid-number diagnostics smoke should report disable id parse failure");
    UT_ASSERT(str_contains(out, "pdb: usage: enable <id>"),
              "pdb invalid-number diagnostics smoke should report enable id parse failure");
    UT_ASSERT(str_contains(out, "pdb: usage: delete <id>"),
              "pdb invalid-number diagnostics smoke should report delete id parse failure");
    UT_ASSERT(str_contains(out, "pdb: usage: pc"),
              "pdb invalid-number diagnostics smoke should report pc usage");
    UT_ASSERT(str_contains(out, "pdb: usage: sp"),
              "pdb invalid-number diagnostics smoke should report sp usage");
    UT_ASSERT(str_contains(out, "child exited 0"),
              "pdb invalid-number diagnostics smoke should allow target to exit");

    unlink("/tmp/pdb_smoke.com");

    argv2[0] = arg_prog;
    argv2[1] = arg_attach_opt;
    argv2[2] = (char *)0;
    argv2[3] = (char *)0;
    argv2[4] = (char *)0;
    n2 = run_capture(argv2, out2, sizeof(out2_buf), &status2);
    UT_ASSERT(n2 > 0, "pdb --attach missing pid should produce output");
    UT_ASSERT(WIFEXITED(status2), "pdb --attach missing pid should exit");
    UT_ASSERT_EQ(WEXITSTATUS(status2), 1);
    UT_ASSERT(str_contains(out2, "pdb: --attach requires a pid"),
              "pdb --attach missing pid should report usage error");

    argv2[0] = arg_prog;
    argv2[1] = arg_attach_opt;
    argv2[2] = arg_zero;
    argv2[3] = (char *)0;
    argv2[4] = (char *)0;
    n2 = run_capture(argv2, out2, sizeof(out2_buf), &status2);
    UT_ASSERT(n2 > 0, "pdb --attach 0 should produce output");
    UT_ASSERT(WIFEXITED(status2), "pdb --attach 0 should exit");
    UT_ASSERT_EQ(WEXITSTATUS(status2), 1);
    UT_ASSERT(str_contains(out2, "pdb: --attach requires a valid positive pid"),
              "pdb --attach 0 should report validation error");

    argv2[0] = arg_prog;
    argv2[1] = arg_attach_opt;
    argv2[2] = arg_bad_pid;
    argv2[3] = (char *)0;
    argv2[4] = (char *)0;
    n2 = run_capture(argv2, out2, sizeof(out2_buf), &status2);
    UT_ASSERT(n2 > 0, "pdb --attach non-numeric pid should produce output");
    UT_ASSERT(WIFEXITED(status2), "pdb --attach non-numeric pid should exit");
    UT_ASSERT_EQ(WEXITSTATUS(status2), 1);
    UT_ASSERT(str_contains(out2, "pdb: --attach requires a valid positive pid"),
              "pdb --attach non-numeric pid should report validation error");

    argv2[0] = arg_prog;
    argv2[1] = arg_attach_opt;
    argv2[2] = arg_sleep_1;
    argv2[3] = arg_target;
    argv2[4] = (char *)0;
    n2 = run_capture(argv2, out2, sizeof(out2_buf), &status2);
    UT_ASSERT(n2 > 0, "pdb --attach with program path should produce output");
    UT_ASSERT(WIFEXITED(status2), "pdb --attach with program path should exit");
    UT_ASSERT_EQ(WEXITSTATUS(status2), 1);
    UT_ASSERT(str_contains(out2, "pdb: --attach does not take a program path"),
              "pdb --attach with program path should reject mixed launch mode");

    argv2[0] = arg_prog;
    argv2[1] = arg_attach_opt;
    argv2[2] = arg_big_pid;
    argv2[3] = (char *)0;
    argv2[4] = (char *)0;
    n2 = run_capture(argv2, out2, sizeof(out2_buf), &status2);
    UT_ASSERT(n2 > 0, "pdb --attach missing target should produce output");
    UT_ASSERT(WIFEXITED(status2), "pdb --attach missing target should exit");
    UT_ASSERT_EQ(WEXITSTATUS(status2), 1);
    UT_ASSERT(str_contains(out2, "pdb: ATTACH failed rc="),
              "pdb --attach missing target should report attach failure");

    argv2[0] = arg_prog;
    argv2[1] = arg_file_opt;
    unlink(arg_missing_script);
    argv2[2] = arg_missing_script;
    argv2[3] = arg_target;
    argv2[4] = (char *)0;
    n2 = run_capture(argv2, out2, sizeof(out2_buf), &status2);
    UT_ASSERT(n2 > 0, "pdb -f missing file should produce output");
    UT_ASSERT(WIFEXITED(status2), "pdb -f missing file should exit");
    UT_ASSERT_EQ(WEXITSTATUS(status2), 1);
    UT_ASSERT(str_contains(out2, "pdb: cannot open script file: /tmp/pdb_missing.script"),
              "pdb -f missing file should report open failure");

    UT_ASSERT_EQ(write_repeat_line(arg_many_script, "help\n", 33), 0);
    argv2[0] = arg_prog;
    argv2[1] = arg_file_opt;
    argv2[2] = arg_many_script;
    argv2[3] = arg_target;
    argv2[4] = (char *)0;
    n2 = run_capture(argv2, out2, sizeof(out2_buf), &status2);
    unlink(arg_many_script);
    UT_ASSERT(n2 > 0, "pdb -f over-limit script should produce output");
    UT_ASSERT(WIFEXITED(status2), "pdb -f over-limit script should exit");
    UT_ASSERT_EQ(WEXITSTATUS(status2), 1);
    UT_ASSERT(str_contains(out2, "pdb: script command limit exceeded while reading /tmp/pdb_many.script"),
              "pdb -f over-limit script should report command limit");

    argv2[0] = arg_prog;
    argv2[1] = arg_file_opt;
    argv2[2] = (char *)0;
    argv2[3] = (char *)0;
    argv2[4] = (char *)0;
    n2 = run_capture(argv2, out2, sizeof(out2_buf), &status2);
    UT_ASSERT(n2 > 0, "pdb -f missing path should produce output");
    UT_ASSERT(WIFEXITED(status2), "pdb -f missing path should exit");
    UT_ASSERT_EQ(WEXITSTATUS(status2), 1);
    UT_ASSERT(str_contains(out2, "pdb: -f requires a script path"),
              "pdb -f missing path should report usage error");

    argv2[0] = arg_prog;
    argv2[1] = arg_file_opt;
    argv2[2] = arg_dev_null;
    argv2[3] = arg_target;
    argv2[4] = (char *)0;
    n2 = run_capture(argv2, out2, sizeof(out2_buf), &status2);
    UT_ASSERT(n2 > 0, "pdb -f /dev/null should produce output");
    UT_ASSERT(WIFEXITED(status2), "pdb -f /dev/null should exit");
    UT_ASSERT_EQ(WEXITSTATUS(status2), 1);
    UT_ASSERT(str_contains(out2, "pdb: no scripted commands"),
              "pdb -f /dev/null should reject empty scripted mode");

    UT_ASSERT_EQ(write_blob(arg_target, pdb_smoke_com,
                            (int)sizeof(pdb_smoke_com)), 0);
    UT_ASSERT_EQ(write_blob(arg_trim_script, pdb_trim_script,
                            (int)sizeof(pdb_trim_script)), 0);
    argv4[0] = arg_prog;
    argv4[1] = arg_quiet;
    argv4[2] = arg_file_opt;
    argv4[3] = arg_trim_script;
    argv4[4] = arg_target;
    argv4[5] = (char *)0;
    n2 = run_capture(argv4, out2, sizeof(out2_buf), &status2);
    unlink(arg_trim_script);
    UT_ASSERT(n2 > 0, "pdb -f trimmed/comment script should produce output");
    UT_ASSERT(WIFEXITED(status2), "pdb -f trimmed/comment script should exit");
    UT_ASSERT_EQ(WEXITSTATUS(status2), 0);
    UT_ASSERT(str_contains(out2, "regset=z80"),
              "pdb -f should execute trimmed show regset line");
    UT_ASSERT(str_contains(out2, "caps="),
              "pdb -f should execute trimmed show caps line");
    UT_ASSERT(!str_contains(out2, "pdb: no scripted commands"),
              "pdb -f should not treat trimmed script as empty");

    UT_ASSERT_EQ(write_blob(arg_crlf_script, pdb_crlf_script,
                            (int)sizeof(pdb_crlf_script)), 0);
    argv4[0] = arg_prog;
    argv4[1] = arg_quiet;
    argv4[2] = arg_file_opt;
    argv4[3] = arg_crlf_script;
    argv4[4] = arg_target;
    argv4[5] = (char *)0;
    n2 = run_capture(argv4, out2, sizeof(out2_buf), &status2);
    unlink(arg_crlf_script);
    UT_ASSERT(n2 > 0, "pdb -f CRLF script should produce output");
    UT_ASSERT(WIFEXITED(status2), "pdb -f CRLF script should exit");
    UT_ASSERT_EQ(WEXITSTATUS(status2), 0);
    UT_ASSERT(str_contains(out2, "regset=z80"),
              "pdb -f should execute show regset from CRLF script");
    UT_ASSERT(str_contains(out2, "caps="),
              "pdb -f should execute show caps from CRLF script");
    UT_ASSERT(!str_contains(out2, "pdb: no scripted commands"),
              "pdb -f should not treat CRLF script as empty");

    UT_ASSERT_EQ(write_blob(arg_order_a_script, pdb_order_a_script,
                            (int)sizeof(pdb_order_a_script)), 0);
    UT_ASSERT_EQ(write_blob(arg_order_b_script, pdb_order_b_script,
                            (int)sizeof(pdb_order_b_script)), 0);
    argv3[0] = arg_prog;
    argv3[1] = arg_quiet;
    argv3[2] = arg_file_opt;
    argv3[3] = arg_order_a_script;
    argv3[4] = arg_opt;
    argv3[5] = arg_show_sp;
    argv3[6] = arg_file_opt;
    argv3[7] = arg_order_b_script;
    argv3[8] = arg_opt;
    argv3[9] = arg_quit_short;
    argv3[10] = arg_target;
    argv3[11] = (char *)0;
    n2 = run_capture(argv3, out2, sizeof(out2_buf), &status2);
    unlink(arg_order_a_script);
    unlink(arg_order_b_script);
    UT_ASSERT(n2 > 0, "pdb mixed -f/-c order smoke should produce output");
    UT_ASSERT(WIFEXITED(status2), "pdb mixed -f/-c order smoke should exit");
    UT_ASSERT_EQ(WEXITSTATUS(status2), 0);
    {
        int idx_regset = str_index(out2, "regset=z80");
        int idx_sp = str_index(out2, "sp=0x");
        int idx_caps = str_index(out2, "caps=");
        UT_ASSERT(idx_regset >= 0 && idx_sp >= 0 && idx_caps >= 0,
                  "pdb mixed -f/-c order smoke should emit all scripted outputs");
        UT_ASSERT(idx_regset < idx_sp && idx_sp < idx_caps,
                  "pdb mixed -f/-c order smoke should preserve argument order");
    }

    for (int i = 0; i < (int)sizeof(long_script_line_buf) - 2; i++)
        long_script_line[i] = 'x';
    long_script_line[sizeof(long_script_line_buf) - 2] = '\n';
    long_script_line[sizeof(long_script_line_buf) - 1] = '\0';
    UT_ASSERT_EQ(write_blob(arg_long_script, long_script_line,
                            (int)sizeof(long_script_line_buf) - 1), 0);
    argv2[0] = arg_prog;
    argv2[1] = arg_file_opt;
    argv2[2] = arg_long_script;
    argv2[3] = arg_target;
    argv2[4] = (char *)0;
    n2 = run_capture(argv2, out2, sizeof(out2_buf), &status2);
    unlink(arg_long_script);
    UT_ASSERT(n2 > 0, "pdb -f long-line script should produce output");
    UT_ASSERT(WIFEXITED(status2), "pdb -f long-line script should exit");
    UT_ASSERT_EQ(WEXITSTATUS(status2), 1);
    UT_ASSERT(str_contains(out2, "pdb: script line too long in /tmp/pdb_long.script"),
              "pdb -f long-line script should reject oversized command line");

    argv2[0] = arg_prog;
    argv2[1] = arg_opt;
    argv2[2] = arg_blank_cmd;
    argv2[3] = arg_target;
    argv2[4] = (char *)0;
    n2 = run_capture(argv2, out2, sizeof(out2_buf), &status2);
    UT_ASSERT(n2 > 0, "pdb -c blank should produce output");
    UT_ASSERT(WIFEXITED(status2), "pdb -c blank should exit");
    UT_ASSERT_EQ(WEXITSTATUS(status2), 1);
    UT_ASSERT(str_contains(out2, "pdb: no scripted commands"),
              "pdb -c blank should reject empty scripted mode");

    for (int i = 0; i < (int)sizeof(long_cmd_buf) - 1; i++)
        long_cmd[i] = 'x';
    long_cmd[sizeof(long_cmd_buf) - 1] = '\0';
    argv2[0] = arg_prog;
    argv2[1] = arg_opt;
    argv2[2] = long_cmd;
    argv2[3] = arg_target;
    argv2[4] = (char *)0;
    n2 = run_capture(argv2, out2, sizeof(out2_buf), &status2);
    UT_ASSERT(n2 > 0, "pdb -c long command should produce output");
    UT_ASSERT(WIFEXITED(status2), "pdb -c long command should exit");
    UT_ASSERT_EQ(WEXITSTATUS(status2), 1);
    UT_ASSERT(str_contains(out2, "pdb: -c command too long"),
              "pdb -c long command should reject truncated command");

    argv2[0] = arg_prog;
    argv2[1] = arg_help;
    argv2[2] = (char *)0;
    argv2[3] = (char *)0;
    argv2[4] = (char *)0;
    n2 = run_capture(argv2, out2, sizeof(out2_buf), &status2);
    UT_ASSERT(n2 > 0, "pdb -h should produce output");
    UT_ASSERT(WIFEXITED(status2), "pdb -h should exit");
    UT_ASSERT_EQ(WEXITSTATUS(status2), 0);
    UT_ASSERT(str_contains(out2, "options:"),
              "pdb -h should print help text");
    UT_ASSERT(str_contains(out2, "-q"),
              "pdb -h should include quiet-mode option");
    UT_ASSERT(str_contains(out2, "--batch"),
              "pdb -h should include batch-mode option");
    UT_ASSERT(str_contains(out2, "--attach"),
              "pdb -h should include attach option");
    UT_ASSERT(str_contains(out2, "pc                show current program counter"),
              "pdb -h should include pc shorthand help");
    UT_ASSERT(str_contains(out2, "sp                show current stack pointer"),
              "pdb -h should include sp shorthand help");
    argv2[0] = arg_prog;
    argv2[1] = arg_help_long;
    argv2[2] = (char *)0;
    argv2[3] = (char *)0;
    argv2[4] = (char *)0;
    n2 = run_capture(argv2, out, sizeof(out_buf), &status2);
    UT_ASSERT(n2 > 0, "pdb --help should produce output");
    UT_ASSERT(WIFEXITED(status2), "pdb --help should exit");
    UT_ASSERT_EQ(WEXITSTATUS(status2), 0);
    UT_ASSERT(str_contains(out, "commands:"),
              "pdb --help should print command list");
    UT_ASSERT(str_contains(out, "info break|b"),
              "pdb --help should include info break alias help");

    UT_SUMMARY("test_pdb");
#endif
}
