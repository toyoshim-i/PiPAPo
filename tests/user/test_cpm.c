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
    unlink(path);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd < 0)
        return -1;
    int n = write(fd, code, size);
    close(fd);
    if (n != size) {
        unlink(path);
        return -1;
    }
    return 0;
}

#define WRITE_COM(path, code) \
    UT_ASSERT_EQ(write_com((path), (code), (int)sizeof(code)), 0)

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
    unlink(path);
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
    unlink(path);
    return total;
}

static int waitpid_timeout(pid_t pid, int *status, int attempts)
{
    int64_t ts[2] = {0, 1000000};   /* 1 ms */

    while (attempts-- > 0) {
        pid_t rc = waitpid(pid, status, WNOHANG);
        if (rc == pid)
            return 1;
        if (rc < 0)
            return 0;
        nanosleep(ts, (void *)0);
    }

    return 0;
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
    WRITE_COM("/tmp/exit0.com", exit_zero_com);
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
    WRITE_COM("/tmp/hello.com", hello_com);
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
    WRITE_COM("/tmp/charout.com", charout_com);
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
    WRITE_COM("/tmp/loop.com", loop_com);
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
    WRITE_COM("/tmp/version.com", version_com);
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
    WRITE_COM("/tmp/halt.com", halt_com);
    int code = run_com("/tmp/halt.com");
    UT_ASSERT_EQ(code, 0);
}

/* ── Test 7: bad extension — execve should fail for non-.com file ──────── */

static void test_bad_extension(void)
{
    /* Write same code but with .bin extension — should not be detected */
    WRITE_COM("/tmp/test.bin", exit_zero_com);
    pid_t pid = vfork();
    if (pid == 0) {
        execve("/tmp/test.bin", (void *)0, (void *)0);
        _exit(127);   /* exec failed → 127 */
    }
    int status = 0;
    waitpid(pid, &status, 0);
    int code = (status >> 8) & 0xff;
    unlink("/tmp/test.bin");
    UT_ASSERT_EQ(code, 127);   /* should fail — not a .COM file */
}

/* ── Test 8: direct I/O output via BDOS fn 6 ──────────────────────────── */
/*
 * Z80 code:
 *   LD E, 'X'     ; 1E 58      — character to output
 *   LD C, 6       ; 0E 06      — BDOS fn 6 (direct console I/O)
 *   CALL 0x0005   ; CD 05 00
 *   LD E, 'Y'     ; 1E 59
 *   LD C, 6       ; 0E 06
 *   CALL 0x0005   ; CD 05 00
 *   LD C, 0       ; 0E 00      — exit
 *   CALL 0x0005   ; CD 05 00
 */
static const unsigned char directio_com[] = {
    0x1E, 0x58,             /* LD E, 'X'     */
    0x0E, 0x06,             /* LD C, 6       */
    0xCD, 0x05, 0x00,       /* CALL 0x0005   */
    0x1E, 0x59,             /* LD E, 'Y'     */
    0x0E, 0x06,             /* LD C, 6       */
    0xCD, 0x05, 0x00,       /* CALL 0x0005   */
    0x0E, 0x00,             /* LD C, 0       */
    0xCD, 0x05, 0x00,       /* CALL 0x0005   */
};

static void test_direct_io(void)
{
    WRITE_COM("/tmp/directio.com", directio_com);
    char buf[64];
    int n = run_com_capture("/tmp/directio.com", buf, sizeof(buf));
    UT_ASSERT_EQ(n, 2);
    UT_ASSERT(buf[0] == 'X' && buf[1] == 'Y', "direct I/O output XY");
}

/* ── Test 9: select disk + current disk (fn 14, 25) ───────────────────── */
/*
 * Z80 code:
 *   LD E, 2       ; 1E 02      — select drive C (0=A, 1=B, 2=C)
 *   LD C, 14      ; 0E 0E      — BDOS fn 14 (select disk)
 *   CALL 0x0005   ; CD 05 00
 *   LD C, 25      ; 0E 19      — BDOS fn 25 (return current disk)
 *   CALL 0x0005   ; CD 05 00   — A = current drive (should be 2)
 *   ADD A, 'A'    ; C6 41      — convert to letter
 *   LD E, A       ; 5F
 *   LD C, 2       ; 0E 02      — BDOS fn 2 (output)
 *   CALL 0x0005   ; CD 05 00
 *   LD C, 0       ; 0E 00      — exit
 *   CALL 0x0005   ; CD 05 00
 */
static const unsigned char seldisk_com[] = {
    0x1E, 0x02,             /* LD E, 2       */
    0x0E, 0x0E,             /* LD C, 14      */
    0xCD, 0x05, 0x00,       /* CALL 0x0005   */
    0x0E, 0x19,             /* LD C, 25      */
    0xCD, 0x05, 0x00,       /* CALL 0x0005   */
    0xC6, 0x41,             /* ADD A, 'A'    */
    0x5F,                   /* LD E, A       */
    0x0E, 0x02,             /* LD C, 2       */
    0xCD, 0x05, 0x00,       /* CALL 0x0005   */
    0x0E, 0x00,             /* LD C, 0       */
    0xCD, 0x05, 0x00,       /* CALL 0x0005   */
};

static void test_select_disk(void)
{
    WRITE_COM("/tmp/seldisk.com", seldisk_com);
    char buf[64];
    int n = run_com_capture("/tmp/seldisk.com", buf, sizeof(buf));
    UT_ASSERT_EQ(n, 1);
    UT_ASSERT_EQ(buf[0], 'C');   /* drive 2 = C */
}

/* ── Test 10: get/set user code (fn 32) ───────────────────────────────── */
/*
 * Z80 code:
 *   LD E, 5       ; 1E 05      — set user 5
 *   LD C, 32      ; 0E 20      — BDOS fn 32 (set/get user)
 *   CALL 0x0005   ; CD 05 00
 *   LD E, 0xFF    ; 1E FF      — get user (E=0xFF → return current)
 *   LD C, 32      ; 0E 20
 *   CALL 0x0005   ; CD 05 00   — A = current user (should be 5)
 *   ADD A, '0'    ; C6 30      — convert to ASCII
 *   LD E, A       ; 5F
 *   LD C, 2       ; 0E 02      — output
 *   CALL 0x0005   ; CD 05 00
 *   LD C, 0       ; 0E 00      — exit
 *   CALL 0x0005   ; CD 05 00
 */
static const unsigned char usercode_com[] = {
    0x1E, 0x05,             /* LD E, 5       */
    0x0E, 0x20,             /* LD C, 32      */
    0xCD, 0x05, 0x00,       /* CALL 0x0005   */
    0x1E, 0xFF,             /* LD E, 0xFF    */
    0x0E, 0x20,             /* LD C, 32      */
    0xCD, 0x05, 0x00,       /* CALL 0x0005   */
    0xC6, 0x30,             /* ADD A, '0'    */
    0x5F,                   /* LD E, A       */
    0x0E, 0x02,             /* LD C, 2       */
    0xCD, 0x05, 0x00,       /* CALL 0x0005   */
    0x0E, 0x00,             /* LD C, 0       */
    0xCD, 0x05, 0x00,       /* CALL 0x0005   */
};

static void test_user_code(void)
{
    WRITE_COM("/tmp/usercode.com", usercode_com);
    char buf[64];
    int n = run_com_capture("/tmp/usercode.com", buf, sizeof(buf));
    UT_ASSERT_EQ(n, 1);
    UT_ASSERT_EQ(buf[0], '5');   /* user 5 */
}

/* ── Test 11: login vector (fn 24) ────────────────────────────────────── */
/*
 * Z80 code:
 *   LD C, 24      ; 0E 18      — BDOS fn 24 (return login vector)
 *   CALL 0x0005   ; CD 05 00   — HL = login vector (0x0001 = drive A)
 *   LD A, L       ; 7D          — low byte
 *   ADD A, '0'    ; C6 30      — convert to ASCII
 *   LD E, A       ; 5F
 *   LD C, 2       ; 0E 02      — output
 *   CALL 0x0005   ; CD 05 00
 *   LD C, 0       ; 0E 00      — exit
 *   CALL 0x0005   ; CD 05 00
 */
static const unsigned char loginvec_com[] = {
    0x0E, 0x18,             /* LD C, 24      */
    0xCD, 0x05, 0x00,       /* CALL 0x0005   */
    0x7D,                   /* LD A, L       */
    0xC6, 0x30,             /* ADD A, '0'    */
    0x5F,                   /* LD E, A       */
    0x0E, 0x02,             /* LD C, 2       */
    0xCD, 0x05, 0x00,       /* CALL 0x0005   */
    0x0E, 0x00,             /* LD C, 0       */
    0xCD, 0x05, 0x00,       /* CALL 0x0005   */
};

static void test_login_vector(void)
{
    WRITE_COM("/tmp/loginvec.com", loginvec_com);
    char buf[64];
    int n = run_com_capture("/tmp/loginvec.com", buf, sizeof(buf));
    UT_ASSERT_EQ(n, 1);
    UT_ASSERT_EQ(buf[0], '1');   /* login vector = 0x0001 */
}

/* ── Test 12: file I/O — create, write, read back (fn 22/21/20/16) ───── */
/*
 * This test creates /a/ directory from the test harness, then runs a
 * .COM that creates a file, writes "OK", reads it back, and prints it.
 *
 * Z80 code:
 *   ; Set DMA to 0x0080 (default, but explicit)
 *   LD DE, 0x0080 ; 11 80 00   — DMA address
 *   LD C, 26      ; 0E 1A      — BDOS fn 26 (set DMA)
 *   CALL 0x0005   ; CD 05 00
 *
 *   ; Create file (fn 22) — FCB at 0x005C
 *   LD DE, 0x005C ; 11 5C 00   — FCB1 address
 *   LD C, 22      ; 0E 16      — BDOS fn 22 (make file)
 *   CALL 0x0005   ; CD 05 00
 *   CP 0xFF       ; FE FF       — check for error
 *   JP Z, 0x0100+err ; CA xx 01 — jump to error handler
 *
 *   ; Write "OK" + padding to DMA, then write sequential (fn 21)
 *   LD HL, 0x0080 ; 21 80 00
 *   LD (HL), 'O'  ; 36 4F
 *   INC HL        ; 23
 *   LD (HL), 'K'  ; 36 4B
 *   INC HL        ; 23
 *   LD (HL), 0x1A ; 36 1A       — CP/M EOF marker
 *   LD DE, 0x005C ; 11 5C 00
 *   LD C, 21      ; 0E 15       — BDOS fn 21 (write sequential)
 *   CALL 0x0005   ; CD 05 00
 *
 *   ; Close file (fn 16)
 *   LD DE, 0x005C ; 11 5C 00
 *   LD C, 16      ; 0E 10       — BDOS fn 16 (close file)
 *   CALL 0x0005   ; CD 05 00
 *
 *   ; Reset FCB position fields for re-open
 *   XOR A         ; AF
 *   LD (0x006C), A; 32 6C 00    — FCB+12 (extent low)
 *   LD (0x006E), A; 32 6E 00    — FCB+14 (extent high)
 *   LD (0x007C), A; 32 7C 00    — FCB+32 (current record)
 *
 *   ; Open file (fn 15)
 *   LD DE, 0x005C ; 11 5C 00
 *   LD C, 15      ; 0E 0F       — BDOS fn 15 (open file)
 *   CALL 0x0005   ; CD 05 00
 *   CP 0xFF       ; FE FF
 *   JP Z, 0x0100+err ; CA xx 01
 *
 *   ; Read sequential (fn 20)
 *   LD DE, 0x005C ; 11 5C 00
 *   LD C, 20      ; 0E 14       — BDOS fn 20 (read sequential)
 *   CALL 0x0005   ; CD 05 00
 *
 *   ; Print first 2 bytes from DMA (should be "OK")
 *   LD A, (0x0080); 3A 80 00
 *   LD E, A       ; 5F
 *   LD C, 2       ; 0E 02
 *   CALL 0x0005   ; CD 05 00
 *   LD A, (0x0081); 3A 81 00
 *   LD E, A       ; 5F
 *   LD C, 2       ; 0E 02
 *   CALL 0x0005   ; CD 05 00
 *
 *   ; Close and exit
 *   LD DE, 0x005C ; 11 5C 00
 *   LD C, 16      ; 0E 10
 *   CALL 0x0005   ; CD 05 00
 *   LD C, 0       ; 0E 00
 *   CALL 0x0005   ; CD 05 00
 *
 * err:
 *   LD E, '!'     ; 1E 21       — error indicator
 *   LD C, 2       ; 0E 02
 *   CALL 0x0005   ; CD 05 00
 *   LD C, 0       ; 0E 00
 *   CALL 0x0005   ; CD 05 00
 */
static const unsigned char fileio_com[] = {
    /* Set DMA */
    0x11, 0x80, 0x00,       /* LD DE, 0x0080 */
    0x0E, 0x1A,             /* LD C, 26      */
    0xCD, 0x05, 0x00,       /* CALL 0x0005   */
    /* Make file */
    0x11, 0x5C, 0x00,       /* LD DE, 0x005C */
    0x0E, 0x16,             /* LD C, 22      */
    0xCD, 0x05, 0x00,       /* CALL 0x0005   */
    0xFE, 0xFF,             /* CP 0xFF       */
    0xCA,                   /* JP Z, err     */
        0x00, 0x00,         /* (patched below) */
    /* Write "OK" + EOF to DMA */
    0x21, 0x80, 0x00,       /* LD HL, 0x0080 */
    0x36, 0x4F,             /* LD (HL), 'O'  */
    0x23,                   /* INC HL        */
    0x36, 0x4B,             /* LD (HL), 'K'  */
    0x23,                   /* INC HL        */
    0x36, 0x1A,             /* LD (HL), 0x1A */
    /* Write sequential */
    0x11, 0x5C, 0x00,       /* LD DE, 0x005C */
    0x0E, 0x15,             /* LD C, 21      */
    0xCD, 0x05, 0x00,       /* CALL 0x0005   */
    /* Close file */
    0x11, 0x5C, 0x00,       /* LD DE, 0x005C */
    0x0E, 0x10,             /* LD C, 16      */
    0xCD, 0x05, 0x00,       /* CALL 0x0005   */
    /* Reset FCB position */
    0xAF,                   /* XOR A         */
    0x32, 0x6C, 0x00,       /* LD (0x006C), A */
    0x32, 0x6E, 0x00,       /* LD (0x006E), A */
    0x32, 0x7C, 0x00,       /* LD (0x007C), A */
    /* Open file */
    0x11, 0x5C, 0x00,       /* LD DE, 0x005C */
    0x0E, 0x0F,             /* LD C, 15      */
    0xCD, 0x05, 0x00,       /* CALL 0x0005   */
    0xFE, 0xFF,             /* CP 0xFF       */
    0xCA,                   /* JP Z, err     */
        0x00, 0x00,         /* (patched below) */
    /* Read sequential */
    0x11, 0x5C, 0x00,       /* LD DE, 0x005C */
    0x0E, 0x14,             /* LD C, 20      */
    0xCD, 0x05, 0x00,       /* CALL 0x0005   */
    /* Print DMA[0] and DMA[1] */
    0x3A, 0x80, 0x00,       /* LD A, (0x0080) */
    0x5F,                   /* LD E, A       */
    0x0E, 0x02,             /* LD C, 2       */
    0xCD, 0x05, 0x00,       /* CALL 0x0005   */
    0x3A, 0x81, 0x00,       /* LD A, (0x0081) */
    0x5F,                   /* LD E, A       */
    0x0E, 0x02,             /* LD C, 2       */
    0xCD, 0x05, 0x00,       /* CALL 0x0005   */
    /* Close and exit */
    0x11, 0x5C, 0x00,       /* LD DE, 0x005C */
    0x0E, 0x10,             /* LD C, 16      */
    0xCD, 0x05, 0x00,       /* CALL 0x0005   */
    0x0E, 0x00,             /* LD C, 0       */
    0xCD, 0x05, 0x00,       /* CALL 0x0005   */
    /* err: print '!' and exit */
    0x1E, 0x21,             /* LD E, '!'     */
    0x0E, 0x02,             /* LD C, 2       */
    0xCD, 0x05, 0x00,       /* CALL 0x0005   */
    0x0E, 0x00,             /* LD C, 0       */
    0xCD, 0x05, 0x00,       /* CALL 0x0005   */
};

static void test_file_io(void)
{
    /* Create /a/ directory for CP/M drive A */
    mkdir("/a", 0755);

    /* Patch the JP Z addresses for the error handler.
     * The error label is at the end of the code.
     * We need to compute its absolute address (0x0100 + offset). */
    unsigned char code[sizeof(fileio_com)];
    int i;
    for (i = 0; i < (int)sizeof(fileio_com); i++)
        code[i] = fileio_com[i];

    /* err handler is the last 12 bytes: LD E,'!' + fn2 + fn0
     * err_offset = sizeof(code) - 12 */
    int err_offset = (int)sizeof(fileio_com) - 12;
    int err_addr = 0x0100 + err_offset;

    /* First JP Z (after make file): CA at offset 18, addr at [19,20] */
    code[19] = err_addr & 0xFF;
    code[20] = (err_addr >> 8) & 0xFF;
    /* Second JP Z (after open file): CA at offset 68, addr at [69,70] */
    code[69] = err_addr & 0xFF;
    code[70] = (err_addr >> 8) & 0xFF;

    /* The .COM loader sets up FCB1 at 0x005C from the command line.
     * We need to pre-populate FCB1 with a filename.
     * exec_cpm passes argv to cpm_load_com which parses it into FCB1.
     * We'll use execve with argv containing the filename. */

    WRITE_COM("/tmp/fileio.com", code);

    /* Build argv: program name + filename argument for FCB parsing */
    /* execve argv: [path, "TEST.DAT", NULL] */
    /* But our syscall.h execve takes char *const argv[], and PIC
     * doesn't support static pointer arrays.  Build at runtime. */
    char *argv[3];
    argv[0] = "/tmp/fileio.com";
    argv[1] = "TEST.DAT";
    argv[2] = (void *)0;

    int pipefd[2];
    pipe(pipefd);

    pid_t pid = vfork();
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], 1);
        close(pipefd[1]);
        execve("/tmp/fileio.com", argv, (void *)0);
        _exit(127);
    }

    close(pipefd[1]);
    char buf[64];
    int total = 0;
    while (total < 63) {
        int n = read(pipefd[0], buf + total, 63 - total);
        if (n <= 0) break;
        total += n;
    }
    buf[total] = '\0';
    close(pipefd[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    unlink("/tmp/fileio.com");

    UT_ASSERT_EQ(total, 2);
    UT_ASSERT(buf[0] == 'O' && buf[1] == 'K', "file I/O roundtrip OK");

    /* Clean up */
    unlink("/a/TEST.DAT");
    rmdir("/a");
}

/* ── Test 13: CALL 0x0000 warm boot exit ──────────────────────────────── */
/*
 * Z80 code:
 *   LD E, 'W'     ; 1E 57
 *   LD C, 2       ; 0E 02
 *   CALL 0x0005   ; CD 05 00
 *   CALL 0x0000   ; CD 00 00    — warm boot (trap catches CALL to 0)
 */
static const unsigned char warmboot_com[] = {
    0x1E, 0x57,             /* LD E, 'W'     */
    0x0E, 0x02,             /* LD C, 2       */
    0xCD, 0x05, 0x00,       /* CALL 0x0005   */
    0xCD, 0x00, 0x00,       /* CALL 0x0000   */
};

static void test_warm_boot(void)
{
    WRITE_COM("/tmp/warmboot.com", warmboot_com);
    char buf[64];
    int n = run_com_capture("/tmp/warmboot.com", buf, sizeof(buf));
    UT_ASSERT_EQ(n, 1);
    UT_ASSERT_EQ(buf[0], 'W');
}

/* ── Test 14: SIGINT should terminate a blocked CP/M console read ──────── */
/*
 * Z80 code:
 *   LD E, 'R'     ; 1E 52      — ready marker
 *   LD C, 2       ; 0E 02      — BDOS fn 2
 *   CALL 0x0005   ; CD 05 00
 *   LD C, 1       ; 0E 01      — BDOS fn 1 (console input, blocking)
 *   CALL 0x0005   ; CD 05 00
 */
static const unsigned char sigint_read_com[] = {
    0x1E, 0x52,             /* LD E, 'R'     */
    0x0E, 0x02,             /* LD C, 2       */
    0xCD, 0x05, 0x00,       /* CALL 0x0005   */
    0x0E, 0x01,             /* LD C, 1       */
    0xCD, 0x05, 0x00,       /* CALL 0x0005   */
};

static void test_sigint_kills_read(void)
{
    WRITE_COM("/tmp/sigint_read.com", sigint_read_com);

    int pipefd[2];
    UT_ASSERT_EQ(pipe(pipefd), 0);

    pid_t pid = vfork();
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], 1);
        close(pipefd[1]);
        execve("/tmp/sigint_read.com", (void *)0, (void *)0);
        _exit(127);
    }

    close(pipefd[1]);

    char ch = 0;
    int n = read(pipefd[0], &ch, 1);
    close(pipefd[0]);

    UT_ASSERT_EQ(n, 1);
    UT_ASSERT_EQ(ch, 'R');
    UT_ASSERT_EQ(kill(pid, 2), 0);

    int status = 0;
    int reaped = waitpid_timeout(pid, &status, 200);
    if (!reaped) {
        kill(pid, 9);
        waitpid(pid, &status, 0);
    }
    unlink("/tmp/sigint_read.com");

    UT_ASSERT(reaped, "SIGINT should terminate looping CP/M process");
    if (reaped)
        UT_ASSERT_EQ((status >> 8) & 0xff, 128 + 2);
}

/*
 * Z80 code:
 *   LD E, 'R'     ; 1E 52      — ready marker
 *   LD C, 2       ; 0E 02      — BDOS fn 2
 *   CALL 0x0005   ; CD 05 00
 * loop:
 *   JR loop       ; 18 FE
 */
static const unsigned char sigint_spin_com[] = {
    0x1E, 0x52,             /* LD E, 'R'     */
    0x0E, 0x02,             /* LD C, 2       */
    0xCD, 0x05, 0x00,       /* CALL 0x0005   */
    0x18, 0xFE,             /* JR loop       */
};

static void test_sigint_kills_spin(void)
{
    WRITE_COM("/tmp/sigint_spin.com", sigint_spin_com);

    int pipefd[2];
    UT_ASSERT_EQ(pipe(pipefd), 0);

    pid_t pid = vfork();
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], 1);
        close(pipefd[1]);
        execve("/tmp/sigint_spin.com", (void *)0, (void *)0);
        _exit(127);
    }

    close(pipefd[1]);

    char ch = 0;
    int n = read(pipefd[0], &ch, 1);
    close(pipefd[0]);

    UT_ASSERT_EQ(n, 1);
    UT_ASSERT_EQ(ch, 'R');
    UT_ASSERT_EQ(kill(pid, 2), 0);

    int status = 0;
    int reaped = waitpid_timeout(pid, &status, 200);
    if (!reaped) {
        kill(pid, 9);
        waitpid(pid, &status, 0);
    }
    unlink("/tmp/sigint_spin.com");

    UT_ASSERT(reaped, "SIGINT should terminate busy-loop CP/M process");
    if (reaped)
        UT_ASSERT_EQ((status >> 8) & 0xff, 128 + 2);
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
    test_direct_io();
    test_select_disk();
    test_user_code();
    test_login_vector();
    test_file_io();
    test_warm_boot();
    test_sigint_kills_read();
    test_sigint_kills_spin();

    UT_SUMMARY("test_cpm");
}
