/*
 * human68k_bridge.c — Human68k DOS call bridge
 *
 * Translates F-line DOS calls into PPAP operations.
 * Phase 1: _EXIT, _EXIT2, _SETBLOCK, _MALLOC, _MFREE.
 * Phase 2: _GETCHAR, _PUTCHAR, _COMINP, _COMOUT, _PRINT, _GETS.
 * Phase 2b: _FGETC, _FGETS, _FPUTC, _FPUTS.
 * Phase 3: _CREATE, _OPEN, _CLOSE, _READ, _WRITE, _DELETE, _SEEK.
 */

#include "kernel/core/subsys/human68k_bridge.h"

#include <stddef.h>
#include <string.h>

#include "common/dirent.h"
#include "common/errno.h"
#include "common/fcntl.h"
#include "common/stat.h"
#include "kernel/common/mod/mod_vfs.h"
#include "kernel/core/exec/exec.h"
#include "kernel/core/mm/mem_region.h"
#include "kernel/core/mm/page.h"
#include "kernel/core/proc/proc.h"
#include "kernel/core/proc/sched.h"
#include "kernel/core/subsys/h68k_util.h"
#include "kernel/core/syscall/syscall.h"

#ifdef PPAP_DEBUG_LOG
#define H68K_TRACE(fmt, ...) mod_vfs.klogf("[h68k] " fmt "\n", ##__VA_ARGS__)
#else
#define H68K_TRACE(fmt, ...) ((void)0)
#endif

static long h68k_fd_desc(long fd) {
  if (fd < 0 || (uint32_t)fd >= FD_MAX) return -(long)EBADF;
  if (current->fd_map[(uint32_t)fd] == FD_DESC_NONE) return -(long)EBADF;
  return current->fd_map[(uint32_t)fd];
}

static int h68k_page_ref(const void *buf, page_id_t *page, uint16_t *off) {
  return (mem_region_ptr_ref(buf, page, off) < 0) ? -EFAULT : 0;
}

static long h68k_fd_read(long fd, void *buf, size_t n) {
  long desc = h68k_fd_desc(fd);
  page_id_t page;
  uint16_t off;
  int rc;

  if (desc < 0) return desc;
  rc = h68k_page_ref(buf, &page, &off);
  if (rc < 0) return rc;
  return mod_vfs.fd_read((int)desc, page, off, n);
}

static long h68k_fd_write(long fd, const void *buf, size_t n) {
  long desc = h68k_fd_desc(fd);
  page_id_t page;
  uint16_t off;
  int rc;

  if (desc < 0) return desc;
  rc = h68k_page_ref(buf, &page, &off);
  if (rc < 0) return rc;
  return mod_vfs.fd_write((int)desc, page, off, n);
}

/* Read big-endian values from user stack (native on m68k) */
static inline uint16_t ustack_u16(uint32_t usp, int offset) {
  return *(volatile uint16_t *)(uintptr_t)(usp + offset);
}

static inline uint32_t ustack_u32(uint32_t usp, int offset) {
  return *(volatile uint32_t *)(uintptr_t)(usp + offset);
}

/* Write big-endian 32-bit to memory (native on m68k) */
static inline void mem_write32(uint32_t addr, uint32_t val) {
  *(volatile uint32_t *)(uintptr_t)addr = val;
}

/* Advance the faulting PC past the 2-byte F-line instruction.
 * PC is at byte offset 62 in the register frame (after 60 bytes of regs
 * and 2 bytes of SR). */
static inline void advance_pc(uint32_t *regs) {
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
 * Implementation in h68k_util.c.
 */

/* ── IOCS-level primitives ─────────────────────────────────────────────
 *
 * These are the low-level I/O building blocks.  IOCS dispatch handlers
 * are thin wrappers around them, and DOS call handlers delegate here
 * so that IOCS is always the base layer.
 */

/* Write one character to stdout */
static void h68k_putc(uint8_t ch) { h68k_fd_write(1, &ch, 1); }

/* Write a NUL-terminated string to stdout; return length written */
static int h68k_print(const char *str) {
  int len = 0;
  while (str[len]) len++;
  for (int i = 0; i < len; i++) h68k_putc((uint8_t)str[i]);
  return len;
}

/* Read one character from stdin (blocking) */
static uint8_t h68k_keyinp(void) {
  uint8_t ch = 0;
  h68k_fd_read(0, &ch, 1);
  return ch;
}

/* ── _EXIT ($FF00) / _EXIT2 ($FF4C) ──────────────────────────────────── */

static int dos_exit(uint32_t usp) {
  uint16_t code = ustack_u16(usp, 0);
  H68K_TRACE("_EXIT(%x)", (uint32_t)(int16_t)code);
  sys_exit((int)(int16_t)code);
  return 1; /* unreachable, but sys_exit doesn't return */
}

/* ── _PUTCHAR ($FF02) / _COMOUT ($FF04) ──────────────────────────────── *
 *
 * Stack: word char_code
 * Writes one character to stdout.  Returns the character in d0.
 */
static int dos_putchar(uint32_t *regs, uint32_t usp) {
  uint8_t ch = (uint8_t)ustack_u16(usp, 0);
  H68K_TRACE("_PUTCHAR(%x)", (uint32_t)ch);
  h68k_putc(ch);
  regs[0] = (uint32_t)ch;
  advance_pc(regs);
  return 2;
}

/* ── _GETCHAR ($FF01) ────────────────────────────────────────────────── *
 *
 * No arguments on stack.
 * Reads one character from stdin with echo.  Returns char in d0.
 */
static int dos_getchar(uint32_t *regs) {
  uint8_t ch = h68k_keyinp();
  H68K_TRACE("_GETCHAR => %x", (uint32_t)ch);
  /* Echo the character back to stdout */
  h68k_putc(ch);
  regs[0] = (uint32_t)ch;
  advance_pc(regs);
  return 2;
}

/* ── _COMINP ($FF03) ─────────────────────────────────────────────────── *
 *
 * No arguments on stack.
 * Reads one character from stdin (raw, no echo).  Returns char in d0.
 */
static int dos_cominp(uint32_t *regs) {
  uint8_t ch = h68k_keyinp();
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
static int dos_print(uint32_t *regs, uint32_t usp) {
  uint32_t str_addr = ustack_u32(usp, 0);
  H68K_TRACE("_PRINT(%x)", str_addr);
  const char *str = (const char *)(uintptr_t)str_addr;
  h68k_print(str);
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
static int dos_gets(uint32_t *regs, uint32_t usp) {
  uint32_t buf_addr = ustack_u32(usp, 0);
  H68K_TRACE("_GETS(%x)", buf_addr);
  uint8_t *buf = (uint8_t *)(uintptr_t)buf_addr;
  uint8_t max = buf[0];
  uint8_t count = 0;

  while (count < max) {
    uint8_t ch = h68k_keyinp();
    if (ch == 0) break;
    /* Echo */
    h68k_putc(ch);
    if (ch == 0x0D || ch == 0x0A) break;
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
#define MMB_HEADER_SIZE 0x10

static int dos_setblock(uint32_t *regs, uint32_t usp) {
  uint32_t block_addr = ustack_u32(usp, 0);
  uint32_t new_size = ustack_u32(usp, 4);
  H68K_TRACE("_SETBLOCK(%x, %x)", block_addr, new_size);
  pcb_t *p = current;

  /* block_addr points past the 16-byte MMB header.  Derive the raw
   * page base and verify it matches our allocation. */
  page_id_t base_id = proc_page_backed_base(p);
  uint32_t base =
      (base_id != PAGE_ID_INVALID) ? mem_region_page_linear(base_id) : 0;
  if (block_addr != base + MMB_HEADER_SIZE) {
    H68K_TRACE("_SETBLOCK: bad block addr %x (expected %x)", block_addr,
               base + MMB_HEADER_SIZE);
    regs[0] = (uint32_t)(-7); /* Human68k: invalid memory block */
    advance_pc(regs);
    return 2;
  }

  /* Current allocation */
  uint32_t cur_pages = proc_page_backed_count(p);

  uint32_t cur_size = cur_pages * PAGE_SIZE;
  /* Total size including MMB header */
  uint32_t total_new = MMB_HEADER_SIZE + new_size;

  if (total_new > cur_size) {
    /* Growing not supported — return Human68k error format:
     * high byte $81 = insufficient memory, low 24 bits = max available */
    uint32_t avail = (cur_size - MMB_HEADER_SIZE) & 0x00FFFFFFu;
    H68K_TRACE("_SETBLOCK: grow %x > cur %x, avail=%x", total_new, cur_size,
               avail);
    regs[0] = 0x81000000u | avail;
    advance_pc(regs);
    return 2;
  }

  /* Shrink: free pages from the tail */
  uint32_t new_pages = (total_new + PAGE_SIZE - 1) / PAGE_SIZE;
  if (new_pages < 1) new_pages = 1; /* keep at least the PMB page */

  for (uint32_t i = new_pages; i < cur_pages; i++) {
    proc_release_tracked_pages(p, i, i + 1);
  }

  /* Update MMB end pointer (offset 0x08 in the block) */
  uint32_t new_end = base + new_pages * PAGE_SIZE;
  mem_write32(base + 0x08, new_end);

  /* Update PMB stack pointer (offset 0x38) */
  mem_write32(base + 0x38, new_end);

  regs[0] = 0; /* success */
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
static int dos_malloc(uint32_t *regs, uint32_t usp) {
  uint32_t size = ustack_u32(usp, 0);
  proc_image_segment_t alloc_region;
  H68K_TRACE("_MALLOC(%x)", size);

  if (size >= 0x01000000u) {
    /* Query: size >= 16MB (24-bit address space) or $FFFFFFFF.
     * Return largest contiguous block size (minus MMB header). */
    uint32_t contig_bytes = mem_region_largest_free_bytes(PPAP_MEM_RAM_DATA);
    uint32_t avail =
        (contig_bytes > MMB_HEADER_SIZE) ? contig_bytes - MMB_HEADER_SIZE : 0;
    H68K_TRACE("_MALLOC: query => %x", avail);
    regs[0] = avail;
    advance_pc(regs);
    return 2;
  }

  /* Actual allocation — try exact requested size only. */
  uint32_t total = MMB_HEADER_SIZE + size;
  if (mem_region_alloc(&alloc_region, PPAP_MEM_RAM_DATA, total,
                       PROC_IMAGE_SEG_OWNED | PROC_IMAGE_SEG_WRITABLE) < 0) {
    /* Report largest contiguous run so caller can retry. */
    uint32_t contig_bytes = mem_region_largest_free_bytes(PPAP_MEM_RAM_DATA);
    uint32_t avail = (contig_bytes > MMB_HEADER_SIZE)
                         ? (contig_bytes - MMB_HEADER_SIZE) & 0x00FFFFFFu
                         : 0;
    H68K_TRACE("_MALLOC: alloc %x bytes failed, max_contig=%x", total, avail);
    regs[0] = 0x81000000u | avail;
    advance_pc(regs);
    return 2;
  }

  /* Track this allocation in the per-process table */
  h68k_proc_t *h = (h68k_proc_t *)current->subsys_data;
  int slot = -1;
  if (h) {
    for (int i = 0; i < H68K_MALLOC_MAX; i++) {
      if (!h->mallocs[i].region.base) {
        slot = i;
        break;
      }
    }
  }
  if (slot < 0) {
    /* No free tracking slot — free the pages and fail */
    mem_region_free(&alloc_region);
    H68K_TRACE("_MALLOC: no tracking slot");
    regs[0] = 0x82000000u; /* Human68k: too many blocks */
    advance_pc(regs);
    return 2;
  }
  h->mallocs[slot].region = alloc_region;

  /* Write MMB header */
  uint32_t base_u = (uint32_t)(uintptr_t)alloc_region.base;
  uint32_t end_u = base_u + alloc_region.size;
  mem_write32(base_u + 0x00, 0);      /* prev */
  mem_write32(base_u + 0x04, base_u); /* owner = self */
  mem_write32(base_u + 0x08, end_u);  /* end+1 */
  mem_write32(base_u + 0x0C, 0);      /* next */

  /* Return pointer past MMB header */
  uint32_t user_ptr = base_u + MMB_HEADER_SIZE;
  H68K_TRACE("_MALLOC: allocated %x bytes at %x, user=%x", alloc_region.size,
             base_u, user_ptr);
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
static int dos_mfree(uint32_t *regs, uint32_t usp) {
  uint32_t block_addr = ustack_u32(usp, 0);
  H68K_TRACE("_MFREE(%x)", block_addr);

  h68k_proc_t *h = (h68k_proc_t *)current->subsys_data;
  if (h) {
    for (int i = 0; i < H68K_MALLOC_MAX; i++) {
      if (!h->mallocs[i].region.base) continue;
      uint32_t user_ptr =
          (uint32_t)(uintptr_t)h->mallocs[i].region.base + MMB_HEADER_SIZE;
      if (user_ptr == block_addr) {
        H68K_TRACE("_MFREE: freeing slot %u (%x bytes)", (uint32_t)i,
                   h->mallocs[i].region.size);
        mem_region_free(&h->mallocs[i].region);
        h->mallocs[i].region = (proc_image_segment_t){0};
        regs[0] = 0;
        advance_pc(regs);
        return 2;
      }
    }
  }

  H68K_TRACE("_MFREE: block %x not found", block_addr);
  regs[0] = (uint32_t)(-7); /* invalid memory block */
  advance_pc(regs);
  return 2;
}

/* ── _FGETC ($FF1B) ─────────────────────────────────────────────────── *
 *
 * Stack: word fileno
 * Reads one byte from the specified file handle.  Returns byte in d0.
 */
static int dos_fgetc(uint32_t *regs, uint32_t usp) {
  int fd = (int)(int16_t)ustack_u16(usp, 0);
  H68K_TRACE("_FGETC(%u)", (uint32_t)fd);
  uint8_t ch = 0;
  long r = h68k_fd_read(fd, &ch, 1);
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
static int dos_fgets(uint32_t *regs, uint32_t usp) {
  uint32_t buf_addr = ustack_u32(usp, 0);
  int fd = (int)(int16_t)ustack_u16(usp, 4);
  H68K_TRACE("_FGETS(%u, %x)", (uint32_t)fd, buf_addr);
  uint8_t *buf = (uint8_t *)(uintptr_t)buf_addr;
  uint8_t max = buf[0];
  uint8_t count = 0;

  while (count < max) {
    uint8_t ch;
    long r = h68k_fd_read(fd, &ch, 1);
    if (r <= 0) break;
    if (ch == 0x0D || ch == 0x0A) break;
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
static int dos_fputc(uint32_t *regs, uint32_t usp) {
  uint8_t ch = (uint8_t)ustack_u16(usp, 0);
  int fd = (int)(int16_t)ustack_u16(usp, 2);
  H68K_TRACE("_FPUTC(%u, %x)", (uint32_t)fd, (uint32_t)ch);
  h68k_fd_write(fd, &ch, 1);
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
static int dos_fputs(uint32_t *regs, uint32_t usp) {
  uint32_t str_addr = ustack_u32(usp, 0);
  int fd = (int)(int16_t)ustack_u16(usp, 4);
  H68K_TRACE("_FPUTS(%u, %x)", (uint32_t)fd, str_addr);
  const char *str = (const char *)(uintptr_t)str_addr;

  uint32_t len = 0;
  while (str[len]) len++;

  if (len > 0) sys_write(fd, (uintptr_t)str, len);

  regs[0] = 0;
  advance_pc(regs);
  return 2;
}

/* ── _CREATE ($FF3C) ─────────────────────────────────────────────────── *
 *
 * Stack: long path_ptr, word attr
 * Creates a new file (or truncates existing).  Returns handle in d0.
 */
static int dos_create(uint32_t *regs, uint32_t usp) {
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
  long r = sys_open((uintptr_t)path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
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
static int dos_open(uint32_t *regs, uint32_t usp) {
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
    case 0:
      flags = O_RDONLY;
      break;
    case 1:
      flags = O_WRONLY;
      break;
    default:
      flags = O_RDWR;
      break;
  }
  long r = sys_open((uintptr_t)path, flags, 0644);
  regs[0] = (uint32_t)h68k_errno(r);
  advance_pc(regs);
  return 2;
}

/* ── _CLOSE ($FF3E) ─────────────────────────────────────────────────── *
 *
 * Stack: word fileno
 * Closes the file handle.  Returns 0 on success.
 */
static int dos_close(uint32_t *regs, uint32_t usp) {
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
static int dos_read(uint32_t *regs, uint32_t usp) {
  int fd = (int)(int16_t)ustack_u16(usp, 0);
  uint32_t buf_addr = ustack_u32(usp, 2);
  uint32_t len = ustack_u32(usp, 6);
  H68K_TRACE("_READ(%u, %x, %x)", (uint32_t)fd, buf_addr, len);
  long r = sys_read(fd, (uintptr_t)buf_addr, (size_t)len);
  regs[0] = (uint32_t)h68k_errno(r);
  advance_pc(regs);
  return 2;
}

/* ── _WRITE ($FF40) ─────────────────────────────────────────────────── *
 *
 * Stack: word fileno, long buf_ptr, long len
 * Writes len bytes.  Returns bytes written in d0.
 */
static int dos_write(uint32_t *regs, uint32_t usp) {
  int fd = (int)(int16_t)ustack_u16(usp, 0);
  uint32_t buf_addr = ustack_u32(usp, 2);
  uint32_t len = ustack_u32(usp, 6);
  H68K_TRACE("_WRITE(%u, %x, %x)", (uint32_t)fd, buf_addr, len);
  long r = sys_write(fd, (uintptr_t)buf_addr, (size_t)len);
  regs[0] = (uint32_t)h68k_errno(r);
  advance_pc(regs);
  return 2;
}

/* ── _DELETE ($FF41) ────────────────────────────────────────────────── *
 *
 * Stack: long path_ptr
 * Deletes a file.  Returns 0 on success.
 */
static int dos_delete(uint32_t *regs, uint32_t usp) {
  uint32_t path_addr = ustack_u32(usp, 0);
  const char *src = (const char *)(uintptr_t)path_addr;
  char path[128];
  if (h68k_translate_path(src, path, sizeof(path)) < 0) {
    regs[0] = (uint32_t)h68k_errno(-ENAMETOOLONG);
    advance_pc(regs);
    return 2;
  }
  H68K_TRACE("_DELETE(%s)", path);
  long r = sys_unlink((uintptr_t)path);
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
static int dos_seek(uint32_t *regs, uint32_t usp) {
  int fd = (int)(int16_t)ustack_u16(usp, 0);
  int32_t offset = (int32_t)ustack_u32(usp, 2);
  uint16_t whence = ustack_u16(usp, 6);
  H68K_TRACE("_SEEK(%u, %x, %u)", (uint32_t)fd, (uint32_t)offset,
             (uint32_t)whence);
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
static int dos_chdir(uint32_t *regs, uint32_t usp) {
  uint32_t path_addr = ustack_u32(usp, 0);
  const char *src = (const char *)(uintptr_t)path_addr;
  char path[128];
  if (h68k_translate_path(src, path, sizeof(path)) < 0) {
    regs[0] = (uint32_t)h68k_errno(-ENAMETOOLONG);
    advance_pc(regs);
    return 2;
  }
  H68K_TRACE("_CHDIR(%s)", path);
  long r = sys_chdir((uintptr_t)path);
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
static int dos_curdir(uint32_t *regs, uint32_t usp) {
  uint16_t drive = ustack_u16(usp, 0);
  uint32_t buf_addr = ustack_u32(usp, 2);
  H68K_TRACE("_CURDIR(%u, %x)", (uint32_t)drive, buf_addr);
  (void)drive; /* single-drive system — ignore drive number */

  char cwd[128];
  long r = sys_getcwd((uintptr_t)cwd, sizeof(cwd));
  if (r < 0) {
    regs[0] = (uint32_t)h68k_errno(r);
    advance_pc(regs);
    return 2;
  }

  /* Strip leading slash and convert / to \ for Human68k convention.
   * Human68k expects the path without drive letter or leading backslash. */
  char *dst = (char *)(uintptr_t)buf_addr;
  const char *p = cwd;
  if (*p == '/') p++; /* skip leading / */
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
static int dos_curdrv(uint32_t *regs) {
  H68K_TRACE("_CURDRV");
  regs[0] = 0; /* always drive A: */
  advance_pc(regs);
  return 2;
}

/* ── _DUP ($FF45) ──────────────────────────────────────────────────── *
 *
 * Stack: word fileno
 * Duplicates a file handle.  Returns new handle in d0.
 */
static int dos_dup(uint32_t *regs, uint32_t usp) {
  int fd = (int)(int16_t)ustack_u16(usp, 0);
  H68K_TRACE("_DUP(%d)", fd);
  long r = sys_dup((long)fd);
  regs[0] = (uint32_t)h68k_errno(r);
  advance_pc(regs);
  return 2;
}

/* ── _DUP2 ($FF46) ─────────────────────────────────────────────────── *
 *
 * Stack: word old_fileno, word new_fileno
 * Duplicates old handle to new handle.  Returns new handle in d0.
 */
static int dos_dup2(uint32_t *regs, uint32_t usp) {
  int old_fd = (int)(int16_t)ustack_u16(usp, 0);
  int new_fd = (int)(int16_t)ustack_u16(usp, 2);
  H68K_TRACE("_DUP2(%d, %d)", old_fd, new_fd);
  long r = sys_dup2((long)old_fd, (long)new_fd);
  regs[0] = (uint32_t)h68k_errno(r);
  advance_pc(regs);
  return 2;
}

/* ── _RENAME ($FF56) ────────────────────────────────────────────────── *
 *
 * Stack: long old_path_ptr, long new_path_ptr
 * Renames (moves) a file or directory.  Returns 0 on success.
 *
 */
static int dos_rename(uint32_t *regs, uint32_t usp) {
  uint32_t old_addr = ustack_u32(usp, 0);
  uint32_t new_addr = ustack_u32(usp, 4);
  const char *old_src = (const char *)(uintptr_t)old_addr;
  const char *new_src = (const char *)(uintptr_t)new_addr;
  char old_path[128], new_path[128];
  h68k_translate_path(old_src, old_path, sizeof(old_path));
  h68k_translate_path(new_src, new_path, sizeof(new_path));
  H68K_TRACE("_RENAME(%s, %s)", old_path, new_path);
  long r = sys_rename((uintptr_t)old_path, (uintptr_t)new_path);
  regs[0] = (uint32_t)h68k_errno((int)r);
  advance_pc(regs);
  return 2;
}

/* ── _VERNUM ($FF30) ───────────────────────────────────────────────── *
 *
 * No stack arguments.
 * Returns Human68k version in d0: high word = "HU", low word = version.
 * Version 3.02 = 0x4855_0302 ("HU" + 0x0302).
 */
static int dos_vernum(uint32_t *regs) {
  H68K_TRACE("_VERNUM");
  regs[0] = 0x36380302u; /* "68" + version 3.02 */
  advance_pc(regs);
  return 2;
}

/* ── _BREAKCK ($FF33) ──────────────────────────────────────────────── *
 *
 * Stack: word mode  (0=get, 1=set, 2=get-and-set)
 * Returns/sets break-check mode.  Always return 1 (break check on).
 */
static int dos_breakck(uint32_t *regs, uint32_t usp) {
  uint16_t mode = ustack_u16(usp, 0);
  H68K_TRACE("_BREAKCK(%u)", (uint32_t)mode);
  (void)mode;
  regs[0] = 1; /* break check on */
  advance_pc(regs);
  return 2;
}

/* ── _INTVCG ($FF35) ───────────────────────────────────────────────── *
 *
 * Stack: word vecno
 * Get interrupt vector.  Return 0 (no vector table).
 */
static int dos_intvcg(uint32_t *regs, uint32_t usp) {
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
static int dos_getpdb(uint32_t *regs) {
  H68K_TRACE("_GETPDB");
  pcb_t *p = current;
  /* PMB starts at the first tracked page-backed base. */
  page_id_t pdb_id = proc_page_backed_base(p);
  regs[0] = (pdb_id != PAGE_ID_INVALID) ? mem_region_page_linear(pdb_id) : 0;
  advance_pc(regs);
  return 2;
}

/* ── _MOVE ($FF05) ──────────────────────────────────────────────────── *
 *
 * Stack: word char_code
 * Output character with cursor advance (like _PUTCHAR but also handles
 * control codes for cursor movement).  We just output the character.
 */
static int dos_move(uint32_t *regs, uint32_t usp) {
  uint8_t ch = (uint8_t)ustack_u16(usp, 0);
  H68K_TRACE("_MOVE(%x)", (uint32_t)ch);
  h68k_putc(ch);
  regs[0] = 0;
  advance_pc(regs);
  return 2;
}

/* ── _NAMECK ($FF18) ───────────────────────────────────────────────── *
 *
 * Stack: long path_ptr, long buf_ptr
 * Parse a pathname into its components.
 * Buffer layout (91 bytes):
 *   [0x00..0x40]  drive+path (65 bytes, NUL-terminated)
 *   [0x41..0x52]  filename (18 bytes, NUL-terminated)
 *   [0x53..0x56]  extension (4 bytes, NUL-terminated)
 *
 * Returns in d0:
 *   0  = file or directory
 *   2  = wildcard in name
 *  -13 = path too long / invalid
 */
static int dos_nameck(uint32_t *regs, uint32_t usp) {
  uint32_t path_addr = ustack_u32(usp, 0);
  uint32_t buf_addr = ustack_u32(usp, 4);
  const char *src = (const char *)(uintptr_t)path_addr;
  uint8_t *buf = (uint8_t *)(uintptr_t)buf_addr;
  H68K_TRACE("_NAMECK(%s)", src);

  /* Find last separator */
  int len = 0, last_sep = -1;
  while (src[len]) {
    if (src[len] == '\\' || src[len] == '/') last_sep = len;
    len++;
  }

  /* Copy drive+path portion (up to and including last separator) */
  int path_len = last_sep + 1;
  if (path_len > 64) path_len = 64;
  for (int i = 0; i < path_len; i++) buf[i] = (uint8_t)src[i];
  buf[path_len] = '\0';

  /* Filename starts after last separator */
  const char *fname = src + last_sep + 1;
  int has_wild = 0;

  /* Split filename and extension */
  int dot_pos = -1;
  int flen = 0;
  while (fname[flen]) {
    if (fname[flen] == '.') dot_pos = flen;
    if (fname[flen] == '*' || fname[flen] == '?') has_wild = 1;
    flen++;
  }

  if (dot_pos >= 0) {
    /* Copy name (before dot) */
    int nlen = dot_pos;
    if (nlen > 17) nlen = 17;
    for (int i = 0; i < nlen; i++) buf[0x41 + i] = (uint8_t)fname[i];
    buf[0x41 + nlen] = '\0';

    /* Copy extension (including dot) */
    const char *ext = fname + dot_pos;
    int elen = flen - dot_pos;
    if (elen > 3) elen = 3;
    for (int i = 0; i < elen; i++) buf[0x53 + i] = (uint8_t)ext[i];
    buf[0x53 + elen] = '\0';
  } else {
    /* No extension */
    int nlen = flen;
    if (nlen > 17) nlen = 17;
    for (int i = 0; i < nlen; i++) buf[0x41 + i] = (uint8_t)fname[i];
    buf[0x41 + nlen] = '\0';
    buf[0x53] = '\0';
  }

  regs[0] = has_wild ? 2 : 0;
  advance_pc(regs);
  return 2;
}

/* ── _ALLCLOSE ($FF1A) ─────────────────────────────────────────────── *
 *
 * No stack arguments.
 * Close all file handles for the current process (except stdin/out/err).
 * Returns 0.
 */
static int dos_allclose(uint32_t *regs) {
  H68K_TRACE("_ALLCLOSE");
  /* Close fds 3 and above */
  for (int fd = 3; fd < 16; fd++) sys_close((long)fd);
  regs[0] = 0;
  advance_pc(regs);
  return 2;
}

/* ── _KEEPPR ($FF31) ───────────────────────────────────────────────── *
 *
 * Stack: long code, long nbytes
 * Terminate and stay resident.  We don't support TSR — just exit.
 */
static int dos_keeppr(uint32_t *regs, uint32_t usp) {
  uint32_t code = ustack_u32(usp, 0);
  H68K_TRACE("_KEEPPR(%u)", code);
  (void)regs;
  sys_exit((long)code);
  return 1; /* process exited */
}

/* ── _SUPER ($FF20) ─────────────────────────────────────────────────── *
 *
 * Stack: long ssp
 *   If ssp == 0: switch to supervisor mode, return previous SSP in d0.
 *   If ssp != 0: switch to user mode, set SSP to given value.
 *
 * On m68k PPAP, user programs always run in user mode.  We track the
 * "logical" supervisor flag in h68k_proc_t but don't actually change
 * the CPU mode — the kernel already handles traps from user mode.
 * Programs calling _SUPER(0) just want to access IOCS memory directly;
 * we return a plausible SSP so they can restore later.
 */
static int dos_super(uint32_t *regs, uint32_t usp) {
  uint32_t ssp = ustack_u32(usp, 0);
  H68K_TRACE("_SUPER(%x)", ssp);

  if (ssp == 0) {
    /* Enter supervisor: return current SSP (use a dummy value) */
    regs[0] = usp; /* return the USP as "previous SSP" */
  } else {
    /* Return to user mode: ssp is the saved SSP to restore */
    regs[0] = 0;
  }
  advance_pc(regs);
  return 2;
}

/* ── _INPOUT ($FF08) ───────────────────────────────────────────────── *
 *
 * d1.w = character to test:
 *   If d1.w == 0x00FF: check for input (return char or 0 if none).
 *   Otherwise: output d1.b to stdout, return d1.w.
 *
 * Stack: word code
 */
static int dos_inpout(uint32_t *regs, uint32_t usp) {
  uint16_t code = ustack_u16(usp, 0);
  H68K_TRACE("_INPOUT(%x)", (uint32_t)code);

  if (code == 0x00FF) {
    /* Input check — we can't do non-blocking read, return 0 */
    regs[0] = 0;
  } else {
    /* Output character */
    h68k_putc((uint8_t)code);
    regs[0] = (uint32_t)code;
  }
  advance_pc(regs);
  return 2;
}

/* ── _KFLUSH ($FF0C) ───────────────────────────────────────────────── *
 *
 * Stack: word mode
 * Flush keyboard buffer, then perform an input function based on mode:
 *   mode 0x01: _GETCHAR (read with echo)
 *   mode 0x06: _RAWREAD (direct, return 0 if no input)
 *   mode 0x07: _RAWREAD (direct, wait)
 *   mode 0x08: _INPOUT(0xFF) check
 *   mode 0x0A: _GETS
 *   else: just flush, return 0
 */
static int dos_kflush(uint32_t *regs, uint32_t usp) {
  uint16_t mode = ustack_u16(usp, 0);
  H68K_TRACE("_KFLUSH(%x)", (uint32_t)mode);

  /* No keyboard buffer to flush on serial.  Perform the secondary op. */
  switch (mode) {
    case 0x01: /* like _GETCHAR */
    case 0x07: {
      uint8_t ch = h68k_keyinp();
      if (mode == 0x01) h68k_putc(ch); /* echo */
      regs[0] = (uint32_t)ch;
      break;
    }
    case 0x06:
    case 0x08:
      regs[0] = 0; /* no buffered input */
      break;
    default:
      regs[0] = 0;
      break;
  }
  advance_pc(regs);
  return 2;
}

/* ── _CONCTRL ($FF10) ──────────────────────────────────────────────── *
 *
 * Stack: word sub_func, …
 * Console control — cursor movement, screen clear, etc.
 *
 * sub-functions:
 *   0: putc (word char)
 *   1: print (long str_ptr)
 *   2: set text color (word color) → return old
 *   3: locate (word x, word y) → move cursor
 *   6: clear line from cursor to end
 *   7: insert line
 *   8: delete line
 *  10: check for Ctrl+C → return 0 (no break)
 *  11: set function key display mode → return 0
 *
 * Minimal implementation using ANSI escape sequences for locate/clear.
 */
static int dos_conctrl(uint32_t *regs, uint32_t usp) {
  uint16_t sub = ustack_u16(usp, 0);
  H68K_TRACE("_CONCTRL(%u)", (uint32_t)sub);

  switch (sub) {
    case 0: { /* putc */
      h68k_putc((uint8_t)ustack_u16(usp, 2));
      regs[0] = 0;
      break;
    }
    case 1: { /* print */
      const char *str = (const char *)(uintptr_t)ustack_u32(usp, 2);
      h68k_print(str);
      regs[0] = 0;
      break;
    }
    case 2:        /* color */
      regs[0] = 0; /* previous color = 0 */
      break;
    case 10:       /* Ctrl+C check */
      regs[0] = 0; /* no break */
      break;
    default:
      regs[0] = 0;
      break;
  }
  advance_pc(regs);
  return 2;
}

/* ── _FFLUSH ($FF24) ───────────────────────────────────────────────── *
 *
 * Stack: word fileno
 * Flush output buffer for a file handle.
 * PPAP I/O is unbuffered — always return 0 (success).
 */
static int dos_fflush(uint32_t *regs, uint32_t usp) {
  H68K_TRACE("_FFLUSH(%d)", (int)(int16_t)ustack_u16(usp, 0));
  regs[0] = 0;
  advance_pc(regs);
  return 2;
}

/* ── _ASSIGN ($FF4D) ───────────────────────────────────────────────── *
 *
 * Stack: word mode, word drive, …
 * Drive assignment (virtual drives, etc.).
 * Stub: return 0 (success / no assignment).
 */
static int dos_assign(uint32_t *regs, uint32_t usp) {
  H68K_TRACE("_ASSIGN(%u)", (uint32_t)ustack_u16(usp, 0));
  (void)usp;
  regs[0] = 0;
  advance_pc(regs);
  return 2;
}

/* ── _EXEC ($FF4B) ─────────────────────────────────────────────────── *
 *
 * Stack: word mode, long name_ptr, long cmdline_ptr, long envp_ptr
 *
 * mode 0 (LOADEXEC): load and execute, wait for child to finish.
 * mode 1 (LOAD):     load only (return without executing).
 * mode 2 (PATHCHK):  check if file exists on PATH.
 * mode 3 (LOADONLY):  load, don't execute, return base address.
 * mode 4 (EXECONLY):  execute previously loaded image.
 *
 * We implement mode 0 using proc_alloc + execve + waitpid.
 */
static int dos_exec(uint32_t *regs, uint32_t usp) {
  uint16_t mode = ustack_u16(usp, 0);
  uint32_t name_addr = ustack_u32(usp, 2);
  uint32_t cmdline_addr = ustack_u32(usp, 6);
  (void)cmdline_addr; /* TODO: pass arguments to child */

  const char *raw = (const char *)(uintptr_t)name_addr;
  char path[128];
  if (h68k_translate_path(raw, path, sizeof(path)) < 0) {
    regs[0] = (uint32_t)h68k_errno(-ENAMETOOLONG);
    advance_pc(regs);
    return 2;
  }
  H68K_TRACE("_EXEC(mode=%u, %s)", (uint32_t)mode, path);

  if (mode != 0) {
    /* Only LOADEXEC is supported for now */
    regs[0] = (uint32_t)h68k_errno(-ENOSYS);
    advance_pc(regs);
    return 2;
  }

  /* Allocate child process */
  pcb_t *child = proc_alloc();
  if (!child) {
    regs[0] = (uint32_t)h68k_errno(-ENOMEM);
    advance_pc(regs);
    return 2;
  }

  /* Set child's parent and subsystem */
  child->ppid = current->pid;
  child->subsys = current->subsys;

  /* Load the executable */
  int err = exec_execve(child, path, NULL);
  if (err < 0) {
    /* execve failed — free the child */
    child->state = PROC_FREE;
    regs[0] = (uint32_t)h68k_errno((long)err);
    advance_pc(regs);
    return 2;
  }

  /* Wait for child to finish by polling with WNOHANG + yield.
   * We're in an exception handler, so we can't use the SVC restart
   * mechanism that sys_waitpid's blocking path relies on. */
  advance_pc(regs);
  long wpid;
  do {
    wpid = sys_waitpid(child->pid, 0, 1 /* WNOHANG */);
    if (wpid == 0) sched_switch();
  } while (wpid == 0);

  regs[0] = (wpid > 0) ? 0 : (uint32_t)h68k_errno(wpid);
  return 2;
}

/* ── _GETDATE ($FF2A) ───────────────────────────────────────────────── *
 *
 * No stack arguments.
 * Returns packed date in d0:
 *   bits 31..16 = day of week (0=Sun..6=Sat)
 *   bits 15..9  = year - 1980
 *   bits  8..5  = month (1-12)
 *   bits  4..0  = day (1-31)
 *
 * We return a fixed date: 2026-01-01 (Thursday = 4).
 */
static int dos_getdate(uint32_t *regs) {
  H68K_TRACE("_GETDATE");
  uint32_t wday = 4;           /* Thursday */
  uint32_t year = 2026 - 1980; /* 46 */
  uint32_t month = 1;
  uint32_t day = 1;
  regs[0] = (wday << 16) | (year << 9) | (month << 5) | day;
  advance_pc(regs);
  return 2;
}

/* ── _SETDATE ($FF2B) ───────────────────────────────────────────────── *
 *
 * Stack: word date (packed as above, no wday)
 * Stub: ignore, return 0.
 */
static int dos_setdate(uint32_t *regs, uint32_t usp) {
  (void)usp;
  H68K_TRACE("_SETDATE");
  regs[0] = 0;
  advance_pc(regs);
  return 2;
}

/* ── _GETTIME ($FF2C) ───────────────────────────────────────────────── *
 *
 * No stack arguments.
 * Returns packed time in d0:
 *   bits 15..11 = hour (0-23)
 *   bits 10..5  = minute (0-59)
 *   bits  4..0  = second / 2 (0-29)
 *
 * We return 00:00:00.
 */
static int dos_gettime(uint32_t *regs) {
  H68K_TRACE("_GETTIME");
  regs[0] = 0;
  advance_pc(regs);
  return 2;
}

/* ── _SETTIME ($FF2D) ───────────────────────────────────────────────── *
 *
 * Stack: word time (packed as above)
 * Stub: ignore, return 0.
 */
static int dos_settime(uint32_t *regs, uint32_t usp) {
  (void)usp;
  H68K_TRACE("_SETTIME");
  regs[0] = 0;
  advance_pc(regs);
  return 2;
}

/* ── _DSKFRE ($FF36) ───────────────────────────────────────────────── *
 *
 * Stack: word drive (0 = current, 1 = A:, …)
 * Returns free space info in a buffer pointed to by a0 (regs[8]):
 *   [0]  uint16_t  free_clusters
 *   [2]  uint16_t  total_clusters
 *   [4]  uint16_t  sectors_per_cluster
 *   [6]  uint16_t  bytes_per_sector
 * d0 = free space in bytes (or -1 on error)
 *
 * We synthesize from page allocator info.
 */
static int dos_dskfre(uint32_t *regs, uint32_t usp) {
  H68K_TRACE("_DSKFRE(%u)", (uint32_t)ustack_u16(usp, 0));

  uint32_t free_pages = mem_region_free_bytes(PPAP_MEM_RAM_STACK) / PAGE_SIZE;
  uint32_t total_pages = mem_region_total_bytes(PPAP_MEM_RAM_STACK) / PAGE_SIZE;
  uint32_t page_sz = PAGE_SIZE;

  /* Model as: 1 sector = PAGE_SIZE bytes, 1 cluster = 1 sector */
  uint32_t buf_addr = regs[8]; /* a0 = output buffer */
  if (buf_addr) {
    *(volatile uint16_t *)(uintptr_t)(buf_addr + 0) = (uint16_t)free_pages;
    *(volatile uint16_t *)(uintptr_t)(buf_addr + 2) = (uint16_t)total_pages;
    *(volatile uint16_t *)(uintptr_t)(buf_addr + 4) = 1; /* 1 sector/cluster */
    *(volatile uint16_t *)(uintptr_t)(buf_addr + 6) = (uint16_t)page_sz;
  }

  regs[0] = free_pages * page_sz;
  advance_pc(regs);
  return 2;
}

/* ── _GETENV ($FF38) ────────────────────────────────────────────────── *
 *
 * Stack: long name_ptr, long env_ptr, long buf_ptr
 * Looks up an environment variable.  We have no environment —
 * return -1 (not found) for all queries.
 */
static int dos_getenv(uint32_t *regs, uint32_t usp) {
  H68K_TRACE("_GETENV(%s)", (const char *)(uintptr_t)ustack_u32(usp, 0));
  regs[0] = (uint32_t)(-1); /* not found */
  advance_pc(regs);
  return 2;
}

/* ── _IOCTRL ($FF44) ───────────────────────────────────────────────── *
 *
 * Stack: word mode, word fileno, …
 * Device I/O control.  Many sub-functions keyed on 'mode'.
 *
 * mode 0: get device info → return device word in d0
 *   For stdin/stdout/stderr (fd 0-2): return 0x80C1 (console, cooked)
 *   For regular files: return 0x0000
 *
 * All other modes: return 0 (no-op).
 */
static int dos_ioctrl(uint32_t *regs, uint32_t usp) {
  uint16_t mode = ustack_u16(usp, 0);
  int fd = (int)(int16_t)ustack_u16(usp, 2);
  H68K_TRACE("_IOCTRL(%u, %d)", (uint32_t)mode, fd);

  if (mode == 0) {
    /* Get device info */
    if (fd >= 0 && fd <= 2)
      regs[0] = 0x80C1; /* character device, console */
    else
      regs[0] = 0x0000; /* regular file */
  } else {
    regs[0] = 0;
  }
  advance_pc(regs);
  return 2;
}

/* ── _FILEDATE ($FF57) ──────────────────────────────────────────────── *
 *
 * Stack: word fileno, long datetime (0 = get, nonzero = set)
 * Get/set file modification date+time.
 *
 * datetime format:  high word = date (year<<9|month<<5|day)
 *                   low word  = time (hour<<11|min<<5|sec/2)
 *
 * Get: return fixed date 2026-01-01 00:00:00 in d0.
 * Set: ignore, return 0.
 */
static int dos_filedate(uint32_t *regs, uint32_t usp) {
  int fd = (int)(int16_t)ustack_u16(usp, 0);
  uint32_t datetime = ustack_u32(usp, 2);
  H68K_TRACE("_FILEDATE(%d, %x)", fd, datetime);
  (void)fd;

  if (datetime == 0) {
    /* Get: return fixed date 2026-01-01 00:00:00 */
    uint16_t date = ((2026 - 1980) << 9) | (1 << 5) | 1;
    uint16_t time = 0;
    regs[0] = ((uint32_t)date << 16) | time;
  } else {
    /* Set: no-op */
    regs[0] = 0;
  }
  advance_pc(regs);
  return 2;
}

/* ── _MKDIR ($FF39) ─────────────────────────────────────────────────── *
 *
 * Stack: long path_ptr
 * Creates a directory.  Returns 0 on success.
 */
static int dos_mkdir(uint32_t *regs, uint32_t usp) {
  uint32_t path_addr = ustack_u32(usp, 0);
  const char *src = (const char *)(uintptr_t)path_addr;
  char path[128];
  if (h68k_translate_path(src, path, sizeof(path)) < 0) {
    regs[0] = (uint32_t)h68k_errno(-ENAMETOOLONG);
    advance_pc(regs);
    return 2;
  }
  H68K_TRACE("_MKDIR(%s)", path);
  long r = sys_mkdir((uintptr_t)path, 0755);
  regs[0] = (uint32_t)h68k_errno(r);
  advance_pc(regs);
  return 2;
}

/* ── _RMDIR ($FF3A) ─────────────────────────────────────────────────── *
 *
 * Stack: long path_ptr
 * Removes an empty directory.  Returns 0 on success.
 */
static int dos_rmdir(uint32_t *regs, uint32_t usp) {
  uint32_t path_addr = ustack_u32(usp, 0);
  const char *src = (const char *)(uintptr_t)path_addr;
  char path[128];
  if (h68k_translate_path(src, path, sizeof(path)) < 0) {
    regs[0] = (uint32_t)h68k_errno(-ENAMETOOLONG);
    advance_pc(regs);
    return 2;
  }
  H68K_TRACE("_RMDIR(%s)", path);
  long r = sys_rmdir((uintptr_t)path);
  regs[0] = (uint32_t)h68k_errno(r);
  advance_pc(regs);
  return 2;
}

/* ── _CHMOD ($FF43) ─────────────────────────────────────────────────── *
 *
 * Stack: word attr, long path_ptr
 * If attr == 0xFF: query mode — return synthesized attributes.
 * Otherwise: set mode (limited).  Returns 0 or attributes.
 */
static int dos_chmod(uint32_t *regs, uint32_t usp) {
  uint16_t attr = ustack_u16(usp, 0);
  (void)attr; /* set-mode not supported yet; query-only */
  uint32_t path_addr = ustack_u32(usp, 2);
  const char *src = (const char *)(uintptr_t)path_addr;
  char path[128];
  if (h68k_translate_path(src, path, sizeof(path)) < 0) {
    regs[0] = (uint32_t)h68k_errno(-ENAMETOOLONG);
    advance_pc(regs);
    return 2;
  }
  H68K_TRACE("_CHMOD(%x, %s)", (uint32_t)attr, path);

  struct stat st;
  long r = sys_stat((uintptr_t)path, (uintptr_t)&st);
  if (r < 0) {
    regs[0] = (uint32_t)h68k_errno(r);
    advance_pc(regs);
    return 2;
  }

  /* Synthesize Human68k attributes from UNIX mode */
  uint8_t h_attr = 0x20;                    /* archive bit always set */
  if (S_ISDIR(st.st_mode)) h_attr |= 0x10;  /* directory */
  if (!(st.st_mode & 0200)) h_attr |= 0x01; /* read-only */

  /* Query mode: return attributes */
  regs[0] = (uint32_t)h_attr;
  advance_pc(regs);
  return 2;
}

/* ── Wildcard pattern matcher ────────────────────────────────────────── *
 *
 * Case-insensitive match with * and ? wildcards.
 * Returns 1 on match, 0 on no match.
 */
static int h68k_wildmatch(const char *pattern, const char *name) {
  while (*pattern) {
    if (*pattern == '*') {
      pattern++;
      /* Skip consecutive stars */
      while (*pattern == '*') pattern++;
      if (!*pattern) return 1; /* trailing * matches everything */
      /* Try matching the rest from every position */
      while (*name) {
        if (h68k_wildmatch(pattern, name)) return 1;
        name++;
      }
      return h68k_wildmatch(pattern, name);
    } else if (*pattern == '?') {
      if (!*name) return 0;
      pattern++;
      name++;
    } else {
      /* Case-insensitive compare */
      char pc = *pattern, nc = *name;
      if (pc >= 'a' && pc <= 'z') pc -= 32;
      if (nc >= 'a' && nc <= 'z') nc -= 32;
      if (pc != nc) return 0;
      pattern++;
      name++;
    }
  }
  return (*name == '\0');
}

/* ── _FILES ($FF4E) / _NFILES ($FF4F) ─────────────────────────────── *
 *
 * FILBUF layout (53 bytes):
 *   [0x00..0x14]  search[21]  — opaque state for iteration
 *   [0x15]        attr        — file attributes
 *   [0x16..0x17]  time        — modification time (DOS format)
 *   [0x18..0x19]  date        — modification date (DOS format)
 *   [0x1A..0x1D]  size        — file size (big-endian uint32)
 *   [0x1E..0x34]  name[23]    — filename (NUL-terminated)
 *
 * Search state stored in search[21]:
 *   [0..3]   fd (dir handle as uint32, big-endian)
 *   [4..20]  wildcard pattern (NUL-terminated, max 16 chars)
 *
 * _FILES: long filbuf_ptr, long pathname_ptr, word attr
 * _NFILES: long filbuf_ptr
 */

/* Write big-endian 16-bit to user memory */
static inline void mem_write16(uint32_t addr, uint16_t val) {
  *(volatile uint16_t *)(uintptr_t)addr = val;
}

/* Fill FILBUF fields from a matched directory entry */
static void filbuf_fill(uint32_t filbuf, const char *dir_path,
                        const char *entry_name, uint8_t d_type) {
  uint8_t *fb = (uint8_t *)(uintptr_t)filbuf;

  /* Synthesize attributes */
  uint8_t attr = 0x20; /* archive */
  if (d_type == DT_DIR) attr |= 0x10;

  /* Get file size via stat */
  uint32_t fsize = 0;
  char fullpath[128];
  int dlen = 0;
  while (dir_path[dlen]) dlen++;
  if (dlen > 0 && dlen < 126) {
    memcpy(fullpath, dir_path, (size_t)dlen);
    fullpath[dlen] = '/';
    int nlen = 0;
    while (entry_name[nlen] && dlen + 1 + nlen < 126) {
      fullpath[dlen + 1 + nlen] = entry_name[nlen];
      nlen++;
    }
    fullpath[dlen + 1 + nlen] = '\0';
    struct stat st;
    if (sys_stat((uintptr_t)fullpath, (uintptr_t)&st) == 0) fsize = st.st_size;
  }

  fb[0x15] = attr;
  mem_write16(filbuf + 0x16, 0);      /* time = 0 (no timestamp) */
  mem_write16(filbuf + 0x18, 0x0021); /* date = 1980-01-01 */
  mem_write32(filbuf + 0x1A, fsize);

  /* Copy filename (max 22 chars + NUL) */
  int i = 0;
  while (entry_name[i] && i < 22) {
    fb[0x1E + i] = (uint8_t)entry_name[i];
    i++;
  }
  fb[0x1E + i] = '\0';
}

/* Extract directory and pattern from a Human68k search path.
 * e.g. "/a/dir/ *.x" → dir_out="/a/dir", pattern_out="*.x"
 * e.g. "/a/dir" → dir_out="/a/dir", pattern_out="*"
 */
static void split_search_path(const char *path, char *dir_out, int dir_size,
                              char *pat_out, int pat_size) {
  /* Find last separator */
  const char *last_sep = NULL;
  for (const char *p = path; *p; p++) {
    if (*p == '/') last_sep = p;
  }

  if (last_sep) {
    int dlen = (int)(last_sep - path);
    if (dlen == 0) dlen = 1; /* root "/" */
    if (dlen >= dir_size) dlen = dir_size - 1;
    memcpy(dir_out, path, (size_t)dlen);
    dir_out[dlen] = '\0';
    /* Pattern is after the last separator */
    const char *p = last_sep + 1;
    int pi = 0;
    while (*p && pi < pat_size - 1) pat_out[pi++] = *p++;
    pat_out[pi] = '\0';
  } else {
    /* No separator — path is just a pattern, dir is cwd */
    dir_out[0] = '.';
    dir_out[1] = '\0';
    int pi = 0;
    while (path[pi] && pi < pat_size - 1) {
      pat_out[pi] = path[pi];
      pi++;
    }
    pat_out[pi] = '\0';
  }

  /* If pattern is empty, match everything */
  if (pat_out[0] == '\0') {
    pat_out[0] = '*';
    pat_out[1] = '\0';
  }
}

static int dos_files(uint32_t *regs, uint32_t usp) {
  uint32_t filbuf_addr = ustack_u32(usp, 0);
  uint32_t pathname_addr = ustack_u32(usp, 4);
  /* uint16_t search_attr = ustack_u16(usp, 8); — not used yet */
  H68K_TRACE("_FILES(%x, %x)", filbuf_addr, pathname_addr);

  const char *raw = (const char *)(uintptr_t)pathname_addr;
  char path[128];
  if (h68k_translate_path(raw, path, sizeof(path)) < 0) {
    regs[0] = (uint32_t)h68k_errno(-ENAMETOOLONG);
    advance_pc(regs);
    return 2;
  }

  /* Split into directory and wildcard pattern */
  char dir[128], pattern[24];
  split_search_path(path, dir, sizeof(dir), pattern, sizeof(pattern));
  H68K_TRACE("_FILES: dir=%s pat=%s", dir, pattern);

  /* Open the directory */
  long fd = sys_open((uintptr_t)dir, O_RDONLY, 0);
  if (fd < 0) {
    regs[0] = (uint32_t)h68k_errno(fd);
    advance_pc(regs);
    return 2;
  }

  /* Store fd and pattern in FILBUF search area */
  uint8_t *fb = (uint8_t *)(uintptr_t)filbuf_addr;
  mem_write32(filbuf_addr, (uint32_t)fd);
  /* Store pattern at search[4..20] (max 16 chars + NUL) */
  int pi = 0;
  while (pattern[pi] && pi < 16) {
    fb[4 + pi] = (uint8_t)pattern[pi];
    pi++;
  }
  fb[4 + pi] = '\0';

  /* Also store dir path — we need it for stat lookups.
   * Store in FILBUF at a separate location: use h68k_proc_t.
   * Actually, simpler: store it in the process subsys_data. */
  h68k_proc_t *h = (h68k_proc_t *)current->subsys_data;
  if (h) {
    int dlen = 0;
    while (dir[dlen] && dlen < (int)sizeof(h->files_dir) - 1) {
      h->files_dir[dlen] = dir[dlen];
      dlen++;
    }
    h->files_dir[dlen] = '\0';
  }

  /* Read entries until we find one matching the pattern */
  struct dirent de;
  while (1) {
    long n = sys_getdents(fd, (uintptr_t)&de, 1);
    if (n <= 0) {
      sys_close(fd);
      mem_write32(filbuf_addr, 0xFFFFFFFFu); /* mark closed */
      regs[0] = (uint32_t)(-2);              /* Human68k: no more files */
      advance_pc(regs);
      return 2;
    }
    /* Skip . and .. */
    if (de.d_name[0] == '.' &&
        (de.d_name[1] == '\0' || (de.d_name[1] == '.' && de.d_name[2] == '\0')))
      continue;
    if (h68k_wildmatch(pattern, de.d_name)) {
      filbuf_fill(filbuf_addr, dir, de.d_name, de.d_type);
      regs[0] = 0;
      advance_pc(regs);
      return 2;
    }
  }
}

static int dos_nfiles(uint32_t *regs, uint32_t usp) {
  uint32_t filbuf_addr = ustack_u32(usp, 0);
  H68K_TRACE("_NFILES(%x)", filbuf_addr);

  uint8_t *fb = (uint8_t *)(uintptr_t)filbuf_addr;
  uint32_t fd = *(volatile uint32_t *)(uintptr_t)filbuf_addr;
  if (fd == 0xFFFFFFFFu) {
    regs[0] = (uint32_t)(-2); /* no more files */
    advance_pc(regs);
    return 2;
  }

  /* Recover pattern from search[4..20] */
  char pattern[24];
  int pi = 0;
  while (fb[4 + pi] && pi < 16) {
    pattern[pi] = (char)fb[4 + pi];
    pi++;
  }
  pattern[pi] = '\0';

  /* Recover dir path from process state */
  const char *dir = ".";
  h68k_proc_t *h = (h68k_proc_t *)current->subsys_data;
  if (h) dir = h->files_dir;

  struct dirent de;
  while (1) {
    long n = sys_getdents((long)fd, (uintptr_t)&de, 1);
    if (n <= 0) {
      sys_close((long)fd);
      mem_write32(filbuf_addr, 0xFFFFFFFFu);
      regs[0] = (uint32_t)(-2); /* no more files */
      advance_pc(regs);
      return 2;
    }
    if (de.d_name[0] == '.' &&
        (de.d_name[1] == '\0' || (de.d_name[1] == '.' && de.d_name[2] == '\0')))
      continue;
    if (h68k_wildmatch(pattern, de.d_name)) {
      filbuf_fill(filbuf_addr, dir, de.d_name, de.d_type);
      regs[0] = 0;
      advance_pc(regs);
      return 2;
    }
  }
}

/* ── Dispatch ─────────────────────────────────────────────────────────── */

static void trace_h68k_subsys_enter(uint32_t abi, uint32_t func,
                                    uint32_t *regs);
static void trace_h68k_subsys_exit(uint32_t abi, uint32_t func, uint32_t *regs);

int human68k_dos_dispatch(uint32_t *regs, uint32_t usp, uint16_t opcode) {
  uint8_t func = opcode & 0xFF;
  int ret;

  /* DOS call alias: $FF80–$FFAF maps to $FF50–$FF7F (subtract $30) */
  if (func >= 0x80 && func <= 0xAF) func -= 0x30;

  trace_h68k_subsys_enter(PPAP_TRACE_ABI_H68K_DOS, func, regs);

  switch (func) {
    case 0x00: /* _EXIT */
    case 0x4C: /* _EXIT2 */
      ret = dos_exit(usp);
      break;

    case 0x01: /* _GETCHAR — read with echo */
      ret = dos_getchar(regs);
      break;

    case 0x02: /* _PUTCHAR */
    case 0x04: /* _COMOUT */
      ret = dos_putchar(regs, usp);
      break;

    case 0x03: /* _COMINP — raw read, no echo */
      ret = dos_cominp(regs);
      break;

    case 0x05: /* _MOVE */
      ret = dos_move(regs, usp);
      break;

    case 0x09: /* _PRINT */
      ret = dos_print(regs, usp);
      break;

    case 0x08: /* _INPOUT */
      ret = dos_inpout(regs, usp);
      break;

    case 0x0A: /* _GETS */
      ret = dos_gets(regs, usp);
      break;

    case 0x0C: /* _KFLUSH */
      ret = dos_kflush(regs, usp);
      break;

    case 0x10: /* _CONCTRL */
      ret = dos_conctrl(regs, usp);
      break;

    case 0x18: /* _NAMECK */
      ret = dos_nameck(regs, usp);
      break;

    case 0x19: /* _CURDRV */
      ret = dos_curdrv(regs);
      break;

    case 0x1A: /* _ALLCLOSE */
      ret = dos_allclose(regs);
      break;

    case 0x1B: /* _FGETC */
      ret = dos_fgetc(regs, usp);
      break;

    case 0x1C: /* _FGETS */
      ret = dos_fgets(regs, usp);
      break;

    case 0x1D: /* _FPUTC */
      ret = dos_fputc(regs, usp);
      break;

    case 0x1E: /* _FPUTS */
      ret = dos_fputs(regs, usp);
      break;

    case 0x20: /* _SUPER */
      ret = dos_super(regs, usp);
      break;

    case 0x24: /* _FFLUSH */
      ret = dos_fflush(regs, usp);
      break;

    case 0x2A: /* _GETDATE */
      ret = dos_getdate(regs);
      break;

    case 0x2B: /* _SETDATE */
      ret = dos_setdate(regs, usp);
      break;

    case 0x2C: /* _GETTIME */
      ret = dos_gettime(regs);
      break;

    case 0x2D: /* _SETTIME */
      ret = dos_settime(regs, usp);
      break;

    case 0x30: /* _VERNUM */
      ret = dos_vernum(regs);
      break;

    case 0x31: /* _KEEPPR */
      ret = dos_keeppr(regs, usp);
      break;

    case 0x33: /* _BREAKCK */
      ret = dos_breakck(regs, usp);
      break;

    case 0x35: /* _INTVCG */
      ret = dos_intvcg(regs, usp);
      break;

    case 0x36: /* _DSKFRE */
      ret = dos_dskfre(regs, usp);
      break;

    case 0x38: /* _GETENV */
      ret = dos_getenv(regs, usp);
      break;

    case 0x39: /* _MKDIR */
      ret = dos_mkdir(regs, usp);
      break;

    case 0x3A: /* _RMDIR */
      ret = dos_rmdir(regs, usp);
      break;

    case 0x3B: /* _CHDIR */
      ret = dos_chdir(regs, usp);
      break;

    case 0x3C: /* _CREATE */
      ret = dos_create(regs, usp);
      break;

    case 0x3D: /* _OPEN */
      ret = dos_open(regs, usp);
      break;

    case 0x3E: /* _CLOSE */
      ret = dos_close(regs, usp);
      break;

    case 0x3F: /* _READ */
      ret = dos_read(regs, usp);
      break;

    case 0x40: /* _WRITE */
      ret = dos_write(regs, usp);
      break;

    case 0x41: /* _DELETE */
      ret = dos_delete(regs, usp);
      break;

    case 0x42: /* _SEEK */
      ret = dos_seek(regs, usp);
      break;

    case 0x43: /* _CHMOD */
      ret = dos_chmod(regs, usp);
      break;

    case 0x44: /* _IOCTRL */
      ret = dos_ioctrl(regs, usp);
      break;

    case 0x45: /* _DUP */
      ret = dos_dup(regs, usp);
      break;

    case 0x46: /* _DUP2 */
      ret = dos_dup2(regs, usp);
      break;

    case 0x47: /* _CURDIR */
      ret = dos_curdir(regs, usp);
      break;

    case 0x4A: /* _SETBLOCK */
      ret = dos_setblock(regs, usp);
      break;

    case 0x4B: /* _EXEC */
      ret = dos_exec(regs, usp);
      break;

    case 0x4D: /* _ASSIGN */
      ret = dos_assign(regs, usp);
      break;

    case 0x48: /* _MALLOC */
      ret = dos_malloc(regs, usp);
      break;

    case 0x49: /* _MFREE */
      ret = dos_mfree(regs, usp);
      break;

    case 0x51: /* _GETPDB */
      ret = dos_getpdb(regs);
      break;

    case 0x4E: /* _FILES */
      ret = dos_files(regs, usp);
      break;

    case 0x4F: /* _NFILES */
      ret = dos_nfiles(regs, usp);
      break;

    case 0x56: /* _RENAME */
      ret = dos_rename(regs, usp);
      break;

    case 0x57: /* _FILEDATE */
      ret = dos_filedate(regs, usp);
      break;

    default:
      H68K_TRACE("DOS notimpl: opcode=%x", (uint32_t)(0xFF00u | func));
      regs[0] = (uint32_t)(-(int32_t)ENOSYS);
      advance_pc(regs);
      ret = 2;
      break;
  }

  H68K_TRACE("  => d0=%x", regs[0]);
  trace_h68k_subsys_exit(PPAP_TRACE_ABI_H68K_DOS, func, regs);
  return ret;
}

/* ── Subsystem ops (layering hooks called by the kernel) ──────────── */

/* Static pool of per-process Human68k state.
 * Indexed by proc_table slot (0..PROC_MAX-1).  Zero-initialized. */
static h68k_proc_t h68k_pool[PROC_MAX];

/* on_init — called when a Human68k binary is exec'd */
static void h68k_on_init(struct pcb *p) {
  H68K_TRACE("on_init pid=%u", (uint32_t)p->pid);
  uint32_t slot = (uint32_t)(p - proc_table);
  h68k_proc_t *h = &h68k_pool[slot];
  h->exitvc = 0;
  h->ctrlvc = 0;
  h->errjvc = 0;
  for (int i = 0; i < H68K_MALLOC_MAX; i++) {
    h->mallocs[i].region = (proc_image_segment_t){0};
  }
  p->subsys_data = h;
}

/* on_crash — handle faults for Human68k processes (_ERRJVC) */
static int h68k_on_crash(struct pcb *p, uint32_t *regs, uint16_t *exc,
                         int is_group0) {
  H68K_TRACE("on_crash pid=%u group0=%u", (uint32_t)p->pid,
             (uint32_t)is_group0);
  h68k_proc_t *h = (h68k_proc_t *)p->subsys_data;
  if (!h) return 0;

  if (h->errjvc) {
    H68K_TRACE("_ERRJVC: jumping to %x", h->errjvc);
    exc[is_group0 ? 5 : 1] = (uint16_t)(h->errjvc >> 16);
    exc[is_group0 ? 6 : 2] = (uint16_t)(h->errjvc & 0xFFFF);
    return 2; /* return to (redirected) user mode */
  }

  H68K_TRACE("_ERRJVC default: _EXIT(-1)");
  sys_exit(-1);
  return 1;
}

/* on_signal — handle SIGINT for Human68k processes (_CTRLVC) */
static int h68k_on_signal(struct pcb *p, int sig, uint32_t *regs) {
  H68K_TRACE("on_signal pid=%u sig=%u", (uint32_t)p->pid, (uint32_t)sig);
  /* Only intercept SIGINT (Ctrl+C) */
  if (sig != 2) /* SIGINT = 2 */
    return 0;

  h68k_proc_t *h = (h68k_proc_t *)p->subsys_data;
  if (!h) return 0;

  if (h->ctrlvc) {
    /* Rewrite exception frame PC to _CTRLVC handler */
    uint8_t *frame = (uint8_t *)regs;
    *(uint32_t *)(frame + 62) = h->ctrlvc;
    return 1; /* handled */
  }

  sys_exit(-1);
  return 1; /* handled (exited) */
}

const subsys_ops_t human68k_subsys_ops = {
    .on_crash = h68k_on_crash,
    .on_signal = h68k_on_signal,
    .on_init = h68k_on_init,
};

/* ── IOCS dispatch (TRAP #15) ────────────────────────────────────────── */

/* ── IOCS _B_KEYINP ($00) ───────────────────────────────────────────── *
 *
 * Wait for key input.  Returns scan code in d0 high byte, ASCII in low byte.
 * We return 0 in the scan code (no scan code available from serial).
 */
static int iocs_b_keyinp(uint32_t *regs) {
  uint8_t ch = h68k_keyinp();
  H68K_TRACE("IOCS _B_KEYINP => %x", (uint32_t)ch);
  regs[0] = (uint32_t)ch; /* scan=0, ascii=ch */
  return 2;
}

/* ── IOCS _B_KEYSNS ($04) ──────────────────────────────────────────── *
 *
 * Check keyboard buffer (non-blocking).
 * Returns 0 if no key available, else scan+ascii like _B_KEYINP.
 *
 * We always return 0 (no buffered input) since we have no non-blocking
 * read capability on the serial console yet.
 */
static int iocs_b_keysns(uint32_t *regs) {
  H68K_TRACE("IOCS _B_KEYSNS");
  regs[0] = 0;
  return 2;
}

/* ── IOCS _B_PUTC ($20) ────────────────────────────────────────────── *
 *
 * d1.w = character code to output.
 */
static int iocs_b_putc(uint32_t *regs) {
  uint8_t ch = (uint8_t)regs[1];
  H68K_TRACE("IOCS _B_PUTC(%x)", (uint32_t)ch);
  h68k_putc(ch);
  regs[0] = 0;
  return 2;
}

/* ── IOCS _B_PRINT ($21) ───────────────────────────────────────────── *
 *
 * a1 = pointer to NUL-terminated string.
 */
static int iocs_b_print(uint32_t *regs) {
  const char *str = (const char *)(uintptr_t)regs[8 + 1]; /* a1 */
  H68K_TRACE("IOCS _B_PRINT(%x)", regs[8 + 1]);
  regs[0] = (uint32_t)h68k_print(str);
  return 2;
}

/* ── IOCS _B_COLOR ($22) ───────────────────────────────────────────── *
 *
 * d1.w = color code.  Stub: no color support on serial, return previous (0).
 */
static int iocs_b_color(uint32_t *regs) {
  H68K_TRACE("IOCS _B_COLOR(%x)", regs[1]);
  regs[0] = 0; /* previous color */
  return 2;
}

/* ── IOCS _B_LOCATE ($23) ──────────────────────────────────────────── *
 *
 * d1.w = x, d2.w = y.  Stub: no cursor positioning on serial, return 0.
 */
static int iocs_b_locate(uint32_t *regs) {
  H68K_TRACE("IOCS _B_LOCATE(%u, %u)", regs[1] & 0xFFFF, regs[2] & 0xFFFF);
  regs[0] = 0;
  return 2;
}

/* ── IOCS _ONTIME ($7F) ────────────────────────────────────────────── *
 *
 * Returns uptime in 1/100 second units.
 * d0 = time of day in 1/100s (wraps at 24h = 8640000).
 * d1 = date (days since some epoch, we return 0).
 */
static int iocs_ontime(uint32_t *regs) {
  uint32_t ticks = sched_get_ticks(); /* 100 Hz tick counter */
  H68K_TRACE("IOCS _ONTIME => %u", ticks);
  regs[0] = ticks % 8640000u; /* wrap at 24 hours */
  regs[1] = 0;                /* date = 0 */
  return 2;
}

/* ── IOCS _DATEGET ($5A) ───────────────────────────────────────────── *
 *
 * Returns BCD date:
 *   d0.l = BCD year(16) | BCD month(8) | BCD day(8)
 *          e.g. 0x20260101 for 2026-01-01
 *   d1.w = day of week (0=Sun..6=Sat)
 * Fixed: 2026-01-01 Thursday (wday=4)
 */
static int iocs_dateget(uint32_t *regs) {
  H68K_TRACE("IOCS _DATEGET");
  regs[0] = 0x20260101u; /* BCD 2026-01-01 */
  regs[1] = 4;           /* Thursday */
  return 2;
}

/* ── IOCS _TIMEGET ($5B) ───────────────────────────────────────────── *
 *
 * Returns BCD time in d0: 0(8) | hour(8) | min(8) | sec(8)
 * Fixed: 00:00:00
 */
static int iocs_timeget(uint32_t *regs) {
  H68K_TRACE("IOCS _TIMEGET");
  regs[0] = 0;
  return 2;
}

/* ── IOCS _SKEY_MOD ($0E) ───────────────────────────────────────────── *
 *
 * d1.w = mode (0=get, 1=get+clear).
 * Returns shift key status in d0.  No physical keyboard — always 0.
 */
static int iocs_skey_mod(uint32_t *regs) {
  H68K_TRACE("IOCS _SKEY_MOD");
  regs[0] = 0;
  return 2;
}

/* ── IOCS cursor movement ($19-$1C) ────────────────────────────────── *
 *
 * _B_UP ($19), _B_DOWN ($1A), _B_RIGHT ($1B), _B_LEFT ($1C)
 * Move cursor one position.  Emit ANSI escape sequences.
 */
static int iocs_b_curmov(uint32_t *regs, char dir) {
  char seq[4] = {'\033', '[', dir, '\0'};
  h68k_fd_write(1, seq, 3);
  regs[0] = 0;
  return 2;
}

/* ── IOCS _B_CLRST ($2A) ──────────────────────────────────────────── *
 *
 * d1.w = mode:
 *   0 = clear from cursor to end of screen
 *   1 = clear from start to cursor
 *   2 = clear entire screen
 * Emit ANSI escape sequence.
 */
static int iocs_b_clrst(uint32_t *regs) {
  uint16_t mode = (uint16_t)(regs[1] & 0xFFFF);
  H68K_TRACE("IOCS _B_CLRST(%u)", (uint32_t)mode);
  char seq[5];
  seq[0] = '\033';
  seq[1] = '[';
  if (mode == 2)
    seq[2] = '2';
  else if (mode == 1)
    seq[2] = '1';
  else
    seq[2] = '0';
  seq[3] = 'J';
  h68k_fd_write(1, seq, 4);
  regs[0] = 0;
  return 2;
}

/* ── IOCS _B_ERA_AL ($2B) ─────────────────────────────────────────── *
 *
 * Clear from cursor to end of line.  Emit ANSI CSI 0K.
 */
static int iocs_b_era_al(uint32_t *regs) {
  H68K_TRACE("IOCS _B_ERA_AL");
  {
    char seq[] = "\033[0K";
    h68k_fd_write(1, seq, 4);
  }
  regs[0] = 0;
  return 2;
}

static void trace_h68k_subsys_enter(uint32_t abi, uint32_t func,
                                    uint32_t *regs) {
  (void)trace_before_subsys(abi, func, regs[0], regs[1], regs[2], regs[3],
                            regs[8], regs[9]);
}

static void trace_h68k_subsys_exit(uint32_t abi, uint32_t func,
                                   uint32_t *regs) {
  trace_after_subsys(abi, func, regs[0], regs[1], regs[2], regs[3], regs[8],
                     regs[9], (int32_t)regs[0]);
}

int human68k_iocs_dispatch(uint32_t *regs) {
  uint8_t func = (uint8_t)regs[0];
  int ret;

  trace_h68k_subsys_enter(PPAP_TRACE_ABI_H68K_IOCS, func, regs);

  switch (func) {
    case 0x00: /* _B_KEYINP */
      ret = iocs_b_keyinp(regs);
      break;

    case 0x04: /* _B_KEYSNS */
      ret = iocs_b_keysns(regs);
      break;

    case 0x0E: /* _SKEY_MOD */
      ret = iocs_skey_mod(regs);
      break;

    case 0x19: /* _B_UP */
      ret = iocs_b_curmov(regs, 'A');
      break;
    case 0x1A: /* _B_DOWN */
      ret = iocs_b_curmov(regs, 'B');
      break;
    case 0x1B: /* _B_RIGHT */
      ret = iocs_b_curmov(regs, 'C');
      break;
    case 0x1C: /* _B_LEFT */
      ret = iocs_b_curmov(regs, 'D');
      break;

    case 0x20: /* _B_PUTC */
      ret = iocs_b_putc(regs);
      break;

    case 0x21: /* _B_PRINT */
      ret = iocs_b_print(regs);
      break;

    case 0x22: /* _B_COLOR */
      ret = iocs_b_color(regs);
      break;

    case 0x23: /* _B_LOCATE */
      ret = iocs_b_locate(regs);
      break;

    case 0x2A: /* _B_CLRST */
      ret = iocs_b_clrst(regs);
      break;

    case 0x2B: /* _B_ERA_AL */
      ret = iocs_b_era_al(regs);
      break;

    case 0x5A: /* _DATEGET */
      ret = iocs_dateget(regs);
      break;

    case 0x5B: /* _TIMEGET */
      ret = iocs_timeget(regs);
      break;

    case 0x7F: /* _ONTIME */
      ret = iocs_ontime(regs);
      break;

    default:
      H68K_TRACE("IOCS notimpl: d0=%x", (uint32_t)func);
      regs[0] = (uint32_t)(-1); /* error return */
      ret = 2;
      break;
  }

  trace_h68k_subsys_exit(PPAP_TRACE_ABI_H68K_IOCS, func, regs);
  return ret;
}
