/*
 * test_cpm.c — CP/M .COM subsystem userland test
 *
 * Creates small .COM binaries in /tmp, exec()s them via vfork+execve,
 * and verifies exit codes and console output.
 *
 * Each .COM is hand-assembled Z80 machine code loaded at 0x0100.
 */

#include "utest.h"

/* ── Helper: write a .COM file to /tmp ──────────────────────────────────── */

static int write_com(const char *path, const unsigned char *code, int size)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd < 0)
        return -1;
    write(fd, code, size);
    close(fd);
    return 0;
}

/* ── Helper: exec a .COM and return exit code ──────────────────────────── */

static int run_com(const char *path)
{
    pid_t pid = vfork();
    if (pid == 0) {
        execve(path, (void *)0, (void *)0);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return (status >> 8) & 0xff;
}

/* ── Helper: exec a .COM with stdout piped, return output length ───────── */

static int run_com_capture(const char *path, char *buf, int bufsize)
{
    int pipefd[2];
    if (pipe(pipefd) < 0)
        return -1;

    pid_t pid = vfork();
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], 1);
        close(pipefd[1]);
        execve(path, (void *)0, (void *)0);
        _exit(127);
    }

    close(pipefd[1]);

    int total = 0;
    while (total < bufsize - 1) {
        int n = read(pipefd[0], buf + total, bufsize - 1 - total);
        if (n <= 0)
            break;
        total += n;
    }
    buf[total] = '\0';
    close(pipefd[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    return total;
}

/* ── Test 1: exit_zero.com — BDOS fn 0 (system reset / exit) ──────────── */
/*
 * Z80 code (loaded at 0x0100):
 *   LD C, 0       ; 0E 00
 *   CALL 0x0005   ; CD 05 00
 */
static const unsigned char exit_zero_com[] = {
    0x0E, 0x00,             /* LD C, 0       */
    0xCD, 0x05, 0x00,       /* CALL 0x0005   */
};

static void test_exit_zero(void)
{
    write_com("/tmp/exit0.com", exit_zero_com, sizeof(exit_zero_com));
    int code = run_com("/tmp/exit0.com");
    UT_ASSERT_EQ(code, 0);
}

/* ── Test 2: hello.com — print "Hi" via BDOS fn 9, then exit ──────────── */
/*
 * Z80 code (loaded at 0x0100):
 *   LD DE, 0x010D ; 11 0D 01   — pointer to message
 *   LD C, 9       ; 0E 09      — BDOS fn 9 (print string)
 *   CALL 0x0005   ; CD 05 00
 *   LD C, 0       ; 0E 00      — BDOS fn 0 (exit)
 *   CALL 0x0005   ; CD 05 00
 *   ; message: "Hi\r\n$"
 */
static const unsigned char hello_com[] = {
    0x11, 0x0D, 0x01,       /* LD DE, 0x010D */
    0x0E, 0x09,             /* LD C, 9       */
    0xCD, 0x05, 0x00,       /* CALL 0x0005   */
    0x0E, 0x00,             /* LD C, 0       */
    0xCD, 0x05, 0x00,       /* CALL 0x0005   */
    'H', 'i', '\r', '\n', '$',
};

static void test_hello(void)
{
    write_com("/tmp/hello.com", hello_com, sizeof(hello_com));
    char buf[64];
    int n = run_com_capture("/tmp/hello.com", buf, sizeof(buf));
    UT_ASSERT(n >= 2, "hello.com produced output");
    UT_ASSERT(buf[0] == 'H' && buf[1] == 'i', "output starts with Hi");
}

/* ── Test 3: charout.com — print 'A' via BDOS fn 2 ───────────────────── */
/*
 * Z80 code (loaded at 0x0100):
 *   LD E, 'A'     ; 1E 41      — character to output
 *   LD C, 2       ; 0E 02      — BDOS fn 2 (console output)
 *   CALL 0x0005   ; CD 05 00
 *   LD C, 0       ; 0E 00      — exit
 *   CALL 0x0005   ; CD 05 00
 */
static const unsigned char charout_com[] = {
    0x1E, 0x41,             /* LD E, 'A'     */
    0x0E, 0x02,             /* LD C, 2       */
    0xCD, 0x05, 0x00,       /* CALL 0x0005   */
    0x0E, 0x00,             /* LD C, 0       */
    0xCD, 0x05, 0x00,       /* CALL 0x0005   */
};

static void test_charout(void)
{
    write_com("/tmp/charout.com", charout_com, sizeof(charout_com));
    char buf[64];
    int n = run_com_capture("/tmp/charout.com", buf, sizeof(buf));
    UT_ASSERT_EQ(n, 1);
    UT_ASSERT_EQ(buf[0], 'A');
}

/* ── Test 4: loop.com — print "AB" via loop + BDOS fn 2 ──────────────── */
/*
 * Z80 code (loaded at 0x0100):
 *   LD B, 2       ; 06 02      — loop counter
 *   LD E, 'A'     ; 1E 41      — starting character
 * loop:
 *   LD C, 2       ; 0E 02      — BDOS fn 2
 *   CALL 0x0005   ; CD 05 00
 *   INC E         ; 1C          — next character
 *   DJNZ loop     ; 10 F8      — loop: offset = -8 (back to LD C,2)
 *   LD C, 0       ; 0E 00      — exit
 *   CALL 0x0005   ; CD 05 00
 */
static const unsigned char loop_com[] = {
    0x06, 0x02,             /* LD B, 2       */
    0x1E, 0x41,             /* LD E, 'A'     */
    0x0E, 0x02,             /* LD C, 2       */
    0xCD, 0x05, 0x00,       /* CALL 0x0005   */
    0x1C,                   /* INC E         */
    0x10, 0xF8,             /* DJNZ -8       */
    0x0E, 0x00,             /* LD C, 0       */
    0xCD, 0x05, 0x00,       /* CALL 0x0005   */
};

static void test_loop(void)
{
    write_com("/tmp/loop.com", loop_com, sizeof(loop_com));
    char buf[64];
    int n = run_com_capture("/tmp/loop.com", buf, sizeof(buf));
    UT_ASSERT_EQ(n, 2);
    UT_ASSERT(buf[0] == 'A' && buf[1] == 'B', "output is AB");
}

/* ── Test 5: version.com — get CP/M version, print major digit ────────── */
/*
 * Z80 code:
 *   LD C, 12      ; 0E 0C      — BDOS fn 12 (return version)
 *   CALL 0x0005   ; CD 05 00   — returns H=0, L=0x22 (CP/M 2.2)
 *   LD A, L       ; 7D          — version low byte
 *   AND 0x0F      ; E6 0F      — major version (2)
 *   ADD A, '0'    ; C6 30      — convert to ASCII
 *   LD E, A       ; 5F          — character to output
 *   LD C, 2       ; 0E 02      — BDOS fn 2
 *   CALL 0x0005   ; CD 05 00
 *   LD C, 0       ; 0E 00      — exit
 *   CALL 0x0005   ; CD 05 00
 */
static const unsigned char version_com[] = {
    0x0E, 0x0C,             /* LD C, 12      */
    0xCD, 0x05, 0x00,       /* CALL 0x0005   */
    0x7D,                   /* LD A, L       */
    0xE6, 0x0F,             /* AND 0x0F      */
    0xC6, 0x30,             /* ADD A, '0'    */
    0x5F,                   /* LD E, A       */
    0x0E, 0x02,             /* LD C, 2       */
    0xCD, 0x05, 0x00,       /* CALL 0x0005   */
    0x0E, 0x00,             /* LD C, 0       */
    0xCD, 0x05, 0x00,       /* CALL 0x0005   */
};

static void test_version(void)
{
    write_com("/tmp/version.com", version_com, sizeof(version_com));
    char buf[64];
    int n = run_com_capture("/tmp/version.com", buf, sizeof(buf));
    UT_ASSERT_EQ(n, 1);
    UT_ASSERT_EQ(buf[0], '2');   /* CP/M version 2.2 → major = 2 */
}

/* ── Test 6: halt.com — HALT instruction triggers clean exit ──────────── */
/*
 * Z80 code:
 *   HALT          ; 76
 */
static const unsigned char halt_com[] = {
    0x76,                   /* HALT          */
};

static void test_halt_exit(void)
{
    write_com("/tmp/halt.com", halt_com, sizeof(halt_com));
    int code = run_com("/tmp/halt.com");
    UT_ASSERT_EQ(code, 0);
}

/* ── Test 7: bad extension — execve should fail for non-.com file ──────── */

static void test_bad_extension(void)
{
    /* Write same code but with .bin extension — should not be detected */
    write_com("/tmp/test.bin", exit_zero_com, sizeof(exit_zero_com));
    pid_t pid = vfork();
    if (pid == 0) {
        execve("/tmp/test.bin", (void *)0, (void *)0);
        _exit(127);   /* exec failed → 127 */
    }
    int status = 0;
    waitpid(pid, &status, 0);
    int code = (status >> 8) & 0xff;
    UT_ASSERT_EQ(code, 127);   /* should fail — not a .COM file */
}

/* ── Main ───────────────────────────────────────────────────────────────── */

int main(void)
{
    test_exit_zero();
    test_hello();
    test_charout();
    test_loop();
    test_version();
    test_halt_exit();
    test_bad_extension();

    UT_SUMMARY("test_cpm");
}
