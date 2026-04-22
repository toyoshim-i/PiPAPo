/*
 * exec_args.c — Populate and read the captured execve arguments page.
 *
 * All page accesses go through mem_region_page_read / mem_region_page_write
 * so i16 (where pool pages live above 64 KB) and 32-bit targets share the
 * same code path.
 */

#include "kernel/core/exec/exec_args.h"

#include "common/errno.h"
#include "kernel/core/mm/mem_region.h"

/* ── Internal helpers ─────────────────────────────────────────────────── */

static uint16_t exec_args_read_off(page_id_t page, uint16_t table_off,
                                   int idx) {
  uint16_t value = 0;
  mem_region_page_read(page, (uint16_t)(table_off + idx * sizeof(uint16_t)),
                       &value, sizeof(value));
  return value;
}

static void exec_args_write_off(page_id_t page, uint16_t table_off, int idx,
                                uint16_t value) {
  mem_region_page_write(page, (uint16_t)(table_off + idx * sizeof(uint16_t)),
                        &value, sizeof(value));
}

static int exec_args_append(page_id_t page, uint16_t table_off,
                            uint16_t buf_off, uint16_t buf_size, uint8_t *count,
                            uint8_t max_entries, const char *str,
                            uint16_t len) {
  if (*count >= max_entries) return -E2BIG;
  uint16_t end = exec_args_read_off(page, table_off, *count);
  uint16_t need = (uint16_t)(len + 1u);
  uint16_t used = (uint16_t)(end - buf_off);
  if ((uint16_t)(buf_size - used) < need) return -E2BIG;
  mem_region_page_write(page, end, str, len);
  uint8_t nul = 0;
  mem_region_page_write(page, (uint16_t)(end + len), &nul, 1);
  *count = (uint8_t)(*count + 1);
  exec_args_write_off(page, table_off, *count, (uint16_t)(end + need));
  return 0;
}

static int exec_args_copy(page_id_t page, uint16_t table_off, int count,
                          int idx, char *out, uint16_t out_size) {
  if (idx < 0 || idx >= count) return -1;
  uint16_t start = exec_args_read_off(page, table_off, idx);
  uint16_t next = exec_args_read_off(page, table_off, idx + 1);
  uint16_t slen = (uint16_t)(next - start - 1u); /* exclude NUL */
  if ((uint16_t)(slen + 1u) > out_size) return -ENAMETOOLONG;
  mem_region_page_read(page, start, out, slen);
  out[slen] = '\0';
  return (int)slen;
}

static uint16_t exec_args_len(page_id_t page, uint16_t table_off, int count,
                              int idx) {
  if (idx < 0 || idx >= count) return 0;
  uint16_t start = exec_args_read_off(page, table_off, idx);
  uint16_t next = exec_args_read_off(page, table_off, idx + 1);
  return (uint16_t)(next - start - 1u);
}

/* ── Builder ─────────────────────────────────────────────────────────── */

void exec_args_init(exec_args_t *a, page_id_t page) {
  a->page = page;
  a->argc = 0;
  a->envc = 0;
  /* Zero the path byte so exec_args_path() finds an empty string. */
  uint8_t zero = 0;
  mem_region_page_write(page, EXEC_ARGS_PATH_OFF, &zero, 1);
  /* Seed the first offset-table entry for each buffer. */
  exec_args_write_off(page, EXEC_ARGS_ARGV_TBL_OFF, 0, EXEC_ARGS_ARGV_BUF_OFF);
  exec_args_write_off(page, EXEC_ARGS_ENVP_TBL_OFF, 0, EXEC_ARGS_ENVP_BUF_OFF);
}

int exec_args_set_path(exec_args_t *a, const char *path) {
  uint16_t i = 0;
  while (i < VFS_PATH_MAX && path[i]) {
    mem_region_page_write(a->page, (uint16_t)(EXEC_ARGS_PATH_OFF + i), &path[i],
                          1);
    i++;
  }
  if (i >= VFS_PATH_MAX) return -ENAMETOOLONG;
  uint8_t nul = 0;
  mem_region_page_write(a->page, (uint16_t)(EXEC_ARGS_PATH_OFF + i), &nul, 1);
  return 0;
}

int exec_args_append_argv(exec_args_t *a, const char *str, uint16_t len) {
  return exec_args_append(a->page, EXEC_ARGS_ARGV_TBL_OFF,
                          EXEC_ARGS_ARGV_BUF_OFF, EXEC_ARGV_BYTES_MAX, &a->argc,
                          EXEC_ARGV_MAX, str, len);
}

int exec_args_append_envp(exec_args_t *a, const char *str, uint16_t len) {
  return exec_args_append(a->page, EXEC_ARGS_ENVP_TBL_OFF,
                          EXEC_ARGS_ENVP_BUF_OFF, EXEC_ENVP_BYTES_MAX, &a->envc,
                          EXEC_ENVP_MAX, str, len);
}

/* ── Reader ─────────────────────────────────────────────────────────── */

int exec_args_path(const exec_args_t *a, char *out, uint16_t out_size) {
  uint16_t i = 0;
  while (i < VFS_PATH_MAX && i + 1 < out_size) {
    uint8_t c = 0;
    mem_region_page_read(a->page, (uint16_t)(EXEC_ARGS_PATH_OFF + i), &c, 1);
    out[i] = (char)c;
    if (c == 0) return (int)i;
    i++;
  }
  if (i < out_size) out[i] = '\0';
  return -ENAMETOOLONG;
}

uint16_t exec_args_argv_len(const exec_args_t *a, int idx) {
  return exec_args_len(a->page, EXEC_ARGS_ARGV_TBL_OFF, a->argc, idx);
}

uint16_t exec_args_envp_len(const exec_args_t *a, int idx) {
  return exec_args_len(a->page, EXEC_ARGS_ENVP_TBL_OFF, a->envc, idx);
}

int exec_args_argv_copy(const exec_args_t *a, int idx, char *out,
                        uint16_t out_size) {
  return exec_args_copy(a->page, EXEC_ARGS_ARGV_TBL_OFF, a->argc, idx, out,
                        out_size);
}

int exec_args_envp_copy(const exec_args_t *a, int idx, char *out,
                        uint16_t out_size) {
  return exec_args_copy(a->page, EXEC_ARGS_ENVP_TBL_OFF, a->envc, idx, out,
                        out_size);
}
