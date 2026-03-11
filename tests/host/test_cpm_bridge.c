/*
 * test_cpm_bridge.c — Host-side tests for the CP/M subsystem
 *
 * Phase 1: .COM loader, memory map, BDOS fn 0/2/9
 * Phase 2: Console I/O — BDOS fn 1/3-8/10-12, BIOS CONST/CONIN/CONOUT
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "kernel/ecpu/ecpu.h"
#include "kernel/ecpu/ecpu_z80.h"
#include "kernel/subsys/cpm_bridge.h"

/* ── Output capture ────────────────────────────────────────────────────── */

static char  output_buf[4096];
static int   output_len;

void cpm_host_putchar(uint8_t ch)
{
    if (output_len < (int)sizeof(output_buf) - 1)
        output_buf[output_len++] = (char)ch;
}

static void reset_output(void)
{
    output_len = 0;
    memset(output_buf, 0, sizeof(output_buf));
}

/* ── Input queue (simulates keyboard input) ────────────────────────────── */

static const uint8_t *input_queue;
static int            input_pos;
static int            input_len;

int cpm_host_getchar(void)
{
    if (input_pos < input_len)
        return input_queue[input_pos++];
    return 0x1A;  /* EOF */
}

int cpm_host_char_ready(void)
{
    return input_pos < input_len;
}

static void set_input(const uint8_t *data, int len)
{
    input_queue = data;
    input_pos = 0;
    input_len = len;
}

/* ── Shared state ──────────────────────────────────────────────────────── */

static z80_state_t  cpu;
static uint8_t      mem[65536];
static cpm_state_t  cpm;

static void setup(void)
{
    memset(mem, 0, sizeof(mem));
    memset(&cpm, 0, sizeof(cpm));
    ecpu_z80_ops.init((ecpu_state_t *)&cpu, mem, sizeof(mem));
    ecpu_z80_ops.set_trap_handler((ecpu_state_t *)&cpu,
                                   cpm_trap_handler, &cpm);
    reset_output();
    input_queue = NULL;
    input_pos = 0;
    input_len = 0;
}

/* Helper: load COM, run, return exit code */
static int run_com(const uint8_t *com, int size, const char *cmdline)
{
    cpm_load_com(&cpu, &cpm, com, (uint32_t)size, cmdline);
    return ecpu_z80_ops.run((ecpu_state_t *)&cpu);
}

/* ═══════════════════════════════════════════════════════════════════════
 * Phase 1 tests (loader + fn 0/2/9)
 * ═══════════════════════════════════════════════════════════════════════ */

static void test_memory_map(void)
{
    setup();
    uint8_t com[] = { 0x76 };
    cpm_load_com(&cpu, &cpm, com, sizeof(com), NULL);

    assert(mem[0x0000] == 0xC3);
    assert(mem[0x0005] == 0xC3);
    assert(mem[0x0100] == 0x76);
    assert(mem[CPM_BIOS_ENTRY] == 0xC9);
    assert(mem[CPM_BIOS_ENTRY + 3] == 0xC9);
    assert(mem[CPM_BIOS_ENTRY + 48] == 0xC9);
    assert(cpu.pc == CPM_TPA_BASE);
    assert(cpu.sp == CPM_TPA_END - 2);
    assert(z80_read16(&cpu, cpu.sp) == 0x0000);
    assert(cpm.dma_addr == CPM_DMA_DEFAULT);
    assert(cpm.current_drive == 0);

    printf("  PASS: memory_map\n");
}

static void test_cmdline(void)
{
    setup();
    uint8_t com[] = { 0x76 };
    cpm_load_com(&cpu, &cpm, com, sizeof(com), " A:TEST.TXT B:OUT.DAT");

    assert(mem[0x0080] == 21);
    assert(memcmp(&mem[0x0081], " A:TEST.TXT B:OUT.DAT", 21) == 0);

    printf("  PASS: cmdline\n");
}

static void test_fcb_parse(void)
{
    setup();
    uint8_t com[] = { 0x76 };
    cpm_load_com(&cpu, &cpm, com, sizeof(com), "A:TEST.TXT B:OUT.DAT");

    assert(mem[CPM_FCB1_ADDR + 0] == 1);
    assert(memcmp(&mem[CPM_FCB1_ADDR + 1], "TEST    ", 8) == 0);
    assert(memcmp(&mem[CPM_FCB1_ADDR + 9], "TXT", 3) == 0);
    assert(mem[CPM_FCB2_ADDR + 0] == 2);
    assert(memcmp(&mem[CPM_FCB2_ADDR + 1], "OUT     ", 8) == 0);
    assert(memcmp(&mem[CPM_FCB2_ADDR + 9], "DAT", 3) == 0);

    printf("  PASS: fcb_parse\n");
}

static void test_fcb_no_drive(void)
{
    setup();
    uint8_t com[] = { 0x76 };
    cpm_load_com(&cpu, &cpm, com, sizeof(com), "HELLO.COM");

    assert(mem[CPM_FCB1_ADDR + 0] == 0);
    assert(memcmp(&mem[CPM_FCB1_ADDR + 1], "HELLO   ", 8) == 0);
    assert(memcmp(&mem[CPM_FCB1_ADDR + 9], "COM", 3) == 0);

    printf("  PASS: fcb_no_drive\n");
}

static void test_fcb_uppercase(void)
{
    setup();
    uint8_t com[] = { 0x76 };
    cpm_load_com(&cpu, &cpm, com, sizeof(com), "a:hello.com");

    assert(mem[CPM_FCB1_ADDR + 0] == 1);
    assert(memcmp(&mem[CPM_FCB1_ADDR + 1], "HELLO   ", 8) == 0);
    assert(memcmp(&mem[CPM_FCB1_ADDR + 9], "COM", 3) == 0);

    printf("  PASS: fcb_uppercase\n");
}

static void test_hello_com(void)
{
    setup();

    static const uint8_t hello_com[] = {
        0x0E, 0x09,              /* LD C, 9         */
        0x11, 0x0D, 0x01,        /* LD DE, 0x010D   */
        0xCD, 0x05, 0x00,        /* CALL 5          */
        0x0E, 0x00,              /* LD C, 0         */
        0xCD, 0x05, 0x00,        /* CALL 5          */
        'H', 'e', 'l', 'l', 'o', ',', ' ',
        'W', 'o', 'r', 'l', 'd', '!',
        0x0D, 0x0A, '$'
    };

    run_com(hello_com, sizeof(hello_com), NULL);

    assert(output_len == 15);
    assert(memcmp(output_buf, "Hello, World!\r\n", 15) == 0);

    printf("  PASS: hello_com\n");
}

static void test_console_output(void)
{
    setup();

    static const uint8_t com[] = {
        0x0E, 0x02, 0x1E, 'X', 0xCD, 0x05, 0x00,
        0x0E, 0x00, 0xCD, 0x05, 0x00,
    };

    run_com(com, sizeof(com), NULL);

    assert(output_len == 1);
    assert(output_buf[0] == 'X');

    printf("  PASS: console_output\n");
}

static void test_ret_exit(void)
{
    setup();

    static const uint8_t com[] = {
        0x0E, 0x00, 0xCD, 0x05, 0x00,
    };

    int rc = run_com(com, sizeof(com), NULL);
    assert(rc == 0);

    printf("  PASS: ret_exit\n");
}

static void test_multi_char_output(void)
{
    setup();

    static const uint8_t com[] = {
        0x0E, 0x02, 0x1E, 'A', 0xCD, 0x05, 0x00,
        0x0E, 0x02, 0x1E, 'B', 0xCD, 0x05, 0x00,
        0x0E, 0x00, 0xCD, 0x05, 0x00,
    };

    run_com(com, sizeof(com), NULL);

    assert(output_len == 2);
    assert(output_buf[0] == 'A');
    assert(output_buf[1] == 'B');

    printf("  PASS: multi_char_output\n");
}

static void test_unknown_bdos(void)
{
    setup();

    static const uint8_t com[] = {
        0x0E, 99, 0xCD, 0x05, 0x00,
        0x0E, 0x00, 0xCD, 0x05, 0x00,
    };

    run_com(com, sizeof(com), NULL);
    printf("  PASS: unknown_bdos\n");
}

static void test_print_via_loop(void)
{
    setup();

    static const uint8_t com[] = {
        0x1E, 'H', 0x0E, 0x02, 0xCD, 0x05, 0x00,
        0x1E, 'i', 0x0E, 0x02, 0xCD, 0x05, 0x00,
        0x0E, 0x00, 0xCD, 0x05, 0x00,
    };

    run_com(com, sizeof(com), NULL);

    assert(output_len == 2);
    assert(memcmp(output_buf, "Hi", 2) == 0);

    printf("  PASS: print_via_loop\n");
}

/* ═══════════════════════════════════════════════════════════════════════
 * Phase 2 tests (console I/O — fn 1, 3-8, 10-12, BIOS)
 * ═══════════════════════════════════════════════════════════════════════ */

/* ── Test: BDOS fn 1 — console input with echo ─────────────────────────── */

static void test_console_input(void)
{
    setup();

    /* Queue 'A' as input */
    static const uint8_t input[] = { 'A' };
    set_input(input, sizeof(input));

    /*
     * BDOS fn 1: Console Input → A = char (echoed)
     * Then store A to memory and print it via fn 2 to verify.
     *
     *   LD  C, 1           ; BDOS fn 1: Console Input
     *   CALL 5             ; A = input char (echoed)
     *   LD  E, A           ; copy to E for output check
     *   LD  C, 2           ; BDOS fn 2: Console Output
     *   CALL 5             ; print it again
     *   LD  C, 0           ; exit
     *   CALL 5
     */
    static const uint8_t com[] = {
        0x0E, 0x01,              /* LD C, 1         */
        0xCD, 0x05, 0x00,        /* CALL 5          */
        0x5F,                    /* LD E, A         */
        0x0E, 0x02,              /* LD C, 2         */
        0xCD, 0x05, 0x00,        /* CALL 5          */
        0x0E, 0x00,              /* LD C, 0         */
        0xCD, 0x05, 0x00,        /* CALL 5          */
    };

    run_com(com, sizeof(com), NULL);

    /* fn 1 echoes 'A', then fn 2 outputs 'A' again → "AA" */
    assert(output_len == 2);
    assert(output_buf[0] == 'A');
    assert(output_buf[1] == 'A');

    printf("  PASS: console_input\n");
}

/* ── Test: BDOS fn 6 — direct console I/O (output mode) ───────────────── */

static void test_direct_io_output(void)
{
    setup();

    /*
     * BDOS fn 6 with E != 0xFF/0xFE → output character
     *
     *   LD  C, 6           ; BDOS fn 6: Direct I/O
     *   LD  E, 'Z'         ; output mode
     *   CALL 5
     *   LD  C, 0           ; exit
     *   CALL 5
     */
    static const uint8_t com[] = {
        0x0E, 0x06,              /* LD C, 6         */
        0x1E, 'Z',               /* LD E, 'Z'       */
        0xCD, 0x05, 0x00,        /* CALL 5          */
        0x0E, 0x00,              /* LD C, 0         */
        0xCD, 0x05, 0x00,        /* CALL 5          */
    };

    run_com(com, sizeof(com), NULL);

    assert(output_len == 1);
    assert(output_buf[0] == 'Z');

    printf("  PASS: direct_io_output\n");
}

/* ── Test: BDOS fn 6 — direct console I/O (input mode, char ready) ───── */

static void test_direct_io_input(void)
{
    setup();

    static const uint8_t input[] = { 'Q' };
    set_input(input, sizeof(input));

    /*
     * BDOS fn 6 with E=0xFF → non-blocking input
     * Then output the result via fn 2
     *
     *   LD  C, 6           ; BDOS fn 6
     *   LD  E, 0xFF        ; input mode
     *   CALL 5             ; A = char or 0
     *   LD  E, A           ; copy result
     *   LD  C, 2           ; output it
     *   CALL 5
     *   LD  C, 0           ; exit
     *   CALL 5
     */
    static const uint8_t com[] = {
        0x0E, 0x06,              /* LD C, 6         */
        0x1E, 0xFF,              /* LD E, 0xFF      */
        0xCD, 0x05, 0x00,        /* CALL 5          */
        0x5F,                    /* LD E, A         */
        0x0E, 0x02,              /* LD C, 2         */
        0xCD, 0x05, 0x00,        /* CALL 5          */
        0x0E, 0x00,              /* LD C, 0         */
        0xCD, 0x05, 0x00,        /* CALL 5          */
    };

    run_com(com, sizeof(com), NULL);

    assert(output_len == 1);
    assert(output_buf[0] == 'Q');

    printf("  PASS: direct_io_input\n");
}

/* ── Test: BDOS fn 6 — direct console I/O (input mode, no char ready) ── */

static void test_direct_io_no_input(void)
{
    setup();
    /* No input queued — should return 0x00 */

    /*
     *   LD  C, 6
     *   LD  E, 0xFF        ; input mode
     *   CALL 5             ; A = 0x00 (nothing ready)
     *   LD  E, A
     *   LD  C, 2           ; output it (should print NUL)
     *   CALL 5
     *   LD  C, 0
     *   CALL 5
     */
    static const uint8_t com[] = {
        0x0E, 0x06,
        0x1E, 0xFF,
        0xCD, 0x05, 0x00,
        0x5F,
        0x0E, 0x02,
        0xCD, 0x05, 0x00,
        0x0E, 0x00,
        0xCD, 0x05, 0x00,
    };

    run_com(com, sizeof(com), NULL);

    /* A was 0x00, output via fn 2 writes NUL */
    assert(output_len == 1);
    assert(output_buf[0] == '\0');

    printf("  PASS: direct_io_no_input\n");
}

/* ── Test: BDOS fn 11 — console status ─────────────────────────────────── */

static void test_console_status(void)
{
    setup();

    static const uint8_t input[] = { 'X' };
    set_input(input, sizeof(input));

    /*
     * BDOS fn 11: Console Status → A=0xFF (char ready)
     * Store result, then check with no input.
     *
     *   LD  C, 11          ; Console Status
     *   CALL 5             ; A = 0xFF (input queued)
     *   LD  B, A           ; save result
     *   ; consume the input char
     *   LD  C, 1           ; Console Input
     *   CALL 5
     *   ; now check status again (should be 0x00)
     *   LD  C, 11
     *   CALL 5             ; A = 0x00 (no more input)
     *   ; output first result (B) then second (A) via fn 2
     *   LD  E, B
     *   LD  C, 2
     *   CALL 5
     *   LD  E, A
     *   LD  C, 2
     *   CALL 5
     *   LD  C, 0
     *   CALL 5
     */
    static const uint8_t com[] = {
        0x0E, 11,                /* LD C, 11        */
        0xCD, 0x05, 0x00,        /* CALL 5          */
        0x47,                    /* LD B, A         */
        0x0E, 0x01,              /* LD C, 1         */
        0xCD, 0x05, 0x00,        /* CALL 5          */
        0x0E, 11,                /* LD C, 11        */
        0xCD, 0x05, 0x00,        /* CALL 5          */
        0x58,                    /* LD E, B         */
        0x0E, 0x02,              /* LD C, 2         */
        0xCD, 0x05, 0x00,        /* CALL 5          */
        0x5F,                    /* LD E, A         */
        0x0E, 0x02,              /* LD C, 2         */
        0xCD, 0x05, 0x00,        /* CALL 5          */
        0x0E, 0x00,              /* LD C, 0         */
        0xCD, 0x05, 0x00,        /* CALL 5          */
    };

    run_com(com, sizeof(com), NULL);

    /* output[0] = echo of 'X' from fn 1,
     * output[1] = first status result (0xFF),
     * output[2] = second status result (0x00) */
    assert(output_len == 3);
    assert((uint8_t)output_buf[1] == 0xFF);
    assert((uint8_t)output_buf[2] == 0x00);

    printf("  PASS: console_status\n");
}

/* ── Test: BDOS fn 12 — return version number ──────────────────────────── */

static void test_version_number(void)
{
    setup();

    /*
     * BDOS fn 12: Return Version → HL=0x0022 (CP/M 2.2)
     * A gets L (0x22), H gets 0x00.
     * Output A via fn 2, then H via fn 2.
     *
     *   LD  C, 12
     *   CALL 5             ; HL = 0x0022, A = 0x22
     *   LD  D, H           ; save H
     *   LD  E, A           ; E = L (0x22)
     *   LD  C, 2
     *   CALL 5             ; output 0x22
     *   LD  E, D           ; E = H (0x00)
     *   LD  C, 2
     *   CALL 5             ; output 0x00
     *   LD  C, 0
     *   CALL 5
     */
    static const uint8_t com[] = {
        0x0E, 12,                /* LD C, 12        */
        0xCD, 0x05, 0x00,        /* CALL 5          */
        0x54,                    /* LD D, H         */
        0x5F,                    /* LD E, A         */
        0x0E, 0x02,              /* LD C, 2         */
        0xCD, 0x05, 0x00,        /* CALL 5          */
        0x5A,                    /* LD E, D         */
        0x0E, 0x02,              /* LD C, 2         */
        0xCD, 0x05, 0x00,        /* CALL 5          */
        0x0E, 0x00,              /* LD C, 0         */
        0xCD, 0x05, 0x00,        /* CALL 5          */
    };

    run_com(com, sizeof(com), NULL);

    assert(output_len == 2);
    assert((uint8_t)output_buf[0] == 0x22);  /* L = CP/M version 2.2 */
    assert((uint8_t)output_buf[1] == 0x00);  /* H = system type (CP/M) */

    printf("  PASS: version_number\n");
}

/* ── Test: BDOS fn 10 — read console buffer ────────────────────────────── */

static void test_read_console_buffer(void)
{
    setup();

    /* Input: "Hi\r" (enter terminates) */
    static const uint8_t input[] = { 'H', 'i', '\r' };
    set_input(input, sizeof(input));

    /*
     * Set up a buffer at a known address (0x0200) with max=10,
     * then call BDOS fn 10.
     *
     *   LD  A, 10          ; max chars
     *   LD  (0x0200), A    ; buf[0] = max
     *   LD  C, 10          ; BDOS fn 10: Read Console Buffer
     *   LD  DE, 0x0200     ; buffer address
     *   CALL 5
     *   ; Read back buf[1] (actual count) and output via fn 2
     *   LD  A, (0x0201)    ; actual count
     *   LD  E, A
     *   LD  C, 2
     *   CALL 5
     *   ; Read back buf[2] (first char) and output
     *   LD  A, (0x0202)
     *   LD  E, A
     *   LD  C, 2
     *   CALL 5
     *   ; Read back buf[3] (second char)
     *   LD  A, (0x0203)
     *   LD  E, A
     *   LD  C, 2
     *   CALL 5
     *   LD  C, 0
     *   CALL 5
     */
    static const uint8_t com[] = {
        0x3E, 10,                /* LD A, 10        */
        0x32, 0x00, 0x02,        /* LD (0x0200), A  */
        0x0E, 10,                /* LD C, 10        */
        0x11, 0x00, 0x02,        /* LD DE, 0x0200   */
        0xCD, 0x05, 0x00,        /* CALL 5          */
        0x3A, 0x01, 0x02,        /* LD A, (0x0201)  */
        0x5F,                    /* LD E, A         */
        0x0E, 0x02,              /* LD C, 2         */
        0xCD, 0x05, 0x00,        /* CALL 5          */
        0x3A, 0x02, 0x02,        /* LD A, (0x0202)  */
        0x5F,                    /* LD E, A         */
        0x0E, 0x02,              /* LD C, 2         */
        0xCD, 0x05, 0x00,        /* CALL 5          */
        0x3A, 0x03, 0x02,        /* LD A, (0x0203)  */
        0x5F,                    /* LD E, A         */
        0x0E, 0x02,              /* LD C, 2         */
        0xCD, 0x05, 0x00,        /* CALL 5          */
        0x0E, 0x00,              /* LD C, 0         */
        0xCD, 0x05, 0x00,        /* CALL 5          */
    };

    run_com(com, sizeof(com), NULL);

    /* fn 10 echoes "Hi\r\n" (4 chars), then fn 2 outputs count(2) + 'H' + 'i' */
    /* Echo: 'H', 'i', '\r', '\n' = 4 chars from fn 10
     * Then: count(0x02), 'H', 'i' = 3 chars from fn 2 calls */
    assert(output_len == 7);
    assert(output_buf[0] == 'H');   /* echo */
    assert(output_buf[1] == 'i');   /* echo */
    assert(output_buf[2] == '\r');  /* echo CR */
    assert(output_buf[3] == '\n');  /* echo LF */
    assert((uint8_t)output_buf[4] == 2);    /* count = 2 */
    assert(output_buf[5] == 'H');   /* buf[2] */
    assert(output_buf[6] == 'i');   /* buf[3] */

    printf("  PASS: read_console_buffer\n");
}

/* ── Test: BDOS fn 7/8 — get/set I/O byte ─────────────────────────────── */

static void test_iobyte(void)
{
    setup();

    /*
     * Set IOBYTE to 0x42 via fn 8, read back via fn 7, output result.
     *
     *   LD  C, 8           ; Set I/O Byte
     *   LD  E, 0x42
     *   CALL 5
     *   LD  C, 7           ; Get I/O Byte
     *   CALL 5             ; A = 0x42
     *   LD  E, A
     *   LD  C, 2           ; output it
     *   CALL 5
     *   LD  C, 0
     *   CALL 5
     */
    static const uint8_t com[] = {
        0x0E, 8,                 /* LD C, 8         */
        0x1E, 0x42,              /* LD E, 0x42      */
        0xCD, 0x05, 0x00,        /* CALL 5          */
        0x0E, 7,                 /* LD C, 7         */
        0xCD, 0x05, 0x00,        /* CALL 5          */
        0x5F,                    /* LD E, A         */
        0x0E, 0x02,              /* LD C, 2         */
        0xCD, 0x05, 0x00,        /* CALL 5          */
        0x0E, 0x00,              /* LD C, 0         */
        0xCD, 0x05, 0x00,        /* CALL 5          */
    };

    run_com(com, sizeof(com), NULL);

    assert(output_len == 1);
    assert((uint8_t)output_buf[0] == 0x42);

    printf("  PASS: iobyte\n");
}

/* ── Test: BDOS fn 3 (reader), fn 4 (punch), fn 5 (list) ──────────────── */

static void test_reader_punch_list(void)
{
    setup();

    static const uint8_t input[] = { 'R' };
    set_input(input, sizeof(input));

    /*
     * fn 3 (reader) → read char (no echo)
     * fn 4 (punch) → output char
     * fn 5 (list)  → output char
     *
     *   LD  C, 3           ; Reader Input
     *   CALL 5             ; A = 'R'
     *   LD  E, A
     *   LD  C, 4           ; Punch Output
     *   CALL 5             ; output 'R'
     *   LD  E, 'L'
     *   LD  C, 5           ; List Output
     *   CALL 5             ; output 'L'
     *   LD  C, 0
     *   CALL 5
     */
    static const uint8_t com[] = {
        0x0E, 3,                 /* LD C, 3         */
        0xCD, 0x05, 0x00,        /* CALL 5          */
        0x5F,                    /* LD E, A         */
        0x0E, 4,                 /* LD C, 4         */
        0xCD, 0x05, 0x00,        /* CALL 5          */
        0x1E, 'L',               /* LD E, 'L'       */
        0x0E, 5,                 /* LD C, 5         */
        0xCD, 0x05, 0x00,        /* CALL 5          */
        0x0E, 0x00,              /* LD C, 0         */
        0xCD, 0x05, 0x00,        /* CALL 5          */
    };

    run_com(com, sizeof(com), NULL);

    assert(output_len == 2);
    assert(output_buf[0] == 'R');  /* punch */
    assert(output_buf[1] == 'L');  /* list */

    printf("  PASS: reader_punch_list\n");
}

/* ── Test: BDOS fn 6 — status check (E=0xFE) ──────────────────────────── */

static void test_direct_io_status(void)
{
    setup();

    static const uint8_t input[] = { 'X' };
    set_input(input, sizeof(input));

    /*
     * fn 6 with E=0xFE → status check (0xFF=ready, 0x00=not)
     *
     *   LD  C, 6
     *   LD  E, 0xFE        ; status check
     *   CALL 5             ; A = 0xFF (ready)
     *   LD  E, A
     *   LD  C, 2
     *   CALL 5             ; output 0xFF
     *   LD  C, 0
     *   CALL 5
     */
    static const uint8_t com[] = {
        0x0E, 0x06,
        0x1E, 0xFE,
        0xCD, 0x05, 0x00,
        0x5F,
        0x0E, 0x02,
        0xCD, 0x05, 0x00,
        0x0E, 0x00,
        0xCD, 0x05, 0x00,
    };

    run_com(com, sizeof(com), NULL);

    assert(output_len == 1);
    assert((uint8_t)output_buf[0] == 0xFF);

    printf("  PASS: direct_io_status\n");
}

/* ── Test: BIOS CONIN/CONOUT/CONST ─────────────────────────────────────── */

static void test_bios_console(void)
{
    setup();

    static const uint8_t input[] = { 'B' };
    set_input(input, sizeof(input));

    /*
     * Call BIOS functions directly via CALL to BIOS entry addresses.
     * BIOS fn 2 (CONST) = CPM_BIOS_ENTRY + 6
     * BIOS fn 3 (CONIN) = CPM_BIOS_ENTRY + 9
     * BIOS fn 4 (CONOUT) = CPM_BIOS_ENTRY + 12
     *
     *   ; Check status (CONST)
     *   CALL BIOS+6        ; A = 0xFF (char ready)
     *   LD  B, A           ; save
     *   ; Read char (CONIN)
     *   CALL BIOS+9        ; A = 'B'
     *   LD  C, A           ; CONOUT expects char in C
     *   ; Write char (CONOUT)
     *   CALL BIOS+12       ; output 'B'
     *   ; Output saved status via BDOS fn 2
     *   LD  E, B
     *   LD  C, 2
     *   CALL 5             ; output 0xFF
     *   LD  C, 0
     *   CALL 5
     */
    uint16_t bios_const  = CPM_BIOS_ENTRY + 6;
    uint16_t bios_conin  = CPM_BIOS_ENTRY + 9;
    uint16_t bios_conout = CPM_BIOS_ENTRY + 12;

    uint8_t com[30];
    int p = 0;
    /* CALL BIOS+6 (CONST) */
    com[p++] = 0xCD;
    com[p++] = bios_const & 0xFF;
    com[p++] = bios_const >> 8;
    /* LD B, A */
    com[p++] = 0x47;
    /* CALL BIOS+9 (CONIN) */
    com[p++] = 0xCD;
    com[p++] = bios_conin & 0xFF;
    com[p++] = bios_conin >> 8;
    /* LD C, A */
    com[p++] = 0x4F;
    /* CALL BIOS+12 (CONOUT) */
    com[p++] = 0xCD;
    com[p++] = bios_conout & 0xFF;
    com[p++] = bios_conout >> 8;
    /* LD E, B */
    com[p++] = 0x58;
    /* LD C, 2 */
    com[p++] = 0x0E;
    com[p++] = 0x02;
    /* CALL 5 */
    com[p++] = 0xCD;
    com[p++] = 0x05;
    com[p++] = 0x00;
    /* LD C, 0 */
    com[p++] = 0x0E;
    com[p++] = 0x00;
    /* CALL 5 */
    com[p++] = 0xCD;
    com[p++] = 0x05;
    com[p++] = 0x00;

    run_com(com, p, NULL);

    /* output[0] = 'B' (CONOUT), output[1] = 0xFF (CONST status) */
    assert(output_len == 2);
    assert(output_buf[0] == 'B');
    assert((uint8_t)output_buf[1] == 0xFF);

    printf("  PASS: bios_console\n");
}

/* ── Test: BIOS READER returns EOF (0x1A) ──────────────────────────────── */

static void test_bios_reader(void)
{
    setup();

    uint16_t bios_reader = CPM_BIOS_ENTRY + 21;  /* fn 7 */

    /*
     *   CALL BIOS+21       ; READER → A = 0x1A (EOF)
     *   LD  E, A
     *   LD  C, 2
     *   CALL 5             ; output 0x1A
     *   LD  C, 0
     *   CALL 5
     */
    uint8_t com[20];
    int p = 0;
    com[p++] = 0xCD;
    com[p++] = bios_reader & 0xFF;
    com[p++] = bios_reader >> 8;
    com[p++] = 0x5F;  /* LD E, A */
    com[p++] = 0x0E;
    com[p++] = 0x02;
    com[p++] = 0xCD;
    com[p++] = 0x05;
    com[p++] = 0x00;
    com[p++] = 0x0E;
    com[p++] = 0x00;
    com[p++] = 0xCD;
    com[p++] = 0x05;
    com[p++] = 0x00;

    run_com(com, p, NULL);

    assert(output_len == 1);
    assert((uint8_t)output_buf[0] == 0x1A);

    printf("  PASS: bios_reader\n");
}

/* ── Test: echo program — reads chars, echoes them, stops on Ctrl-Z ───── */

static void test_echo_program(void)
{
    setup();

    /* Input: "AB\x1A" — program should echo A and B, then stop */
    static const uint8_t input[] = { 'A', 'B', 0x1A };
    set_input(input, sizeof(input));

    /*
     * Simple echo loop:
     *   loop:
     *     LD  C, 1         ; Console Input (echo)
     *     CALL 5           ; A = char
     *     CP  0x1A         ; EOF?
     *     JP  Z, done      ; yes → exit
     *     JP  loop         ; no → repeat
     *   done:
     *     LD  C, 0
     *     CALL 5
     */
    /* Addresses: 0x0100=loop, 0x010A=done */
    static const uint8_t com[] = {
        /* 0x0100: loop */
        0x0E, 0x01,              /* LD C, 1         */
        0xCD, 0x05, 0x00,        /* CALL 5          */
        0xFE, 0x1A,              /* CP 0x1A         */
        0xCA, 0x0D, 0x01,        /* JP Z, 0x010D    */
        0xC3, 0x00, 0x01,        /* JP 0x0100       */
        /* 0x010D: done */
        0x0E, 0x00,              /* LD C, 0         */
        0xCD, 0x05, 0x00,        /* CALL 5          */
    };

    run_com(com, sizeof(com), NULL);

    /* fn 1 echoes each char: A, B, then reads 0x1A (echoed too) → exits */
    assert(output_len == 3);
    assert(output_buf[0] == 'A');
    assert(output_buf[1] == 'B');
    assert(output_buf[2] == 0x1A);

    printf("  PASS: echo_program\n");
}

/* ── main ──────────────────────────────────────────────────────────────── */

int main(void)
{
    printf("test_cpm_bridge:\n");

    /* Phase 1: Loader tests */
    test_memory_map();
    test_cmdline();
    test_fcb_parse();
    test_fcb_no_drive();
    test_fcb_uppercase();

    /* Phase 1: BDOS output + exit */
    test_hello_com();
    test_console_output();
    test_ret_exit();
    test_multi_char_output();
    test_unknown_bdos();
    test_print_via_loop();

    /* Phase 2: Console I/O */
    test_console_input();
    test_direct_io_output();
    test_direct_io_input();
    test_direct_io_no_input();
    test_direct_io_status();
    test_console_status();
    test_version_number();
    test_read_console_buffer();
    test_iobyte();
    test_reader_punch_list();
    test_bios_console();
    test_bios_reader();
    test_echo_program();

    printf("All CP/M bridge tests passed.\n");
    return 0;
}
