/*
 * human68k_bridge.c — Human68k DOS call bridge
 *
 * Translates F-line DOS calls into PPAP operations.
 * Phase 1: _EXIT, _EXIT2, _SETBLOCK, _MALLOC, _MFREE.
 * Phase 2: _GETCHAR, _PUTCHAR, _COMINP, _COMOUT, _PRINT, _GETS.
 * Phase 3: _FGETC, _FGETS, _FPUTC, _FPUTS.
 */

#include "human68k_bridge.h"
#include "kernel/proc/proc.h"
#include "kernel/mm/page.h"
#include "kernel/syscall/syscall.h"
#include "kernel/klog.h"
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

/* ── _FGETC ($FF1B) ─────────────────────────────────────────────────── *
 *
 * Stack: word fileno
 * Reads one byte from the specified file handle.  Returns byte in d0.
 */
static int dos_fgetc(uint32_t *regs, uint32_t usp)
{
    int fd = (int)(int16_t)ustack_u16(usp, 0);
    uint8_t ch = 0;
    long r = sys_read(fd, (char *)&ch, 1);
    regs[0] = (r > 0) ? (uint32_t)ch : (uint32_t)(-1);
    advance_pc(regs);
    return 2;
}

/* ── _FGETS ($FF1C) ─────────────────────────────────────────────────── *
 *
 * Stack: word fileno, long buf_ptr
 * Line-buffered read from file handle into linebuf structure:
 *   byte 0: max (max chars to read)
 *   byte 1: len (filled with actual count)
 *   byte 2+: buffer data
 * Reads until newline or max chars.  Returns count in d0.
 */
static int dos_fgets(uint32_t *regs, uint32_t usp)
{
    int fd = (int)(int16_t)ustack_u16(usp, 0);
    uint32_t buf_addr = ustack_u32(usp, 2);
    uint8_t *buf = (uint8_t *)(uintptr_t)buf_addr;
    uint8_t max = buf[0];
    uint8_t count = 0;

    while (count < max) {
        uint8_t ch;
        long r = sys_read(fd, (char *)&ch, 1);
        if (r <= 0)
            break;
        if (ch == 0x0D || ch == 0x0A)
            break;
        buf[2 + count] = ch;
        count++;
    }

    buf[1] = count;
    regs[0] = (uint32_t)count;
    advance_pc(regs);
    return 2;
}

/* ── _FPUTC ($FF1D) ─────────────────────────────────────────────────── *
 *
 * Stack: word fileno, word code
 * Writes one byte to the specified file handle.  Returns byte in d0.
 */
static int dos_fputc(uint32_t *regs, uint32_t usp)
{
    int fd = (int)(int16_t)ustack_u16(usp, 0);
    uint8_t ch = (uint8_t)ustack_u16(usp, 2);
    sys_write(fd, (const char *)&ch, 1);
    regs[0] = (uint32_t)ch;
    advance_pc(regs);
    return 2;
}

/* ── _FPUTS ($FF1E) ─────────────────────────────────────────────────── *
 *
 * Stack: word fileno, long str_ptr
 * Writes a NUL-terminated string to the file handle.
 * The NUL terminator is not written.  Returns 0 in d0.
 */
static int dos_fputs(uint32_t *regs, uint32_t usp)
{
    int fd = (int)(int16_t)ustack_u16(usp, 0);
    uint32_t str_addr = ustack_u32(usp, 2);
    const char *str = (const char *)(uintptr_t)str_addr;

    uint32_t len = 0;
    while (str[len])
        len++;

    if (len > 0)
        sys_write(fd, str, len);

    regs[0] = 0;
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

    case 0x1B:  /* _FGETC */
        return dos_fgetc(regs, usp);

    case 0x1C:  /* _FGETS */
        return dos_fgets(regs, usp);

    case 0x1D:  /* _FPUTC */
        return dos_fputc(regs, usp);

    case 0x1E:  /* _FPUTS */
        return dos_fputs(regs, usp);

    case 0x4A:  /* _SETBLOCK */
        return dos_setblock(regs, usp);

    case 0x48:  /* _MALLOC */
        return dos_malloc(regs, usp);

    case 0x49:  /* _MFREE */
        return dos_mfree(regs, usp);

    default:
        klogf("[human68k] unimplemented DOS call $FF%x\n", (uint32_t)func);
        regs[0] = (uint32_t)(-(int32_t)ENOSYS);
        advance_pc(regs);
        return 2;
    }
}

/* ── Subsystem ops (layering hooks called by the kernel) ──────────── */

/* Static pool of per-process Human68k state.
 * Indexed by pid (max PROC_MAX).  Zero-initialized = default vectors. */
static h68k_proc_t h68k_pool[PROC_MAX];

/* on_init — called when a Human68k binary is exec'd */
static void h68k_on_init(struct pcb *p)
{
    h68k_proc_t *h = &h68k_pool[p->pid];
    h->exitvc = 0;   /* default: no-op (normal exit proceeds)      */
    h->ctrlvc = 0;   /* default: _EXIT(-1) on Ctrl+C               */
    h->errjvc = 0;   /* default: _EXIT(-1) on error abort           */
    p->subsys_data = h;
}

/* on_crash — handle faults for Human68k processes (_ERRJVC) */
static int h68k_on_crash(struct pcb *p, uint32_t *regs, uint16_t *exc,
                          int is_group0)
{
    h68k_proc_t *h = (h68k_proc_t *)p->subsys_data;
    if (!h)
        return 0;

    if (h->errjvc) {
        klogf("  _ERRJVC: jumping to %x", h->errjvc);
        exc[is_group0 ? 5 : 1] = (uint16_t)(h->errjvc >> 16);
        exc[is_group0 ? 6 : 2] = (uint16_t)(h->errjvc & 0xFFFF);
        return 2;  /* return to (redirected) user mode */
    }

    klogf("  _ERRJVC default: _EXIT(-1)");
    sys_exit(-1);
    return 1;
}

/* on_signal — handle SIGINT for Human68k processes (_CTRLVC) */
static int h68k_on_signal(struct pcb *p, int sig, uint32_t *regs)
{
    /* Only intercept SIGINT (Ctrl+C) */
    if (sig != 2)  /* SIGINT = 2 */
        return 0;

    h68k_proc_t *h = (h68k_proc_t *)p->subsys_data;
    if (!h)
        return 0;

    if (h->ctrlvc) {
        /* Rewrite exception frame PC to _CTRLVC handler */
        uint8_t *frame = (uint8_t *)regs;
        *(uint32_t *)(frame + 62) = h->ctrlvc;
        return 1;  /* handled */
    }

    sys_exit(-1);
    return 1;  /* handled (exited) */
}

const subsys_ops_t human68k_subsys_ops = {
    .on_crash  = h68k_on_crash,
    .on_signal = h68k_on_signal,
    .on_init   = h68k_on_init,
};

/* ── IOCS dispatch (TRAP #15) ────────────────────────────────────────── */

int human68k_iocs_dispatch(uint32_t *regs)
{
    uint8_t func = (uint8_t)regs[0];

    switch (func) {
    /* TODO: implement IOCS calls as needed */

    default:
        klogf("[human68k] unimplemented IOCS call $%x\n", (uint32_t)func);
        regs[0] = (uint32_t)(-1);  /* error return */
        return 2;
    }
}
