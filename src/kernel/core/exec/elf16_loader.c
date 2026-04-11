/*
 * elf16_loader.c — 16-bit ELF loader for i8086 real mode
 *
 * Loads static ELF32 executables produced by ia16-elf-gcc into a
 * contiguous memory region.  Each process gets its own segment
 * (CS=DS=SS=base>>4).
 *
 * Limitations:
 *   - Static executables only (ET_EXEC, no relocations)
 *   - All PT_LOAD segments must fit in 64 KB
 *   - Linked at vaddr 0x0000 (standard for ia16-elf-ld)
 *   - No shared libraries, no PIE
 */

#include <string.h>

#include "common/errno.h"
#include "kernel/common/mod/mod_vfs.h"
#include "kernel/common/vfs/vfs_types.h"
#include "kernel/core/arch.h"
#include "kernel/core/cpu/cpu.h"
#include "kernel/core/exec/elf.h"
#include "kernel/core/exec/loader.h"
#include "kernel/core/mm/mem_region.h"
#include "kernel/core/proc/proc.h"

#define USER_SEG_PAGES 16u
#define USER_STACK_PAGE (USER_SEG_PAGES - 1u)
#define USER_STACK_BASE ((uint32_t)USER_STACK_PAGE * PAGE_SIZE)
#define USER_SEG_BYTES ((uint32_t)USER_SEG_PAGES * PAGE_SIZE)
#define ELF16_MAX_SIZE (60u * 1024u) /* 60 KB max (leave room for stack) */
#define ELF16_STACK_SIZE 2048u       /* 2 KB user stack */
#define ELF16_MAX_PHDRS 16u
#define ELF16_ZERO_CHUNK 64u

/* ── Detection ─────────────────────────────────────────────────────────── */

static int elf16_detect(const uint8_t *buf, uint32_t size, const char *path) {
  (void)path;

  /* Only on native i16 host */
  if (HOST_ARCH_ID != CPU_ARCH_8086) return 0;

  /* Need at least the ELF header */
  if (size < sizeof(elf32_ehdr_t)) return 0;

  const elf32_ehdr_t *ehdr = (const elf32_ehdr_t *)buf;

  /* Check ELF magic */
  if (ehdr->e_ident[EI_MAG0] != ELFMAG0 || ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
      ehdr->e_ident[EI_MAG2] != ELFMAG2 || ehdr->e_ident[EI_MAG3] != ELFMAG3)
    return 0;

  /* Must be 32-bit, little-endian, executable, EM_386 */
  if (ehdr->e_ident[EI_CLASS] != ELFCLASS32) return 0;
  if (ehdr->e_ident[EI_DATA] != ELFDATA2LSB) return 0;
  if (ehdr->e_type != ET_EXEC) return 0;
  if (ehdr->e_machine != EM_386) return 0;

  return 1;
}

/* ── Loading ───────────────────────────────────────────────────────────── */

static long elf16_read_near(vnode_t *vn, uint32_t off, void *buf,
                            uint16_t len) {
  page_id_t page;
  uint16_t page_off;

  /* TODO(mem_region_wrapup Phase 5): inline ptr→page encoding */
  uintptr_t addr = (uintptr_t)buf;
  page = (page_id_t)(addr / PAGE_SIZE);
  page_off = (uint16_t)(addr & (PAGE_SIZE - 1u));
  return mod_vfs.vnode_read(vn, page, page_off, len, off);
}

static int elf16_read_phdr(const elf32_ehdr_t *ehdr, const elf32_phdr_t *phdrs,
                           vnode_t *vn, uint16_t index, elf32_phdr_t *out) {
  if (!ehdr || !out || index >= ehdr->e_phnum) return -ENOEXEC;

  if (phdrs != NULL) {
    *out = phdrs[index];
    return 0;
  }

  if (vn == NULL) return -ENOEXEC;

  long nread = elf16_read_near(
      vn, ehdr->e_phoff + (uint32_t)index * sizeof(*out), out, sizeof(*out));
  if (nread < 0) return (int)nread;
  if ((uint16_t)nread != sizeof(*out)) return -ENOEXEC;
  return 0;
}

static int elf16_load_from_headers(pcb_t *p, const elf32_ehdr_t *ehdr,
                                   const elf32_phdr_t *phdrs, uint16_t phnum,
                                   const uint8_t *file_buf, vnode_t *vn,
                                   uint32_t file_size, const cpu_ops_t *cpu_ops,
                                   void *cpu_state, const char *const *argv,
                                   uint32_t flags) {
  (void)flags;
  (void)cpu_ops;
  (void)cpu_state;

  if (phnum == 0) return -ENOEXEC;
  if (ehdr->e_phoff == 0) return -ENOEXEC;
  if (ehdr->e_phoff + (uint32_t)phnum * sizeof(elf32_phdr_t) > file_size)
    return -ENOEXEC;

  /* First pass: find total memory footprint of PT_LOAD segments */
  uint32_t mem_end = 0;
  elf32_phdr_t phdr;
  for (uint16_t i = 0; i < phnum; i++) {
    int rc = elf16_read_phdr(ehdr, phdrs, vn, i, &phdr);
    if (rc < 0) return rc;
    if (phdr.p_type != PT_LOAD) continue;
    uint32_t seg_end = phdr.p_vaddr + phdr.p_memsz;
    if (seg_end > mem_end) mem_end = seg_end;
  }

  if (mem_end == 0) return -ENOEXEC;
  if (mem_end > USER_STACK_BASE) return -ENOMEM;

  /* Round the initial brk up to the loader's existing paragraph alignment. */
  uint32_t brk_base = (mem_end + 15u) & ~15u;
  if (brk_base > USER_STACK_BASE) return -ENOMEM;

  /* Allocate contiguous pages via page-indexed API.
   * mem_region_page_alloc_contiguous returns a page_id_t (index), not a
   * pointer — safe on i16 where near pointers can't address pages
   * above 64 KB.  mem_region_page_write copies data via segment:offset. */
  page_id_t base_id = mem_region_page_alloc_contiguous(USER_SEG_PAGES);
  if (base_id == PAGE_ID_INVALID) return -ENOMEM;

  /* Zero entire region via page-indexed writes */
  {
    uint8_t zeros[ELF16_ZERO_CHUNK];
    memset(zeros, 0, sizeof(zeros));
    for (uint16_t pg = 0; pg < USER_SEG_PAGES; pg++) {
      for (uint16_t off = 0; off < PAGE_SIZE; off += sizeof(zeros))
        mem_region_page_write(base_id + pg, off, zeros, sizeof(zeros));
    }
  }

  /* Second pass: copy PT_LOAD segment data via mem_region_page_write */
  for (uint16_t i = 0; i < phnum; i++) {
    int rc = elf16_read_phdr(ehdr, phdrs, vn, i, &phdr);
    if (rc < 0) {
      for (uint16_t j = 0; j < USER_SEG_PAGES; j++)
        mem_region_page_free(base_id + j);
      return rc;
    }
    if (phdr.p_type != PT_LOAD) continue;
    if (phdr.p_filesz == 0) continue;

    if (phdr.p_offset + phdr.p_filesz > file_size) {
      for (uint16_t j = 0; j < USER_SEG_PAGES; j++)
        mem_region_page_free(base_id + j);
      return -ENOEXEC;
    }

    uint32_t vaddr = phdr.p_vaddr;
    uint32_t remaining = phdr.p_filesz;
    uint32_t src_off = phdr.p_offset;

    if (vaddr + remaining > USER_STACK_BASE) {
      for (uint16_t j = 0; j < USER_SEG_PAGES; j++)
        mem_region_page_free(base_id + j);
      return -ENOMEM;
    }

    while (remaining > 0) {
      uint16_t pg_idx = (uint16_t)(vaddr / PAGE_SIZE);
      uint16_t pg_off = (uint16_t)(vaddr % PAGE_SIZE);
      uint16_t chunk = PAGE_SIZE - pg_off;
      if (chunk > remaining) chunk = (uint16_t)remaining;

      if (file_buf != NULL) {
        mem_region_page_write(base_id + pg_idx, pg_off, file_buf + src_off,
                              chunk);
      } else {
        long nread =
            mod_vfs.vnode_read(vn, base_id + pg_idx, pg_off, chunk, src_off);
        if (nread < 0 || (uint16_t)nread != chunk) {
          for (uint16_t j = 0; j < USER_SEG_PAGES; j++)
            mem_region_page_free(base_id + j);
          return (nread < 0) ? (int)nread : -ENOEXEC;
        }
      }
      src_off += chunk;
      vaddr += chunk;
      remaining -= chunk;
    }
  }

  /* Compute process segment from the 32-bit linear address of page 0. */
  uint32_t base_linear = mem_region_page_linear(base_id);
  uint16_t proc_seg = (uint16_t)(base_linear >> 4);
  uint16_t entry_ip = (uint16_t)ehdr->e_entry;
  uint16_t brk_pages = (uint16_t)((brk_base + PAGE_SIZE - 1u) / PAGE_SIZE);

  /* User stack: SS=proc_seg, SP is segment-relative.
   * Stack lives in the last page of the 64 KB segment. argc/argv are
   * placed near the top, then HW/SW frames below them. After ISR restore
   * pops both frames, SP points to argc. */
  uint32_t user_sp_top = USER_SEG_BYTES;

  /* ── Build argc/argv on user stack ────────────────────────────────────
   * Layout (addresses grow upward, stack grows down):
   *   [user_sp_top - args_size]  argc          ← SP after IRET
   *   [+2]                       argv[0] ptr
   *   [+4]                       argv[1] ptr
   *   ...
   *   [+2+argc*2]                NULL
   *   [+4+argc*2]                "string0\0"
   *   ...                        "stringN\0"
   *   [user_sp_top]              (HW frame starts here)
   */
#define ARGV_MAX 16
  int argc = 0;
  while (argv && argv[argc]) {
    if (argc >= ARGV_MAX) break;
    argc++;
  }

  {
    /* Measure total string bytes */
    uint16_t str_total = 0;
    for (int i = 0; i < argc; i++) str_total += (uint16_t)strlen(argv[i]) + 1;

    /* Frame: argc(2) + argv[](argc*2) + NULL(2) + strings */
    uint16_t frame_size = (uint16_t)(2 + (uint16_t)argc * 2 + 2 + str_total);
    /* Align to 2 bytes */
    frame_size = (frame_size + 1) & ~(uint16_t)1;

    user_sp_top -= frame_size;

    /* Write argc */
    uint16_t pos = (uint16_t)user_sp_top;
    uint16_t argc16 = (uint16_t)argc;
    mem_region_page_write(base_id + pos / PAGE_SIZE, pos % PAGE_SIZE, &argc16,
                          2);
    pos += 2;

    /* argv pointers start here; strings start after argv[]+NULL */
    uint16_t argv_pos = pos;
    uint16_t str_pos =
        (uint16_t)(pos + (uint16_t)argc * 2 + 2); /* after NULL */

    for (int i = 0; i < argc; i++) {
      /* Write argv[i] pointer (segment-relative offset of string) */
      mem_region_page_write(base_id + argv_pos / PAGE_SIZE,
                            argv_pos % PAGE_SIZE, &str_pos, 2);
      argv_pos += 2;

      /* Write string to user pages, handling page boundaries */
      uint16_t slen = (uint16_t)strlen(argv[i]) + 1;
      const uint8_t *src = (const uint8_t *)argv[i];
      uint16_t rem = slen;
      uint16_t dst = str_pos;
      while (rem > 0) {
        uint16_t pg_off = dst % PAGE_SIZE;
        uint16_t chunk = PAGE_SIZE - pg_off;
        if (chunk > rem) chunk = rem;
        mem_region_page_write(base_id + dst / PAGE_SIZE, pg_off, src, chunk);
        src += chunk;
        dst += chunk;
        rem -= chunk;
      }
      str_pos += slen;
    }

    /* Write NULL terminator for argv[] */
    uint16_t null16 = 0;
    mem_region_page_write(base_id + argv_pos / PAGE_SIZE, argv_pos % PAGE_SIZE,
                          &null16, 2);
  }

  /* Build the 24-byte frame on the user stack (proc_seg segment).
   * This is what the ISR restore path pops: GP regs + IRET frame.
   * Frame grows downward within the segment. */
  uint16_t hw_frame[3];
  hw_frame[0] = entry_ip; /* IP */
  hw_frame[1] = proc_seg; /* CS = process segment */
  hw_frame[2] = 0x0200;   /* FLAGS: IF=1 */

  uint16_t sw_frame[9];
  sw_frame[0] = proc_seg; /* ES = process segment */
  sw_frame[1] = proc_seg; /* DS = process segment */
  sw_frame[2] = 0;        /* BP */
  sw_frame[3] = 0;        /* DI */
  sw_frame[4] = 0;        /* SI */
  sw_frame[5] = 0;        /* DX */
  sw_frame[6] = 0;        /* CX */
  sw_frame[7] = 0;        /* BX */
  sw_frame[8] = 0;        /* AX */

  /* Write hardware frame (6B) at top of user stack */
  uint16_t hw_off_seg = (uint16_t)(user_sp_top - sizeof(hw_frame));
  uint32_t hw_pos = base_linear + hw_off_seg;
  uint16_t hw_pg = (uint16_t)((hw_pos - base_linear) / PAGE_SIZE);
  uint16_t hw_off = (uint16_t)((hw_pos - base_linear) % PAGE_SIZE);
  mem_region_page_write(base_id + hw_pg, hw_off, hw_frame, sizeof(hw_frame));

  /* Write software frame (18B) below hardware frame */
  uint16_t sw_off_seg = hw_off_seg - sizeof(sw_frame);
  uint32_t sw_pos = base_linear + sw_off_seg;
  uint16_t sw_pg = (uint16_t)((sw_pos - base_linear) / PAGE_SIZE);
  uint16_t sw_off = (uint16_t)((sw_pos - base_linear) % PAGE_SIZE);
  mem_region_page_write(base_id + sw_pg, sw_off, sw_frame, sizeof(sw_frame));

  /* user_SP points to top of GP frame (segment-relative) */
  uint16_t user_sp = sw_off_seg;

  /* Store new user SS:SP in PCB fields for the exec_pending path
   * in trap.S (which builds the kernel stack frame after C returns).
   * Also write the frame directly — safe for the initial exec from
   * kmain where the kernel stack isn't in use.  For the syscall case,
   * trap.S overwrites it with the same values from the PCB fields. */
  p->exec_user_ss = proc_seg;
  p->exec_user_sp = user_sp;
  {
    uint16_t ksp = p->kernel_stack_top;
    uint16_t *kstack = (uint16_t *)(uintptr_t)ksp;
    *--kstack = proc_seg; /* user_SS */
    *--kstack = user_sp;  /* user_SP */
    /* Reserve 34-byte vfork-save slot (matches trap.S subw $34,%sp).
     * The shared restore path always addw $34,%sp before popping
     * user_SP/user_SS, so every saved kernel SP must point at the
     * post-reserve position. */
    kstack -= 17;
    p->sp = (uint32_t)(uintptr_t)kstack;
  }

  /* Track logical occupancy by segment page index:
   *   - low pages up to brk_current belong to the image / heap window
   *   - the last page is the user stack
   * The whole 16-page segment is owned separately via image.data. */
  for (uint16_t i = 0; i < brk_pages; i++) proc_track_page(p, i, base_id + i);
  proc_track_page(p, USER_STACK_PAGE, base_id + USER_STACK_PAGE);

  p->image.text =
      proc_image_segment_make((void *)(uintptr_t)(uint16_t)base_linear, mem_end,
                              PPAP_MEM_RAM_TEXT, PROC_IMAGE_SEG_EXECUTABLE);
  p->image.text.base_page = base_id;
  /* The full 16-page user segment is owned here; user_pages[] only tracks
   * which segment pages are logically in use. */
  p->image.data = proc_image_segment_make(
      (void *)(uintptr_t)(uint16_t)base_linear, USER_SEG_BYTES,
      PPAP_MEM_RAM_DATA, PROC_IMAGE_SEG_WRITABLE | PROC_IMAGE_SEG_OWNED);
  p->image.data.base_page = base_id;
  p->image.entry = (uintptr_t)entry_ip;
  p->brk_base = brk_base;
  p->brk_current = brk_base;
  p->ticks_remaining = PROC_DEFAULT_TICKS;

  return 0;
}

static int elf16_load(pcb_t *p, const uint8_t *file_buf, uint32_t file_size,
                      const cpu_ops_t *cpu_ops, void *cpu_state,
                      const char *const *argv, uint32_t flags) {
  const elf32_ehdr_t *ehdr = (const elf32_ehdr_t *)file_buf;

  /* Validate program header table */
  if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0) return -ENOEXEC;
  if (ehdr->e_phentsize != sizeof(elf32_phdr_t)) return -ENOEXEC;
  if (ehdr->e_phnum > ELF16_MAX_PHDRS) return -ENOEXEC;
  if (ehdr->e_phoff + ehdr->e_phnum * sizeof(elf32_phdr_t) > file_size)
    return -ENOEXEC;

  return elf16_load_from_headers(
      p, ehdr, (const elf32_phdr_t *)(file_buf + ehdr->e_phoff), ehdr->e_phnum,
      file_buf, NULL, file_size, cpu_ops, cpu_state, argv, flags);
}

int elf16_load_vnode(pcb_t *p, vnode_t *vn, uint32_t file_size,
                     const cpu_ops_t *cpu_ops, void *cpu_state,
                     const char *const *argv, uint32_t flags) {
  elf32_ehdr_t ehdr;
  uint16_t phnum;
  uint32_t phbytes;
  long nread;

  if (file_size < sizeof(ehdr)) return -ENOEXEC;

  nread = elf16_read_near(vn, 0, &ehdr, sizeof(ehdr));
  if (nread < 0) return (int)nread;
  if ((uint16_t)nread != sizeof(ehdr)) return -ENOEXEC;
  if (!elf16_detect((const uint8_t *)&ehdr, file_size, "")) return -ENOEXEC;

  if (ehdr.e_phoff == 0 || ehdr.e_phnum == 0) return -ENOEXEC;
  if (ehdr.e_phentsize != sizeof(elf32_phdr_t)) return -ENOEXEC;

  phnum = ehdr.e_phnum;
  if (phnum > ELF16_MAX_PHDRS) return -ENOEXEC;

  phbytes = (uint32_t)phnum * sizeof(elf32_phdr_t);
  if (ehdr.e_phoff + phbytes > file_size) return -ENOEXEC;

  return elf16_load_from_headers(p, &ehdr, NULL, phnum, NULL, vn, file_size,
                                 cpu_ops, cpu_state, argv, flags);
}

/* ── Loader registration ──────────────────────────────────────────────── */

const loader_t elf16_loader = {
    .name = "elf16",
    .detect = elf16_detect,
    .load = elf16_load,
    .required_arch_id = CPU_ARCH_8086,
    .xip = 0,
};
