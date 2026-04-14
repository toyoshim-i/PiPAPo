/*
 * test_msdos.c --- MS-DOS .COM subsystem userland test
 *
 * Creates small .COM binaries in /tmp, exec()s them via vfork+execve,
 * and verifies exit codes and console output.
 *
 * Each .COM is hand-assembled 8086 machine code loaded at seg:0100.
 * Requires native i16 host (pcxt target).
 */

#include "utest.h"

/* -- Helper: write a .COM file to /tmp ------------------------------------ */

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

/* -- Helper: exec a .COM and return exit code ----------------------------- */

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

/* -- Helper: exec a .COM with stdout piped, return output length ---------- */

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

/* -- Test 1: exit_zero.com --- INT 21h AH=4Ch exit with code 0 ----------- */
/*
 * 8086 code (loaded at seg:0100):
 *   MOV AX, 4C00h  ; B8 00 4C   — INT 21h fn 4Ch, exit code 0
 *   INT 21h         ; CD 21
 */
static const unsigned char exit_zero_com[] = {
    0xB8, 0x00, 0x4C,      /* MOV AX, 4C00h */
    0xCD, 0x21,             /* INT 21h       */
};

static void test_exit_zero(void)
{
    WRITE_COM("/tmp/dos_ex0.com", exit_zero_com);
    int code = run_com("/tmp/dos_ex0.com");
    UT_ASSERT_EQ(code, 0);
}

/* -- Test 2: exit_code.com --- INT 21h AH=4Ch exit with code 42 ---------- */
/*
 * 8086 code (loaded at seg:0100):
 *   MOV AX, 4C2Ah  ; B8 2A 4C   — exit code 42
 *   INT 21h         ; CD 21
 */
static const unsigned char exit_code_com[] = {
    0xB8, 0x2A, 0x4C,      /* MOV AX, 4C2Ah */
    0xCD, 0x21,             /* INT 21h       */
};

static void test_exit_code(void)
{
    WRITE_COM("/tmp/dos_ex42.com", exit_code_com);
    int code = run_com("/tmp/dos_ex42.com");
    UT_ASSERT_EQ(code, 42);
}

/* -- Test 3: charout.com --- INT 21h AH=02h write character 'A' ---------- */
/*
 * 8086 code (loaded at seg:0100):
 *   MOV AH, 02h    ; B4 02       — write character
 *   MOV DL, 'A'    ; B2 41
 *   INT 21h         ; CD 21
 *   MOV AX, 4C00h  ; B8 00 4C    — exit 0
 *   INT 21h         ; CD 21
 */
static const unsigned char charout_com[] = {
    0xB4, 0x02,             /* MOV AH, 02h   */
    0xB2, 0x41,             /* MOV DL, 'A'   */
    0xCD, 0x21,             /* INT 21h       */
    0xB8, 0x00, 0x4C,       /* MOV AX, 4C00h */
    0xCD, 0x21,             /* INT 21h       */
};

static void test_charout(void)
{
    WRITE_COM("/tmp/dos_chr.com", charout_com);
    char buf[64];
    int n = run_com_capture("/tmp/dos_chr.com", buf, sizeof(buf));
    UT_ASSERT_EQ(n, 1);
    UT_ASSERT_EQ(buf[0], 'A');
}

/* -- Test 4: hello.com --- INT 21h AH=09h print $-terminated string ------ */
/*
 * 8086 code (loaded at seg:0100):
 *   MOV AH, 09h    ; B4 09       — print string
 *   MOV DX, 010Bh  ; BA 0B 01    — offset of message (DS:010B)
 *   INT 21h         ; CD 21
 *   MOV AX, 4C00h  ; B8 00 4C    — exit 0
 *   INT 21h         ; CD 21
 *   ; message at offset 0x0B from .COM start:
 *   db "Hi", 0Dh, 0Ah, '$'
 */
static const unsigned char hello_com[] = {
    0xB4, 0x09,             /* MOV AH, 09h       */
    0xBA, 0x0B, 0x01,       /* MOV DX, 010Bh     */
    0xCD, 0x21,             /* INT 21h           */
    0xB8, 0x00, 0x4C,       /* MOV AX, 4C00h     */
    0xCD, 0x21,             /* INT 21h           */
    'H', 'i', '\r', '\n', '$',
};

static void test_hello(void)
{
    WRITE_COM("/tmp/dos_hi.com", hello_com);
    char buf[64];
    int n = run_com_capture("/tmp/dos_hi.com", buf, sizeof(buf));
    UT_ASSERT(n >= 2, "hello.com produced output");
    UT_ASSERT(buf[0] == 'H' && buf[1] == 'i', "output starts with Hi");
}

/* -- Test 5: version.com --- INT 21h AH=30h get DOS version --------------- */
/*
 * 8086 code (loaded at seg:0100):
 *   MOV AH, 30h    ; B4 30       — get DOS version
 *   INT 21h         ; CD 21       — AL=major(3), AH=minor(30)
 *   CMP AL, 03h    ; 3C 03       — check major == 3
 *   JNE fail       ; 75 05       — skip to fail (+5 bytes)
 *   MOV AX, 4C00h  ; B8 00 4C    — exit 0 (pass)
 *   INT 21h         ; CD 21
 * fail:
 *   MOV AX, 4C01h  ; B8 01 4C    — exit 1 (fail)
 *   INT 21h         ; CD 21
 */
static const unsigned char version_com[] = {
    0xB4, 0x30,             /* MOV AH, 30h   */
    0xCD, 0x21,             /* INT 21h       */
    0x3C, 0x03,             /* CMP AL, 03h   */
    0x75, 0x05,             /* JNE fail      */
    0xB8, 0x00, 0x4C,       /* MOV AX, 4C00h */
    0xCD, 0x21,             /* INT 21h       */
    0xB8, 0x01, 0x4C,       /* MOV AX, 4C01h */
    0xCD, 0x21,             /* INT 21h       */
};

static void test_version(void)
{
    WRITE_COM("/tmp/dos_ver.com", version_com);
    int code = run_com("/tmp/dos_ver.com");
    UT_ASSERT_EQ(code, 0);
}

/* -- main ----------------------------------------------------------------- */

int main(void)
{
    test_exit_zero();
    test_exit_code();
    test_charout();
    test_hello();
    test_version();

    UT_SUMMARY("test_msdos");
}
