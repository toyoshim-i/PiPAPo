/*
 * test_sos.c — S-OS "SWORD" subsystem userland test
 *
 * Creates small _SOS .obj binaries in /tmp, exec()s them via vfork+execve,
 * and verifies exit codes and console output.
 *
 * Each .obj contains a _SOS header (18 bytes) followed by hand-assembled
 * Z80 machine code loaded at the header-specified address.
 *
 * S-OS monitor entry points (descending from 0x1FFD):
 *   addr = 0x1FFD - fn * 3
 *   #COLD=0x1FFD  #HOT=0x1FFA  #VER=0x1FF7  #PRINT=0x1FF4
 *   #PRINTS=0x1FF1  #LTNL=0x1FEE  #NL=0x1FEB  #MSG=0x1FE8
 *   #MSX=0x1FE5  #MPRINT=0x1FE2  #TAB=0x1FDF  #LPRINT=0x1FDC
 *   #PRTHX=0x1FC1  #PRTHL=0x1FBE  #ASC=0x1FBB  #HEX=0x1FB8
 *   #2HEX=0x1FB5  #HLHEX=0x1FB2
 *   #WOPEN=0x1FAF  #WRD=0x1FAC  #RDD=0x1FA6
 *   #POKE=0x1F9A  #POKEA=0x1F97  #PEEK=0x1F94  #PEEKA=0x1F91
 *
 * Extended API (ascending from 0x2000):
 *   #ROPEN=0x2009  #NAME=0x2012  #KILL=0x2015
 *   #CSR=0x2018  #SCRN=0x201B  #LOC=0x201E
 *
 * Work area addresses:
 *   SOS_FNAM=0x1F40  SOS_DSK=0x1F5D
 *   SOS_DTADR=0x1F70  SOS_SIZE=0x1F72
 */

#include "utest.h"

/* ── Helper: write a _SOS .obj file to /tmp ────────────────────────────── */

static int write_obj(const char *path, const unsigned char *code, int size)
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

#define WRITE_OBJ(path, code) \
    UT_ASSERT_EQ(write_obj((path), (code), (int)sizeof(code)), 0)

/* ── Helper: exec a _SOS .obj and return exit code ─────────────────────── */

static int run_obj(const char *path)
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

/* ── Helper: exec a _SOS .obj with stdout piped, return output length ── */

static int run_obj_capture(const char *path, char *buf, int bufsize)
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

/*
 * _SOS header: "_SOS 01 3000 3000\n"  (18 bytes)
 *   mode=01 (binary), load=0x3000, exec=0x3000
 */
#define SOS_HDR  '_','S','O','S',' ','0','1',' ','3','0','0','0',' ','3','0','0','0',0x0A

/* ── Test 1: exit via #COLD ────────────────────────────────────────────── */
/*
 * Z80 code (loaded at 0x3000):
 *   JP 0x1FFD       ; C3 FD 1F  — #COLD (exit)
 */
static const unsigned char exit_cold_obj[] = {
    SOS_HDR,
    0xC3, 0xFD, 0x1F,       /* JP 0x1FFD (#COLD) */
};

static void test_exit_cold(void)
{
    WRITE_OBJ("/tmp/cold.obj", exit_cold_obj);
    int code = run_obj("/tmp/cold.obj");
    UT_ASSERT_EQ(code, 0);
}

/* ── Test 2: #PRINT — print single character 'X' ──────────────────────── */
/*
 * Z80 code:
 *   LD A, 'X'       ; 3E 58
 *   CALL #PRINT      ; CD F4 1F
 *   JP #COLD         ; C3 FD 1F
 */
static const unsigned char print_char_obj[] = {
    SOS_HDR,
    0x3E, 0x58,             /* LD A, 'X'       */
    0xCD, 0xF4, 0x1F,       /* CALL #PRINT     */
    0xC3, 0xFD, 0x1F,       /* JP #COLD        */
};

static void test_print_char(void)
{
    WRITE_OBJ("/tmp/pchar.obj", print_char_obj);
    char buf[64];
    int n = run_obj_capture("/tmp/pchar.obj", buf, sizeof(buf));
    /* Output includes ESC[2J ESC[H prefix from sos_run_process */
    UT_ASSERT(n >= 1, "print_char produced output");
    /* Find 'X' in output (skip ANSI clear-screen prefix) */
    int found = 0;
    for (int i = 0; i < n; i++)
        if (buf[i] == 'X') { found = 1; break; }
    UT_ASSERT(found, "output contains 'X'");
}

/* ── Test 3: #VER — return version, print as hex ──────────────────────── */
/*
 * Z80 code:
 *   CALL #VER        ; CD F7 1F  — H=0x66, L=0x20
 *   LD A, H          ; 7C
 *   CALL #PRTHX      ; CD C1 1F  — prints "66"
 *   LD A, L          ; 7D
 *   CALL #PRTHX      ; CD C1 1F  — prints "20"
 *   JP #COLD         ; C3 FD 1F
 *
 * Expected output (after ANSI prefix): "6620"
 */
static const unsigned char ver_obj[] = {
    SOS_HDR,
    0xCD, 0xF7, 0x1F,       /* CALL #VER       */
    0x7C,                   /* LD A, H         */
    0xCD, 0xC1, 0x1F,       /* CALL #PRTHX     */
    0x7D,                   /* LD A, L         */
    0xCD, 0xC1, 0x1F,       /* CALL #PRTHX     */
    0xC3, 0xFD, 0x1F,       /* JP #COLD        */
};

static void test_ver(void)
{
    WRITE_OBJ("/tmp/ver.obj", ver_obj);
    char buf[64];
    int n = run_obj_capture("/tmp/ver.obj", buf, sizeof(buf));
    UT_ASSERT(n >= 4, "ver produced output");
    /* Find "6620" in output */
    int found = 0;
    for (int i = 0; i <= n - 4; i++)
        if (buf[i]=='6' && buf[i+1]=='6' && buf[i+2]=='2' && buf[i+3]=='0')
            { found = 1; break; }
    UT_ASSERT(found, "ver output contains 6620");
}

/* ── Test 4: #MSG — print string terminated by 0x0D ───────────────────── */
/*
 * Z80 code:
 *   LD DE, 0x3008    ; 11 08 30  — pointer to message
 *   CALL #MSG        ; CD E8 1F
 *   JP #COLD         ; C3 FD 1F
 *   ; data at 0x3008: "Hi" 0x0D
 */
static const unsigned char msg_obj[] = {
    SOS_HDR,
    0x11, 0x08, 0x30,       /* LD DE, 0x3008   */
    0xCD, 0xE8, 0x1F,       /* CALL #MSG       */
    0xC3, 0xFD, 0x1F,       /* JP #COLD        */
    'H', 'i', 0x0D,         /* msg at 0x3008   */
};

static void test_msg(void)
{
    WRITE_OBJ("/tmp/msg.obj", msg_obj);
    char buf[64];
    int n = run_obj_capture("/tmp/msg.obj", buf, sizeof(buf));
    UT_ASSERT(n >= 2, "msg produced output");
    int found = 0;
    for (int i = 0; i <= n - 2; i++)
        if (buf[i]=='H' && buf[i+1]=='i') { found = 1; break; }
    UT_ASSERT(found, "msg output contains Hi");
}

/* ── Test 5: #MSX — print string terminated by 0x00 ───────────────────── */
/*
 * Z80 code:
 *   LD DE, 0x3008    ; 11 08 30
 *   CALL #MSX        ; CD E5 1F
 *   JP #COLD         ; C3 FD 1F
 *   ; data at 0x3008: "OK" 0x00
 */
static const unsigned char msx_obj[] = {
    SOS_HDR,
    0x11, 0x08, 0x30,       /* LD DE, 0x3008   */
    0xCD, 0xE5, 0x1F,       /* CALL #MSX       */
    0xC3, 0xFD, 0x1F,       /* JP #COLD        */
    'O', 'K', 0x00,         /* msg at 0x3008   */
};

static void test_msx(void)
{
    WRITE_OBJ("/tmp/msx.obj", msx_obj);
    char buf[64];
    int n = run_obj_capture("/tmp/msx.obj", buf, sizeof(buf));
    UT_ASSERT(n >= 2, "msx produced output");
    int found = 0;
    for (int i = 0; i <= n - 2; i++)
        if (buf[i]=='O' && buf[i+1]=='K') { found = 1; break; }
    UT_ASSERT(found, "msx output contains OK");
}

/* ── Test 6: #PRTHX — print A as 2-digit hex ──────────────────────────── */
/*
 * Z80 code:
 *   LD A, 0xAB       ; 3E AB
 *   CALL #PRTHX      ; CD C1 1F
 *   JP #COLD         ; C3 FD 1F
 *
 * Expected: "AB"
 */
static const unsigned char prthx_obj[] = {
    SOS_HDR,
    0x3E, 0xAB,             /* LD A, 0xAB      */
    0xCD, 0xC1, 0x1F,       /* CALL #PRTHX     */
    0xC3, 0xFD, 0x1F,       /* JP #COLD        */
};

static void test_prthx(void)
{
    WRITE_OBJ("/tmp/prthx.obj", prthx_obj);
    char buf[64];
    int n = run_obj_capture("/tmp/prthx.obj", buf, sizeof(buf));
    UT_ASSERT(n >= 2, "prthx produced output");
    int found = 0;
    for (int i = 0; i <= n - 2; i++)
        if (buf[i]=='A' && buf[i+1]=='B') { found = 1; break; }
    UT_ASSERT(found, "prthx output contains AB");
}

/* ── Test 7: #PRTHL — print HL as 4-digit hex ─────────────────────────── */
/*
 * Z80 code:
 *   LD HL, 0x1234    ; 21 34 12
 *   CALL #PRTHL      ; CD BE 1F
 *   JP #COLD         ; C3 FD 1F
 *
 * Expected: "1234"
 */
static const unsigned char prthl_obj[] = {
    SOS_HDR,
    0x21, 0x34, 0x12,       /* LD HL, 0x1234   */
    0xCD, 0xBE, 0x1F,       /* CALL #PRTHL     */
    0xC3, 0xFD, 0x1F,       /* JP #COLD        */
};

static void test_prthl(void)
{
    WRITE_OBJ("/tmp/prthl.obj", prthl_obj);
    char buf[64];
    int n = run_obj_capture("/tmp/prthl.obj", buf, sizeof(buf));
    UT_ASSERT(n >= 4, "prthl produced output");
    int found = 0;
    for (int i = 0; i <= n - 4; i++)
        if (buf[i]=='1' && buf[i+1]=='2' && buf[i+2]=='3' && buf[i+3]=='4')
            { found = 1; break; }
    UT_ASSERT(found, "prthl output contains 1234");
}

/* ── Test 8: #ASC — 4-bit to ASCII hex char ────────────────────────────── */
/*
 * Z80 code:
 *   LD A, 0x0A       ; 3E 0A     — low nibble = 10 → 'A'
 *   CALL #ASC        ; CD BB 1F
 *   CALL #PRINT      ; CD F4 1F  — prints 'A'
 *   LD A, 0x05       ; 3E 05     — low nibble = 5 → '5'
 *   CALL #ASC        ; CD BB 1F
 *   CALL #PRINT      ; CD F4 1F  — prints '5'
 *   JP #COLD         ; C3 FD 1F
 *
 * Expected: "A5"
 */
static const unsigned char asc_obj[] = {
    SOS_HDR,
    0x3E, 0x0A,             /* LD A, 0x0A      */
    0xCD, 0xBB, 0x1F,       /* CALL #ASC       */
    0xCD, 0xF4, 0x1F,       /* CALL #PRINT     */
    0x3E, 0x05,             /* LD A, 0x05      */
    0xCD, 0xBB, 0x1F,       /* CALL #ASC       */
    0xCD, 0xF4, 0x1F,       /* CALL #PRINT     */
    0xC3, 0xFD, 0x1F,       /* JP #COLD        */
};

static void test_asc(void)
{
    WRITE_OBJ("/tmp/asc.obj", asc_obj);
    char buf[64];
    int n = run_obj_capture("/tmp/asc.obj", buf, sizeof(buf));
    UT_ASSERT(n >= 2, "asc produced output");
    int found = 0;
    for (int i = 0; i <= n - 2; i++)
        if (buf[i]=='A' && buf[i+1]=='5') { found = 1; break; }
    UT_ASSERT(found, "asc output contains A5");
}

/* ── Test 9: #HEX — ASCII hex char to 4-bit value ─────────────────────── */
/*
 * Z80 code:
 *   LD A, 'B'        ; 3E 42     — ASCII 'B'
 *   CALL #HEX        ; CD B8 1F  — A = 0x0B
 *   CALL #PRTHX      ; CD C1 1F  — prints "0B"
 *   JP #COLD         ; C3 FD 1F
 *
 * Expected: "0B"
 */
static const unsigned char hex_obj[] = {
    SOS_HDR,
    0x3E, 0x42,             /* LD A, 'B'       */
    0xCD, 0xB8, 0x1F,       /* CALL #HEX       */
    0xCD, 0xC1, 0x1F,       /* CALL #PRTHX     */
    0xC3, 0xFD, 0x1F,       /* JP #COLD        */
};

static void test_hex(void)
{
    WRITE_OBJ("/tmp/hex.obj", hex_obj);
    char buf[64];
    int n = run_obj_capture("/tmp/hex.obj", buf, sizeof(buf));
    UT_ASSERT(n >= 2, "hex produced output");
    int found = 0;
    for (int i = 0; i <= n - 2; i++)
        if (buf[i]=='0' && buf[i+1]=='B') { found = 1; break; }
    UT_ASSERT(found, "hex output contains 0B");
}

/* ── Test 10: #2HEX — parse 2 hex chars at (HL) to byte in A ──────────── */
/*
 * Z80 code (loaded at 0x3000):
 *   LD HL, 0x300C    ; 21 0C 30  — points to "A7"
 *   CALL #2HEX       ; CD B5 1F  — A = 0xA7
 *   CALL #PRTHX      ; CD C1 1F  — prints "A7"
 *   JP #COLD         ; C3 FD 1F
 *   ; data at 0x300C: 'A' '7'
 */
static const unsigned char hex2_obj[] = {
    SOS_HDR,
    0x21, 0x0C, 0x30,       /* LD HL, 0x300C   */
    0xCD, 0xB5, 0x1F,       /* CALL #2HEX      */
    0xCD, 0xC1, 0x1F,       /* CALL #PRTHX     */
    0xC3, 0xFD, 0x1F,       /* JP #COLD        */
    'A', '7',               /* data at 0x300C  */
};

static void test_2hex(void)
{
    WRITE_OBJ("/tmp/2hex.obj", hex2_obj);
    char buf[64];
    int n = run_obj_capture("/tmp/2hex.obj", buf, sizeof(buf));
    UT_ASSERT(n >= 2, "2hex produced output");
    int found = 0;
    for (int i = 0; i <= n - 2; i++)
        if (buf[i]=='A' && buf[i+1]=='7') { found = 1; break; }
    UT_ASSERT(found, "2hex output contains A7");
}

/* ── Test 11: #HLHEX — parse 4 hex chars at (DE) to 16-bit HL ─────────── */
/*
 * Z80 code (loaded at 0x3000):
 *   LD DE, 0x300C    ; 11 0C 30  — points to "12AB"
 *   CALL #HLHEX      ; CD B2 1F  — HL = 0x12AB
 *   CALL #PRTHL      ; CD BE 1F  — prints "12AB"
 *   JP #COLD         ; C3 FD 1F
 *   ; data at 0x300C: '1' '2' 'A' 'B'
 */
static const unsigned char hlhex_obj[] = {
    SOS_HDR,
    0x11, 0x0C, 0x30,       /* LD DE, 0x300C   */
    0xCD, 0xB2, 0x1F,       /* CALL #HLHEX     */
    0xCD, 0xBE, 0x1F,       /* CALL #PRTHL     */
    0xC3, 0xFD, 0x1F,       /* JP #COLD        */
    '1', '2', 'A', 'B',     /* data at 0x300C  */
};

static void test_hlhex(void)
{
    WRITE_OBJ("/tmp/hlhex.obj", hlhex_obj);
    char buf[64];
    int n = run_obj_capture("/tmp/hlhex.obj", buf, sizeof(buf));
    UT_ASSERT(n >= 4, "hlhex produced output");
    int found = 0;
    for (int i = 0; i <= n - 4; i++)
        if (buf[i]=='1' && buf[i+1]=='2' && buf[i+2]=='A' && buf[i+3]=='B')
            { found = 1; break; }
    UT_ASSERT(found, "hlhex output contains 12AB");
}

/* ── Test 12: #POKE / #PEEK — single byte memory access ───────────────── */
/*
 * Z80 code:
 *   LD A, 0x42       ; 3E 42     — value to store
 *   LD HL, 0x4000    ; 21 00 40  — target address
 *   CALL #POKE       ; CD 9A 1F  — mem[0x4000] = 0x42
 *   LD HL, 0x4000    ; 21 00 40
 *   CALL #PEEK       ; CD 94 1F  — A = mem[0x4000]
 *   CALL #PRTHX      ; CD C1 1F  — prints "42"
 *   JP #COLD         ; C3 FD 1F
 *
 * Expected: "42"
 */
static const unsigned char poke_peek_obj[] = {
    SOS_HDR,
    0x3E, 0x42,             /* LD A, 0x42      */
    0x21, 0x00, 0x40,       /* LD HL, 0x4000   */
    0xCD, 0x9A, 0x1F,       /* CALL #POKE      */
    0x21, 0x00, 0x40,       /* LD HL, 0x4000   */
    0xCD, 0x94, 0x1F,       /* CALL #PEEK      */
    0xCD, 0xC1, 0x1F,       /* CALL #PRTHX     */
    0xC3, 0xFD, 0x1F,       /* JP #COLD        */
};

static void test_poke_peek(void)
{
    WRITE_OBJ("/tmp/pkpk.obj", poke_peek_obj);
    char buf[64];
    int n = run_obj_capture("/tmp/pkpk.obj", buf, sizeof(buf));
    UT_ASSERT(n >= 2, "poke_peek produced output");
    int found = 0;
    for (int i = 0; i <= n - 2; i++)
        if (buf[i]=='4' && buf[i+1]=='2') { found = 1; break; }
    UT_ASSERT(found, "poke_peek output contains 42");
}

/* ── Test 13: #POKEA / #PEEKA — block memory access ───────────────────── */
/*
 * Z80 code (loaded at 0x3000):
 * 0x3000: LD HL, 0x4000    ; 21 00 40  — dest for POKEA
 * 0x3003: LD DE, 0x302C    ; 11 2C 30  — src = inline data
 * 0x3006: LD BC, 3         ; 01 03 00
 * 0x3009: CALL #POKEA      ; CD 97 1F  — copy 3 bytes src→dest
 * 0x300C: LD HL, 0x4000    ; 21 00 40  — src for PEEKA
 * 0x300F: LD DE, 0x5000    ; 11 00 50  — dest
 * 0x3012: LD BC, 3         ; 01 03 00
 * 0x3015: CALL #PEEKA      ; CD 91 1F  — copy 3 bytes
 * 0x3018: LD HL, 0x5000    ; 21 00 50  — read back
 * 0x301B: LD A, (HL)       ; 7E
 * 0x301C: CALL #PRTHX      ; CD C1 1F
 * 0x301F: INC HL           ; 23
 * 0x3020: LD A, (HL)       ; 7E
 * 0x3021: CALL #PRTHX      ; CD C1 1F
 * 0x3024: INC HL           ; 23
 * 0x3025: LD A, (HL)       ; 7E
 * 0x3026: CALL #PRTHX      ; CD C1 1F
 * 0x3029: JP #COLD         ; C3 FD 1F
 * 0x302C: data: 0x41 0x42 0x43  ("ABC")
 *
 * Expected: "414243"
 */
static const unsigned char pokea_peeka_obj[] = {
    SOS_HDR,
    0x21, 0x00, 0x40,       /* LD HL, 0x4000   */
    0x11, 0x2C, 0x30,       /* LD DE, 0x302C   */
    0x01, 0x03, 0x00,       /* LD BC, 3        */
    0xCD, 0x97, 0x1F,       /* CALL #POKEA     */
    0x21, 0x00, 0x40,       /* LD HL, 0x4000   */
    0x11, 0x00, 0x50,       /* LD DE, 0x5000   */
    0x01, 0x03, 0x00,       /* LD BC, 3        */
    0xCD, 0x91, 0x1F,       /* CALL #PEEKA     */
    0x21, 0x00, 0x50,       /* LD HL, 0x5000   */
    0x7E,                   /* LD A, (HL)      */
    0xCD, 0xC1, 0x1F,       /* CALL #PRTHX     */
    0x23,                   /* INC HL          */
    0x7E,                   /* LD A, (HL)      */
    0xCD, 0xC1, 0x1F,       /* CALL #PRTHX     */
    0x23,                   /* INC HL          */
    0x7E,                   /* LD A, (HL)      */
    0xCD, 0xC1, 0x1F,       /* CALL #PRTHX     */
    0xC3, 0xFD, 0x1F,       /* JP #COLD        */
    0x41, 0x42, 0x43,       /* data at 0x302C  */
};

static void test_pokea_peeka(void)
{
    WRITE_OBJ("/tmp/blkmem.obj", pokea_peeka_obj);
    char buf[64];
    int n = run_obj_capture("/tmp/blkmem.obj", buf, sizeof(buf));
    UT_ASSERT(n >= 6, "pokea_peeka produced output");
    int found = 0;
    for (int i = 0; i <= n - 6; i++)
        if (buf[i]=='4' && buf[i+1]=='1' && buf[i+2]=='4' &&
            buf[i+3]=='2' && buf[i+4]=='4' && buf[i+5]=='3')
            { found = 1; break; }
    UT_ASSERT(found, "pokea_peeka output contains 414243");
}

/* ── Test 14: File I/O — #WOPEN, #WRD, #ROPEN, #RDD, #KILL ────────────── */
/*
 * Z80 code (loaded at 0x3000):
 * Writes "HELLO" to /a/TST, reads it back, prints, and deletes.
 * Uses individual LD (HL),n to set SOS_FNAM byte-by-byte.
 *
 * Offset  Instruction             ; Comment
 * 0x00    LD HL, 0x1F40           ; → SOS_FNAM
 * 0x03    LD (HL), 'T'            ; filename[0]
 * 0x05    INC HL
 * 0x06    LD (HL), 'S'            ; filename[1]
 * 0x08    INC HL
 * 0x09    LD (HL), 'T'            ; filename[2]
 * 0x0B    INC HL
 * 0x0C    LD (HL), 0              ; NUL terminator
 * 0x0E    LD A, 0
 * 0x10    LD (0x1F5D), A          ; DSK = 0 → drive 'a'
 * 0x13    CALL #WOPEN             ; open /a/TST for write
 * 0x16    LD HL, wdata            ; → "HELLO" in program
 * 0x19    LD (0x1F70), HL         ; DTADR
 * 0x1C    LD HL, 5
 * 0x1F    LD (0x1F72), HL         ; SIZE = 5
 * 0x22    CALL #WRD               ; write + close
 *         --- re-set FNAM ---
 * 0x25    LD HL, 0x1F40
 * 0x28    LD (HL), 'T'
 * 0x2A    INC HL; LD (HL), 'S'
 * 0x2D    INC HL; LD (HL), 'T'
 * 0x30    INC HL; LD (HL), 0
 * 0x33    CALL #ROPEN             ; open /a/TST for read
 * 0x36    LD HL, 0x5000
 * 0x39    LD (0x1F70), HL         ; DTADR = 0x5000
 * 0x3C    CALL #RDD               ; read + close
 *         --- print 5 bytes ---
 * 0x3F    LD HL, 0x5000
 * 0x42    LD B, 5
 * 0x44    LD A, (HL)
 * 0x45    CALL #PRINT
 * 0x48    INC HL
 * 0x49    DJNZ -7 (→ 0x44)
 *         --- re-set FNAM ---
 * 0x4B    LD HL, 0x1F40
 * 0x4E    LD (HL), 'T'
 * 0x50    INC HL; LD (HL), 'S'
 * 0x53    INC HL; LD (HL), 'T'
 * 0x56    INC HL; LD (HL), 0
 * 0x59    CALL #KILL
 * 0x5C    JP #COLD
 * 0x5F    "HELLO" (wdata)
 */
static const unsigned char file_io_obj[] = {
    SOS_HDR,
    /* Set FNAM = "TST\0" */
    0x21, 0x40, 0x1F,       /* LD HL, 0x1F40       */
    0x36, 'T',              /* LD (HL), 'T'        */
    0x23,                   /* INC HL              */
    0x36, 'S',              /* LD (HL), 'S'        */
    0x23,                   /* INC HL              */
    0x36, 'T',              /* LD (HL), 'T'        */
    0x23,                   /* INC HL              */
    0x36, 0x00,             /* LD (HL), 0          */
    /* Set DSK = 0 */
    0x3E, 0x00,             /* LD A, 0             */
    0x32, 0x5D, 0x1F,       /* LD (0x1F5D), A      */
    /* WOPEN */
    0xCD, 0xAF, 0x1F,       /* CALL #WOPEN         */
    /* Set DTADR = wdata (0x305F), SIZE = 5 */
    0x21, 0x5F, 0x30,       /* LD HL, 0x305F       */
    0x22, 0x70, 0x1F,       /* LD (0x1F70), HL     */
    0x21, 0x05, 0x00,       /* LD HL, 5            */
    0x22, 0x72, 0x1F,       /* LD (0x1F72), HL     */
    /* WRD */
    0xCD, 0xAC, 0x1F,       /* CALL #WRD           */
    /* Re-set FNAM = "TST\0" for ROPEN */
    0x21, 0x40, 0x1F,       /* LD HL, 0x1F40       */
    0x36, 'T',              /* LD (HL), 'T'        */
    0x23,                   /* INC HL              */
    0x36, 'S',              /* LD (HL), 'S'        */
    0x23,                   /* INC HL              */
    0x36, 'T',              /* LD (HL), 'T'        */
    0x23,                   /* INC HL              */
    0x36, 0x00,             /* LD (HL), 0          */
    /* ROPEN */
    0xCD, 0x09, 0x20,       /* CALL #ROPEN         */
    /* Set DTADR = 0x5000 for read */
    0x21, 0x00, 0x50,       /* LD HL, 0x5000       */
    0x22, 0x70, 0x1F,       /* LD (0x1F70), HL     */
    /* RDD */
    0xCD, 0xA6, 0x1F,       /* CALL #RDD           */
    /* Print 5 bytes from 0x5000 */
    0x21, 0x00, 0x50,       /* LD HL, 0x5000       */
    0x06, 0x05,             /* LD B, 5             */
    /* print_loop: */
    0x7E,                   /* LD A, (HL)          */
    0xCD, 0xF4, 0x1F,       /* CALL #PRINT         */
    0x23,                   /* INC HL              */
    0x10, 0xF9,             /* DJNZ -7             */
    /* Re-set FNAM = "TST\0" for KILL */
    0x21, 0x40, 0x1F,       /* LD HL, 0x1F40       */
    0x36, 'T',              /* LD (HL), 'T'        */
    0x23,                   /* INC HL              */
    0x36, 'S',              /* LD (HL), 'S'        */
    0x23,                   /* INC HL              */
    0x36, 'T',              /* LD (HL), 'T'        */
    0x23,                   /* INC HL              */
    0x36, 0x00,             /* LD (HL), 0          */
    /* KILL */
    0xCD, 0x15, 0x20,       /* CALL #KILL          */
    /* EXIT */
    0xC3, 0xFD, 0x1F,       /* JP #COLD            */
    /* wdata at 0x305F */
    'H','E','L','L','O',
};

static void test_file_io(void)
{
    /* Drive A root is derived from argv[0] directory.
     * Since we exec /tmp/fio.obj, drive A maps to /tmp. */
    WRITE_OBJ("/tmp/fio.obj", file_io_obj);
    char buf[128];
    int n = run_obj_capture("/tmp/fio.obj", buf, sizeof(buf));
    UT_ASSERT(n >= 5, "file_io produced output");
    int found = 0;
    for (int i = 0; i <= n - 5; i++)
        if (buf[i]=='H' && buf[i+1]=='E' && buf[i+2]=='L' &&
            buf[i+3]=='L' && buf[i+4]=='O')
            { found = 1; break; }
    UT_ASSERT(found, "file_io output contains HELLO");

    /* Verify file was deleted by #KILL */
    int fd = open("/tmp/TST", O_RDONLY, 0);
    if (fd >= 0) {
        close(fd);
        UT_ASSERT(0, "TST should have been deleted by #KILL");
    }
}

/* ── Test 15: #LOC / #CSR / #SCRN — cursor and screen buffer ──────────── */
/*
 * Z80 code:
 *   LD A, 'Z'        ; 3E 5A
 *   CALL #PRINT      ; CD F4 1F  — prints 'Z', cursor advances
 *   LD A, 'Q'        ; 3E 51
 *   CALL #PRINT      ; CD F4 1F  — prints 'Q'
 *   CALL #CSR        ; CD 18 20  — L=cursor_x, H=cursor_y
 *   LD A, L          ; 7D
 *   CALL #PRTHX      ; CD C1 1F  — print cursor X
 *   LD A, H          ; 7C
 *   CALL #PRTHX      ; CD C1 1F  — print cursor Y
 *   ; Read 'Z' at (0,0) via #SCRN
 *   LD L, 0          ; 2E 00     — X = 0
 *   LD H, 0          ; 26 00     — Y = 0
 *   CALL #SCRN       ; CD 1B 20  — A = screen char at (0,0)
 *   CALL #PRTHX      ; CD C1 1F  — print as hex
 *   JP #COLD         ; C3 FD 1F
 *
 * After printing "ZQ", cursor_x=2, cursor_y=0.
 * Expected output (after ANSI): "ZQ" + "0200" + "5A"
 *   0200 = cursor at x=2, y=0
 *   5A = hex for 'Z' at position (0,0)
 */
static const unsigned char loc_csr_scrn_obj[] = {
    SOS_HDR,
    0x3E, 0x5A,             /* LD A, 'Z'       */
    0xCD, 0xF4, 0x1F,       /* CALL #PRINT     */
    0x3E, 0x51,             /* LD A, 'Q'       */
    0xCD, 0xF4, 0x1F,       /* CALL #PRINT     */
    0xCD, 0x18, 0x20,       /* CALL #CSR       */
    0x7D,                   /* LD A, L (X)     */
    0xCD, 0xC1, 0x1F,       /* CALL #PRTHX     */
    0x7C,                   /* LD A, H (Y)     */
    0xCD, 0xC1, 0x1F,       /* CALL #PRTHX     */
    0x2E, 0x00,             /* LD L, 0 (X=0)   */
    0x26, 0x00,             /* LD H, 0 (Y=0)   */
    0xCD, 0x1B, 0x20,       /* CALL #SCRN      */
    0xCD, 0xC1, 0x1F,       /* CALL #PRTHX     */
    0xC3, 0xFD, 0x1F,       /* JP #COLD        */
};

static void test_loc_csr_scrn(void)
{
    WRITE_OBJ("/tmp/lcs.obj", loc_csr_scrn_obj);
    char buf[128];
    int n = run_obj_capture("/tmp/lcs.obj", buf, sizeof(buf));
    UT_ASSERT(n >= 6, "loc_csr_scrn produced output");

    /* Look for cursor position "0200" (X=2, Y=0) */
    int found_csr = 0;
    for (int i = 0; i <= n - 4; i++)
        if (buf[i]=='0' && buf[i+1]=='2' && buf[i+2]=='0' && buf[i+3]=='0')
            { found_csr = 1; break; }
    UT_ASSERT(found_csr, "cursor position is 0200 (x=2,y=0)");

    /* Look for screen char "5A" ('Z' at position 0,0) */
    int found_scrn = 0;
    for (int i = 0; i <= n - 2; i++)
        if (buf[i]=='5' && buf[i+1]=='A') { found_scrn = 1; break; }
    UT_ASSERT(found_scrn, "screen char at (0,0) is 5A ('Z')");
}

/* ── Test 16: #NL — newline (CR+LF) output ────────────────────────────── */
/*
 * Z80 code:
 *   LD A, 'A'        ; 3E 41
 *   CALL #PRINT      ; CD F4 1F
 *   CALL #NL         ; CD EB 1F  — outputs CR+LF
 *   LD A, 'B'        ; 3E 42
 *   CALL #PRINT      ; CD F4 1F
 *   JP #COLD         ; C3 FD 1F
 *
 * Expected output contains "A\r\nB"
 */
static const unsigned char nl_obj[] = {
    SOS_HDR,
    0x3E, 0x41,             /* LD A, 'A'       */
    0xCD, 0xF4, 0x1F,       /* CALL #PRINT     */
    0xCD, 0xEB, 0x1F,       /* CALL #NL        */
    0x3E, 0x42,             /* LD A, 'B'       */
    0xCD, 0xF4, 0x1F,       /* CALL #PRINT     */
    0xC3, 0xFD, 0x1F,       /* JP #COLD        */
};

static void test_nl(void)
{
    WRITE_OBJ("/tmp/nl.obj", nl_obj);
    char buf[64];
    int n = run_obj_capture("/tmp/nl.obj", buf, sizeof(buf));
    UT_ASSERT(n >= 4, "nl produced output");
    /* Find "A" followed eventually by "B" with CR or LF in between */
    int found_a = -1, found_b = -1;
    for (int i = 0; i < n; i++) {
        if (buf[i] == 'A' && found_a < 0) found_a = i;
        if (buf[i] == 'B' && found_a >= 0) { found_b = i; break; }
    }
    UT_ASSERT(found_a >= 0 && found_b > found_a, "output has A then B");
}

/* ── Test 17: #MPRINT — inline string print ───────────────────────────── */
/*
 * Z80 code:
 *   CALL #MPRINT     ; CD E2 1F
 *   db "YO", 0       ; inline string (NUL-terminated)
 *   JP #COLD         ; C3 FD 1F
 *
 * #MPRINT pops the return address (pointing to "YO\0"), prints until
 * NUL, then pushes the address past the NUL as new return address.
 */
static const unsigned char mprint_obj[] = {
    SOS_HDR,
    0xCD, 0xE2, 0x1F,       /* CALL #MPRINT    */
    'Y', 'O', 0x00,         /* inline "YO\0"   */
    0xC3, 0xFD, 0x1F,       /* JP #COLD        */
};

static void test_mprint(void)
{
    WRITE_OBJ("/tmp/mprnt.obj", mprint_obj);
    char buf[64];
    int n = run_obj_capture("/tmp/mprnt.obj", buf, sizeof(buf));
    UT_ASSERT(n >= 2, "mprint produced output");
    int found = 0;
    for (int i = 0; i <= n - 2; i++)
        if (buf[i]=='Y' && buf[i+1]=='O') { found = 1; break; }
    UT_ASSERT(found, "mprint output contains YO");
}

/* ── Main ──────────────────────────────────────────────────────────────── */

int main(void)
{
    test_exit_cold();
    test_print_char();
    test_ver();
    test_msg();
    test_msx();
    test_prthx();
    test_prthl();
    test_asc();
    test_hex();
    test_2hex();
    test_hlhex();
    test_poke_peek();
    test_pokea_peeka();
    test_file_io();
    test_loc_csr_scrn();
    test_nl();
    test_mprint();
    UT_SUMMARY("test_sos");
}
