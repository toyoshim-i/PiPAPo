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

    int saved_stdout = dup(1);
    if (saved_stdout < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (dup2(pipefd[1], 1) < 0) {
        close(saved_stdout);
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    close(pipefd[1]);

    pid_t pid = vfork();
    if (pid == 0) {
        execve(path, (void *)0, (void *)0);
        _exit(127);
    }

    dup2(saved_stdout, 1);
    close(saved_stdout);

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
 *   MOV DX, 010Ch  ; BA 0C 01    — offset of message (DS:010C)
 *   INT 21h         ; CD 21
 *   MOV AX, 4C00h  ; B8 00 4C    — exit 0
 *   INT 21h         ; CD 21
 *   ; message at offset 0x0C from .COM start (12 bytes of code above):
 *   db "Hi", 0Dh, 0Ah, '$'
 */
static const unsigned char hello_com[] = {
    0xB4, 0x09,             /* MOV AH, 09h       */
    0xBA, 0x0C, 0x01,       /* MOV DX, 010Ch     */
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

/* -- Helper: assert a path exists (host-side check) ----------------------- */

static int path_exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0;
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

/* -- Test 6: create_close.com --- AH=3Ch CREATE + AH=3Eh CLOSE ----------- */
/*
 * 8086 code at seg:0100 — resolves C:\D2.TXT against exec_dir (/tmp):
 *   MOV AH, 3Ch        ; B4 3C       — CREATE
 *   MOV CX, 0          ; B9 00 00    — attribute (normal)
 *   MOV DX, path       ; BA 1E 01    — DS:DX = path
 *   INT 21h            ; CD 21
 *   JC  fail           ; 72 0D
 *   MOV BX, AX         ; 89 C3       — handle → BX
 *   MOV AH, 3Eh        ; B4 3E       — CLOSE
 *   INT 21h            ; CD 21
 *   JC  fail           ; 72 05
 *   MOV AX, 4C00h      ; B8 00 4C    — exit 0 (pass)
 *   INT 21h            ; CD 21
 * fail:
 *   MOV AX, 4C01h      ; B8 01 4C    — exit 1 (fail)
 *   INT 21h            ; CD 21
 *   db "C:\D2.TXT", 0  ; at offset 0x011E
 */
static const unsigned char create_close_com[] = {
    0xB4, 0x3C,                 /* MOV AH, 3Ch */
    0xB9, 0x00, 0x00,           /* MOV CX, 0 */
    0xBA, 0x1E, 0x01,           /* MOV DX, 011Eh */
    0xCD, 0x21,                 /* INT 21h */
    0x72, 0x0D,                 /* JC fail */
    0x89, 0xC3,                 /* MOV BX, AX */
    0xB4, 0x3E,                 /* MOV AH, 3Eh */
    0xCD, 0x21,                 /* INT 21h */
    0x72, 0x05,                 /* JC fail */
    0xB8, 0x00, 0x4C,           /* MOV AX, 4C00h */
    0xCD, 0x21,                 /* INT 21h */
    0xB8, 0x01, 0x4C,           /* MOV AX, 4C01h (fail) */
    0xCD, 0x21,                 /* INT 21h */
    'C', ':', '\\', 'D', '2', '.', 'T', 'X', 'T', 0,
};

static void test_create_close(void)
{
    unlink("/tmp/D2.TXT");
    WRITE_COM("/tmp/dos_cc.com", create_close_com);
    int code = run_com("/tmp/dos_cc.com");
    UT_ASSERT_EQ(code, 0);
    UT_ASSERT(path_exists("/tmp/D2.TXT"), "D2.TXT created by .COM");
    unlink("/tmp/D2.TXT");
}

/* -- Test 7: open_missing.com --- OPEN non-existent → error 2 ----------- */
/*
 *   MOV AX, 3D00h      ; B8 00 3D    — OPEN, AL=0 (read-only)
 *   MOV DX, path       ; BA 1B 01
 *   INT 21h            ; CD 21
 *   JNC fail           ; 73 07       — expected to fail
 *   CMP AX, 2          ; 3D 02 00    — expect FILE_NOT_FOUND
 *   JNE fail           ; 75 02
 *   JMP success        ; EB 05
 * fail:
 *   MOV AX, 4C01h      ; B8 01 4C
 *   INT 21h            ; CD 21
 * success:
 *   MOV AX, 4C00h      ; B8 00 4C
 *   INT 21h            ; CD 21
 *   db "C:\MISS.TXT", 0  ; at offset 0x011B
 */
static const unsigned char open_missing_com[] = {
    0xB8, 0x00, 0x3D,           /* MOV AX, 3D00h */
    0xBA, 0x1B, 0x01,           /* MOV DX, 011Bh */
    0xCD, 0x21,                 /* INT 21h */
    0x73, 0x07,                 /* JNC fail */
    0x3D, 0x02, 0x00,           /* CMP AX, 2 */
    0x75, 0x02,                 /* JNE fail */
    0xEB, 0x05,                 /* JMP success */
    0xB8, 0x01, 0x4C,           /* MOV AX, 4C01h */
    0xCD, 0x21,
    0xB8, 0x00, 0x4C,           /* MOV AX, 4C00h */
    0xCD, 0x21,
    'C', ':', '\\', 'M', 'I', 'S', 'S', '.', 'T', 'X', 'T', 0,
};

static void test_open_missing(void)
{
    unlink("/tmp/MISS.TXT");
    WRITE_COM("/tmp/dos_om.com", open_missing_com);
    int code = run_com("/tmp/dos_om.com");
    UT_ASSERT_EQ(code, 0);
}

/* -- Test 8: bad_handle_close.com --- CLOSE handle 17 → error 6 --------- */
/*
 *   MOV BX, 17         ; BB 11 00
 *   MOV AH, 3Eh        ; B4 3E
 *   INT 21h            ; CD 21       — expected to fail
 *   JNC fail           ; 73 07
 *   CMP AX, 6          ; 3D 06 00    — expect INVALID_HANDLE
 *   JNE fail           ; 75 02
 *   JMP success        ; EB 05
 * fail:
 *   MOV AX, 4C01h ; INT 21h
 * success:
 *   MOV AX, 4C00h ; INT 21h
 */
static const unsigned char bad_handle_close_com[] = {
    0xBB, 0x11, 0x00,           /* MOV BX, 17 */
    0xB4, 0x3E,                 /* MOV AH, 3Eh */
    0xCD, 0x21,                 /* INT 21h */
    0x73, 0x07,                 /* JNC fail */
    0x3D, 0x06, 0x00,           /* CMP AX, 6 */
    0x75, 0x02,                 /* JNE fail */
    0xEB, 0x05,                 /* JMP success */
    0xB8, 0x01, 0x4C,           /* MOV AX, 4C01h */
    0xCD, 0x21,
    0xB8, 0x00, 0x4C,           /* MOV AX, 4C00h */
    0xCD, 0x21,
};

static void test_bad_handle_close(void)
{
    WRITE_COM("/tmp/dos_bh.com", bad_handle_close_com);
    int code = run_com("/tmp/dos_bh.com");
    UT_ASSERT_EQ(code, 0);
}

/* -- Test 9: double_close.com --- CREATE + CLOSE + CLOSE → error 6 ------ */
/*
 *   MOV AH, 3Ch ; MOV CX, 0 ; MOV DX, path ; INT 21h   — CREATE
 *   JC  fail
 *   MOV BX, AX                                         — handle
 *   MOV AH, 3Eh ; INT 21h                              — first CLOSE
 *   JC  fail
 *   MOV AH, 3Eh ; INT 21h                              — second CLOSE
 *   JNC fail                                           — must fail
 *   CMP AX, 6                                          — INVALID_HANDLE
 *   JNE fail
 *   JMP success
 * fail:   MOV AX, 4C01h ; INT 21h
 * success: MOV AX, 4C00h ; INT 21h
 *   db "C:\DC.TXT", 0  ; at offset 0x012B
 */
static const unsigned char double_close_com[] = {
    0xB4, 0x3C,                 /* MOV AH, 3Ch */
    0xB9, 0x00, 0x00,           /* MOV CX, 0 */
    0xBA, 0x2B, 0x01,           /* MOV DX, 012Bh */
    0xCD, 0x21,                 /* INT 21h */
    0x72, 0x15,                 /* JC fail */
    0x89, 0xC3,                 /* MOV BX, AX */
    0xB4, 0x3E,                 /* MOV AH, 3Eh */
    0xCD, 0x21,                 /* INT 21h — first close */
    0x72, 0x0D,                 /* JC fail */
    0xB4, 0x3E,                 /* MOV AH, 3Eh */
    0xCD, 0x21,                 /* INT 21h — second close */
    0x73, 0x07,                 /* JNC fail */
    0x3D, 0x06, 0x00,           /* CMP AX, 6 */
    0x75, 0x02,                 /* JNE fail */
    0xEB, 0x05,                 /* JMP success */
    0xB8, 0x01, 0x4C,           /* MOV AX, 4C01h */
    0xCD, 0x21,
    0xB8, 0x00, 0x4C,           /* MOV AX, 4C00h */
    0xCD, 0x21,
    'C', ':', '\\', 'D', 'C', '.', 'T', 'X', 'T', 0,
};

static void test_double_close(void)
{
    unlink("/tmp/DC.TXT");
    WRITE_COM("/tmp/dos_dc.com", double_close_com);
    int code = run_com("/tmp/dos_dc.com");
    UT_ASSERT_EQ(code, 0);
    unlink("/tmp/DC.TXT");
}

/* -- Test 10: invalid_drive.com --- OPEN Y: → error 15 ------------------ */
/*
 *   MOV AX, 3D00h ; MOV DX, path ; INT 21h   — OPEN "Y:\FOO.TXT"
 *   JNC fail                                 — must fail
 *   CMP AX, 15                               — INVALID_DRIVE
 *   JNE fail
 *   JMP success
 *   db "Y:\FOO.TXT", 0   ; at offset 0x011B
 */
static const unsigned char invalid_drive_com[] = {
    0xB8, 0x00, 0x3D,           /* MOV AX, 3D00h */
    0xBA, 0x1B, 0x01,           /* MOV DX, 011Bh */
    0xCD, 0x21,
    0x73, 0x07,                 /* JNC fail */
    0x3D, 0x0F, 0x00,           /* CMP AX, 15 */
    0x75, 0x02,                 /* JNE fail */
    0xEB, 0x05,                 /* JMP success */
    0xB8, 0x01, 0x4C,
    0xCD, 0x21,
    0xB8, 0x00, 0x4C,
    0xCD, 0x21,
    'Y', ':', '\\', 'F', 'O', 'O', '.', 'T', 'X', 'T', 0,
};

static void test_invalid_drive(void)
{
    WRITE_COM("/tmp/dos_id.com", invalid_drive_com);
    int code = run_com("/tmp/dos_id.com");
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
    test_create_close();
    test_open_missing();
    test_bad_handle_close();
    test_double_close();
    test_invalid_drive();

    UT_SUMMARY("test_msdos");
}
