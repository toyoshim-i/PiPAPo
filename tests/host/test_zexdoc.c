/*
 * test_zexdoc.c — Run ZEXDOC (Z80 instruction exerciser) on host
 *
 * Loads the ZEXDOC .COM binary into a minimal CP/M 2.2 environment
 * (BDOS console output + exit only) and runs it through the Z80 emulator.
 * Verifies that all instruction groups pass CRC checks.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "kernel/cpu/cpu.h"
#include "kernel/cpu/ecpu_z80.h"
#include "zexdoc_com.h"

/* ── Stubs for page allocator (not linked in host tests) ────────────────── */
void *page_alloc(void) { return NULL; }
void  page_free(void *p) { (void)p; }

/* ── CP/M memory map constants ──────────────────────────────────────────── */
#define CPM_TPA_BASE    0x0100
#define CPM_STUB_BASE   0xFDC0
#define CPM_BIOS_ENTRY  0xFE00
#define CPM_BIOS_FN_COUNT 17

/* ── Output capture ─────────────────────────────────────────────────────── */
#define OUTPUT_SIZE (256 * 1024)
static char output[OUTPUT_SIZE];
static int output_len;

static void emit_char(char ch)
{
    if (output_len < OUTPUT_SIZE - 1)
        output[output_len++] = ch;
    putchar(ch);
    if (ch == '\n')
        fflush(stdout);
}

/* ── BDOS trap handler ──────────────────────────────────────────────────── */

static int bdos_trap_handler(cpu_state_t *state, int trap_type,
                             uint32_t param, void *ctx)
{
    (void)ctx;
    z80_state_t *cpu = (z80_state_t *)state;

    if (trap_type == CPU_TRAP_RST && param == 0x0000) {
        uint16_t rst_addr = cpu->pc - 1;
        if (rst_addr == CPM_STUB_BASE) {
            /* BDOS call — function number in C register */
            uint8_t fn = cpu->c;
            switch (fn) {
            case 0: /* System reset / exit */
                return CPU_TRAP_EXIT;
            case 2: /* Console output: char in E */
                emit_char(cpu->e);
                return CPU_TRAP_HANDLED;
            case 9: { /* Print string: DE points to '$'-terminated string */
                uint16_t addr = z80_de(cpu);
                for (;;) {
                    uint8_t ch = cpu->memory[addr];
                    if (ch == '$')
                        break;
                    emit_char(ch);
                    addr++;
                }
                return CPU_TRAP_HANDLED;
            }
            default:
                /* Ignore unimplemented BDOS functions */
                return CPU_TRAP_HANDLED;
            }
        }
        /* BIOS warm boot (WBOOT = function 1, stub index 2) → exit */
        if (rst_addr == CPM_STUB_BASE + 2 * 2)
            return CPU_TRAP_EXIT;
        /* Other BIOS calls — just return handled */
        if (rst_addr > CPM_STUB_BASE &&
            rst_addr < CPM_STUB_BASE + (1 + CPM_BIOS_FN_COUNT) * 2)
            return CPU_TRAP_HANDLED;
    }

    if (trap_type == CPU_TRAP_HALT)
        return CPU_TRAP_EXIT;

    return CPU_TRAP_UNHANDLED;
}

/* ── CP/M memory setup ──────────────────────────────────────────────────── */

static void setup_cpm_memory(uint8_t *mem)
{
    /* Zero page */
    mem[0x0000] = 0xC3;  /* JP BIOS+3 (warm boot) */
    mem[0x0001] = (CPM_BIOS_ENTRY + 3) & 0xFF;
    mem[0x0002] = (CPM_BIOS_ENTRY + 3) >> 8;
    mem[0x0003] = 0x00;  /* IOBYTE */
    mem[0x0004] = 0x00;  /* Current drive/user */
    mem[0x0005] = 0xC3;  /* JP BDOS stub */
    mem[0x0006] = CPM_STUB_BASE & 0xFF;
    mem[0x0007] = CPM_STUB_BASE >> 8;

    /* Default FCBs and DMA — leave zeroed */

    /* RST stub area: each slot = RST 0 + RET */
    for (int i = 0; i < 1 + CPM_BIOS_FN_COUNT; i++) {
        uint16_t stub = CPM_STUB_BASE + i * 2;
        mem[stub]     = 0xC7;  /* RST 0 */
        mem[stub + 1] = 0xC9;  /* RET */
    }

    /* BIOS jump table: JP to RST stubs */
    for (int i = 0; i < CPM_BIOS_FN_COUNT; i++) {
        uint16_t addr = CPM_BIOS_ENTRY + i * 3;
        uint16_t stub = CPM_STUB_BASE + (1 + i) * 2;
        mem[addr]     = 0xC3;  /* JP */
        mem[addr + 1] = stub & 0xFF;
        mem[addr + 2] = stub >> 8;
    }

    /* Load ZEXDOC .COM at TPA base */
    memcpy(&mem[CPM_TPA_BASE], zexdoc_com, zexdoc_com_len);
}

/* ── Main ───────────────────────────────────────────────────────────────── */

int main(void)
{
    static uint8_t mem[65536];
    static z80_state_t cpu;

    printf("=== ZEXDOC: Z80 instruction exerciser (documented flags) ===\n");

    /* Initialize */
    memset(mem, 0, sizeof(mem));
    memset(&cpu, 0, sizeof(cpu));
    ecpu_z80_ops.init((cpu_state_t *)&cpu, mem, sizeof(mem));
    setup_cpm_memory(mem);

    /* Set up CPU state */
    cpu.pc = CPM_TPA_BASE;
    cpu.sp = CPM_STUB_BASE;  /* Stack grows down from stub area */

    /* Register trap handler */
    ecpu_z80_ops.set_trap_handler((cpu_state_t *)&cpu,
                                  bdos_trap_handler, NULL);

    /* Run ZEXDOC */
    output_len = 0;
    output[0] = '\0';
    ecpu_z80_ops.run((cpu_state_t *)&cpu);
    output[output_len] = '\0';

    if (output_len > 0 && output[output_len - 1] != '\n')
        printf("\n");

    /* Count results */
    int ok_count = 0;
    int error_count = 0;
    const char *p = output;
    while ((p = strstr(p, "OK")) != NULL) {
        ok_count++;
        p += 2;
    }
    p = output;
    while ((p = strstr(p, "ERROR")) != NULL) {
        error_count++;
        p += 5;
    }

    printf("\n--- Results: %d OK, %d ERROR ---\n", ok_count, error_count);

    assert(error_count == 0);
    assert(ok_count >= 60);  /* ZEXDOC has ~67 test groups */

    printf("ZEXDOC PASSED (%d instruction groups verified)\n", ok_count);
    return 0;
}
