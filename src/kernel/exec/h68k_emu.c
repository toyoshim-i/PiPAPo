/*
 * h68k_emu.c — Shared Human68k m68k emulator code
 *
 * Contains the DOS call trap handler and helper functions shared by
 * X-format and R-format loaders when running Human68k binaries on
 * non-m68k hosts via the m68k emulator.
 */

#if !defined(__m68k__)

#include "h68k_emu.h"

#include <string.h>

#include "common/fcntl.h"
#include "kernel/cpu/ecpu_m68k.h"
#include "kernel/errno.h"
#include "kernel/subsys/h68k_util.h"
#include "kernel/syscall/syscall.h"

/* ── Helper functions ──────────────────────────────────────────────────── */

static inline uint16_t h68k_emu_ustack_u16(m68k_state_t *cpu, uint32_t offset) {
  return m68k_read16(cpu, cpu->a[7] + offset);
}

static inline uint32_t h68k_emu_ustack_u32(m68k_state_t *cpu, uint32_t offset) {
  return m68k_read32(cpu, cpu->a[7] + offset);
}

static inline uint32_t h68k_emu_align4(uint32_t x) { return (x + 3u) & ~3u; }

static void h68k_emu_putc(uint8_t ch) { sys_write(1, (const char *)&ch, 1); }

static uint8_t h68k_emu_getc(void) {
  uint8_t ch = 0;
  sys_read(0, (char *)&ch, 1);
  return ch;
}

static int h68k_emu_print(m68k_state_t *cpu, uint32_t guest_addr) {
  uint32_t i;
  for (i = 0; i < cpu->mem_size; i++) {
    uint8_t ch = m68k_read8(cpu, guest_addr + i);
    if (ch == 0) break;
    h68k_emu_putc(ch);
  }
  return 0;
}

static int h68k_emu_copy_str(m68k_state_t *cpu, uint32_t guest_addr, char *dst,
                             uint32_t dst_size) {
  if (!dst || dst_size == 0) return -1;

  for (uint32_t i = 0; i + 1 < dst_size; i++) {
    uint8_t ch = m68k_read8(cpu, guest_addr + i);
    dst[i] = (char)ch;
    if (ch == 0) return 0;
  }
  dst[dst_size - 1] = '\0';
  return -1;
}

static int h68k_emu_guest_path(m68k_state_t *cpu, uint32_t guest_addr,
                               char *path, uint32_t path_size) {
  char raw[128];
  if (h68k_emu_copy_str(cpu, guest_addr, raw, sizeof(raw)) < 0)
    return -(int)ENAMETOOLONG;
  if (h68k_translate_path(raw, path, (int)path_size) < 0)
    return -(int)ENAMETOOLONG;
  return 0;
}

static long h68k_emu_write_guest_to_fd(m68k_state_t *cpu, int fd,
                                       uint32_t guest_addr, uint32_t len) {
  uint8_t tmp[128];
  uint32_t done = 0;

  while (done < len) {
    uint32_t chunk = len - done;
    if (chunk > sizeof(tmp)) chunk = sizeof(tmp);
    for (uint32_t i = 0; i < chunk; i++)
      tmp[i] = m68k_read8(cpu, guest_addr + done + i);
    long wr = sys_write(fd, (const char *)tmp, chunk);
    if (wr < 0) return (done > 0) ? (long)done : wr;
    done += (uint32_t)wr;
    if ((uint32_t)wr < chunk) break;
  }
  return (long)done;
}

static long h68k_emu_read_fd_to_guest(m68k_state_t *cpu, int fd,
                                      uint32_t guest_addr, uint32_t len) {
  uint8_t tmp[128];
  uint32_t done = 0;

  while (done < len) {
    uint32_t chunk = len - done;
    if (chunk > sizeof(tmp)) chunk = sizeof(tmp);
    long rd = sys_read(fd, (char *)tmp, chunk);
    if (rd < 0) return (done > 0) ? (long)done : rd;
    if (rd == 0) break;
    for (uint32_t i = 0; i < (uint32_t)rd; i++)
      m68k_write8(cpu, guest_addr + done + i, tmp[i]);
    done += (uint32_t)rd;
    if ((uint32_t)rd < chunk) break;
  }
  return (long)done;
}

static uint32_t h68k_emu_malloc_avail(const h68k_emu_exec_state_t *st) {
  uint32_t end = st->block_end;
  if (end <= st->heap_next + H68K_MMB_HEADER_SIZE) return 0;
  return end - st->heap_next - H68K_MMB_HEADER_SIZE;
}

/* ── Trap handler ──────────────────────────────────────────────────────── */

int h68k_emu_trap_handler(cpu_state_t *state, int trap_type, uint32_t param,
                          void *ctx) {
  m68k_state_t *cpu = (m68k_state_t *)state;
  h68k_emu_exec_state_t *st = (h68k_emu_exec_state_t *)ctx;

  if (trap_type == CPU_TRAP_ILLEGAL) {
    uint16_t opcode = (uint16_t)param;
    if ((opcode & 0xFF00u) == 0xFF00u) {
      uint8_t func = (uint8_t)(opcode & 0xFFu);
      if (func >= 0x80u && func <= 0xAFu) func = (uint8_t)(func - 0x30u);

      switch (func) {
        case 0x00:   /* _EXIT */
        case 0x4C: { /* _EXIT2 */
          uint16_t code = h68k_emu_ustack_u16(cpu, 0);
          st->exit_code = (int16_t)code;
          return CPU_TRAP_EXIT;
        }

        case 0x4A: { /* _SETBLOCK */
          uint32_t block_addr = h68k_emu_ustack_u32(cpu, 0);
          uint32_t new_size = h68k_emu_ustack_u32(cpu, 4);
          uint32_t expected = cpu->a[0] + H68K_MMB_HEADER_SIZE;
          if (block_addr != expected) {
            cpu->d[0] = (uint32_t)(int32_t)-7;
            return CPU_TRAP_HANDLED;
          }
          uint32_t total_new = H68K_MMB_HEADER_SIZE + new_size;
          if (total_new > cpu->mem_size) {
            uint32_t avail =
                (cpu->mem_size > H68K_MMB_HEADER_SIZE)
                    ? ((cpu->mem_size - H68K_MMB_HEADER_SIZE) & 0x00FFFFFFu)
                    : 0;
            cpu->d[0] = 0x81000000u | avail;
            return CPU_TRAP_HANDLED;
          }
          st->block_end = total_new;
          if (st->heap_next > st->block_end) st->heap_next = st->block_end;
          m68k_write32(cpu, cpu->a[0] + 0x08u, st->block_end);
          m68k_write32(cpu, cpu->a[0] + 0x38u, st->block_end);
          cpu->a[1] = st->block_end;
          cpu->d[0] = 0;
          return CPU_TRAP_HANDLED;
        }

        case 0x48: { /* _MALLOC */
          uint32_t req = h68k_emu_ustack_u32(cpu, 0);
          if (req >= 0x01000000u) {
            cpu->d[0] = h68k_emu_malloc_avail(st);
            return CPU_TRAP_HANDLED;
          }

          uint32_t slot = H68K_EMU_MALLOC_MAX;
          for (uint32_t i = 0; i < H68K_EMU_MALLOC_MAX; i++) {
            if (st->mallocs[i].base == 0) {
              slot = i;
              break;
            }
          }
          if (slot == H68K_EMU_MALLOC_MAX) {
            cpu->d[0] = 0x82000000u;
            return CPU_TRAP_HANDLED;
          }

          uint32_t total = H68K_MMB_HEADER_SIZE + req;
          uint32_t base = h68k_emu_align4(st->heap_next);
          if (base + total > st->block_end) {
            cpu->d[0] = 0x81000000u | (h68k_emu_malloc_avail(st) & 0x00FFFFFFu);
            return CPU_TRAP_HANDLED;
          }

          st->mallocs[slot].base = base;
          st->mallocs[slot].nbytes = total;
          st->heap_next = h68k_emu_align4(base + total);

          m68k_write32(cpu, base + 0x00u, 0);
          m68k_write32(cpu, base + 0x04u, base);
          m68k_write32(cpu, base + 0x08u, base + total);
          m68k_write32(cpu, base + 0x0Cu, 0);

          cpu->d[0] = base + H68K_MMB_HEADER_SIZE;
          return CPU_TRAP_HANDLED;
        }

        case 0x49: { /* _MFREE */
          uint32_t block_addr = h68k_emu_ustack_u32(cpu, 0);
          for (uint32_t i = 0; i < H68K_EMU_MALLOC_MAX; i++) {
            if (st->mallocs[i].base != 0 &&
                st->mallocs[i].base + H68K_MMB_HEADER_SIZE == block_addr) {
              st->mallocs[i].base = 0;
              st->mallocs[i].nbytes = 0;
              cpu->d[0] = 0;
              return CPU_TRAP_HANDLED;
            }
          }
          cpu->d[0] = (uint32_t)(int32_t)-7;
          return CPU_TRAP_HANDLED;
        }

        case 0x01: { /* _GETCHAR (echo) */
          uint8_t ch = h68k_emu_getc();
          h68k_emu_putc(ch);
          cpu->d[0] = (uint32_t)ch;
          return CPU_TRAP_HANDLED;
        }

        case 0x02:   /* _PUTCHAR */
        case 0x04: { /* _COMOUT */
          uint8_t ch = (uint8_t)h68k_emu_ustack_u16(cpu, 0);
          h68k_emu_putc(ch);
          cpu->d[0] = (uint32_t)ch;
          return CPU_TRAP_HANDLED;
        }

        case 0x03: { /* _COMINP (raw read) */
          uint8_t ch = h68k_emu_getc();
          cpu->d[0] = (uint32_t)ch;
          return CPU_TRAP_HANDLED;
        }

        case 0x05: { /* _MOVE */
          uint8_t ch = (uint8_t)h68k_emu_ustack_u16(cpu, 0);
          h68k_emu_putc(ch);
          cpu->d[0] = 0;
          return CPU_TRAP_HANDLED;
        }

        case 0x08: { /* _INPOUT */
          uint16_t code = h68k_emu_ustack_u16(cpu, 0);
          if (code == 0x00FFu) {
            cpu->d[0] = 0;
          } else {
            h68k_emu_putc((uint8_t)code);
            cpu->d[0] = (uint32_t)code;
          }
          return CPU_TRAP_HANDLED;
        }

        case 0x09: { /* _PRINT */
          uint32_t str_addr = h68k_emu_ustack_u32(cpu, 0);
          cpu->d[0] = (uint32_t)h68k_emu_print(cpu, str_addr);
          return CPU_TRAP_HANDLED;
        }

        case 0x0C: { /* _KFLUSH */
          uint16_t mode = h68k_emu_ustack_u16(cpu, 0);
          switch (mode) {
            case 0x01: { /* like _GETCHAR */
              uint8_t ch = h68k_emu_getc();
              h68k_emu_putc(ch);
              cpu->d[0] = (uint32_t)ch;
              break;
            }
            case 0x07: { /* raw wait */
              uint8_t ch = h68k_emu_getc();
              cpu->d[0] = (uint32_t)ch;
              break;
            }
            case 0x06: /* no buffered input */
            case 0x08:
            default:
              cpu->d[0] = 0;
              break;
          }
          return CPU_TRAP_HANDLED;
        }

        case 0x10: { /* _CONCTRL */
          uint16_t sub = h68k_emu_ustack_u16(cpu, 0);
          switch (sub) {
            case 0x00: { /* putc */
              uint8_t ch = (uint8_t)h68k_emu_ustack_u16(cpu, 2);
              h68k_emu_putc(ch);
              cpu->d[0] = (uint32_t)ch;
              break;
            }
            case 0x01: { /* print */
              uint32_t str_addr = h68k_emu_ustack_u32(cpu, 2);
              cpu->d[0] = (uint32_t)h68k_emu_print(cpu, str_addr);
              break;
            }
            case 0x0A: /* check break */
            case 0x0B: /* fn-key mode */
            default:
              cpu->d[0] = 0;
              break;
          }
          return CPU_TRAP_HANDLED;
        }

        case 0x19: /* _CURDRV */
          cpu->d[0] = 0;
          return CPU_TRAP_HANDLED;

        case 0x1A: /* _ALLCLOSE */
          for (int fd = 3; fd < 16; fd++) sys_close(fd);
          cpu->d[0] = 0;
          return CPU_TRAP_HANDLED;

        case 0x20: { /* _SUPER */
          uint32_t ssp = h68k_emu_ustack_u32(cpu, 0);
          cpu->d[0] = (ssp == 0) ? cpu->a[7] : 0;
          return CPU_TRAP_HANDLED;
        }

        case 0x24: /* _FFLUSH */
          cpu->d[0] = 0;
          return CPU_TRAP_HANDLED;

        case 0x30: /* _VERNUM */
          cpu->d[0] = 0x36380302u;
          return CPU_TRAP_HANDLED;

        case 0x2A: /* _GETDATE (2026-01-01 Thu) */
          cpu->d[0] = 0x00045C21u;
          return CPU_TRAP_HANDLED;

        case 0x2C: /* _GETTIME */
          cpu->d[0] = 0;
          return CPU_TRAP_HANDLED;

        case 0x33: /* _BREAKCK */
          cpu->d[0] = 1;
          return CPU_TRAP_HANDLED;

        case 0x35: /* _INTVCG */
          cpu->d[0] = 0;
          return CPU_TRAP_HANDLED;

        case 0x39: { /* _MKDIR */
          char path[128];
          int err = h68k_emu_guest_path(cpu, h68k_emu_ustack_u32(cpu, 0), path,
                                        sizeof(path));
          if (err < 0) {
            cpu->d[0] = (uint32_t)h68k_errno(err);
            return CPU_TRAP_HANDLED;
          }
          cpu->d[0] = (uint32_t)h68k_errno(sys_mkdir(path, 0755));
          return CPU_TRAP_HANDLED;
        }

        case 0x3A: { /* _RMDIR */
          char path[128];
          int err = h68k_emu_guest_path(cpu, h68k_emu_ustack_u32(cpu, 0), path,
                                        sizeof(path));
          if (err < 0) {
            cpu->d[0] = (uint32_t)h68k_errno(err);
            return CPU_TRAP_HANDLED;
          }
          cpu->d[0] = (uint32_t)h68k_errno(sys_rmdir(path));
          return CPU_TRAP_HANDLED;
        }

        case 0x3B: { /* _CHDIR */
          char path[128];
          int err = h68k_emu_guest_path(cpu, h68k_emu_ustack_u32(cpu, 0), path,
                                        sizeof(path));
          if (err < 0) {
            cpu->d[0] = (uint32_t)h68k_errno(err);
            return CPU_TRAP_HANDLED;
          }
          cpu->d[0] = (uint32_t)h68k_errno(sys_chdir(path));
          return CPU_TRAP_HANDLED;
        }

        case 0x3C: { /* _CREATE */
          char path[128];
          int err = h68k_emu_guest_path(cpu, h68k_emu_ustack_u32(cpu, 0), path,
                                        sizeof(path));
          if (err < 0) {
            cpu->d[0] = (uint32_t)h68k_errno(err);
            return CPU_TRAP_HANDLED;
          }
          cpu->d[0] = (uint32_t)h68k_errno(
              sys_open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644));
          return CPU_TRAP_HANDLED;
        }

        case 0x3D: { /* _OPEN */
          char path[128];
          int err = h68k_emu_guest_path(cpu, h68k_emu_ustack_u32(cpu, 0), path,
                                        sizeof(path));
          if (err < 0) {
            cpu->d[0] = (uint32_t)h68k_errno(err);
            return CPU_TRAP_HANDLED;
          }
          int flags;
          switch (h68k_emu_ustack_u16(cpu, 4) & 0x0Fu) {
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
          cpu->d[0] = (uint32_t)h68k_errno(sys_open(path, flags, 0644));
          return CPU_TRAP_HANDLED;
        }

        case 0x3E: { /* _CLOSE */
          int fd = (int)(int16_t)h68k_emu_ustack_u16(cpu, 0);
          cpu->d[0] = (uint32_t)h68k_errno(sys_close(fd));
          return CPU_TRAP_HANDLED;
        }

        case 0x3F: { /* _READ */
          int fd = (int)(int16_t)h68k_emu_ustack_u16(cpu, 0);
          uint32_t buf = h68k_emu_ustack_u32(cpu, 2);
          uint32_t len = h68k_emu_ustack_u32(cpu, 6);
          cpu->d[0] = (uint32_t)h68k_errno(
              h68k_emu_read_fd_to_guest(cpu, fd, buf, len));
          return CPU_TRAP_HANDLED;
        }

        case 0x40: { /* _WRITE */
          int fd = (int)(int16_t)h68k_emu_ustack_u16(cpu, 0);
          uint32_t buf = h68k_emu_ustack_u32(cpu, 2);
          uint32_t len = h68k_emu_ustack_u32(cpu, 6);
          cpu->d[0] = (uint32_t)h68k_errno(
              h68k_emu_write_guest_to_fd(cpu, fd, buf, len));
          return CPU_TRAP_HANDLED;
        }

        case 0x41: { /* _DELETE */
          char path[128];
          int err = h68k_emu_guest_path(cpu, h68k_emu_ustack_u32(cpu, 0), path,
                                        sizeof(path));
          if (err < 0) {
            cpu->d[0] = (uint32_t)h68k_errno(err);
            return CPU_TRAP_HANDLED;
          }
          cpu->d[0] = (uint32_t)h68k_errno(sys_unlink(path));
          return CPU_TRAP_HANDLED;
        }

        case 0x42: { /* _SEEK */
          int fd = (int)(int16_t)h68k_emu_ustack_u16(cpu, 0);
          int32_t off = (int32_t)h68k_emu_ustack_u32(cpu, 2);
          uint16_t whence = h68k_emu_ustack_u16(cpu, 6);
          cpu->d[0] = (uint32_t)h68k_errno(sys_lseek(fd, off, whence));
          return CPU_TRAP_HANDLED;
        }

        case 0x44: { /* _IOCTRL */
          uint16_t mode = h68k_emu_ustack_u16(cpu, 0);
          int fd = (int)(int16_t)h68k_emu_ustack_u16(cpu, 2);
          if (mode == 0 && fd >= 0 && fd <= 2)
            cpu->d[0] = 0x80C1u;
          else
            cpu->d[0] = 0;
          return CPU_TRAP_HANDLED;
        }

        case 0x47: { /* _CURDIR */
          uint32_t buf = h68k_emu_ustack_u32(cpu, 2);
          char cwd[128];
          long r = sys_getcwd(cwd, sizeof(cwd));
          if (r < 0) {
            cpu->d[0] = (uint32_t)h68k_errno(r);
            return CPU_TRAP_HANDLED;
          }
          const char *p = cwd;
          if (*p == '/') p++;
          uint32_t i = 0;
          while (p[i] && i < 63) {
            char ch = (p[i] == '/') ? '\\' : p[i];
            m68k_write8(cpu, buf + i, (uint8_t)ch);
            i++;
          }
          m68k_write8(cpu, buf + i, 0);
          cpu->d[0] = 0;
          return CPU_TRAP_HANDLED;
        }

        case 0x51: /* _GETPDB */
          cpu->d[0] =
              cpu->a[0] ? cpu->a[0] : (cpu->a[0] + H68K_MMB_HEADER_SIZE);
          return CPU_TRAP_HANDLED;

        case 0x56: { /* _RENAME */
          char old_path[128], new_path[128];
          int err = h68k_emu_guest_path(cpu, h68k_emu_ustack_u32(cpu, 0),
                                        old_path, sizeof(old_path));
          if (err < 0) {
            cpu->d[0] = (uint32_t)h68k_errno(err);
            return CPU_TRAP_HANDLED;
          }
          err = h68k_emu_guest_path(cpu, h68k_emu_ustack_u32(cpu, 4), new_path,
                                    sizeof(new_path));
          if (err < 0) {
            cpu->d[0] = (uint32_t)h68k_errno(err);
            return CPU_TRAP_HANDLED;
          }
          cpu->d[0] = (uint32_t)h68k_errno(sys_rename(old_path, new_path));
          return CPU_TRAP_HANDLED;
        }

        default:
          cpu->d[0] = (uint32_t)h68k_errno(-(int32_t)ENOSYS);
          return CPU_TRAP_HANDLED;
      }
    }
    st->exit_code = 132;
    return CPU_TRAP_EXIT;
  }

  if (trap_type == CPU_TRAP_HALT) {
    st->exit_code = 0;
    return CPU_TRAP_EXIT;
  }

  return CPU_TRAP_UNHANDLED;
}

/* ── Kernel-mode entry point ───────────────────────────────────────────── */

void h68k_emu_run_process(void) {
  pcb_t *p = current;
  h68k_emu_exec_state_t *st = (h68k_emu_exec_state_t *)p->subsys_data;
  st->exit_code = 0;
  ecpu_m68k_ops.run((cpu_state_t *)&st->m68k);
  sys_exit((long)st->exit_code);
}

#endif /* !defined(__m68k__) */
