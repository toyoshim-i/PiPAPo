/*
 * pile_ops.c — File operation helpers and transient status line
 *
 * Design: docs/proposals/pile.md Phase P3
 *
 * P3a provides the status-message infrastructure, yes/no confirmation,
 * and the F7 (mkdir) / F8 (delete) ops.  F5 / F6 copy / move land in
 * P3b and P3c.
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
  char buf[128];
  int pos = 0;
  int plen = uc_strlen(prefix);
  if (plen > 60) plen = 60;
  for (int i = 0; i < plen; i++) buf[pos++] = prefix[i];
  buf[pos++] = ':';
  buf[pos++] = ' ';
  const char *es = errstr(errcode);
  while (*es && pos < (int)sizeof(buf) - 1) buf[pos++] = *es++;
  buf[pos] = '\0';
  pile_status_set(buf, 1);
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

/* ── Path helpers ─────────────────────────────────────────────────────── */

static int path_join(char *out, int cap, const char *dir, const char *name) {
  int dlen = uc_strlen(dir);
  int nlen = uc_strlen(name);
  int need = dlen + (dlen > 0 && dir[dlen - 1] == '/' ? 0 : 1) + nlen + 1;
  if (need > cap) return -1;
  int pos = 0;
  for (int i = 0; i < dlen; i++) out[pos++] = dir[i];
  if (dlen == 0 || dir[dlen - 1] != '/') out[pos++] = '/';
  for (int i = 0; i < nlen; i++) out[pos++] = name[i];
  out[pos] = '\0';
  return 0;
}

/* ── Pane reload with cursor preservation ─────────────────────────────── */

static void reload_keep_cursor(pile_pane_t *pane) {
  char prev_name[64];
  prev_name[0] = '\0';
  int prev_idx = pane->cursor;
  if (pane->count > 0 && pane->cursor < pane->count) {
    uc_strcpy(prev_name, pane->entries[pane->cursor].name);
  }
  pile_pane_load(pane);
  if (prev_name[0]) {
    for (int i = 0; i < pane->count; i++) {
      if (uc_strcmp(pane->entries[i].name, prev_name) == 0) {
        pane->cursor = i;
        int vr = pile_draw_visible_rows();
        if (pane->cursor >= pane->scroll + vr)
          pane->scroll = pane->cursor - vr + 1;
        return;
      }
    }
  }
  if (prev_idx >= pane->count) prev_idx = pane->count - 1;
  if (prev_idx < 0) prev_idx = 0;
  pane->cursor = prev_idx;
}

/* ── F7: mkdir ────────────────────────────────────────────────────────── */

void pile_op_mkdir(pile_pane_t *pane) {
  char name[64];
  if (pile_prompt("New directory name: ", name, (int)sizeof(name)) != 0) return;
  if (!name[0]) return;

  char full[PILE_PATH_MAX];
  if (path_join(full, (int)sizeof(full), pane->path, name) != 0) {
    pile_status_set("pile: path too long", 1);
    return;
  }
  int rc = mkdir(full, 0755);
  if (rc < 0) {
    pile_status_set_errno("mkdir", rc);
    return;
  }
  refresh_panes(pane);
  jump_to_name(pane, name);
}

/* ── F8: delete ───────────────────────────────────────────────────────── */

/* Collect the operand set: marked entries if any, else the cursor.
 * Returns number of entries written to idx_out; if any is a directory,
 * sets *has_dir and the caller refuses (P3 defers recursion). */
static int collect_targets(const pile_pane_t *pane, int *idx_out, int cap,
                           int *has_dir) {
  int n = 0;
  *has_dir = 0;
  int sel = pile_pane_sel_count(pane);
  if (sel > 0) {
    for (int i = 0; i < pane->count && n < cap; i++) {
      const pile_entry_t *e = &pane->entries[i];
      if (!(e->flags & PILE_EFLAG_MARKED)) continue;
      if (e->d_type == DT_DIR) *has_dir = 1;
      idx_out[n++] = i;
    }
  } else if (pane->count > 0) {
    const pile_entry_t *e = &pane->entries[pane->cursor];
    /* ".." is not a valid operand. */
    if (e->name[0] == '.' && e->name[1] == '.' && e->name[2] == '\0') return 0;
    if (e->d_type == DT_DIR) *has_dir = 1;
    idx_out[n++] = pane->cursor;
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

/* Stream-copy src_path → dst_path.  Returns 0 on success, negative
 * errno on failure.  Truncates dst. */
static int copy_file(const char *src_path, const char *dst_path) {
  int src_fd = open(src_path, O_RDONLY, 0);
  if (src_fd < 0) return src_fd;
  int dst_fd = open(dst_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (dst_fd < 0) {
    close(src_fd);
    return dst_fd;
  }
  long n = uc_copy_fd(src_fd, dst_fd);
  close(src_fd);
  close(dst_fd);
  if (n < 0) {
    unlink(dst_path);
    return -EIO;
  }
  return 0;
}

/* Summarise the result of a batch into a status message.  For n == 1
 * a single success is silent; batch success is reported. */
static void report_batch(const char *verb, const char *verb_past, int n,
                         int ok, int fail, int last_err) {
  if (fail) {
    char buf[96];
    uc_snprintf(buf, (int)sizeof(buf), "%s: %u ok, %u failed (%s)", verb,
                (unsigned)ok, (unsigned)fail, errstr(last_err));
    pile_status_set(buf, 1);
  } else if (n > 1) {
    char buf[64];
    uc_snprintf(buf, (int)sizeof(buf), "%s %u files", verb_past, (unsigned)ok);
    pile_status_set(buf, 0);
  }
}

void pile_op_copy(pile_pane_t *pane) {
  int targets[PILE_MAX_ENTRIES];
  int has_dir = 0;
  int n = collect_targets(pane, targets, PILE_MAX_ENTRIES, &has_dir);
  if (n == 0) return;
  if (has_dir) {
    pile_status_set("pile: directories not supported", 1);
    return;
  }

  pile_pane_t *dst_pane = other_pane(pane);
  if (paths_equal(pane->path, dst_pane->path)) {
    pile_status_set("pile: source and destination are the same", 1);
    return;
  }

  /* Pre-op confirmation: per-file overwrite prompt for n == 1, single
   * batch prompt for n > 1. */
  if (n == 1) {
    const char *name = pane->entries[targets[0]].name;
    char dst_path[PILE_PATH_MAX];
    if (path_join(dst_path, (int)sizeof(dst_path), dst_pane->path, name) != 0) {
      pile_status_set("pile: path too long", 1);
      return;
    }
    if (file_exists(dst_path)) {
      char q[96];
      uc_snprintf(q, (int)sizeof(q), "Overwrite %s? [y/N]: ", name);
      if (!pile_confirm(q)) return;
    }
  } else {
    char q[96];
    uc_snprintf(q, (int)sizeof(q), "Copy %u files to %s? [y/N]: ",
                (unsigned)n, dst_pane->path);
    if (!pile_confirm(q)) return;
  }

  char first_name[64];
  uc_strcpy(first_name, pane->entries[targets[0]].name);

  int ok = 0, fail = 0, last_err = 0;
  for (int i = 0; i < n; i++) {
    const char *name = pane->entries[targets[i]].name;
    char src_path[PILE_PATH_MAX];
    char dst_path[PILE_PATH_MAX];
    if (path_join(src_path, (int)sizeof(src_path), pane->path, name) != 0 ||
        path_join(dst_path, (int)sizeof(dst_path), dst_pane->path, name) != 0) {
      fail++;
      continue;
    }
    int cr = copy_file(src_path, dst_path);
    if (cr < 0) {
      fail++;
      last_err = cr;
    } else {
      ok++;
    }
  }
  report_batch("copy", "copied", n, ok, fail, last_err);

  refresh_panes(pane);
  jump_to_name(dst_pane, first_name);
}

void pile_op_move(pile_pane_t *pane) {
  int targets[PILE_MAX_ENTRIES];
  int has_dir = 0;
  int n = collect_targets(pane, targets, PILE_MAX_ENTRIES, &has_dir);
  if (n == 0) return;
  if (has_dir) {
    pile_status_set("pile: directories not supported", 1);
    return;
  }

  pile_pane_t *dst_pane = other_pane(pane);
  int same_dir = paths_equal(pane->path, dst_pane->path);

  /* Same-directory move is a rename — only sensible for a single
   * operand, since each batch entry would need its own new name. */
  if (same_dir) {
    if (n > 1) {
      pile_status_set("pile: batch rename not supported", 1);
      return;
    }
    const char *name = pane->entries[targets[0]].name;
    char new_name[64];
    if (pile_prompt("Rename to: ", new_name, (int)sizeof(new_name)) != 0)
      return;
    if (!new_name[0]) return;

    char src_path[PILE_PATH_MAX];
    char dst_path[PILE_PATH_MAX];
    if (path_join(src_path, (int)sizeof(src_path), pane->path, name) != 0 ||
        path_join(dst_path, (int)sizeof(dst_path), pane->path, new_name) != 0) {
      pile_status_set("pile: path too long", 1);
      return;
    }
    if (paths_equal(src_path, dst_path)) return;
    if (file_exists(dst_path)) {
      char q[96];
      uc_snprintf(q, (int)sizeof(q), "Overwrite %s? [y/N]: ", new_name);
      if (!pile_confirm(q)) return;
    }
    int rn = rename(src_path, dst_path);
    if (rn < 0) {
      pile_status_set_errno("rename", rn);
      return;
    }
    refresh_panes(pane);
    jump_to_name(pane, new_name);
    return;
  }

  /* Cross-directory move — single-file overwrite prompt, or a single
   * batch prompt. */
  if (n == 1) {
    const char *name = pane->entries[targets[0]].name;
    char dst_path[PILE_PATH_MAX];
    if (path_join(dst_path, (int)sizeof(dst_path), dst_pane->path, name) != 0) {
      pile_status_set("pile: path too long", 1);
      return;
    }
    if (file_exists(dst_path)) {
      char q[96];
      uc_snprintf(q, (int)sizeof(q), "Overwrite %s? [y/N]: ", name);
      if (!pile_confirm(q)) return;
    }
  } else {
    char q[96];
    uc_snprintf(q, (int)sizeof(q), "Move %u files to %s? [y/N]: ",
                (unsigned)n, dst_pane->path);
    if (!pile_confirm(q)) return;
  }

  char first_name[64];
  uc_strcpy(first_name, pane->entries[targets[0]].name);

  int ok = 0, fail = 0, last_err = 0;
  for (int i = 0; i < n; i++) {
    const char *name = pane->entries[targets[i]].name;
    char src_path[PILE_PATH_MAX];
    char dst_path[PILE_PATH_MAX];
    if (path_join(src_path, (int)sizeof(src_path), pane->path, name) != 0 ||
        path_join(dst_path, (int)sizeof(dst_path), dst_pane->path, name) != 0) {
      fail++;
      continue;
    }
    int rn = rename(src_path, dst_path);
    if (rn < 0 && -rn == EXDEV) {
      int cr = copy_file(src_path, dst_path);
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
  jump_to_name(dst_pane, first_name);
}

void pile_op_delete(pile_pane_t *pane) {
  int targets[PILE_MAX_ENTRIES];
  int has_dir = 0;
  int n = collect_targets(pane, targets, PILE_MAX_ENTRIES, &has_dir);
  if (n == 0) return;
  if (has_dir) {
    pile_status_set("pile: directories not supported (use rmdir/rm -r)", 1);
    return;
  }

  char prompt[64];
  if (n == 1) {
    int pos = 0;
    const char *pre = "Delete ";
    while (*pre && pos < (int)sizeof(prompt) - 1) prompt[pos++] = *pre++;
    const char *nm = pane->entries[targets[0]].name;
    while (*nm && pos < (int)sizeof(prompt) - 4) prompt[pos++] = *nm++;
    const char *suf = "? [y/N]: ";
    while (*suf && pos < (int)sizeof(prompt) - 1) prompt[pos++] = *suf++;
    prompt[pos] = '\0';
  } else {
    uc_snprintf(prompt, (int)sizeof(prompt),
                "Delete %d files? [y/N]: ", n);
  }
  if (!pile_confirm(prompt)) return;

  int ok = 0, fail = 0;
  int last_err = 0;
  for (int i = 0; i < n; i++) {
    char full[PILE_PATH_MAX];
    if (path_join(full, (int)sizeof(full), pane->path,
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
    char buf[96];
    uc_snprintf(buf, (int)sizeof(buf),
                "delete: %d ok, %d failed (%s)", ok, fail, errstr(last_err));
    pile_status_set(buf, 1);
  } else if (ok > 1) {
    char buf[64];
    uc_snprintf(buf, (int)sizeof(buf), "deleted %d files", ok);
    pile_status_set(buf, 0);
  }
  refresh_panes(pane);
}
