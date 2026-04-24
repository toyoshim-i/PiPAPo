/*
 * pile_ops.c — File operation helpers and transient status line
 *
 * User guide: docs/user/pile.md
 */

#include "pile.h"

#include "common/errno.h"

/* Forward declarations for helpers defined later in the file. */
static void jump_to_name(pile_pane_t *pane, const char *name);
static void refresh_panes(pile_pane_t *src);

/* ── Status line ──────────────────────────────────────────────────────── */

char pile_status_msg[128];
int pile_status_is_error;

void pile_status_set(const char *msg, int is_error) {
  int i = 0;
  while (i < (int)sizeof(pile_status_msg) - 1 && msg[i]) {
    pile_status_msg[i] = msg[i];
    i++;
  }
  pile_status_msg[i] = '\0';
  pile_status_is_error = is_error;
}

void pile_status_clear(void) {
  pile_status_msg[0] = '\0';
  pile_status_is_error = 0;
}

/* ── errno → string ───────────────────────────────────────────────────── */

static const char *errstr(int err) {
  int e = err < 0 ? -err : err;
  switch (e) {
    case EPERM:  return "operation not permitted";
    case ENOENT: return "no such file";
    case EINTR:  return "interrupted";
    case EIO:    return "I/O error";
    case EBADF:  return "bad file descriptor";
    case ENOMEM: return "out of memory";
    case EACCES: return "permission denied";
    case EBUSY:  return "resource busy";
    case EEXIST: return "already exists";
    case EXDEV:  return "cross-device link";
    case ENODEV: return "no such device";
    case ENOTDIR: return "not a directory";
    case EISDIR:  return "is a directory";
    case EINVAL:  return "invalid argument";
    case ENOSPC:  return "no space left";
    case EROFS:   return "read-only filesystem";
    case ENAMETOOLONG: return "name too long";
    case ENOTEMPTY: return "directory not empty";
  }
  return "error";
}

void pile_status_set_errno(const char *prefix, int errcode) {
  char *buf = uc_malloc(128);
  if (!buf) {
    pile_status_set(errstr(errcode), 1);
    return;
  }
  int pos = 0;
  int plen = uc_strlen(prefix);
  if (plen > 60) plen = 60;
  for (int i = 0; i < plen; i++) buf[pos++] = prefix[i];
  buf[pos++] = ':';
  buf[pos++] = ' ';
  const char *es = errstr(errcode);
  while (*es && pos < 127) buf[pos++] = *es++;
  buf[pos] = '\0';
  pile_status_set(buf, 1);
  uc_free(buf);
}

/* ── Yes/no prompt ────────────────────────────────────────────────────── */

int pile_confirm(const char *prompt) {
  pile_draw_cursor_to(pile_rows - 1, 0);
  pile_draw_clear_to_eol();
  uc_puts(prompt);
  for (;;) {
    int k = pile_read_key();
    if (k == PKEY_NONE) continue;
    return (k == 'y' || k == 'Y');
  }
}

/* ── Pane reload with cursor preservation ─────────────────────────────── */

static void reload_keep_cursor(pile_pane_t *pane) {
  char *prev_name = uc_malloc(64);
  if (prev_name) {
    prev_name[0] = '\0';
    if (pane->count > 0 && pane->cursor < pane->count) {
      uc_strcpy(prev_name, pane->entries[pane->cursor].name);
    }
  }
  int prev_idx = pane->cursor;
  pile_pane_load(pane);
  if (prev_name && prev_name[0]) {
    for (int i = 0; i < pane->count; i++) {
      if (uc_strcmp(pane->entries[i].name, prev_name) == 0) {
        pane->cursor = i;
        int vr = pile_draw_visible_rows();
        if (pane->cursor >= pane->scroll + vr)
          pane->scroll = pane->cursor - vr + 1;
        uc_free(prev_name);
        return;
      }
    }
  }
  uc_free(prev_name);
  if (prev_idx >= pane->count) prev_idx = pane->count - 1;
  if (prev_idx < 0) prev_idx = 0;
  pane->cursor = prev_idx;
}

/* ── F7: mkdir ────────────────────────────────────────────────────────── */

void pile_op_mkdir(pile_pane_t *pane) {
  char *name = uc_malloc(64);
  char *full = uc_malloc(PILE_PATH_MAX);
  if (!name || !full) {
    pile_status_set("pile: out of heap", 1);
    goto cleanup;
  }
  if (pile_prompt("New directory name: ", name, 64) != 0) goto cleanup;
  if (!name[0]) goto cleanup;

  if (pile_path_join(full, PILE_PATH_MAX, pane->path, name) != 0) {
    pile_status_set("pile: path too long", 1);
    goto cleanup;
  }
  int rc = mkdir(full, 0755);
  if (rc < 0) {
    pile_status_set_errno("mkdir", rc);
    goto cleanup;
  }
  refresh_panes(pane);
  jump_to_name(pane, name);

cleanup:
  uc_free(full);
  uc_free(name);
}

/* ── F8: delete ───────────────────────────────────────────────────────── */

/* Collect the operand set: marked entries if any, else the cursor.
 * Returns number of entries written to idx_out; if any is a directory,
 * sets *has_dir and the caller refuses (P3 defers recursion).
 * idx_out uses uint8_t to keep the buffer compact (one byte per
 * entry) — PILE_MAX_ENTRIES fits in 8 bits. */
static int collect_targets(const pile_pane_t *pane, uint8_t *idx_out, int cap,
                           int *has_dir) {
  int n = 0;
  *has_dir = 0;
  int sel = pile_pane_sel_count(pane);
  if (sel > 0) {
    for (int i = 0; i < pane->count && n < cap; i++) {
      const pile_entry_t *e = &pane->entries[i];
      if (!(e->flags & PILE_EFLAG_MARKED)) continue;
      if (e->d_type == DT_DIR) *has_dir = 1;
      idx_out[n++] = (uint8_t)i;
    }
  } else if (pane->count > 0) {
    const pile_entry_t *e = &pane->entries[pane->cursor];
    /* ".." is not a valid operand. */
    if (e->name[0] == '.' && e->name[1] == '.' && e->name[2] == '\0') return 0;
    if (e->d_type == DT_DIR) *has_dir = 1;
    idx_out[n++] = (uint8_t)pane->cursor;
  }
  return n;
}

static pile_pane_t *other_pane(const pile_pane_t *p) {
  return (p == &pile_pane_a) ? &pile_pane_b : &pile_pane_a;
}

/* Jump `pane`'s cursor to the entry named `name` if present; scroll
 * to keep it on screen.  No-op if the name is absent. */
static void jump_to_name(pile_pane_t *pane, const char *name) {
  for (int i = 0; i < pane->count; i++) {
    if (uc_strcmp(pane->entries[i].name, name) == 0) {
      pane->cursor = i;
      int vr = pile_draw_visible_rows();
      if (pane->cursor >= pane->scroll + vr)
        pane->scroll = pane->cursor - vr + 1;
      return;
    }
  }
}

/* Refresh both panes after any op, preserving cursor on the source
 * pane and plain-reloading the other (the caller can jump_to_name it
 * after this if it wants to land the cursor on a specific entry). */
void pile_refresh_panes(pile_pane_t *src) { refresh_panes(src); }

static void refresh_panes(pile_pane_t *src) {
  reload_keep_cursor(src);
  pile_pane_t *other = other_pane(src);
  int vr = pile_draw_visible_rows();
  int old_cursor = other->cursor;
  pile_pane_load(other);
  if (old_cursor >= other->count) old_cursor = other->count - 1;
  if (old_cursor < 0) old_cursor = 0;
  other->cursor = old_cursor;
  if (other->cursor >= other->scroll + vr)
    other->scroll = other->cursor - vr + 1;
}

static int paths_equal(const char *a, const char *b) {
  return uc_strcmp(a, b) == 0;
}

static int file_exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0;
}

/* Stream-copy src_path → dst_path via a caller-supplied buffer.
 * Returns 0 on success, negative errno on failure.  Truncates dst.
 * The caller-supplied buffer keeps this function's own stack frame
 * tiny — pile's 64 KB ia16 segment can't afford uc_copy_fd's 512 B
 * stack-local buffer on the op path (see Phase PS). */
static int copy_file(const char *src_path, const char *dst_path,
                     uint8_t *buf, size_t buf_size) {
  int src_fd = open(src_path, O_RDONLY, 0);
  if (src_fd < 0) return src_fd;
  int dst_fd = open(dst_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (dst_fd < 0) {
    close(src_fd);
    return dst_fd;
  }
  int rc = 0;
  for (;;) {
    int r = (int)read(src_fd, buf, buf_size);
    if (r == 0) break;
    if (r < 0) { rc = -EIO; break; }
    int w = (int)write(dst_fd, buf, r);
    if (w != r) { rc = -EIO; break; }
  }
  close(src_fd);
  close(dst_fd);
  if (rc < 0) {
    unlink(dst_path);
    return rc;
  }
  return 0;
}

/* Summarise the result of a batch into a status message.  For n == 1
 * a single success is silent; batch success is reported.  Uses a
 * heap buffer rather than a stack local to keep op frames lean
 * (Phase PS: no stack vectors on the op path). */
static void report_batch(const char *verb, const char *verb_past, int n,
                         int ok, int fail, int last_err) {
  if (!fail && n <= 1) return;
  char *buf = uc_malloc(96);
  if (!buf) return;  /* silent drop on OOM — op still succeeded */
  if (fail) {
    uc_snprintf(buf, 96, "%s: %u ok, %u failed (%s)", verb,
                (unsigned)ok, (unsigned)fail, errstr(last_err));
    pile_status_set(buf, 1);
  } else {
    uc_snprintf(buf, 96, "%s %u files", verb_past, (unsigned)ok);
    pile_status_set(buf, 0);
  }
  uc_free(buf);
}

void pile_op_copy(pile_pane_t *pane) {
  uint8_t *targets = uc_malloc(PILE_MAX_ENTRIES);
  char *src_path = uc_malloc(PILE_PATH_MAX);
  char *dst_path = uc_malloc(PILE_PATH_MAX);
  char *first_name = uc_malloc(64);
  char *buf = uc_malloc(96);
  uint8_t *io_buf = uc_malloc(256);
  if (!targets || !src_path || !dst_path || !first_name || !buf || !io_buf) {
    pile_status_set("pile: out of heap", 1);
    goto cleanup;
  }

  int has_dir = 0;
  int n = collect_targets(pane, targets, PILE_MAX_ENTRIES, &has_dir);
  if (n == 0) goto cleanup;
  if (has_dir) {
    pile_status_set("pile: directories not supported", 1);
    goto cleanup;
  }

  pile_pane_t *dst_pane = other_pane(pane);
  if (paths_equal(pane->path, dst_pane->path)) {
    pile_status_set("pile: source and destination are the same", 1);
    goto cleanup;
  }

  /* Pre-op confirmation: per-file overwrite prompt for n == 1, single
   * batch prompt for n > 1. */
  if (n == 1) {
    const char *name = pane->entries[targets[0]].name;
    if (pile_path_join(dst_path, PILE_PATH_MAX, dst_pane->path, name) != 0) {
      pile_status_set("pile: path too long", 1);
      goto cleanup;
    }
    if (file_exists(dst_path)) {
      uc_snprintf(buf, 96, "Overwrite %s? [y/N]: ", name);
      if (!pile_confirm(buf)) goto cleanup;
    }
  } else {
    uc_snprintf(buf, 96, "Copy %u files to %s? [y/N]: ",
                (unsigned)n, dst_pane->path);
    if (!pile_confirm(buf)) goto cleanup;
  }

  uc_strcpy(first_name, pane->entries[targets[0]].name);

  int ok = 0, fail = 0, last_err = 0;
  for (int i = 0; i < n; i++) {
    const char *name = pane->entries[targets[i]].name;
    if (pile_path_join(src_path, PILE_PATH_MAX, pane->path, name) != 0 ||
        pile_path_join(dst_path, PILE_PATH_MAX, dst_pane->path, name) != 0) {
      fail++;
      continue;
    }
    int cr = copy_file(src_path, dst_path, io_buf, 256);
    if (cr < 0) {
      fail++;
      last_err = cr;
    } else {
      ok++;
    }
  }
  report_batch("copy", "copied", n, ok, fail, last_err);

  refresh_panes(pane);
  jump_to_name(other_pane(pane), first_name);

cleanup:
  uc_free(io_buf);
  uc_free(buf);
  uc_free(first_name);
  uc_free(dst_path);
  uc_free(src_path);
  uc_free(targets);
}

void pile_op_move(pile_pane_t *pane) {
  uint8_t *targets = uc_malloc(PILE_MAX_ENTRIES);
  char *src_path = uc_malloc(PILE_PATH_MAX);
  char *dst_path = uc_malloc(PILE_PATH_MAX);
  char *first_name = uc_malloc(64);
  char *new_name = uc_malloc(64);
  char *buf = uc_malloc(96);
  uint8_t *io_buf = uc_malloc(256);
  if (!targets || !src_path || !dst_path || !first_name || !new_name ||
      !buf || !io_buf) {
    pile_status_set("pile: out of heap", 1);
    goto cleanup;
  }

  int has_dir = 0;
  int n = collect_targets(pane, targets, PILE_MAX_ENTRIES, &has_dir);
  if (n == 0) goto cleanup;
  if (has_dir) {
    pile_status_set("pile: directories not supported", 1);
    goto cleanup;
  }

  pile_pane_t *dst_pane = other_pane(pane);
  int same_dir = paths_equal(pane->path, dst_pane->path);

  /* Same-directory move is a rename — only sensible for a single
   * operand, since each batch entry would need its own new name. */
  if (same_dir) {
    if (n > 1) {
      pile_status_set("pile: batch rename not supported", 1);
      goto cleanup;
    }
    const char *name = pane->entries[targets[0]].name;
    if (pile_prompt("Rename to: ", new_name, 64) != 0) goto cleanup;
    if (!new_name[0]) goto cleanup;

    if (pile_path_join(src_path, PILE_PATH_MAX, pane->path, name) != 0 ||
        pile_path_join(dst_path, PILE_PATH_MAX, pane->path, new_name) != 0) {
      pile_status_set("pile: path too long", 1);
      goto cleanup;
    }
    if (paths_equal(src_path, dst_path)) goto cleanup;
    if (file_exists(dst_path)) {
      uc_snprintf(buf, 96, "Overwrite %s? [y/N]: ", new_name);
      if (!pile_confirm(buf)) goto cleanup;
    }
    int rn = rename(src_path, dst_path);
    if (rn < 0) {
      pile_status_set_errno("rename", rn);
      goto cleanup;
    }
    refresh_panes(pane);
    jump_to_name(pane, new_name);
    goto cleanup;
  }

  /* Cross-directory move — single-file overwrite prompt, or a single
   * batch prompt. */
  if (n == 1) {
    const char *name = pane->entries[targets[0]].name;
    if (pile_path_join(dst_path, PILE_PATH_MAX, dst_pane->path, name) != 0) {
      pile_status_set("pile: path too long", 1);
      goto cleanup;
    }
    if (file_exists(dst_path)) {
      uc_snprintf(buf, 96, "Overwrite %s? [y/N]: ", name);
      if (!pile_confirm(buf)) goto cleanup;
    }
  } else {
    uc_snprintf(buf, 96, "Move %u files to %s? [y/N]: ",
                (unsigned)n, dst_pane->path);
    if (!pile_confirm(buf)) goto cleanup;
  }

  uc_strcpy(first_name, pane->entries[targets[0]].name);

  int ok = 0, fail = 0, last_err = 0;
  for (int i = 0; i < n; i++) {
    const char *name = pane->entries[targets[i]].name;
    if (pile_path_join(src_path, PILE_PATH_MAX, pane->path, name) != 0 ||
        pile_path_join(dst_path, PILE_PATH_MAX, dst_pane->path, name) != 0) {
      fail++;
      continue;
    }
    int rn = rename(src_path, dst_path);
    if (rn < 0 && -rn == EXDEV) {
      int cr = copy_file(src_path, dst_path, io_buf, 256);
      if (cr < 0) {
        fail++;
        last_err = cr;
        continue;
      }
      int ur = unlink(src_path);
      if (ur < 0) {
        fail++;
        last_err = ur;
        continue;
      }
      ok++;
    } else if (rn < 0) {
      fail++;
      last_err = rn;
    } else {
      ok++;
    }
  }
  report_batch("move", "moved", n, ok, fail, last_err);

  refresh_panes(pane);
  jump_to_name(other_pane(pane), first_name);

cleanup:
  uc_free(io_buf);
  uc_free(buf);
  uc_free(new_name);
  uc_free(first_name);
  uc_free(dst_path);
  uc_free(src_path);
  uc_free(targets);
}

void pile_op_delete(pile_pane_t *pane) {
  uint8_t *targets = uc_malloc(PILE_MAX_ENTRIES);
  char *full = uc_malloc(PILE_PATH_MAX);
  char *buf = uc_malloc(96);
  if (!targets || !full || !buf) {
    pile_status_set("pile: out of heap", 1);
    goto cleanup;
  }

  int has_dir = 0;
  int n = collect_targets(pane, targets, PILE_MAX_ENTRIES, &has_dir);
  if (n == 0) goto cleanup;
  if (has_dir) {
    pile_status_set("pile: directories not supported (use rmdir/rm -r)", 1);
    goto cleanup;
  }

  if (n == 1) {
    int pos = 0;
    const char *pre = "Delete ";
    while (*pre && pos < 95) buf[pos++] = *pre++;
    const char *nm = pane->entries[targets[0]].name;
    while (*nm && pos < 95 - 10) buf[pos++] = *nm++;
    const char *suf = "? [y/N]: ";
    while (*suf && pos < 95) buf[pos++] = *suf++;
    buf[pos] = '\0';
  } else {
    uc_snprintf(buf, 96, "Delete %d files? [y/N]: ", n);
  }
  if (!pile_confirm(buf)) goto cleanup;

  int ok = 0, fail = 0;
  int last_err = 0;
  for (int i = 0; i < n; i++) {
    if (pile_path_join(full, PILE_PATH_MAX, pane->path,
                       pane->entries[targets[i]].name) != 0) {
      fail++;
      continue;
    }
    int rc = unlink(full);
    if (rc < 0) {
      fail++;
      last_err = rc;
    } else {
      ok++;
    }
  }
  if (fail) {
    uc_snprintf(buf, 96,
                "delete: %d ok, %d failed (%s)", ok, fail, errstr(last_err));
    pile_status_set(buf, 1);
  } else if (ok > 1) {
    uc_snprintf(buf, 96, "deleted %d files", ok);
    pile_status_set(buf, 0);
  }
  refresh_panes(pane);

cleanup:
  uc_free(buf);
  uc_free(full);
  uc_free(targets);
}
