/*
 * target_pcxt.c — PC/XT target hooks for the PPAP kernel
 *
 * Isolated segment split.  Core + VFS modules in separate segments.
 * Reads mod_info from stage2, initializes segment manager, patches
 * far pointer tables.  Chains BIOS INT 08h for floppy motor timeout.
 */

#include "kernel/common/boot_params.h"
#include "kernel/common/core/seg.h"
#include "kernel/common/mod/mod_vfs.h"
#include "kernel/core/driver/rtc_cmos.h"
#include "kernel/core/driver/timer_pit.h"
#include "kernel/core/syscall/syscall.h"
#include "target/target.h"

/* Page pool base: set from stage2's mod_info before mm_init(). */
uint32_t i16_page_pool_base = 0x10000ul;  /* safe default */

#define VFS_DATA_UPPER_BSS_BASE 0xC000u
#define VFS_DATA_UPPER_BSS_SIZE 0x2000u

/* isa-debug-exit I/O port — requires QEMU -device isa-debug-exit */
#define ISA_DEBUG_EXIT_PORT 0x0501u

/* Far pointer tables from the stub assembly files */
extern uint16_t vfs_fptrs[];  /* in core: caller stubs for VFS */

/* Boot device parameters — read from mod_info, used by bios_blk.
 * Declarations + mod_info_t layout live in kernel/common/boot_params.h. */
uint8_t i16_boot_drive;
uint16_t i16_ufs_base_sector;
uint16_t i16_dev_spt;
uint16_t i16_dev_heads;
uint32_t i16_dev_sectors;

/*
 * VFS module header layout (at offset 0 of VFS binary):
 *   [0] uint16_t magic (0x5646 = "VF")
 *   [2] uint16_t entry_count
 *   [4] uint16_t entry_offsets[entry_count]
 */
#define VFS_HDR_MAGIC 0x5646u

/* Read a 16-bit word from seg:offset using ES:BX */
static uint16_t far_read16(uint16_t seg, uint16_t off) {
  uint16_t val;
  __asm__ volatile(
    "push %%es\n\t"
    "mov  %1, %%es\n\t"
    "mov  %%es:(%%bx), %0\n\t"
    "pop  %%es"
    : "=a"(val)
    : "r"(seg), "b"(off)
    : "memory"
  );
  return val;
}

/* Forward-declare every far-call entry label core_entries.S emits.  We only
 * ever take `&foo_entry` to populate the core_fptrs table, never invoke
 * them from C — the declarations are function prototypes (not extern
 * variables) so the list can be auto-generated from mod_core.inc and the
 * compiler still catches accidental invocation. */
#define MOD_CORE_ENTRY(name, idx) void name##_entry(void);
#include "kernel/common/mod/mod_core.inc"
#undef MOD_CORE_ENTRY

static void patch_vfs_fptrs(uint16_t vfs_seg) {
  if (far_read16(vfs_seg, 0) != VFS_HDR_MAGIC) {
    mod_vfs.klogf("SEG: VFS header magic mismatch!\n");
    return;
  }

  uint16_t count = far_read16(vfs_seg, 2);

  /* Patch core→VFS far pointers (in core's vfs_fptrs table) */
  for (uint16_t i = 0; i < count; i++) {
    vfs_fptrs[i * 2]     = far_read16(vfs_seg, 4 + i * 2);
    vfs_fptrs[i * 2 + 1] = vfs_seg;
  }

  /* Patch VFS→core far pointers (in VFS's core_fptrs table at DS=0).
   * The core_fptrs address is stored after the entry offsets
   * in the VFS header (at offset 4 + count*2). */
  uint16_t core_fptrs_addr = far_read16(vfs_seg, 4 + count * 2);
  uint16_t core_seg = seg_get(MOD_ID_CORE);
  volatile uint16_t *cfp = (volatile uint16_t *)(uintptr_t)core_fptrs_addr;

  /* Core entry offsets — order MUST match mod_core.inc indices. */
#define PATCH_CORE(idx, sym) \
  cfp[(idx)*2] = (uint16_t)(uintptr_t)&sym##_entry; \
  cfp[(idx)*2+1] = core_seg
  PATCH_CORE( 0, kmem_alloc);
  PATCH_CORE( 1, kmem_free);
  PATCH_CORE( 2, kmem_free_count);
  PATCH_CORE( 3, kmem_pool_init);
  PATCH_CORE( 4, kmutex_init);
  PATCH_CORE( 5, kmutex_lock);
  PATCH_CORE( 6, kmutex_release_owned);
  PATCH_CORE( 7, kmutex_try_lock);
  PATCH_CORE( 8, kmutex_unlock);
  PATCH_CORE( 9, page_read);
  PATCH_CORE(10, page_write);
  PATCH_CORE(11, page_zero);
  PATCH_CORE(12, region_alloc);
  PATCH_CORE(13, region_free);
  PATCH_CORE(14, region_free_bytes);
  PATCH_CORE(15, region_total_bytes);
  PATCH_CORE(16, sched_block_current);
  PATCH_CORE(17, sched_get_ticks);
  PATCH_CORE(18, sched_sleep_current);
  PATCH_CORE(19, sched_sleep_current_unlock);
  PATCH_CORE(20, sched_switch);
  PATCH_CORE(21, sched_wakeup);
  PATCH_CORE(22, subsys_read_proc);
  PATCH_CORE(23, time_now_sec);
#undef PATCH_CORE
}

static void seg_init_modules(void) {
  volatile mod_info_t *info = (volatile mod_info_t *)MOD_INFO_ADDR;

  /* Page pool start: stage2 stores it in mod[2].segment */
  if (info->mod[2].segment) {
    i16_page_pool_base = (uint32_t)info->mod[2].segment << 4;
  }

  /* Boot device parameters */
  i16_boot_drive = info->boot_dev;
  i16_ufs_base_sector = info->ufs_base;
  i16_dev_spt = info->dev_spt;
  i16_dev_heads = info->dev_heads;
  i16_dev_sectors = info->dev_sectors;

  /* Module 0: core — already running in far CS, register segment */
  if (info->count >= 1) {
    seg_register(MOD_ID_CORE, info->mod[0].segment);
  }

  /* Module 1: VFS */
  if (info->count >= 2) {
    uint16_t vfs_seg = info->mod[1].segment;
    seg_register(MOD_ID_VFS, vfs_seg);
    patch_vfs_fptrs(vfs_seg);
  }
}

/* ── Target hooks ────────────────────────────────────────────────────────── */

/* ── Panic / unexpected interrupt dump ───────────────────────────────────── */

/* Panic frame, written by the asm stubs and read by panic_dump_c.
 * Lives in the core data segment.  All fields are uint16_t.  The C
 * definition below replaces the old asm-side `.data ... panic_frame:
 * .word 0 x15` reservation so target_pcxt.c does not need an extern
 * to see a symbol that is defined (and only used) in this file. */
struct panic_frame {
  uint16_t vec;
  uint16_t ax, bx, cx, dx;
  uint16_t si, di, bp;
  uint16_t ds, es, ss, sp;
  uint16_t ip, cs, flags;
};
struct panic_frame panic_frame;

/* -- Panic output helpers (write to COM1 and VGA simultaneously) ----------- */

static uint16_t panic_vga_pos;  /* byte offset within B800: */

static void panic_com1_putc(char c) {
  __asm__ volatile (
    "mov $0x03F8, %%dx\n\t"
    "out %%al, %%dx"
    : : "a"(c) : "dx"
  );
}

/* Scroll the VGA text screen up by one row (rows 1..24 → 0..23, blank 24). */
static void panic_vga_scroll(void) {
  __asm__ volatile (
    "push %%es\n\t"
    "push %%ds\n\t"
    "mov  $0xB800, %%ax\n\t"
    "mov  %%ax, %%es\n\t"
    "mov  %%ax, %%ds\n\t"
    "xor  %%di, %%di\n\t"
    "mov  $160, %%si\n\t"
    "mov  $1920, %%cx\n\t"      /* 24 rows × 80 cols */
    "cld\n\t"
    "rep movsw\n\t"
    "mov  $0x0720, %%ax\n\t"    /* blank: light grey space */
    "mov  $80, %%cx\n\t"
    "rep stosw\n\t"
    "pop  %%ds\n\t"
    "pop  %%es"
    : : : "ax", "cx", "si", "di", "memory"
  );
}

static void panic_vga_advance_row(void) {
  panic_vga_pos = (panic_vga_pos / 160u + 1u) * 160u;
  if (panic_vga_pos >= 80u * 25u * 2u) {
    panic_vga_scroll();
    panic_vga_pos = 80u * 24u * 2u;  /* stay on last row */
  }
}

static void panic_vga_putc(char c) {
  if (c == '\r') { panic_vga_pos = (panic_vga_pos / 160u) * 160u; return; }
  if (c == '\n') { panic_vga_advance_row(); return; }
  __asm__ volatile (
    "push %%es\n\t"
    "mov  $0xB800, %%ax\n\t"
    "mov  %%ax, %%es\n\t"
    "mov  %1, %%di\n\t"
    "mov  %0, %%al\n\t"
    "mov  $0x07, %%ah\n\t"      /* light grey on black */
    "stosw\n\t"
    "pop  %%es"
    : : "r"((uint8_t)c), "r"(panic_vga_pos)
    : "ax", "di", "memory"
  );
  panic_vga_pos += 2;
  /* If we ran past the end of a row mid-line, treat as auto-newline. */
  if (panic_vga_pos >= 80u * 25u * 2u) {
    panic_vga_scroll();
    panic_vga_pos = 80u * 24u * 2u;
  }
}

static void panic_putc(char c) { panic_com1_putc(c); panic_vga_putc(c); }

static void panic_puts(const char *s) {
  while (*s) panic_putc(*s++);
}

static void panic_hex4(uint8_t nyb) {
  panic_putc((char)(nyb < 10 ? '0' + nyb : 'A' + nyb - 10));
}

static void panic_hex8(uint8_t v) {
  panic_hex4((uint8_t)(v >> 4));
  panic_hex4((uint8_t)(v & 0x0F));
}

static void panic_hex16(uint16_t v) {
  panic_hex8((uint8_t)(v >> 8));
  panic_hex8((uint8_t)(v & 0xFF));
}

/* Resume VGA writes right after the last non-blank row so the dump
 * appears below the existing kernel console output.  Scans B800:
 * bottom-up for the last row whose chars are not all spaces, then
 * starts on the row after it. */
static void panic_vga_init(void) {
  int row;
  for (row = 24; row >= 0; row--) {
    uint8_t blank;
    __asm__ volatile (
      "push %%es\n\t"
      "mov  $0xB800, %%ax\n\t"
      "mov  %%ax, %%es\n\t"
      "mov  %1, %%di\n\t"
      "mov  $80, %%cx\n\t"
      "mov  $1, %%al\n\t"     /* assume blank */
      "1:\n\t"
      "mov  %%es:(%%di), %%ah\n\t"
      "cmp  $0x20, %%ah\n\t"
      "je   2f\n\t"
      "xor  %%al, %%al\n\t"   /* not blank */
      "jmp  3f\n\t"
      "2: add $2, %%di\n\t"
      "loop 1b\n\t"
      "3:\n\t"
      "mov  %%al, %0\n\t"
      "pop  %%es"
      : "=r"(blank)
      : "r"((uint16_t)(row * 160u))
      : "ax", "cx", "di", "memory"
    );
    if (!blank) break;
  }
  /* row is the last non-blank row (or -1 if screen is entirely blank).
   * Start the dump on the row after that (clamped to 24). */
  int dump_row = row + 1;
  if (dump_row > 24) dump_row = 24;
  if (dump_row < 0)  dump_row = 0;
  panic_vga_pos = (uint16_t)(dump_row * 160);
}

/* -- Register dump --------------------------------------------------------- */

static void panic_field(const char *name, uint16_t val) {
  panic_puts(name);
  panic_hex16(val);
}

/* Print the register dump from panic_frame.  Both the unexpected-INT
 * and warm-reboot paths use this so the format is identical. */
static void panic_dump_regs(void) {
  panic_field("CS:IP=", panic_frame.cs);
  panic_putc(':'); panic_hex16(panic_frame.ip);
  panic_field(" FLAGS=", panic_frame.flags);
  panic_puts("\r\n");
  panic_field("AX=", panic_frame.ax);
  panic_field(" BX=", panic_frame.bx);
  panic_field(" CX=", panic_frame.cx);
  panic_field(" DX=", panic_frame.dx);
  panic_puts("\r\n");
  panic_field("SI=", panic_frame.si);
  panic_field(" DI=", panic_frame.di);
  panic_field(" BP=", panic_frame.bp);
  panic_field(" SP=", panic_frame.sp);
  panic_puts("\r\n");
  panic_field("DS=", panic_frame.ds);
  panic_field(" ES=", panic_frame.es);
  panic_field(" SS=", panic_frame.ss);
  panic_puts("\r\n");
}

void panic_dump_c(void) {
  panic_vga_init();
  panic_puts("\r\nPANIC: unexpected INT ");
  panic_hex8((uint8_t)panic_frame.vec);
  panic_puts("\r\n");
  panic_dump_regs();
  target_may_poweroff(1);
  for (;;) __asm__ volatile ("cli\n hlt");
}

/* -- Asm panic stubs ------------------------------------------------------- *
 *
 * Per-vector stub does:  movw $vec, %cs:panic_frame_vec ; jmp int_panic_common
 * (8086 has no PUSH imm; mov-imm-to-mem avoids clobbering any register so we
 *  can capture the full faulting register state in panic_common.)
 *
 * panic_common saves all regs and segments to the panic_frame, then switches
 * to a private panic stack in the core segment and calls panic_dump_c.
 *
 * Hardware-pushed frame on entry: IP, CS, FLAGS at SS:[SP], +2, +4.
 *
 * Vectors covered: 0,1,2,3,4,5,6,7 (CPU exceptions on 8086/286+) plus
 * 0x18 (ROM BASIC) and 0x19 (BIOS bootstrap — catches stray reboots).
 * Vectors NOT touched (must keep BIOS): 08h IRQ0 (chained), 09h IRQ1 kbd,
 * 0Eh IRQ6 floppy, 10h-17h / 1Ah-1Fh BIOS services and parameter tables.
 */
void int_panic_v00(void);
void int_panic_v01(void);
void int_panic_v02(void);
void int_panic_v03(void);
void int_panic_v04(void);
void int_panic_v05(void);
void int_panic_v06(void);
void int_panic_v07(void);
void int_panic_v18(void);
void int_panic_v19(void);
__asm__(
  ".code16\n"
  /* Each stub records the vector # in a CS-accessible scratch slot
   * (no register clobber).  int_panic_common then copies it into
   * panic_frame.vec via DS once DS has been re-pointed at the kernel
   * data segment. */
  "int_panic_v00: movw $0x00, %cs:panic_scratch_vec; jmp int_panic_common\n"
  "int_panic_v01: movw $0x01, %cs:panic_scratch_vec; jmp int_panic_common\n"
  "int_panic_v02: movw $0x02, %cs:panic_scratch_vec; jmp int_panic_common\n"
  "int_panic_v03: movw $0x03, %cs:panic_scratch_vec; jmp int_panic_common\n"
  "int_panic_v04: movw $0x04, %cs:panic_scratch_vec; jmp int_panic_common\n"
  "int_panic_v05: movw $0x05, %cs:panic_scratch_vec; jmp int_panic_common\n"
  "int_panic_v06: movw $0x06, %cs:panic_scratch_vec; jmp int_panic_common\n"
  "int_panic_v07: movw $0x07, %cs:panic_scratch_vec; jmp int_panic_common\n"
  "int_panic_v18: movw $0x18, %cs:panic_scratch_vec; jmp int_panic_common\n"
  "int_panic_v19: movw $0x19, %cs:panic_scratch_vec; jmp int_panic_common\n"
  "int_panic_common:\n"
  "  cli\n"
  /* Stash AX in a CS-accessible scratch slot so we can free up AX to
   * load DS=0 (the kernel data segment) before touching panic_frame. */
  "  mov %ax, %cs:panic_scratch_ax\n"
  "  xor %ax, %ax\n"
  "  mov %ax, %ds\n"
  "  mov %cs:panic_scratch_ax, %ax\n"
  /* Copy the stub-recorded vector # into panic_frame.vec via DS. */
  "  push %ax\n"
  "  mov %cs:panic_scratch_vec, %ax\n"
  "  mov %ax, panic_frame\n"
  "  pop %ax\n"
  /* Now save all GP regs to panic_frame via DS (default segment). */
  "  mov %ax, panic_frame +  2\n"
  "  mov %bx, panic_frame +  4\n"
  "  mov %cx, panic_frame +  6\n"
  "  mov %dx, panic_frame +  8\n"
  "  mov %si, panic_frame + 10\n"
  "  mov %di, panic_frame + 12\n"
  "  mov %bp, panic_frame + 14\n"
  /* Original DS was overwritten — recover it from the scratch we'll
   * borrow next.  For now we report DS as 0 (the value we just set),
   * which is the kernel DS regardless of the faulting context. */
  "  mov %ds, %ax\n"
  "  mov %ax, panic_frame + 16\n"
  "  mov %es, %ax\n"
  "  mov %ax, panic_frame + 18\n"
  "  mov %ss, %ax\n"
  "  mov %ax, panic_frame + 20\n"
  "  mov %sp, panic_frame + 22\n"
  /* Read IP/CS/FLAGS from the faulting stack (still SS:SP). */
  "  mov %sp, %bp\n"
  "  mov %ss:(%bp),  %ax\n"        /* IP */
  "  mov %ax, panic_frame + 24\n"
  "  mov %ss:2(%bp), %ax\n"        /* CS */
  "  mov %ax, panic_frame + 26\n"
  "  mov %ss:4(%bp), %ax\n"        /* FLAGS */
  "  mov %ax, panic_frame + 28\n"
  /* Switch to private panic stack: SS=0 (kernel DS) so [bp] still
   * points at panic_frame area; SP at top of dedicated buffer. */
  "  xor %ax, %ax\n"
  "  mov %ax, %ss\n"
  "  mov $panic_stack_top, %sp\n"
  "  call panic_dump_c\n"
  "1: cli\n"
  "  hlt\n"
  "  jmp 1b\n"

  /* Scratch slots in .text (live in the far CS segment, reachable via
   * %cs: when DS is unknown).  Used by panic stubs / int_panic_common
   * to record vector # and free up AX without clobbering registers. */
  "panic_scratch_ax:  .word 0\n"
  "panic_scratch_vec: .word 0\n"

  /* Entry-time register snapshot.  Filled by boot.S as the very first
   * thing the kernel does, before any segreg is touched, so we capture
   * the state the previous wild-jump / reboot left us in.  Lives in
   * .text (CS-accessible) so the snapshot does not depend on DS. */
  ".globl entry_ax\n"
  ".globl entry_bx\n"
  ".globl entry_cx\n"
  ".globl entry_dx\n"
  ".globl entry_si\n"
  ".globl entry_di\n"
  ".globl entry_bp\n"
  ".globl entry_sp\n"
  ".globl entry_ds\n"
  ".globl entry_es\n"
  ".globl entry_ss\n"
  "entry_ax: .word 0\n"
  "entry_bx: .word 0\n"
  "entry_cx: .word 0\n"
  "entry_dx: .word 0\n"
  "entry_si: .word 0\n"
  "entry_di: .word 0\n"
  "entry_bp: .word 0\n"
  "entry_sp: .word 0\n"
  "entry_ds: .word 0\n"
  "entry_es: .word 0\n"
  "entry_ss: .word 0\n"

  ".data\n"
  "  .lcomm panic_stack, 256\n"
  "panic_stack_top:\n"
  ".text\n"
);

static void install_int_panic(void) {
  volatile uint16_t *ivt = (volatile uint16_t *)0;
  uint16_t cs = seg_get(MOD_ID_CORE);
  static const struct { uint8_t vec; void (*fn)(void); } stubs[] = {
    { 0x00, int_panic_v00 },
    { 0x01, int_panic_v01 },
    { 0x02, int_panic_v02 },
    { 0x03, int_panic_v03 },
    { 0x04, int_panic_v04 },
    { 0x05, int_panic_v05 },
    { 0x06, int_panic_v06 },
    { 0x07, int_panic_v07 },
    { 0x18, int_panic_v18 },
    { 0x19, int_panic_v19 },
  };
  for (unsigned i = 0; i < sizeof(stubs)/sizeof(stubs[0]); i++) {
    ivt[stubs[i].vec * 2 + 0] = (uint16_t)(uintptr_t)stubs[i].fn;
    ivt[stubs[i].vec * 2 + 1] = cs;
  }
}

/* ── Warm-reboot breadcrumb ──────────────────────────────────────────────── *
 *
 * Detects kernel re-entry without BIOS POST (wild jump / stray INT 19h /
 * Ctrl-Alt-Del / etc.) which would otherwise bypass the PANIC stubs.
 *
 * Cookie at 0040:00F0 (BIOS intra-app comm area, 16 bytes, unused by BIOS
 * and not touched by stage1/stage2).  BDA 0040:0072 is set to 0x1234 so
 * any subsequent reset is taken as a warm boot, preserving the cookie.
 */
#define BREADCRUMB_OFF      0x00F0u
#define BREADCRUMB_MAGIC    0xB00Bu
#define BDA_RESET_FLAG_OFF  0x0072u
#define BDA_RESET_WARM      0x1234u

static uint16_t bda_read16(uint16_t off) {
  uint16_t val;
  __asm__ volatile (
    "push %%es\n\t"
    "mov  $0x0040, %%ax\n\t"    /* ES = 0x0040 (BDA segment) */
    "mov  %%ax, %%es\n\t"
    "mov  %%es:(%%bx), %0\n\t"
    "pop  %%es"
    : "=a"(val) : "b"(off) : "memory"
  );
  return val;
}

static void bda_write16(uint16_t off, uint16_t val) {
  __asm__ volatile (
    "push %%es\n\t"
    "push %%ax\n\t"
    "mov  $0x0040, %%ax\n\t"    /* ES = 0x0040 (BDA segment) */
    "mov  %%ax, %%es\n\t"
    "pop  %%ax\n\t"
    "mov  %0, %%es:(%%bx)\n\t"
    "pop  %%es"
    : : "a"(val), "b"(off) : "memory"
  );
}

/* Copy the entry-time register snapshot from CS-resident scratch slots
 * (filled by boot.S before any segreg was touched) into panic_frame.
 * Called from boot.S after BSS clear / DS setup, before kmain. */
void copy_entry_snapshot(void);
__asm__(
  ".code16\n"
  ".globl copy_entry_snapshot\n"
  "copy_entry_snapshot:\n"
  "  mov %cs:entry_ax, %ax\n"
  "  mov %ax, panic_frame +  2\n"
  "  mov %cs:entry_bx, %ax\n"
  "  mov %ax, panic_frame +  4\n"
  "  mov %cs:entry_cx, %ax\n"
  "  mov %ax, panic_frame +  6\n"
  "  mov %cs:entry_dx, %ax\n"
  "  mov %ax, panic_frame +  8\n"
  "  mov %cs:entry_si, %ax\n"
  "  mov %ax, panic_frame + 10\n"
  "  mov %cs:entry_di, %ax\n"
  "  mov %ax, panic_frame + 12\n"
  "  mov %cs:entry_bp, %ax\n"
  "  mov %ax, panic_frame + 14\n"
  "  mov %cs:entry_ds, %ax\n"
  "  mov %ax, panic_frame + 16\n"
  "  mov %cs:entry_es, %ax\n"
  "  mov %ax, panic_frame + 18\n"
  "  mov %cs:entry_ss, %ax\n"
  "  mov %ax, panic_frame + 20\n"
  "  mov %cs:entry_sp, %ax\n"
  "  mov %ax, panic_frame + 22\n"
  "  mov %cs, %ax\n"
  "  mov %ax, panic_frame + 26\n"   /* CS = current core seg */
  "  ret\n"
);

static void check_warm_reboot(void) {
  /* Disable interrupts for the entire dump path so a stray IRQ on a
   * potentially-broken stack can't trigger a secondary fault. */
  __asm__ volatile ("cli");
  uint16_t magic = bda_read16(BREADCRUMB_OFF);
  if (magic == BREADCRUMB_MAGIC) {
    panic_vga_init();
    panic_puts("\r\nPANIC: warm reboot (kernel re-entered without BIOS POST)\r\n");
    panic_dump_regs();
    target_may_poweroff(1);
    for (;;) __asm__ volatile ("cli\n hlt");
  }
  bda_write16(BREADCRUMB_OFF, BREADCRUMB_MAGIC);
  bda_write16(BDA_RESET_FLAG_OFF, BDA_RESET_WARM);
}

static void clear_vfs_data_upper_bss(void) {
  volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)VFS_DATA_UPPER_BSS_BASE;
  for (uint16_t i = 0; i < VFS_DATA_UPPER_BSS_SIZE; i++) p[i] = 0;
}

void target_early_init(void) {
  check_warm_reboot();
  seg_init_modules();
  clear_vfs_data_upper_bss();
  install_int_panic();
  /* Register UART logger early so mm_init/proc_init messages are visible.
   * Far-call stubs are patched by seg_init_modules() above. */
  mod_vfs.notify(VFS_EVENT_MODULE_READY);
  mod_vfs.klogf("SEG: core=%x vfs=%x\n",
                seg_get(MOD_ID_CORE), seg_get(MOD_ID_VFS));

  /* Seed the kernel wallclock from the CMOS RTC.  After this, file
   * timestamps and clock_gettime return real wall-clock time instead
   * of seconds-since-boot.  On probe failure (no RTC, or every two
   * sweeps disagree) the clock stays at 0 — same behaviour as targets
   * that have no RTC at all. */
  uint32_t rtc_sec;
  if (cmos_rtc_read_epoch(&rtc_sec) == 0) {
    time_set_wallclock(rtc_sec);
    mod_vfs.klogf("RTC: seeded wallclock from CMOS (epoch=%lx)\n", rtc_sec);
  } else {
    mod_vfs.klogf("RTC: CMOS read failed; wallclock stays at epoch 0\n");
  }
}

void target_late_init(void)
{
  /* PIT timer deferred — BIOS INT 13h floppy driver needs the
   * original INT 08h timer for motor control. */
  mod_vfs.notify(VFS_EVENT_LATE_INIT);
}


int target_mount_rootfs(void)
{
  /* blkdev + bios_blk init done in VFS (pcxt_vfs_init.c VFS_EVENT_LATE_INIT).
   * Device name depends on boot drive: "fd0" for floppy, "hd0" for HDD. */
  const char *dev = (i16_boot_drive >= 0x80) ? "hd0" : "fd0";
  return mod_vfs.mount_ufs("/", 0, dev);
}

void target_post_mount(void)
{
  /* PIT timer is deferred — BIOS INT 13h floppy driver needs the
   * original INT 08h handler for motor timeout.  Timer will be
   * started after init exec completes (in kmain). */
}

void target_enable_deferred_timer(void)
{
  static uint8_t pit_started;

  if (!pit_started) {
    timer_init();
    pit_started = 1;
    mod_vfs.klogf("PIT: timer started\n");
  }
}

const char *target_name(void)
{
  return "pcxt";
}

uint32_t target_caps(void)
{
  return TARGET_CAP_REALUART;
}

/*
 * QEMU poweroff via isa-debug-exit device.
 *
 * Requires -device isa-debug-exit,iobase=0x501,iosize=2 on the QEMU
 * command line.  QEMU exits with code (value << 1) | 1.
 */
void target_may_poweroff(uint8_t status) {
  __asm__ volatile("outb %0, %1"
                   :
                   : "a"(status), "Nd"((uint16_t)ISA_DEBUG_EXIT_PORT));
  for (;;) __asm__ volatile("cli\n hlt");
}
