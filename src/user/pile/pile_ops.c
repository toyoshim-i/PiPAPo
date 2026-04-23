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
  /* Reload and try to land the cursor on the new entry. */
  pile_pane_load(pane);
  for (int i = 0; i < pane->count; i++) {
    if (uc_strcmp(pane->entries[i].name, name) == 0) {
      pane->cursor = i;
      int vr = pile_draw_visible_rows();
      if (pane->cursor >= pane->scroll + vr)
        pane->scroll = pane->cursor - vr + 1;
      break;
    }
  }
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
  reload_keep_cursor(pane);
}
