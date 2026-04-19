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

/* .EXE files use the same raw byte writer; WRITE_EXE is a readability
 * alias so the reader can tell which format a test uses at a glance. */
#define WRITE_EXE(path, code) \
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

/* -- Test 11: rw_roundtrip.com --- AH=3Fh READ / AH=40h WRITE ----------- */
/*
 * 8086 code (loaded at seg:0100):
 *   Create "C:\RW.TXT", write "DOS_RW" (6 bytes), close.
 *   Re-open read-only, read back into a 6-byte buffer, compare, close.
 *   Exit 0 on success, 1 on any step failure.
 *
 *   data    at 0x0160:  "DOS_RW"
 *   buf     at 0x0166:  six-byte scratch
 *   path    at 0x016C:  "C:\\RW.TXT\\0"
 */
static const unsigned char rw_roundtrip_com[] = {
    /* 0x100: CREATE */
    0xB4, 0x3C,                 /* MOV AH, 3Ch */
    0xB9, 0x00, 0x00,           /* MOV CX, 0   */
    0xBA, 0x6C, 0x01,           /* MOV DX, 016Ch */
    0xCD, 0x21,                 /* INT 21h     */
    0x72, 0x4F,                 /* JC fail     */
    0x89, 0xC3,                 /* MOV BX, AX  */
    /* 0x10E: WRITE */
    0xB4, 0x40,                 /* MOV AH, 40h */
    0xB9, 0x06, 0x00,           /* MOV CX, 6   */
    0xBA, 0x60, 0x01,           /* MOV DX, 0160h */
    0xCD, 0x21,                 /* INT 21h     */
    0x72, 0x41,                 /* JC fail     */
    0x3D, 0x06, 0x00,           /* CMP AX, 6   */
    0x75, 0x3C,                 /* JNE fail    */
    /* 0x11F: CLOSE */
    0xB4, 0x3E,                 /* MOV AH, 3Eh */
    0xCD, 0x21,                 /* INT 21h     */
    0x72, 0x36,                 /* JC fail     */
    /* 0x125: OPEN RDONLY */
    0xB8, 0x00, 0x3D,           /* MOV AX, 3D00h */
    0xBA, 0x6C, 0x01,           /* MOV DX, 016Ch */
    0xCD, 0x21,                 /* INT 21h     */
    0x72, 0x2C,                 /* JC fail     */
    0x89, 0xC3,                 /* MOV BX, AX  */
    /* 0x131: READ */
    0xB4, 0x3F,                 /* MOV AH, 3Fh */
    0xB9, 0x06, 0x00,           /* MOV CX, 6   */
    0xBA, 0x66, 0x01,           /* MOV DX, 0166h */
    0xCD, 0x21,                 /* INT 21h     */
    0x72, 0x1E,                 /* JC fail     */
    0x3D, 0x06, 0x00,           /* CMP AX, 6   */
    0x75, 0x19,                 /* JNE fail    */
    /* 0x142: REPE CMPSB — compare data vs buf */
    0xBE, 0x60, 0x01,           /* MOV SI, 0160h */
    0xBF, 0x66, 0x01,           /* MOV DI, 0166h */
    0xB9, 0x06, 0x00,           /* MOV CX, 6   */
    0xFC,                       /* CLD         */
    0xF3, 0xA6,                 /* REPE CMPSB  */
    0x75, 0x0B,                 /* JNE fail    */
    /* 0x150: CLOSE */
    0xB4, 0x3E,                 /* MOV AH, 3Eh */
    0xCD, 0x21,                 /* INT 21h     */
    0x72, 0x05,                 /* JC fail     */
    /* 0x156: exit 0 */
    0xB8, 0x00, 0x4C,           /* MOV AX, 4C00h */
    0xCD, 0x21,                 /* INT 21h     */
    /* 0x15B: fail → exit 1 */
    0xB8, 0x01, 0x4C,           /* MOV AX, 4C01h */
    0xCD, 0x21,                 /* INT 21h     */
    /* 0x160: data */
    'D', 'O', 'S', '_', 'R', 'W',
    /* 0x166: read buffer (6 B scratch) */
    0, 0, 0, 0, 0, 0,
    /* 0x16C: path */
    'C', ':', '\\', 'R', 'W', '.', 'T', 'X', 'T', 0,
};

static void test_rw_roundtrip(void)
{
    unlink("/tmp/RW.TXT");
    WRITE_COM("/tmp/dos_rw.com", rw_roundtrip_com);
    int code = run_com("/tmp/dos_rw.com");
    UT_ASSERT_EQ(code, 0);
    struct stat st;
    UT_ASSERT_EQ(stat("/tmp/RW.TXT", &st), 0);
    UT_ASSERT_EQ(st.st_size, 6);
    unlink("/tmp/RW.TXT");
}

/* -- Test 12: seek_delete.com --- AH=42h LSEEK + AH=41h DELETE ---------- */
/*
 * Create C:\SK.TXT, write "0123456789ABCDEF" (16 B), close.
 * Re-open r/o, LSEEK SET +4 → expect AX=4, READ 4 B, REPE CMPSB "4567",
 * close, DELETE, then OPEN again expecting error 2 (FILE_NOT_FOUND).
 *
 *   data    at 0x018A:  "0123456789ABCDEF"  (16 B)
 *   exp     at 0x019A:  "4567"              (4 B)
 *   buf     at 0x019E:  4-byte scratch
 *   path    at 0x01A2:  "C:\\SK.TXT\\0"      (10 B)
 */
static const unsigned char seek_delete_com[] = {
    /* 0x100: CREATE */
    0xB4, 0x3C,                /* MOV AH, 3Ch */
    0xB9, 0x00, 0x00,          /* MOV CX, 0   */
    0xBA, 0xA2, 0x01,          /* MOV DX, 01A2h (path) */
    0xCD, 0x21,                /* INT 21h     */
    0x72, 0x79,                /* JC fail     */
    0x89, 0xC3,                /* MOV BX, AX  */
    /* 0x10E: WRITE 16 */
    0xB4, 0x40,                /* MOV AH, 40h */
    0xB9, 0x10, 0x00,          /* MOV CX, 16  */
    0xBA, 0x8A, 0x01,          /* MOV DX, 018Ah (data) */
    0xCD, 0x21,                /* INT 21h     */
    0x72, 0x6B,                /* JC fail     */
    0x3D, 0x10, 0x00,          /* CMP AX, 16  */
    0x75, 0x66,                /* JNE fail    */
    /* 0x11F: CLOSE */
    0xB4, 0x3E,                /* MOV AH, 3Eh */
    0xCD, 0x21,                /* INT 21h     */
    0x72, 0x60,                /* JC fail     */
    /* 0x125: OPEN RDONLY */
    0xB8, 0x00, 0x3D,          /* MOV AX, 3D00h */
    0xBA, 0xA2, 0x01,          /* MOV DX, path */
    0xCD, 0x21,                /* INT 21h     */
    0x72, 0x56,                /* JC fail     */
    0x89, 0xC3,                /* MOV BX, AX  */
    /* 0x131: LSEEK SET 4 */
    0xB8, 0x00, 0x42,          /* MOV AX, 4200h (whence=0) */
    0xB9, 0x00, 0x00,          /* MOV CX, 0   */
    0xBA, 0x04, 0x00,          /* MOV DX, 4   */
    0xCD, 0x21,                /* INT 21h     */
    0x72, 0x47,                /* JC fail     */
    0x3D, 0x04, 0x00,          /* CMP AX, 4   */
    0x75, 0x42,                /* JNE fail    */
    /* 0x143: READ 4 */
    0xB4, 0x3F,                /* MOV AH, 3Fh */
    0xB9, 0x04, 0x00,          /* MOV CX, 4   */
    0xBA, 0x9E, 0x01,          /* MOV DX, 019Eh (buf) */
    0xCD, 0x21,                /* INT 21h     */
    0x72, 0x36,                /* JC fail     */
    0x3D, 0x04, 0x00,          /* CMP AX, 4   */
    0x75, 0x31,                /* JNE fail    */
    /* 0x154: REPE CMPSB exp vs buf */
    0xBE, 0x9A, 0x01,          /* MOV SI, 019Ah (exp) */
    0xBF, 0x9E, 0x01,          /* MOV DI, 019Eh (buf) */
    0xB9, 0x04, 0x00,          /* MOV CX, 4   */
    0xFC,                      /* CLD         */
    0xF3, 0xA6,                /* REPE CMPSB  */
    0x75, 0x23,                /* JNE fail    */
    /* 0x162: CLOSE */
    0xB4, 0x3E,                /* MOV AH, 3Eh */
    0xCD, 0x21,                /* INT 21h     */
    0x72, 0x1D,                /* JC fail     */
    /* 0x168: DELETE */
    0xB4, 0x41,                /* MOV AH, 41h */
    0xBA, 0xA2, 0x01,          /* MOV DX, path */
    0xCD, 0x21,                /* INT 21h     */
    0x72, 0x14,                /* JC fail     */
    /* 0x171: OPEN expecting err 2 */
    0xB8, 0x00, 0x3D,          /* MOV AX, 3D00h */
    0xBA, 0xA2, 0x01,          /* MOV DX, path */
    0xCD, 0x21,                /* INT 21h     */
    0x73, 0x0A,                /* JNC fail (must fail) */
    0x3D, 0x02, 0x00,          /* CMP AX, 2   */
    0x75, 0x05,                /* JNE fail    */
    /* 0x180: exit 0 */
    0xB8, 0x00, 0x4C,          /* MOV AX, 4C00h */
    0xCD, 0x21,                /* INT 21h     */
    /* 0x185: fail → exit 1 */
    0xB8, 0x01, 0x4C,          /* MOV AX, 4C01h */
    0xCD, 0x21,                /* INT 21h     */
    /* 0x18A: data */
    '0', '1', '2', '3', '4', '5', '6', '7',
    '8', '9', 'A', 'B', 'C', 'D', 'E', 'F',
    /* 0x19A: exp */
    '4', '5', '6', '7',
    /* 0x19E: buf */
    0, 0, 0, 0,
    /* 0x1A2: path */
    'C', ':', '\\', 'S', 'K', '.', 'T', 'X', 'T', 0,
};

static void test_seek_delete(void)
{
    unlink("/tmp/SK.TXT");
    WRITE_COM("/tmp/dos_sk.com", seek_delete_com);
    int code = run_com("/tmp/dos_sk.com");
    UT_ASSERT_EQ(code, 0);
    /* DOS DELETE removed it; if it still exists the .COM should have
     * exited 1 above. */
    struct stat st;
    UT_ASSERT(stat("/tmp/SK.TXT", &st) < 0, "SK.TXT removed by DOS DELETE");
}

/* -- Test 13: bad_whence.com --- LSEEK with invalid whence → AX=1 ------- */
/*
 * Verifies the error-mapping path for bad arguments: open the
 * already-staged file (or use stdin handle 0), call LSEEK with
 * AL=5 (invalid whence ∉ {0,1,2}), expect CF=1 and AX=1
 * (DOS_ERR_INVALID_FUNCTION).  Uses handle 0 (stdin) so no file
 * setup is needed.
 *
 *   MOV BX, 0             ; BB 00 00       — handle 0
 *   MOV AX, 4205h         ; B8 05 42        — LSEEK, AL=5 (bad whence)
 *   MOV CX, 0             ; B9 00 00
 *   MOV DX, 0             ; BA 00 00
 *   INT 21h               ; CD 21
 *   JNC fail              ; 73 07
 *   CMP AX, 1             ; 3D 01 00
 *   JNE fail              ; 75 02
 *   JMP success           ; EB 05
 * fail: MOV AX, 4C01h ; INT 21h
 * success: MOV AX, 4C00h ; INT 21h
 */
static const unsigned char bad_whence_com[] = {
    0xBB, 0x00, 0x00,           /* MOV BX, 0     */
    0xB8, 0x05, 0x42,           /* MOV AX, 4205h */
    0xB9, 0x00, 0x00,           /* MOV CX, 0     */
    0xBA, 0x00, 0x00,           /* MOV DX, 0     */
    0xCD, 0x21,                 /* INT 21h       */
    0x73, 0x07,                 /* JNC fail      */
    0x3D, 0x01, 0x00,           /* CMP AX, 1     */
    0x75, 0x02,                 /* JNE fail      */
    0xEB, 0x05,                 /* JMP success   */
    0xB8, 0x01, 0x4C,           /* MOV AX, 4C01h */
    0xCD, 0x21,
    0xB8, 0x00, 0x4C,           /* MOV AX, 4C00h */
    0xCD, 0x21,
};

static void test_bad_whence(void)
{
    WRITE_COM("/tmp/dos_bw.com", bad_whence_com);
    int code = run_com("/tmp/dos_bw.com");
    UT_ASSERT_EQ(code, 0);
}

/* -- Test 14: mkdir_rmdir.com --- AH=39h MKDIR + AH=3Ah RMDIR ----------- */
/*
 *   MOV AH, 39h     ; B4 39       — MKDIR
 *   MOV DX, path    ; BA 1C 01
 *   INT 21h         ; CD 21
 *   JC fail
 *   MOV AH, 3Ah     ; B4 3A       — RMDIR
 *   MOV DX, path
 *   INT 21h
 *   JC fail
 *   MOV AX, 4C00h   ; exit 0
 *   INT 21h
 * fail: MOV AX, 4C01h ; INT 21h
 *   db "C:\D3DIR", 0  at 0x011C
 */
static const unsigned char mkdir_rmdir_com[] = {
    0xB4, 0x39,                 /* MOV AH, 39h */
    0xBA, 0x1C, 0x01,           /* MOV DX, 011Ch */
    0xCD, 0x21,                 /* INT 21h */
    0x72, 0x0E,                 /* JC fail */
    0xB4, 0x3A,                 /* MOV AH, 3Ah */
    0xBA, 0x1C, 0x01,           /* MOV DX, 011Ch */
    0xCD, 0x21,                 /* INT 21h */
    0x72, 0x05,                 /* JC fail */
    0xB8, 0x00, 0x4C,           /* MOV AX, 4C00h */
    0xCD, 0x21,
    0xB8, 0x01, 0x4C,           /* MOV AX, 4C01h */
    0xCD, 0x21,
    'C', ':', '\\', 'D', '3', 'D', 'I', 'R', 0,
};

static void test_mkdir_rmdir(void)
{
    /* Make sure the dir doesn't exist beforehand. */
    rmdir("/tmp/D3DIR");
    WRITE_COM("/tmp/dos_md.com", mkdir_rmdir_com);
    int code = run_com("/tmp/dos_md.com");
    UT_ASSERT_EQ(code, 0);
    /* RMDIR ran inside the .COM, so directory should be gone now. */
    struct stat st;
    UT_ASSERT(stat("/tmp/D3DIR", &st) < 0, "D3DIR removed by DOS RMDIR");
}

/* -- Test 15: dup_handle.com --- AH=45h DUP --------------------------- */
/*
 * Create a file, DUP the handle, write through the duped handle, then
 * CLOSE the duped handle.  The .COM exits without explicitly closing
 * the source handle — kernel cleanup releases it on exit.  Host-side
 * verifies the resulting file has 2 bytes.
 *
 * Layout (recounted carefully):
 *   0x100  CREATE        14   B4 3C; B9 00 00; BA pp pp; CD 21; 72 dd; 89 C3
 *   0x10E  DUP            6   B4 45; CD 21; 72 dd
 *   0x114  CMP/JB         5   3D 05 00; 72 dd        (handle ≥ 5)
 *   0x119  MOV BX,AX      2   89 C3
 *   0x11B  WRITE         17   B4 40; B9 02 00; BA dd dd; CD 21; 72 dd;
 *                              3D 02 00; 75 dd
 *   0x12C  CLOSE          6   B4 3E; CD 21; 72 dd
 *   0x132  exit 0         5   B8 00 4C; CD 21
 *   0x137  fail           5   B8 01 4C; CD 21
 *   0x13C  data "ok"      2
 *   0x13E  path           13  "C:\D3DUP.TXT" + NUL
 *   end    0x14B = 75 bytes
 *
 * Fail target = 0x137; JC/JNE displacements:
 *   0x10A → 0x137-0x10C = 0x2B
 *   0x112 → 0x137-0x114 = 0x23
 *   0x117 (JB) → 0x137-0x119 = 0x1E
 *   0x125 → 0x137-0x127 = 0x10
 *   0x12A (JNE) → 0x137-0x12C = 0x0B
 *   0x130 → 0x137-0x132 = 0x05
 */
static const unsigned char dup_handle_com[] = {
    /* 0x100: CREATE */
    0xB4, 0x3C,                 /* MOV AH, 3Ch */
    0xB9, 0x00, 0x00,           /* MOV CX, 0 */
    0xBA, 0x3E, 0x01,           /* MOV DX, 013Eh (path) */
    0xCD, 0x21,                 /* INT 21h */
    0x72, 0x2B,                 /* JC fail */
    0x89, 0xC3,                 /* MOV BX, AX (handle1) */
    /* 0x10E: DUP */
    0xB4, 0x45,                 /* MOV AH, 45h */
    0xCD, 0x21,                 /* INT 21h */
    0x72, 0x23,                 /* JC fail */
    /* 0x114: CMP AX,5 ; JB fail (handle2 must be ≥5) */
    0x3D, 0x05, 0x00,           /* CMP AX, 5 */
    0x72, 0x1E,                 /* JB fail */
    /* 0x119: MOV BX, AX */
    0x89, 0xC3,                 /* MOV BX, AX */
    /* 0x11B: WRITE */
    0xB4, 0x40,                 /* MOV AH, 40h */
    0xB9, 0x02, 0x00,           /* MOV CX, 2 */
    0xBA, 0x3C, 0x01,           /* MOV DX, 013Ch (data) */
    0xCD, 0x21,                 /* INT 21h */
    0x72, 0x10,                 /* JC fail */
    0x3D, 0x02, 0x00,           /* CMP AX, 2 */
    0x75, 0x0B,                 /* JNE fail */
    /* 0x12C: CLOSE handle2 */
    0xB4, 0x3E,                 /* MOV AH, 3Eh */
    0xCD, 0x21,                 /* INT 21h */
    0x72, 0x05,                 /* JC fail */
    /* 0x132: exit 0 */
    0xB8, 0x00, 0x4C,
    0xCD, 0x21,
    /* 0x137: fail */
    0xB8, 0x01, 0x4C,
    0xCD, 0x21,
    /* 0x13C: data */
    'o', 'k',
    /* 0x13E: path */
    'C', ':', '\\', 'D', '3', 'D', 'U', 'P', '.', 'T', 'X', 'T', 0,
};

static void test_dup_handle(void)
{
    unlink("/tmp/D3DUP.TXT");
    WRITE_COM("/tmp/dos_dp.com", dup_handle_com);
    int code = run_com("/tmp/dos_dp.com");
    UT_ASSERT_EQ(code, 0);
    /* File should exist with the 2 bytes written via the duped handle. */
    struct stat st;
    UT_ASSERT_EQ(stat("/tmp/D3DUP.TXT", &st), 0);
    UT_ASSERT_EQ(st.st_size, 2);
    unlink("/tmp/D3DUP.TXT");
}

/* -- Test 16: rename.com --- AH=56h RENAME ------------------------------ */
/*
 *   CREATE "C:\OLD.TXT" → handle in BX
 *   CLOSE
 *   MOV AX, 5600h                — RENAME (no sub-function)
 *   MOV DX, old_path             — DS:DX = old
 *   PUSH ES ; PUSH DS ; POP ES   — DOS contract: ES:DI = new path; for
 *                                  .COM with ES=DS we just leave ES alone
 *                                  but set DI explicitly.
 *   POP ES (skip — already proc_seg)
 *   MOV DI, new_path
 *   INT 21h
 *   JC fail
 *   OPEN "C:\NEW.TXT" → handle, CLOSE, DELETE
 *   exit
 *
 * For tiny model .COMs ES = DS = SS already, so we just need to MOV DI.
 * Layout (inside the segment):
 *   0x100  CREATE              14
 *   0x10E  CLOSE                6
 *   0x114  RENAME (with MOV DI) 12  (B8 00 56; BA pp pp; BF qq qq; CD 21; 72 dd)
 *           wait let me recount:
 *           B8 00 56 (3) MOV AX, 5600h
 *           BA 6F 01 (3) MOV DX, old
 *           BF 78 01 (3) MOV DI, new
 *           CD 21 (2)
 *           72 dd (2)    JC fail
 *           = 13 bytes
 *   0x121  OPEN R/O           12 (B8 00 3D; BA qq qq; CD 21; JC; MOV BX,AX)
 *   0x12D  CLOSE              6
 *   0x133  DELETE             9 (B4 41; BA qq qq; CD 21; JC fail)
 *   0x13C  exit 0             5
 *   0x141  fail               5
 *   0x146  old "C:\OLD.TXT" + NUL (11 bytes)
 *   0x151  new "C:\NEW.TXT" + NUL (11 bytes)
 *   end 0x15C = 92 bytes
 *
 * Wait — old and new offsets should be 0x146 and 0x151.
 * Let me recompute:
 *   0x100 + 14 = 0x10E (CREATE end)
 *   0x10E + 6 = 0x114 (CLOSE end)
 *   0x114 + 13 = 0x121 (RENAME end)
 *   0x121 + 12 = 0x12D (OPEN end)
 *   0x12D + 6 = 0x133 (CLOSE2 end)
 *   0x133 + 9 = 0x13C (DELETE end)
 *   0x13C + 5 = 0x141 (exit 0 end)
 *   0x141 + 5 = 0x146 (fail end, data starts here)
 *   0x146..0x150: "C:\OLD.TXT\0" (11 bytes)
 *   0x151..0x15B: "C:\NEW.TXT\0" (11 bytes)
 *   0x15C end (92 bytes total)
 */
static const unsigned char rename_com[] = {
    /* 0x100: CREATE old */
    0xB4, 0x3C,                 /* MOV AH, 3Ch */
    0xB9, 0x00, 0x00,           /* MOV CX, 0 */
    0xBA, 0x46, 0x01,           /* MOV DX, 0146h (old) */
    0xCD, 0x21,                 /* INT 21h */
    0x72, 0x35,                 /* JC fail (+0x35=53) */
    0x89, 0xC3,                 /* MOV BX, AX */
    /* 0x10E: CLOSE */
    0xB4, 0x3E,                 /* MOV AH, 3Eh */
    0xCD, 0x21,                 /* INT 21h */
    0x72, 0x2F,                 /* JC fail (+47) */
    /* 0x114: RENAME */
    0xB8, 0x00, 0x56,           /* MOV AX, 5600h */
    0xBA, 0x46, 0x01,           /* MOV DX, 0146h (old) */
    0xBF, 0x51, 0x01,           /* MOV DI, 0151h (new) */
    0xCD, 0x21,                 /* INT 21h */
    0x72, 0x24,                 /* JC fail (+36) */
    /* 0x121: OPEN R/O new */
    0xB8, 0x00, 0x3D,           /* MOV AX, 3D00h */
    0xBA, 0x51, 0x01,           /* MOV DX, 0151h (new) */
    0xCD, 0x21,                 /* INT 21h */
    0x72, 0x1A,                 /* JC fail (+26) */
    0x89, 0xC3,                 /* MOV BX, AX */
    /* 0x12D: CLOSE */
    0xB4, 0x3E,
    0xCD, 0x21,
    0x72, 0x14,                 /* JC fail */
    /* 0x133: DELETE new */
    0xB4, 0x41,
    0xBA, 0x51, 0x01,           /* MOV DX, 0151h (new) */
    0xCD, 0x21,
    0x72, 0x0B,                 /* JC fail */
    /* 0x13C: exit 0 */
    0xB8, 0x00, 0x4C,
    0xCD, 0x21,
    /* 0x141: fail */
    0xB8, 0x01, 0x4C,
    0xCD, 0x21,
    /* 0x146: old */
    'C', ':', '\\', 'O', 'L', 'D', '.', 'T', 'X', 'T', 0,
    /* 0x151: new */
    'C', ':', '\\', 'N', 'E', 'W', '.', 'T', 'X', 'T', 0,
};

static void test_rename(void)
{
    unlink("/tmp/OLD.TXT");
    unlink("/tmp/NEW.TXT");
    WRITE_COM("/tmp/dos_rn.com", rename_com);
    int code = run_com("/tmp/dos_rn.com");
    UT_ASSERT_EQ(code, 0);
    /* The .COM deletes NEW.TXT itself; OLD.TXT should be gone after rename. */
    struct stat st;
    UT_ASSERT(stat("/tmp/OLD.TXT", &st) < 0, "OLD.TXT was renamed");
    UT_ASSERT(stat("/tmp/NEW.TXT", &st) < 0, "NEW.TXT was deleted");
}

/* -- Test 17: exe_exit.exe --- minimal MZ .EXE with exit code 42 --------- */
/*
 * Smallest possible zero-relocation .EXE: 32-byte MZ header + 5 bytes
 * of code.  Verifies header parsing, image placement at load_seg:0,
 * CS/IP=0/0 relative to load_seg, SS=0x10 (a separate stack segment).
 *
 * MZ header (28 bytes of info + 4 bytes pad = 32 = 2 paragraphs):
 *   signature      MZ
 *   last_page_size 37  (file_size mod 512)
 *   page_count     1   (ceil(file_size/512))
 *   reloc_count    0
 *   header_size    2   paragraphs
 *   min_alloc      0x20 paragraphs (512 B past image — covers the stack)
 *   max_alloc      0xFFFF
 *   init_ss        0x10  (SS = load_seg + 0x10, stack distinct from code)
 *   init_sp        0x100
 *   init_ip        0
 *   init_cs        0
 *
 * Code at load_seg:0:
 *   MOV AX, 4C2Ah       ; B8 2A 4C
 *   INT 21h              ; CD 21
 */
static const unsigned char exe_exit42_exe[] = {
    /* MZ header (32 bytes) */
    'M', 'Z',                       /* 0x00: signature 'MZ'             */
    0x25, 0x00,                     /* 0x02: last_page_size = 37        */
    0x01, 0x00,                     /* 0x04: page_count = 1             */
    0x00, 0x00,                     /* 0x06: reloc_count = 0            */
    0x02, 0x00,                     /* 0x08: header_size = 2 para (32B) */
    0x20, 0x00,                     /* 0x0A: min_alloc = 0x20 para      */
    0xFF, 0xFF,                     /* 0x0C: max_alloc                  */
    0x10, 0x00,                     /* 0x0E: init_ss = 0x10 (relative)  */
    0x00, 0x01,                     /* 0x10: init_sp = 0x100            */
    0x00, 0x00,                     /* 0x12: checksum                   */
    0x00, 0x00,                     /* 0x14: init_ip = 0                */
    0x00, 0x00,                     /* 0x16: init_cs = 0 (relative)     */
    0x1C, 0x00,                     /* 0x18: reloc_offset = 0x1C        */
    0x00, 0x00,                     /* 0x1A: overlay = 0                */
    0x00, 0x00, 0x00, 0x00,         /*       pad to 32 bytes            */

    /* Code at load_seg:0 */
    0xB8, 0x2A, 0x4C,               /* MOV AX, 4C2Ah                    */
    0xCD, 0x21,                     /* INT 21h                          */
};

static void test_exe_exit_code(void)
{
    WRITE_EXE("/tmp/dos_x42.exe", exe_exit42_exe);
    int code = run_com("/tmp/dos_x42.exe");
    UT_ASSERT_EQ(code, 42);
}

/* -- Test 18: hello.exe --- AH=09h via DS=CS reload (classic .EXE) ------- */
/*
 * Standard .EXE idiom: DS = ES = PSP seg on entry, so the program reloads
 * DS from CS to point at its own image.  Proves that the DOS bridge
 * resolves DS:DX through the whole proc-image range (not just the PSP
 * segment), and that our initial frame leaves DS pointing at the PSP.
 *
 * Code at load_seg:0 (29 bytes total):
 *   0x00  MOV AX, CS          ; 8C C8
 *   0x02  MOV DS, AX          ; 8E D8
 *   0x04  MOV AH, 09h         ; B4 09
 *   0x06  MOV DX, 0010h       ; BA 10 00  (msg offset from DS=CS base)
 *   0x09  INT 21h             ; CD 21
 *   0x0B  MOV AX, 4C00h       ; B8 00 4C
 *   0x0E  INT 21h             ; CD 21
 *   0x10  "Hello EXE!\r\n$"    (13 bytes)
 *
 * file_size = 32 (header) + 29 (code+data) = 61 bytes → last_page_size=61.
 */
static const unsigned char exe_hello_exe[] = {
    /* MZ header (32 bytes) */
    'M', 'Z',
    0x3D, 0x00,                     /* last_page_size = 61              */
    0x01, 0x00,                     /* page_count = 1                   */
    0x00, 0x00,                     /* reloc_count = 0                  */
    0x02, 0x00,                     /* header_size = 2 para             */
    0x20, 0x00,                     /* min_alloc = 0x20 para            */
    0xFF, 0xFF,                     /* max_alloc                        */
    0x10, 0x00,                     /* init_ss = 0x10                   */
    0x00, 0x01,                     /* init_sp = 0x100                  */
    0x00, 0x00,                     /* checksum                         */
    0x00, 0x00,                     /* init_ip = 0                      */
    0x00, 0x00,                     /* init_cs = 0                      */
    0x1C, 0x00,                     /* reloc_offset                     */
    0x00, 0x00,                     /* overlay                          */
    0x00, 0x00, 0x00, 0x00,         /* pad                              */

    /* Code at load_seg:0 */
    0x8C, 0xC8,                     /* MOV AX, CS                       */
    0x8E, 0xD8,                     /* MOV DS, AX                       */
    0xB4, 0x09,                     /* MOV AH, 09h                      */
    0xBA, 0x10, 0x00,               /* MOV DX, 0010h  (msg offset in DS)*/
    0xCD, 0x21,                     /* INT 21h                          */
    0xB8, 0x00, 0x4C,               /* MOV AX, 4C00h                    */
    0xCD, 0x21,                     /* INT 21h                          */
    /* 0x10: msg */
    'H', 'e', 'l', 'l', 'o', ' ', 'E', 'X', 'E', '!',
    '\r', '\n', '$',
};

static void test_exe_hello(void)
{
    WRITE_EXE("/tmp/dos_hi.exe", exe_hello_exe);
    char buf[64];
    int n = run_com_capture("/tmp/dos_hi.exe", buf, sizeof(buf));
    UT_ASSERT(n >= 10, "hello.exe produced output");
    UT_ASSERT(buf[0] == 'H' && buf[1] == 'e' && buf[2] == 'l',
              "output starts with Hel");
}

/* -- Test 19: farjmp.exe --- MZ .EXE with 1 relocation (CS of JMP FAR) --- */
/*
 * Exercises the D-4b relocation path.  The file contains a JMP FAR
 * instruction whose CS field is initially 0; a single relocation
 * entry tells the loader to add load_seg to the word at (load_seg:3).
 * After patching, the JMP goes to load_seg:0010 where the exit-42
 * stub lives.  Without the patch, the JMP would target 0000:0010
 * (inside the IVT) and the program would not exit with code 42.
 *
 * File layout (53 bytes, header_size=2 paragraphs):
 *   0x00..0x1B  MZ fixed header (28 bytes)
 *   0x1C..0x1F  one 4-byte relocation entry (offset=3, seg=0)
 *   0x20..0x34  image (21 bytes):
 *     load_seg:0000  EA 10 00 00 00    ; JMP FAR 0000:0010 (CS patched)
 *     load_seg:0005  00 ... 00         ; 11 bytes padding
 *     load_seg:0010  B8 2A 4C          ; MOV AX, 4C2Ah
 *     load_seg:0013  CD 21             ; INT 21h
 */
static const unsigned char exe_farjmp_exe[] = {
    /* MZ header (28 bytes of fields + 4 bytes reloc entry = 32) */
    'M', 'Z',
    0x35, 0x00,                     /* last_page_size = 53              */
    0x01, 0x00,                     /* page_count = 1                   */
    0x01, 0x00,                     /* reloc_count = 1                  */
    0x02, 0x00,                     /* header_size = 2 para             */
    0x20, 0x00,                     /* min_alloc = 0x20 para            */
    0xFF, 0xFF,                     /* max_alloc                        */
    0x10, 0x00,                     /* init_ss = 0x10                   */
    0x00, 0x01,                     /* init_sp = 0x100                  */
    0x00, 0x00,                     /* checksum                         */
    0x00, 0x00,                     /* init_ip = 0                      */
    0x00, 0x00,                     /* init_cs = 0                      */
    0x1C, 0x00,                     /* reloc_offset = 0x1C              */
    0x00, 0x00,                     /* overlay                          */
    /* Reloc entry at 0x1C: patch word at (seg=0):offset=3 */
    0x03, 0x00, 0x00, 0x00,

    /* Image at load_seg:0 (file offset 0x20) */
    0xEA, 0x10, 0x00, 0x00, 0x00,   /* JMP FAR 0000:0010 (CS relocated) */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,/* pad to load_seg:0010             */
    0xB8, 0x2A, 0x4C,               /* MOV AX, 4C2Ah                    */
    0xCD, 0x21,                     /* INT 21h                          */
};

static void test_exe_reloc_farjmp(void)
{
    WRITE_EXE("/tmp/dos_fj.exe", exe_farjmp_exe);
    int code = run_com("/tmp/dos_fj.exe");
    UT_ASSERT_EQ(code, 42);
}

/* -- Test 20: multiseg.exe --- DS != CS != PSP via reloc, INT 21h AH=09h -- */
/*
 * Confirms the DOS bridge resolves DS:DX correctly when DS is neither
 * the PSP segment nor the code segment.  The program relocates a 16-bit
 * immediate to load_seg + 2, loads DS from it, then asks AH=09h to
 * print a string at DS:0000.
 *
 *   load_seg:0000  B8 02 00        ; MOV AX, 0002h    (relocated)
 *   load_seg:0003  8E D8            ; MOV DS, AX       (DS = load_seg+2)
 *   load_seg:0005  B4 09            ; MOV AH, 09h
 *   load_seg:0007  BA 00 00         ; MOV DX, 0
 *   load_seg:000A  CD 21            ; INT 21h          (print DS:0 = msg)
 *   load_seg:000C  B8 00 4C         ; MOV AX, 4C00h
 *   load_seg:000F  CD 21            ; INT 21h          (exit 0)
 *   load_seg:0020  "Multi!\r\n$"                       (9 bytes)
 *
 * msg sits at (load_seg+2):0 = load_seg:0x20 = image offset 0x20.  The
 * relocation entry patches the imm16 at (seg=0):offset=1 from 0x0002 to
 * 0x0002 + load_seg, so DS ends up pointing at the second paragraph of
 * the image — distinct from PSP (proc_seg), load_seg, and the stack
 * segment (init_ss+load_seg).
 */
static const unsigned char exe_multiseg_exe[] = {
    /* MZ header (32 bytes) */
    'M', 'Z',
    0x49, 0x00,                     /* last_page_size = 73              */
    0x01, 0x00,                     /* page_count = 1                   */
    0x01, 0x00,                     /* reloc_count = 1                  */
    0x02, 0x00,                     /* header_size = 2 para             */
    0x20, 0x00,                     /* min_alloc                        */
    0xFF, 0xFF,                     /* max_alloc                        */
    0x10, 0x00,                     /* init_ss                          */
    0x00, 0x01,                     /* init_sp                          */
    0x00, 0x00,                     /* checksum                         */
    0x00, 0x00,                     /* init_ip                          */
    0x00, 0x00,                     /* init_cs                          */
    0x1C, 0x00,                     /* reloc_offset                     */
    0x00, 0x00,                     /* overlay                          */
    /* Reloc entry: patch (seg=0):offset=1 — the imm16 of MOV AX */
    0x01, 0x00, 0x00, 0x00,

    /* Image at load_seg:0 (file offset 0x20) */
    0xB8, 0x02, 0x00,               /* MOV AX, 0002h  (reloc target)    */
    0x8E, 0xD8,                     /* MOV DS, AX                       */
    0xB4, 0x09,                     /* MOV AH, 09h                      */
    0xBA, 0x00, 0x00,               /* MOV DX, 0                        */
    0xCD, 0x21,                     /* INT 21h                          */
    0xB8, 0x00, 0x4C,               /* MOV AX, 4C00h                    */
    0xCD, 0x21,                     /* INT 21h                          */
    /* Pad image offset 0x11..0x1F to reach the msg at 0x20 */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x20: msg */
    'M', 'u', 'l', 't', 'i', '!', '\r', '\n', '$',
};

static void test_exe_multiseg_ds(void)
{
    WRITE_EXE("/tmp/dos_ms.exe", exe_multiseg_exe);
    char buf[64];
    int n = run_com_capture("/tmp/dos_ms.exe", buf, sizeof(buf));
    UT_ASSERT(n >= 6, "multiseg.exe produced output");
    UT_ASSERT(buf[0] == 'M' && buf[1] == 'u' && buf[2] == 'l' && buf[3] == 't' &&
                  buf[4] == 'i' && buf[5] == '!',
              "output is Multi!");
}

/* -- Test 21: mcb_entry.com --- read MCB at (DS-1):0 at program entry --- */
/*
 * Verifies the static MCB header set up by D-5a.1: at process entry,
 * the paragraph immediately before the PSP must hold a 'Z' (last-block)
 * MCB whose owner equals the PSP segment and whose size is non-zero.
 *
 *   0x100  MOV AX, DS                ; 8C D8
 *   0x102  DEC AX                    ; 48
 *   0x103  MOV ES, AX                ; 8E C0
 *   0x105  CMP byte ES:[0], 5Ah      ; 26 80 3E 00 00 5A   sig == 'Z'
 *   0x10B  JNE fail
 *   0x10D  MOV AX, DS                ; 8C D8
 *   0x10F  CMP word ES:[1], AX       ; 26 39 06 01 00       owner == DS
 *   0x114  JNE fail
 *   0x116  MOV AX, word ES:[3]       ; 26 A1 03 00          size != 0
 *   0x11A  TEST AX, AX
 *   0x11C  JZ  fail
 *   0x11E  exit 0                    ; B8 00 4C; CD 21
 *   0x123  fail: exit 1              ; B8 01 4C; CD 21
 */
static const unsigned char mcb_entry_com[] = {
    0x8C, 0xD8,                                /* MOV AX, DS         */
    0x48,                                      /* DEC AX             */
    0x8E, 0xC0,                                /* MOV ES, AX         */
    0x26, 0x80, 0x3E, 0x00, 0x00, 0x5A,        /* CMP es:[0], 5Ah    */
    0x75, 0x16,                                /* JNE fail (+0x16)   */
    0x8C, 0xD8,                                /* MOV AX, DS         */
    0x26, 0x39, 0x06, 0x01, 0x00,              /* CMP es:[1], AX     */
    0x75, 0x0D,                                /* JNE fail (+0x0D)   */
    0x26, 0xA1, 0x03, 0x00,                    /* MOV AX, es:[3]     */
    0x85, 0xC0,                                /* TEST AX, AX        */
    0x74, 0x05,                                /* JZ fail (+0x05)    */
    0xB8, 0x00, 0x4C,                          /* MOV AX, 4C00h      */
    0xCD, 0x21,                                /* INT 21h            */
    0xB8, 0x01, 0x4C,                          /* MOV AX, 4C01h      */
    0xCD, 0x21,                                /* INT 21h            */
};

static void test_dos_mcb_entry(void)
{
    WRITE_COM("/tmp/dos_mb.com", mcb_entry_com);
    int code = run_com("/tmp/dos_mb.com");
    UT_ASSERT_EQ(code, 0);
}

/* -- Test 22: resize.com --- AH=4Ah Resize Block ----------------------- */
/*
 * Three-phase test:
 *   (1) AH=4Ah BX=FFFFh — must fail with CF=1, AX=8 (insufficient
 *       memory).  Bridge also returns BX = max paragraphs available.
 *   (2) AH=4Ah BX=10h — must succeed.  Block is now 16 paragraphs
 *       (256 B); the rest of the run is converted into a free 'Z'
 *       block immediately after.
 *   (3) Re-read the MCB at (DS-1):0 and verify sig is now 'M'
 *       (more-follows) and size is 0x10.
 *
 * In a .COM, ES = DS = PSP segment at entry, so AH=4Ah's ES contract
 * is already satisfied without any setup.
 *
 *   0x100  MOV AH, 4Ah              ; B4 4A
 *   0x102  MOV BX, FFFFh            ; BB FF FF
 *   0x105  INT 21h                  ; CD 21
 *   0x107  JNC fail (+0x29)         ; 73 29
 *   0x109  CMP AX, 8                ; 3D 08 00
 *   0x10C  JNE fail (+0x24)         ; 75 24
 *   0x10E  MOV AH, 4Ah              ; B4 4A
 *   0x110  MOV BX, 10h              ; BB 10 00
 *   0x113  INT 21h                  ; CD 21
 *   0x115  JC  fail (+0x1B)         ; 72 1B
 *   0x117  MOV AX, DS               ; 8C D8
 *   0x119  DEC AX                   ; 48
 *   0x11A  MOV ES, AX               ; 8E C0
 *   0x11C  CMP byte es:[0], 4Dh     ; 26 80 3E 00 00 4D
 *   0x122  JNE fail (+0x0E)         ; 75 0E
 *   0x124  CMP word es:[3], 10h     ; 26 81 3E 03 00 10 00
 *   0x12B  JNE fail (+0x05)         ; 75 05
 *   0x12D  exit 0
 *   0x132  fail: exit 1
 */
static const unsigned char resize_com[] = {
    0xB4, 0x4A,                                /* MOV AH, 4Ah        */
    0xBB, 0xFF, 0xFF,                          /* MOV BX, FFFFh      */
    0xCD, 0x21,                                /* INT 21h            */
    0x73, 0x29,                                /* JNC fail (+0x29)   */
    0x3D, 0x08, 0x00,                          /* CMP AX, 8          */
    0x75, 0x24,                                /* JNE fail (+0x24)   */
    0xB4, 0x4A,                                /* MOV AH, 4Ah        */
    0xBB, 0x10, 0x00,                          /* MOV BX, 10h        */
    0xCD, 0x21,                                /* INT 21h            */
    0x72, 0x1B,                                /* JC fail (+0x1B)    */
    0x8C, 0xD8,                                /* MOV AX, DS         */
    0x48,                                      /* DEC AX             */
    0x8E, 0xC0,                                /* MOV ES, AX         */
    0x26, 0x80, 0x3E, 0x00, 0x00, 0x4D,        /* CMP es:[0], 4Dh    */
    0x75, 0x0E,                                /* JNE fail (+0x0E)   */
    0x26, 0x81, 0x3E, 0x03, 0x00, 0x10, 0x00,  /* CMP es:[3], 10h    */
    0x75, 0x05,                                /* JNE fail (+0x05)   */
    0xB8, 0x00, 0x4C,                          /* MOV AX, 4C00h      */
    0xCD, 0x21,                                /* INT 21h            */
    0xB8, 0x01, 0x4C,                          /* MOV AX, 4C01h      */
    0xCD, 0x21,                                /* INT 21h            */
};

static void test_dos_resize(void)
{
    WRITE_COM("/tmp/dos_rz.com", resize_com);
    int code = run_com("/tmp/dos_rz.com");
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
    test_rw_roundtrip();
    test_seek_delete();
    test_bad_whence();
    test_mkdir_rmdir();
    test_dup_handle();
    test_rename();
    test_exe_exit_code();
    test_exe_hello();
    test_exe_reloc_farjmp();
    test_exe_multiseg_ds();
    test_dos_mcb_entry();
    test_dos_resize();

    UT_SUMMARY("test_msdos");
}
