/*
 * human68k_bridge.c — Human68k DOS call bridge
 *
 * Translates F-line DOS calls into PPAP operations.
 * Phase 1: _EXIT, _EXIT2, _SETBLOCK, _MALLOC, _MFREE.
 * Phase 2: _GETCHAR, _PUTCHAR, _COMINP, _COMOUT, _PRINT, _GETS.
 * Phase 2b: _FGETC, _FGETS, _FPUTC, _FPUTS.
 * Phase 3: _CREATE, _OPEN, _CLOSE, _READ, _WRITE, _DELETE, _SEEK.
 */

#include "human68k_bridge.h"
#include "kernel/proc/proc.h"
#include "kernel/mm/page.h"
#include "kernel/exec/exec.h"
#include "kernel/syscall/syscall.h"
#include "kernel/klog.h"
#include "kernel/errno.h"
#include "common/fcntl.h"
#include <stddef.h>
#include <string.h>

/* Debug tracing — enable with -DH68K_DEBUG in CMake */
#ifdef H68K_DEBUG
#define H68K_TRACE(fmt, ...) klogf("[h68k] " fmt "\n", ##__VA_ARGS__)
#else
#define H68K_TRACE(fmt, ...) ((void)0)
#endif

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

/* ── Path translation: Human68k → PPAP ─────────────────────────────── *
 *
 * Rules:
 *   A:\DIR\FILE.X  →  /a/DIR/FILE.X   (drive letter → /x/ prefix)
 *   \DIR\FILE      →  /DIR/FILE        (absolute, current drive — treat as /)
 *   DIR\FILE       →  DIR/FILE         (relative to cwd)
 *   backslash      →  forward slash
 *
 * Returns length of translated path, or -1 on overflow.
 */
static int h68k_translate_path(const char *src, char *dst, int dstsize)
{
    int si = 0, di = 0;

    /* Drive letter: "X:" */
    if (src[0] && src[1] == ':') {
        char drive = src[0];
        if (drive >= 'A' && drive <= 'Z')
            drive += ('a' - 'A');
        if (di + 3 > dstsize) return -1;
        dst[di++] = '/';
        dst[di++] = drive;
        dst[di++] = '/';
        si = 2;
        /* Skip leading backslash after drive letter */
        if (src[si] == '\\' || src[si] == '/')
            si++;
    } else if (src[0] == '\\' || src[0] == '/') {
        /* Absolute path without drive letter */
        if (di + 1 > dstsize) return -1;
        dst[di++] = '/';
        si = 1;
    }
    /* else: relative path — no prefix */

    /* Copy rest, converting backslash to forward slash */
    while (src[si]) {
        if (di + 1 >= dstsize) return -1;
        dst[di++] = (src[si] == '\\') ? '/' : src[si];
        si++;
    }
    dst[di] = '\0';
    return di;
}

/* ── Error code translation: PPAP errno → Human68k ─────────────────── */

static int32_t h68k_errno(long ppap_err)
{
    if (ppap_err >= 0)
        return (int32_t)ppap_err;

    switch ((int)(-ppap_err)) {
    case ENOENT:     return -2;   /* File not found */
    case ENOTDIR:    return -3;   /* Directory not found */
    case EMFILE:     return -4;   /* Too many open files */
    case EISDIR:     return -5;   /* Is a directory */
    case EBADF:      return -6;   /* Invalid handle */
    case ENOMEM:     return -8;   /* Out of memory */
    case EACCES:     return -13;  /* Access denied */
    case EROFS:      return -13;  /* Read-only → access denied */
    case EINVAL:     return -22;  /* Invalid data */
    case ENOSPC:     return -23;  /* Disk full */
    case EEXIST:     return -80;  /* File exists */
    case ENOTEMPTY:  return -21;  /* Directory not empty */
    case ENOSYS:     return -1;   /* Invalid function */
    default:         return -1;   /* Generic error */
    }
}

/* ── _EXIT ($FF00) / _EXIT2 ($FF4C) ──────────────────────────────────── */

static int dos_exit(uint32_t usp)
{
    uint16_t code = ustack_u16(usp, 0);
    H68K_TRACE("_EXIT(%x)", (uint32_t)(int16_t)code);
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
    H68K_TRACE("_PUTCHAR(%x)", (uint32_t)ch);
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
    H68K_TRACE("_GETCHAR => %x", (uint32_t)ch);
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
    H68K_TRACE("_COMINP => %x", (uint32_t)ch);
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
    H68K_TRACE("_PRINT(%x)", str_addr);
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
    H68K_TRACE("_GETS(%x)", buf_addr);
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
 * Resizes the process memory block.  block_addr is the user-visible
 * address (base + 0x10, past the 16-byte MMB header).  new_size is
 * the desired size starting from block_addr.
 *
 * Shrinking frees pages back to the allocator; growing is not yet
 * supported.  Returns d0 = 0 on success, $81xxxxxx on error.
 */
#define MMB_HEADER_SIZE  0x10

static int dos_setblock(uint32_t *regs, uint32_t usp)
{
    uint32_t block_addr = ustack_u32(usp, 0);
    uint32_t new_size   = ustack_u32(usp, 4);
    H68K_TRACE("_SETBLOCK(%x, %x)", block_addr, new_size);
    pcb_t *p = current;

    /* block_addr points past the 16-byte MMB header.  Derive the raw
     * page base and verify it matches our allocation. */
    uint32_t base = (uint32_t)(uintptr_t)p->user_pages[0];
    if (block_addr != base + MMB_HEADER_SIZE) {
        H68K_TRACE("_SETBLOCK: bad block addr %x (expected %x)",
                   block_addr, base + MMB_HEADER_SIZE);
        regs[0] = (uint32_t)(-7);  /* Human68k: invalid memory block */
        advance_pc(regs);
        return 2;
    }

    /* Current allocation */
    uint32_t cur_pages = 0;
    while (cur_pages < USER_PAGES_MAX && p->user_pages[cur_pages])
        cur_pages++;

    uint32_t cur_size = cur_pages * PAGE_SIZE;
    /* Total size including MMB header */
    uint32_t total_new = MMB_HEADER_SIZE + new_size;

    if (total_new > cur_size) {
        /* Growing not supported — return Human68k error format:
         * high byte $81 = insufficient memory, low 24 bits = max available */
        uint32_t avail = (cur_size - MMB_HEADER_SIZE) & 0x00FFFFFFu;
        H68K_TRACE("_SETBLOCK: grow %x > cur %x, avail=%x",
                   total_new, cur_size, avail);
        regs[0] = 0x81000000u | avail;
        advance_pc(regs);
        return 2;
    }

    /* Shrink: free pages from the tail */
    uint32_t new_pages = (total_new + PAGE_SIZE - 1) / PAGE_SIZE;
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
 * Otherwise, allocates a contiguous block and returns pointer to
 * usable area (past the 16-byte MMB header).
 *
 * Returns: d0 = pointer to allocated block (past MMB header)
 *          d0 = $81xxxxxx on error (low 24 bits = max available)
 */
static int dos_malloc(uint32_t *regs, uint32_t usp)
{
    uint32_t size = ustack_u32(usp, 0);
    H68K_TRACE("_MALLOC(%x)", size);

    extern uint32_t page_max_contiguous(void);

    if (size >= 0x01000000u) {
        /* Query: size >= 16MB (24-bit address space) or $FFFFFFFF.
         * Return largest contiguous block size (minus MMB header). */
        uint32_t contig_bytes = page_max_contiguous() * PAGE_SIZE;
        uint32_t avail = (contig_bytes > MMB_HEADER_SIZE)
                       ? contig_bytes - MMB_HEADER_SIZE : 0;
        H68K_TRACE("_MALLOC: query => %x", avail);
        regs[0] = avail;
        advance_pc(regs);
        return 2;
    }

    /* Actual allocation — try exact requested size only. */
    uint32_t total = MMB_HEADER_SIZE + size;
    uint32_t n_pages = (total + PAGE_SIZE - 1) / PAGE_SIZE;

    uint8_t *base = alloc_contiguous(n_pages);
    if (!base) {
        /* Report largest contiguous run so caller can retry. */
        uint32_t contig_bytes = page_max_contiguous() * PAGE_SIZE;
        uint32_t avail = (contig_bytes > MMB_HEADER_SIZE)
                       ? (contig_bytes - MMB_HEADER_SIZE) & 0x00FFFFFFu : 0;
        H68K_TRACE("_MALLOC: alloc %x pages failed, max_contig=%x", n_pages, avail);
        regs[0] = 0x81000000u | avail;
        advance_pc(regs);
        return 2;
    }

    /* Track this allocation in the per-process table */
    h68k_proc_t *h = (h68k_proc_t *)current->subsys_data;
    int slot = -1;
    if (h) {
        for (int i = 0; i < H68K_MALLOC_MAX; i++) {
            if (!h->mallocs[i].base) {
                slot = i;
                break;
            }
        }
    }
    if (slot < 0) {
        /* No free tracking slot — free the pages and fail */
        for (uint32_t i = 0; i < n_pages; i++)
            page_free(base + i * PAGE_SIZE);
        H68K_TRACE("_MALLOC: no tracking slot");
        regs[0] = 0x82000000u;  /* Human68k: too many blocks */
        advance_pc(regs);
        return 2;
    }
    h->mallocs[slot].base = base;
    h->mallocs[slot].n_pages = n_pages;

    /* Write MMB header */
    uint32_t base_u = (uint32_t)(uintptr_t)base;
    uint32_t end_u  = base_u + n_pages * PAGE_SIZE;
    mem_write32(base_u + 0x00, 0);                   /* prev */
    mem_write32(base_u + 0x04, base_u);               /* owner = self */
    mem_write32(base_u + 0x08, end_u);                /* end+1 */
    mem_write32(base_u + 0x0C, 0);                    /* next */

    /* Return pointer past MMB header */
    uint32_t user_ptr = base_u + MMB_HEADER_SIZE;
    H68K_TRACE("_MALLOC: allocated %x pages at %x, user=%x",
               n_pages, base_u, user_ptr);
    regs[0] = user_ptr;
    advance_pc(regs);
    return 2;
}

/* ── _MFREE ($FF49) ──────────────────────────────────────────────────── *
 *
 * Stack: long block_addr
 * Frees a block previously allocated by _MALLOC.
 * block_addr is the user pointer (base + MMB_HEADER_SIZE).
 */
static int dos_mfree(uint32_t *regs, uint32_t usp)
{
    uint32_t block_addr = ustack_u32(usp, 0);
    H68K_TRACE("_MFREE(%x)", block_addr);

    h68k_proc_t *h = (h68k_proc_t *)current->subsys_data;
    if (h) {
        for (int i = 0; i < H68K_MALLOC_MAX; i++) {
            if (!h->mallocs[i].base)
                continue;
            uint32_t user_ptr = (uint32_t)(uintptr_t)h->mallocs[i].base
                              + MMB_HEADER_SIZE;
            if (user_ptr == block_addr) {
                H68K_TRACE("_MFREE: freeing slot %u (%x pages)",
                           (uint32_t)i, h->mallocs[i].n_pages);
                for (uint32_t j = 0; j < h->mallocs[i].n_pages; j++)
                    page_free(h->mallocs[i].base + j * PAGE_SIZE);
                h->mallocs[i].base = NULL;
                h->mallocs[i].n_pages = 0;
                regs[0] = 0;
                advance_pc(regs);
                return 2;
            }
        }
    }

    H68K_TRACE("_MFREE: block %x not found", block_addr);
    regs[0] = (uint32_t)(-7);  /* invalid memory block */
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
    H68K_TRACE("_FGETC(%u)", (uint32_t)fd);
    uint8_t ch = 0;
    long r = sys_read(fd, (char *)&ch, 1);
    regs[0] = (r > 0) ? (uint32_t)ch : (uint32_t)(-1);
    advance_pc(regs);
    return 2;
}

/* ── _FGETS ($FF1C) ─────────────────────────────────────────────────── *
 *
 * Stack: long buf_ptr, word fileno
 * Line-buffered read from file handle into linebuf structure:
 *   byte 0: max (max chars to read)
 *   byte 1: len (filled with actual count)
 *   byte 2+: buffer data
 * Reads until newline or max chars.  Returns count in d0.
 */
static int dos_fgets(uint32_t *regs, uint32_t usp)
{
    uint32_t buf_addr = ustack_u32(usp, 0);
    int fd = (int)(int16_t)ustack_u16(usp, 4);
    H68K_TRACE("_FGETS(%u, %x)", (uint32_t)fd, buf_addr);
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
 * Stack: word code, word fileno
 * Writes one byte to the specified file handle.  Returns byte in d0.
 */
static int dos_fputc(uint32_t *regs, uint32_t usp)
{
    uint8_t ch = (uint8_t)ustack_u16(usp, 0);
    int fd = (int)(int16_t)ustack_u16(usp, 2);
    H68K_TRACE("_FPUTC(%u, %x)", (uint32_t)fd, (uint32_t)ch);
    sys_write(fd, (const char *)&ch, 1);
    regs[0] = (uint32_t)ch;
    advance_pc(regs);
    return 2;
}

/* ── _FPUTS ($FF1E) ─────────────────────────────────────────────────── *
 *
 * Stack: long str_ptr, word fileno
 * Writes a NUL-terminated string to the file handle.
 * The NUL terminator is not written.  Returns 0 in d0.
 */
static int dos_fputs(uint32_t *regs, uint32_t usp)
{
    uint32_t str_addr = ustack_u32(usp, 0);
    int fd = (int)(int16_t)ustack_u16(usp, 4);
    H68K_TRACE("_FPUTS(%u, %x)", (uint32_t)fd, str_addr);
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

/* ── _CREATE ($FF3C) ─────────────────────────────────────────────────── *
 *
 * Stack: long path_ptr, word attr
 * Creates a new file (or truncates existing).  Returns handle in d0.
 */
static int dos_create(uint32_t *regs, uint32_t usp)
{
    uint32_t path_addr = ustack_u32(usp, 0);
    /* uint16_t attr = ustack_u16(usp, 4); — ignored (PPAP uses mode) */
    const char *src = (const char *)(uintptr_t)path_addr;
    char path[128];
    if (h68k_translate_path(src, path, sizeof(path)) < 0) {
        regs[0] = (uint32_t)h68k_errno(-ENAMETOOLONG);
        advance_pc(regs);
        return 2;
    }
    H68K_TRACE("_CREATE(%s)", path);
    long r = sys_open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    regs[0] = (uint32_t)h68k_errno(r);
    advance_pc(regs);
    return 2;
}

/* ── _OPEN ($FF3D) ──────────────────────────────────────────────────── *
 *
 * Stack: long path_ptr, word mode
 * Opens a file.  mode: 0=read, 1=write, 2=read/write.
 * Returns handle in d0.
 */
static int dos_open(uint32_t *regs, uint32_t usp)
{
    uint32_t path_addr = ustack_u32(usp, 0);
    uint16_t mode = ustack_u16(usp, 4);
    const char *src = (const char *)(uintptr_t)path_addr;
    char path[128];
    if (h68k_translate_path(src, path, sizeof(path)) < 0) {
        regs[0] = (uint32_t)h68k_errno(-ENAMETOOLONG);
        advance_pc(regs);
        return 2;
    }
    H68K_TRACE("_OPEN(%s, %x)", path, (uint32_t)mode);
    int flags;
    switch (mode & 0x0F) {
    case 0:  flags = O_RDONLY; break;
    case 1:  flags = O_WRONLY; break;
    default: flags = O_RDWR;   break;
    }
    long r = sys_open(path, flags, 0644);
    regs[0] = (uint32_t)h68k_errno(r);
    advance_pc(regs);
    return 2;
}

/* ── _CLOSE ($FF3E) ─────────────────────────────────────────────────── *
 *
 * Stack: word fileno
 * Closes the file handle.  Returns 0 on success.
 */
static int dos_close(uint32_t *regs, uint32_t usp)
{
    int fd = (int)(int16_t)ustack_u16(usp, 0);
    H68K_TRACE("_CLOSE(%u)", (uint32_t)fd);
    long r = sys_close(fd);
    regs[0] = (uint32_t)h68k_errno(r);
    advance_pc(regs);
    return 2;
}

/* ── _READ ($FF3F) ──────────────────────────────────────────────────── *
 *
 * Stack: word fileno, long buf_ptr, long len
 * Reads up to len bytes.  Returns bytes read in d0.
 */
static int dos_read(uint32_t *regs, uint32_t usp)
{
    int fd = (int)(int16_t)ustack_u16(usp, 0);
    uint32_t buf_addr = ustack_u32(usp, 2);
    uint32_t len = ustack_u32(usp, 6);
    H68K_TRACE("_READ(%u, %x, %x)", (uint32_t)fd, buf_addr, len);
    long r = sys_read(fd, (char *)(uintptr_t)buf_addr, (size_t)len);
    regs[0] = (uint32_t)h68k_errno(r);
    advance_pc(regs);
    return 2;
}

/* ── _WRITE ($FF40) ─────────────────────────────────────────────────── *
 *
 * Stack: word fileno, long buf_ptr, long len
 * Writes len bytes.  Returns bytes written in d0.
 */
static int dos_write(uint32_t *regs, uint32_t usp)
{
    int fd = (int)(int16_t)ustack_u16(usp, 0);
    uint32_t buf_addr = ustack_u32(usp, 2);
    uint32_t len = ustack_u32(usp, 6);
    H68K_TRACE("_WRITE(%u, %x, %x)", (uint32_t)fd, buf_addr, len);
    long r = sys_write(fd, (const char *)(uintptr_t)buf_addr, (size_t)len);
    regs[0] = (uint32_t)h68k_errno(r);
    advance_pc(regs);
    return 2;
}

/* ── _DELETE ($FF41) ────────────────────────────────────────────────── *
 *
 * Stack: long path_ptr
 * Deletes a file.  Returns 0 on success.
 */
static int dos_delete(uint32_t *regs, uint32_t usp)
{
    uint32_t path_addr = ustack_u32(usp, 0);
    const char *src = (const char *)(uintptr_t)path_addr;
    char path[128];
    if (h68k_translate_path(src, path, sizeof(path)) < 0) {
        regs[0] = (uint32_t)h68k_errno(-ENAMETOOLONG);
        advance_pc(regs);
        return 2;
    }
    H68K_TRACE("_DELETE(%s)", path);
    long r = sys_unlink(path);
    regs[0] = (uint32_t)h68k_errno(r);
    advance_pc(regs);
    return 2;
}

/* ── _SEEK ($FF42) ──────────────────────────────────────────────────── *
 *
 * Stack: word fileno, long offset, word whence
 * whence: 0=SET, 1=CUR, 2=END (same as POSIX)
 * Returns new position in d0.
 */
static int dos_seek(uint32_t *regs, uint32_t usp)
{
    int fd = (int)(int16_t)ustack_u16(usp, 0);
    int32_t offset = (int32_t)ustack_u32(usp, 2);
    uint16_t whence = ustack_u16(usp, 6);
    H68K_TRACE("_SEEK(%u, %x, %u)", (uint32_t)fd, (uint32_t)offset, (uint32_t)whence);
    long r = sys_lseek(fd, offset, whence);
    regs[0] = (uint32_t)h68k_errno(r);
    advance_pc(regs);
    return 2;
}

/* ── _CHDIR ($FF3B) ─────────────────────────────────────────────────── *
 *
 * Stack: long path_ptr
 * Changes the current working directory.  Returns 0 on success.
 */
static int dos_chdir(uint32_t *regs, uint32_t usp)
{
    uint32_t path_addr = ustack_u32(usp, 0);
    const char *src = (const char *)(uintptr_t)path_addr;
    char path[128];
    if (h68k_translate_path(src, path, sizeof(path)) < 0) {
        regs[0] = (uint32_t)h68k_errno(-ENAMETOOLONG);
        advance_pc(regs);
        return 2;
    }
    H68K_TRACE("_CHDIR(%s)", path);
    long r = sys_chdir(path);
    regs[0] = (uint32_t)h68k_errno(r);
    advance_pc(regs);
    return 2;
}

/* ── _CURDIR ($FF47) ────────────────────────────────────────────────── *
 *
 * Stack: word drive, long buf_ptr
 * Returns the current directory for the specified drive (0=default).
 * The buffer receives the path without drive letter or leading backslash
 * (e.g. "DIR\SUBDIR"), NUL-terminated, max 64 bytes.
 */
static int dos_curdir(uint32_t *regs, uint32_t usp)
{
    uint16_t drive = ustack_u16(usp, 0);
    uint32_t buf_addr = ustack_u32(usp, 2);
    H68K_TRACE("_CURDIR(%u, %x)", (uint32_t)drive, buf_addr);
    (void)drive;  /* single-drive system — ignore drive number */

    char cwd[128];
    long r = sys_getcwd(cwd, sizeof(cwd));
    if (r < 0) {
        regs[0] = (uint32_t)h68k_errno(r);
        advance_pc(regs);
        return 2;
    }

    /* Strip leading slash and convert / to \ for Human68k convention.
     * Human68k expects the path without drive letter or leading backslash. */
    char *dst = (char *)(uintptr_t)buf_addr;
    const char *p = cwd;
    if (*p == '/') p++;  /* skip leading / */
    int i = 0;
    while (*p && i < 63) {
        dst[i++] = (*p == '/') ? '\\' : *p;
        p++;
    }
    dst[i] = '\0';

    regs[0] = 0;
    advance_pc(regs);
    return 2;
}

/* ── _CURDRV ($FF19) ────────────────────────────────────────────────── *
 *
 * No stack arguments.
 * Returns the current drive number in d0 (0=A:, 1=B:, ...).
 */
static int dos_curdrv(uint32_t *regs)
{
    H68K_TRACE("_CURDRV");
    regs[0] = 0;  /* always drive A: */
    advance_pc(regs);
    return 2;
}

/* ── _DUP ($FF45) ──────────────────────────────────────────────────── *
 *
 * Stack: word fileno
 * Duplicates a file handle.  Returns new handle in d0.
 */
static int dos_dup(uint32_t *regs, uint32_t usp)
{
    int fd = (int)(int16_t)ustack_u16(usp, 0);
    H68K_TRACE("_DUP(%u)", (uint32_t)fd);
    /* Stub: return the same fd (no real dup in PPAP yet) */
    regs[0] = (uint32_t)fd;
    advance_pc(regs);
    return 2;
}

/* ── _DUP2 ($FF46) ─────────────────────────────────────────────────── *
 *
 * Stack: word old_fileno, word new_fileno
 * Duplicates old handle to new handle.  Returns new handle in d0.
 */
static int dos_dup2(uint32_t *regs, uint32_t usp)
{
    int old_fd = (int)(int16_t)ustack_u16(usp, 0);
    int new_fd = (int)(int16_t)ustack_u16(usp, 2);
    H68K_TRACE("_DUP2(%u, %u)", (uint32_t)old_fd, (uint32_t)new_fd);
    /* Stub: return new_fd (no real dup2 in PPAP yet) */
    (void)old_fd;
    regs[0] = (uint32_t)new_fd;
    advance_pc(regs);
    return 2;
}

/* ── _RENAME ($FF56) ────────────────────────────────────────────────── *
 *
 * Stack: long old_path_ptr, long new_path_ptr
 * Renames (moves) a file or directory.  Returns 0 on success.
 *
 * Note: PPAP doesn't have sys_rename() yet.  We implement it directly
 * via VFS lookup_parent + FS-level operations.  For now, return -1
 * (unsupported) until sys_rename is added.
 */
static int dos_rename(uint32_t *regs, uint32_t usp)
{
    uint32_t old_addr = ustack_u32(usp, 0);
    uint32_t new_addr = ustack_u32(usp, 4);
    const char *old_src = (const char *)(uintptr_t)old_addr;
    const char *new_src = (const char *)(uintptr_t)new_addr;
    char old_path[128], new_path[128];
    h68k_translate_path(old_src, old_path, sizeof(old_path));
    h68k_translate_path(new_src, new_path, sizeof(new_path));
    H68K_TRACE("_RENAME(%s, %s)", old_path, new_path);
    /* TODO: implement when sys_rename is available */
    regs[0] = (uint32_t)h68k_errno(-ENOSYS);
    advance_pc(regs);
    return 2;
}

/* ── _VERNUM ($FF30) ───────────────────────────────────────────────── *
 *
 * No stack arguments.
 * Returns Human68k version in d0: high word = "HU", low word = version.
 * Version 3.02 = 0x4855_0302 ("HU" + 0x0302).
 */
static int dos_vernum(uint32_t *regs)
{
    H68K_TRACE("_VERNUM");
    regs[0] = 0x36380302u;  /* "68" + version 3.02 */
    advance_pc(regs);
    return 2;
}

/* ── _BREAKCK ($FF33) ──────────────────────────────────────────────── *
 *
 * Stack: word mode  (0=get, 1=set, 2=get-and-set)
 * Returns/sets break-check mode.  Always return 1 (break check on).
 */
static int dos_breakck(uint32_t *regs, uint32_t usp)
{
    uint16_t mode = ustack_u16(usp, 0);
    H68K_TRACE("_BREAKCK(%u)", (uint32_t)mode);
    (void)mode;
    regs[0] = 1;  /* break check on */
    advance_pc(regs);
    return 2;
}

/* ── _INTVCG ($FF35) ───────────────────────────────────────────────── *
 *
 * Stack: word vecno
 * Get interrupt vector.  Return 0 (no vector table).
 */
static int dos_intvcg(uint32_t *regs, uint32_t usp)
{
    uint16_t vecno = ustack_u16(usp, 0);
    H68K_TRACE("_INTVCG(%x)", (uint32_t)vecno);
    (void)vecno;
    regs[0] = 0;
    advance_pc(regs);
    return 2;
}

/* ── _GETPDB ($FF51) ───────────────────────────────────────────────── *
 *
 * No stack arguments.
 * Returns the address of the current Process Descriptor Block (PMB base).
 */
static int dos_getpdb(uint32_t *regs)
{
    H68K_TRACE("_GETPDB");
    pcb_t *p = current;
    /* PMB is at user_pages[0] */
    regs[0] = (uint32_t)(uintptr_t)p->user_pages[0];
    advance_pc(regs);
    return 2;
}

/* ── Dispatch ─────────────────────────────────────────────────────────── */

int human68k_dos_dispatch(uint32_t *regs, uint32_t usp, uint16_t opcode)
{
    uint8_t func = opcode & 0xFF;
    int ret;

    /* DOS call alias: $FF80–$FFAF maps to $FF50–$FF7F (subtract $30) */
    if (func >= 0x80 && func <= 0xAF)
        func -= 0x30;

    switch (func) {
    case 0x00:  /* _EXIT */
    case 0x4C:  /* _EXIT2 */
        return dos_exit(usp);

    case 0x01:  /* _GETCHAR — read with echo */
        ret = dos_getchar(regs);
        break;

    case 0x02:  /* _PUTCHAR */
    case 0x04:  /* _COMOUT */
        ret = dos_putchar(regs, usp);
        break;

    case 0x03:  /* _COMINP — raw read, no echo */
        ret = dos_cominp(regs);
        break;

    case 0x09:  /* _PRINT */
        ret = dos_print(regs, usp);
        break;

    case 0x0A:  /* _GETS */
        ret = dos_gets(regs, usp);
        break;

    case 0x19:  /* _CURDRV */
        ret = dos_curdrv(regs);
        break;

    case 0x1B:  /* _FGETC */
        ret = dos_fgetc(regs, usp);
        break;

    case 0x1C:  /* _FGETS */
        ret = dos_fgets(regs, usp);
        break;

    case 0x1D:  /* _FPUTC */
        ret = dos_fputc(regs, usp);
        break;

    case 0x1E:  /* _FPUTS */
        ret = dos_fputs(regs, usp);
        break;

    case 0x30:  /* _VERNUM */
        ret = dos_vernum(regs);
        break;

    case 0x33:  /* _BREAKCK */
        ret = dos_breakck(regs, usp);
        break;

    case 0x35:  /* _INTVCG */
        ret = dos_intvcg(regs, usp);
        break;

    case 0x3B:  /* _CHDIR */
        ret = dos_chdir(regs, usp);
        break;

    case 0x3C:  /* _CREATE */
        ret = dos_create(regs, usp);
        break;

    case 0x3D:  /* _OPEN */
        ret = dos_open(regs, usp);
        break;

    case 0x3E:  /* _CLOSE */
        ret = dos_close(regs, usp);
        break;

    case 0x3F:  /* _READ */
        ret = dos_read(regs, usp);
        break;

    case 0x40:  /* _WRITE */
        ret = dos_write(regs, usp);
        break;

    case 0x41:  /* _DELETE */
        ret = dos_delete(regs, usp);
        break;

    case 0x42:  /* _SEEK */
        ret = dos_seek(regs, usp);
        break;

    case 0x45:  /* _DUP */
        ret = dos_dup(regs, usp);
        break;

    case 0x46:  /* _DUP2 */
        ret = dos_dup2(regs, usp);
        break;

    case 0x47:  /* _CURDIR */
        ret = dos_curdir(regs, usp);
        break;

    case 0x4A:  /* _SETBLOCK */
        ret = dos_setblock(regs, usp);
        break;

    case 0x48:  /* _MALLOC */
        ret = dos_malloc(regs, usp);
        break;

    case 0x49:  /* _MFREE */
        ret = dos_mfree(regs, usp);
        break;

    case 0x51:  /* _GETPDB */
        ret = dos_getpdb(regs);
        break;

    case 0x56:  /* _RENAME */
        ret = dos_rename(regs, usp);
        break;

    default:
        H68K_TRACE("DOS notimpl: opcode=%x",
              (uint32_t)(0xFF00u | func));
        regs[0] = (uint32_t)(-(int32_t)ENOSYS);
        advance_pc(regs);
        ret = 2;
        break;
    }

    H68K_TRACE("  => d0=%x", regs[0]);
    return ret;
}

/* ── Subsystem ops (layering hooks called by the kernel) ──────────── */

/* Static pool of per-process Human68k state.
 * Indexed by pid (max PROC_MAX).  Zero-initialized = default vectors. */
static h68k_proc_t h68k_pool[PROC_MAX];

/* on_init — called when a Human68k binary is exec'd */
static void h68k_on_init(struct pcb *p)
{
    H68K_TRACE("on_init pid=%u", (uint32_t)p->pid);
    h68k_proc_t *h = &h68k_pool[p->pid];
    h->exitvc = 0;
    h->ctrlvc = 0;
    h->errjvc = 0;
    for (int i = 0; i < H68K_MALLOC_MAX; i++) {
        h->mallocs[i].base = NULL;
        h->mallocs[i].n_pages = 0;
    }
    p->subsys_data = h;
}

/* on_crash — handle faults for Human68k processes (_ERRJVC) */
static int h68k_on_crash(struct pcb *p, uint32_t *regs, uint16_t *exc,
                          int is_group0)
{
    H68K_TRACE("on_crash pid=%u group0=%u", (uint32_t)p->pid, (uint32_t)is_group0);
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
    H68K_TRACE("on_signal pid=%u sig=%u", (uint32_t)p->pid, (uint32_t)sig);
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
        H68K_TRACE("IOCS notimpl: d0=%x", (uint32_t)func);
        regs[0] = (uint32_t)(-1);  /* error return */
        return 2;
    }
}
