/*
 * human68k_bridge.c — Human68k DOS call bridge
 *
 * Translates F-line DOS calls into PPAP operations.
 * Phase 1: _EXIT, _EXIT2, _SETBLOCK, _MALLOC, _MFREE.
 * Phase 2: _GETCHAR, _PUTCHAR, _COMINP, _COMOUT, _PRINT, _GETS.
 */

#include "human68k_bridge.h"
#include "kernel/proc/proc.h"
#include "kernel/mm/page.h"
#include "kernel/syscall/syscall.h"
#include "kernel/errno.h"
#include <stddef.h>

/* Read big-endian values from user stack (native on m68k) */
static inline uint16_t ustack_u16(uint32_t usp, int offset)
{
    return *(volatile uint16_t *)(uintptr_t)(usp + offset);
}

static inline uint32_t ustack_u32(uint32_t usp, int offset)
{
    return *(volatile uint32_t *)(uintptr_t)(usp + offset);
}

/* Write big-endian 32-bit to memory (native on m68k) */
static inline void mem_write32(uint32_t addr, uint32_t val)
{
    *(volatile uint32_t *)(uintptr_t)addr = val;
}

/* Advance the faulting PC past the 2-byte F-line instruction.
 * PC is at byte offset 62 in the register frame (after 60 bytes of regs
 * and 2 bytes of SR). */
static inline void advance_pc(uint32_t *regs)
{
    /* PC is stored as a 32-bit value at regs + 60 bytes of regs + 2 bytes SR.
     * regs is an array of uint32_t: [0..14] = d0-d7,a0-a6 (60 bytes).
     * Exception frame starts at regs[15]: SR(16-bit) + PC(32-bit).
     * On m68k, SR is 16-bit at byte offset 60, PC is 32-bit at byte offset 62.
     * Access via byte pointer for correct alignment. */
    uint8_t *frame = (uint8_t *)regs;
    uint32_t pc = *(uint32_t *)(frame + 62);
    *(uint32_t *)(frame + 62) = pc + 2;
}

/* ── _EXIT ($FF00) / _EXIT2 ($FF4C) ──────────────────────────────────── */

static int dos_exit(uint32_t usp)
{
    uint16_t code = ustack_u16(usp, 0);
    sys_exit((int)(int16_t)code);
    return 1;  /* unreachable, but sys_exit doesn't return */
}

/* ── _PUTCHAR ($FF02) / _COMOUT ($FF04) ──────────────────────────────── *
 *
 * Stack: word char_code
 * Writes one character to stdout.  Returns the character in d0.
 */
static int dos_putchar(uint32_t *regs, uint32_t usp)
{
    uint8_t ch = (uint8_t)ustack_u16(usp, 0);
    sys_write(1, (const char *)&ch, 1);
    regs[0] = (uint32_t)ch;
    advance_pc(regs);
    return 2;
}

/* ── _GETCHAR ($FF01) ────────────────────────────────────────────────── *
 *
 * No arguments on stack.
 * Reads one character from stdin with echo.  Returns char in d0.
 */
static int dos_getchar(uint32_t *regs)
{
    uint8_t ch = 0;
    sys_read(0, (char *)&ch, 1);
    /* Echo the character back to stdout */
    sys_write(1, (const char *)&ch, 1);
    regs[0] = (uint32_t)ch;
    advance_pc(regs);
    return 2;
}

/* ── _COMINP ($FF03) ─────────────────────────────────────────────────── *
 *
 * No arguments on stack.
 * Reads one character from stdin (raw, no echo).  Returns char in d0.
 */
static int dos_cominp(uint32_t *regs)
{
    uint8_t ch = 0;
    sys_read(0, (char *)&ch, 1);
    regs[0] = (uint32_t)ch;
    advance_pc(regs);
    return 2;
}

/* ── _PRINT ($FF09) ──────────────────────────────────────────────────── *
 *
 * Stack: long str_ptr
 * Writes a NUL-terminated string to stdout.  Returns 0 in d0.
 */
static int dos_print(uint32_t *regs, uint32_t usp)
{
    uint32_t str_addr = ustack_u32(usp, 0);
    const char *str = (const char *)(uintptr_t)str_addr;

    /* Find string length (NUL-terminated) */
    uint32_t len = 0;
    while (str[len])
        len++;

    if (len > 0)
        sys_write(1, str, len);

    regs[0] = 0;
    advance_pc(regs);
    return 2;
}

/* ── _GETS ($FF0A) ───────────────────────────────────────────────────── *
 *
 * Stack: long buf_ptr
 * Line-buffered read into a linebuf structure:
 *   byte 0: max (max chars to read)
 *   byte 1: len (filled by DOS with actual count)
 *   byte 2+: buffer (filled with input, no NUL terminator)
 *
 * Reads up to max chars, stopping at CR (0x0D).
 * Echoes input and writes len.  Returns buf_ptr in d0.
 */
static int dos_gets(uint32_t *regs, uint32_t usp)
{
    uint32_t buf_addr = ustack_u32(usp, 0);
    uint8_t *buf = (uint8_t *)(uintptr_t)buf_addr;
    uint8_t max = buf[0];
    uint8_t count = 0;

    while (count < max) {
        uint8_t ch;
        long r = sys_read(0, (char *)&ch, 1);
        if (r <= 0)
            break;
        /* Echo */
        sys_write(1, (const char *)&ch, 1);
        if (ch == 0x0D || ch == 0x0A)
            break;
        buf[2 + count] = ch;
        count++;
    }

    buf[1] = count;
    regs[0] = buf_addr;
    advance_pc(regs);
    return 2;
}

/* ── _SETBLOCK ($FF4A) ───────────────────────────────────────────────── *
 *
 * Stack: long block_addr, long new_size
 * Resizes the process memory block.  Shrinking frees pages back to the
 * allocator; growing is not yet supported.
 * Returns d0 = 0 on success, negative error otherwise.
 */
static int dos_setblock(uint32_t *regs, uint32_t usp)
{
    uint32_t block_addr = ustack_u32(usp, 0);
    uint32_t new_size   = ustack_u32(usp, 4);
    pcb_t *p = current;

    /* Find the block in user_pages — must match base address */
    uint32_t base = (uint32_t)(uintptr_t)p->user_pages[0];
    if (block_addr != base) {
        regs[0] = (uint32_t)(-(int32_t)EFAULT);  /* invalid block */
        advance_pc(regs);
        return 2;
    }

    /* Current allocation */
    uint32_t cur_pages = 0;
    while (cur_pages < USER_PAGES_MAX && p->user_pages[cur_pages])
        cur_pages++;

    uint32_t cur_size = cur_pages * PAGE_SIZE;

    if (new_size > cur_size) {
        /* Growing not implemented yet — return error -8 (ENOMEM) */
        regs[0] = (uint32_t)(-(int32_t)ENOMEM);
        advance_pc(regs);
        return 2;
    }

    /* Shrink: free pages from the tail */
    uint32_t new_pages = (new_size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (new_pages < 1)
        new_pages = 1;  /* keep at least the PMB page */

    for (uint32_t i = new_pages; i < cur_pages; i++) {
        page_free(p->user_pages[i]);
        p->user_pages[i] = NULL;
    }

    /* Update MMB end pointer (offset 0x08 in the block) */
    uint32_t new_end = base + new_pages * PAGE_SIZE;
    mem_write32(base + 0x08, new_end);

    /* Update PMB stack pointer (offset 0x38) */
    mem_write32(base + 0x38, new_end);

    regs[0] = 0;  /* success */
    advance_pc(regs);
    return 2;
}

/* ── _MALLOC ($FF48) ─────────────────────────────────────────────────── *
 *
 * Stack: long size
 * If size == -1 ($FFFFFFFF), returns the largest available block size.
 * Otherwise, allocates memory (not implemented in Phase 1).
 */
static int dos_malloc(uint32_t *regs, uint32_t usp)
{
    uint32_t size = ustack_u32(usp, 0);

    if (size == 0xFFFFFFFF) {
        /* Query: return largest available block = free pages × PAGE_SIZE */
        extern uint32_t page_free_count(void);
        uint32_t free_bytes = page_free_count() * PAGE_SIZE;
        regs[0] = free_bytes;
        advance_pc(regs);
        return 2;
    }

    /* Allocation not implemented — return error */
    regs[0] = (uint32_t)(-(int32_t)ENOMEM);
    advance_pc(regs);
    return 2;
}

/* ── _MFREE ($FF49) ──────────────────────────────────────────────────── *
 *
 * Stack: long block_addr
 * No-op in Phase 1 minimal implementation.
 */
static int dos_mfree(uint32_t *regs, uint32_t usp)
{
    (void)usp;
    regs[0] = 0;  /* success (no-op) */
    advance_pc(regs);
    return 2;
}

/* ── Dispatch ─────────────────────────────────────────────────────────── */

int human68k_dos_dispatch(uint32_t *regs, uint32_t usp, uint16_t opcode)
{
    uint8_t func = opcode & 0xFF;

    switch (func) {
    case 0x00:  /* _EXIT */
    case 0x4C:  /* _EXIT2 */
        return dos_exit(usp);

    case 0x01:  /* _GETCHAR — read with echo */
        return dos_getchar(regs);

    case 0x02:  /* _PUTCHAR */
    case 0x04:  /* _COMOUT */
        return dos_putchar(regs, usp);

    case 0x03:  /* _COMINP — raw read, no echo */
        return dos_cominp(regs);

    case 0x09:  /* _PRINT */
        return dos_print(regs, usp);

    case 0x0A:  /* _GETS */
        return dos_gets(regs, usp);

    case 0x4A:  /* _SETBLOCK */
        return dos_setblock(regs, usp);

    case 0x48:  /* _MALLOC */
        return dos_malloc(regs, usp);

    case 0x49:  /* _MFREE */
        return dos_mfree(regs, usp);

    default:
        return -1;  /* unhandled */
    }
}
