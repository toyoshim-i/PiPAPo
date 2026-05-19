/*
 * exec_args.c — Populate and read the captured execve arguments page.
 *
 * All page accesses go through page_read / page_write
 * so i16 (where pool pages live above 64 KB) and 32-bit targets share the
 * same code path.
 */

#include "kernel/core/exec/exec_args.h"

#include "common/errno.h"
#include "kernel/core/mm/page_io.h"
#include "kernel/core/mm/region.h"

/* ── Internal helpers ─────────────────────────────────────────────────── */

static uint16_t exec_args_read_off(page_id_t page, uint16_t table_off,
                                   int idx) {
  uint16_t value = 0;
  page_read(page, (uint16_t)(table_off + idx * sizeof(uint16_t)), &value,
            sizeof(value));
  return value;
}

static void exec_args_write_off(page_id_t page, uint16_t table_off, int idx,
                                uint16_t value) {
  page_write(page, (uint16_t)(table_off + idx * sizeof(uint16_t)), &value,
             sizeof(value));
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
  page_write(page, end, str, len);
  uint8_t nul = 0;
  page_write(page, (uint16_t)(end + len), &nul, 1);
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
  page_read(page, start, out, slen);
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
  page_write(page, EXEC_ARGS_PATH_OFF, &zero, 1);
  /* Seed the first offset-table entry for each buffer. */
  exec_args_write_off(page, EXEC_ARGS_ARGV_TBL_OFF, 0, EXEC_ARGS_ARGV_BUF_OFF);
  exec_args_write_off(page, EXEC_ARGS_ENVP_TBL_OFF, 0, EXEC_ARGS_ENVP_BUF_OFF);
}

int exec_args_set_path(exec_args_t *a, const char *path) {
  uint16_t i = 0;
  while (i < VFS_PATH_MAX && path[i]) {
    page_write(a->page, (uint16_t)(EXEC_ARGS_PATH_OFF + i), &path[i], 1);
    i++;
  }
  if (i >= VFS_PATH_MAX) return -ENAMETOOLONG;
  uint8_t nul = 0;
  page_write(a->page, (uint16_t)(EXEC_ARGS_PATH_OFF + i), &nul, 1);
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

static int exec_args_begin(page_id_t page, uint16_t table_off, uint16_t buf_off,
                           uint16_t buf_size, uint8_t count,
                           uint8_t max_entries, page_id_t *out_page,
                           uint16_t *out_off, uint16_t *out_max_len) {
  if (count >= max_entries) return -E2BIG;
  uint16_t end = exec_args_read_off(page, table_off, count);
  uint16_t used = (uint16_t)(end - buf_off);
  if (used >= buf_size) return -E2BIG;
  /* Reserve room for the NUL the commit step will write. */
  uint16_t avail = (uint16_t)(buf_size - used - 1u);
  *out_page = page;
  *out_off = end;
  *out_max_len = avail;
  return 0;
}

static int exec_args_commit(page_id_t page, uint16_t table_off,
                            uint16_t buf_off, uint16_t buf_size, uint8_t *count,
                            uint16_t len) {
  uint16_t end = exec_args_read_off(page, table_off, *count);
  uint16_t used = (uint16_t)(end - buf_off);
  if ((uint16_t)(buf_size - used) < (uint16_t)(len + 1u)) return -E2BIG;
  uint8_t nul = 0;
  page_write(page, (uint16_t)(end + len), &nul, 1);
  *count = (uint8_t)(*count + 1);
  exec_args_write_off(page, table_off, *count, (uint16_t)(end + len + 1u));
  return 0;
}

int exec_args_argv_begin(exec_args_t *a, page_id_t *out_page, uint16_t *out_off,
                         uint16_t *out_max_len) {
  return exec_args_begin(a->page, EXEC_ARGS_ARGV_TBL_OFF,
                         EXEC_ARGS_ARGV_BUF_OFF, EXEC_ARGV_BYTES_MAX, a->argc,
                         EXEC_ARGV_MAX, out_page, out_off, out_max_len);
}

int exec_args_argv_commit(exec_args_t *a, uint16_t len) {
  return exec_args_commit(a->page, EXEC_ARGS_ARGV_TBL_OFF,
                          EXEC_ARGS_ARGV_BUF_OFF, EXEC_ARGV_BYTES_MAX, &a->argc,
                          len);
}

int exec_args_envp_begin(exec_args_t *a, page_id_t *out_page, uint16_t *out_off,
                         uint16_t *out_max_len) {
  return exec_args_begin(a->page, EXEC_ARGS_ENVP_TBL_OFF,
                         EXEC_ARGS_ENVP_BUF_OFF, EXEC_ENVP_BYTES_MAX, a->envc,
                         EXEC_ENVP_MAX, out_page, out_off, out_max_len);
}

int exec_args_envp_commit(exec_args_t *a, uint16_t len) {
  return exec_args_commit(a->page, EXEC_ARGS_ENVP_TBL_OFF,
                          EXEC_ARGS_ENVP_BUF_OFF, EXEC_ENVP_BYTES_MAX, &a->envc,
                          len);
}

/* ── Reader ─────────────────────────────────────────────────────────── */

int exec_args_path(const exec_args_t *a, char *out, uint16_t out_size) {
  uint16_t i = 0;
  while (i < VFS_PATH_MAX && i + 1 < out_size) {
    uint8_t c = 0;
    page_read(a->page, (uint16_t)(EXEC_ARGS_PATH_OFF + i), &c, 1);
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

static int exec_args_byte(page_id_t src_page, uint16_t table_off, int count,
                          int idx, uint16_t byte_off, char *out) {
  if (idx < 0 || idx >= count) return -1;
  uint16_t start = exec_args_read_off(src_page, table_off, idx);
  uint16_t next = exec_args_read_off(src_page, table_off, idx + 1);
  uint16_t slen = (uint16_t)(next - start - 1u);
  if (byte_off >= slen) return -1;
  uint8_t c;
  page_read(src_page, (uint16_t)(start + byte_off), &c, 1);
  *out = (char)c;
  return 0;
}

int exec_args_argv_byte(const exec_args_t *a, int idx, uint16_t byte_off,
                        char *out) {
  return exec_args_byte(a->page, EXEC_ARGS_ARGV_TBL_OFF, a->argc, idx, byte_off,
                        out);
}

int exec_args_envp_byte(const exec_args_t *a, int idx, uint16_t byte_off,
                        char *out) {
  return exec_args_byte(a->page, EXEC_ARGS_ENVP_TBL_OFF, a->envc, idx, byte_off,
                        out);
}

static void exec_args_put_byte(page_id_t base_page, uint32_t off, uint8_t c) {
  page_id_t pg = base_page + (page_id_t)(off / PAGE_SIZE);
  uint16_t pg_off = (uint16_t)(off % PAGE_SIZE);
  page_write(pg, pg_off, &c, 1);
}

static int exec_args_to_page(page_id_t src_page, uint16_t table_off, int count,
                             int idx, page_id_t base_page, uint32_t start_off) {
  if (idx < 0 || idx >= count) return -1;
  uint16_t start = exec_args_read_off(src_page, table_off, idx);
  uint16_t next = exec_args_read_off(src_page, table_off, idx + 1);
  uint16_t slen = (uint16_t)(next - start - 1u); /* exclude NUL */
  for (uint16_t i = 0; i < slen; i++) {
    uint8_t c;
    page_read(src_page, (uint16_t)(start + i), &c, 1);
    exec_args_put_byte(base_page, start_off + i, c);
  }
  return (int)slen;
}

int exec_args_argv_to_page(const exec_args_t *a, int idx, page_id_t base_page,
                           uint32_t start_off) {
  return exec_args_to_page(a->page, EXEC_ARGS_ARGV_TBL_OFF, a->argc, idx,
                           base_page, start_off);
}

int exec_args_envp_to_page(const exec_args_t *a, int idx, page_id_t base_page,
                           uint32_t start_off) {
  return exec_args_to_page(a->page, EXEC_ARGS_ENVP_TBL_OFF, a->envc, idx,
                           base_page, start_off);
}

int exec_args_path_to_page(const exec_args_t *a, page_id_t base_page,
                           uint32_t start_off, uint16_t dst_max) {
  uint16_t limit = dst_max < VFS_PATH_MAX ? dst_max : (uint16_t)VFS_PATH_MAX;
  for (uint16_t i = 0; i < limit; i++) {
    uint8_t c;
    page_read(a->page, (uint16_t)(EXEC_ARGS_PATH_OFF + i), &c, 1);
    if (c == 0) return (int)i;
    exec_args_put_byte(base_page, start_off + i, c);
  }
  return -ENAMETOOLONG;
}
