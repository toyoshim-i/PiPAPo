/*
 * pile_pane.c — Directory reading, sorting, cursor navigation
 *
 * Design: docs/proposals/pile.md
 */

#include "pile.h"

#include "common/errno.h"

/* ── Path join ─────────────────────────────────────────────────────────── */

/* Build dir/name in out, at most cap-1 chars, NUL-terminated.  Returns
 * 0 on success, -1 on overflow.  Normalises double slashes. */
int pile_path_join(char *out, int cap, const char *dir, const char *name) {
  int dlen = uc_strlen(dir);
  int nlen = uc_strlen(name);
  int need = dlen + 1 + nlen + 1;
  if (dlen > 0 && dir[dlen - 1] == '/') need--;  /* already has slash */
  if (need > cap) return -1;
  int pos = 0;
  for (int i = 0; i < dlen; i++) out[pos++] = dir[i];
  if (dlen == 0 || dir[dlen - 1] != '/') out[pos++] = '/';
  for (int i = 0; i < nlen; i++) out[pos++] = name[i];
  out[pos] = '\0';
  return 0;
}

/* ── Sort ──────────────────────────────────────────────────────────────── */

/* Dirs first, then by the active sort mode.  ".." is pinned to the top
 * so navigation always sees it as entry 0 when present.  Ties fall
 * through to lexicographic name order for stability. */
static int entry_cmp(const pile_entry_t *a, const pile_entry_t *b,
                     int sort_mode) {
  int a_is_dotdot = (a->name[0] == '.' && a->name[1] == '.' && a->name[2] == '\0');
  int b_is_dotdot = (b->name[0] == '.' && b->name[1] == '.' && b->name[2] == '\0');
  if (a_is_dotdot != b_is_dotdot) return b_is_dotdot - a_is_dotdot;
  int a_is_dir = (a->d_type == DT_DIR);
  int b_is_dir = (b->d_type == DT_DIR);
  if (a_is_dir != b_is_dir) return b_is_dir - a_is_dir;
  if (sort_mode == PILE_SORT_SIZE) {
    if (a->size != b->size) return (a->size > b->size) ? -1 : 1;
  } else if (sort_mode == PILE_SORT_MTIME) {
    if (a->mtime != b->mtime) return (a->mtime > b->mtime) ? -1 : 1;
  }
  return uc_strcmp(a->name, b->name);
}

static void sort_entries(pile_entry_t *a, int n, int sort_mode) {
  /* Insertion sort: n ≤ PILE_MAX_ENTRIES, O(n²) is fine and keeps code
   * small.  The "hole" element is 76 B (pile_entry_t.name is a char
   * vector), so we borrow it from the heap instead of the stack —
   * Phase PS no-vector-on-stack rule. */
  if (n < 2) return;
  pile_entry_t *tmp = uc_malloc(sizeof(*tmp));
  if (!tmp) return;  /* leave unsorted rather than crash */
  for (int i = 1; i < n; i++) {
    *tmp = a[i];
    int j = i;
    while (j > 0 && entry_cmp(&a[j - 1], tmp, sort_mode) > 0) {
      a[j] = a[j - 1];
      j--;
    }
    a[j] = *tmp;
  }
  uc_free(tmp);
}

/* ── Load ──────────────────────────────────────────────────────────────── */

/* Fill e's size/mode/mtime/d_type by stat()-ing dir/e->name.  The
 * caller owns the scratch path buffer so pile_pane_load doesn't pay
 * a malloc/free round-trip per entry. */
static void fill_stat(pile_entry_t *e, const char *dir, char *path_buf) {
  if (pile_path_join(path_buf, PILE_PATH_MAX, dir, e->name) != 0) {
    e->flags |= PILE_EFLAG_STATFAIL;
    return;
  }
  struct stat st;
  if (stat(path_buf, &st) != 0) {
    e->flags |= PILE_EFLAG_STATFAIL;
    return;
  }
  e->size = (uint32_t)st.st_size;
  e->mtime = (uint32_t)st.st_mtime;
  e->mode = (uint16_t)st.st_mode;
  /* Prefer stat's mode for type if d_type was absent (0). */
  if (e->d_type == 0) {
    if (S_ISDIR(st.st_mode)) e->d_type = DT_DIR;
    else if (S_ISCHR(st.st_mode)) e->d_type = DT_CHR;
    else if (S_ISLNK(st.st_mode)) e->d_type = DT_LNK;
    else e->d_type = DT_REG;
  }
}

int pile_pane_load(pile_pane_t *pane) {
  pane->count = 0;
  pane->cursor = 0;
  pane->scroll = 0;
  pane->truncated = 0;

  int fd = open(pane->path, O_RDONLY, 0);
  if (fd < 0) return -1;

  /* struct dirent is 68 B (includes 64-byte name vector); path_buf is
   * PILE_PATH_MAX (128 B).  Both sit on the heap so pile_pane_load's
   * own stack frame stays lean — see Phase PS. */
  struct dirent *de = uc_malloc(sizeof(struct dirent));
  char *path_buf = uc_malloc(PILE_PATH_MAX);
  if (!de || !path_buf) {
    uc_free(path_buf);
    uc_free(de);
    close(fd);
    return -1;
  }

  while (pane->count < PILE_MAX_ENTRIES) {
    int n = getdents(fd, de, sizeof(struct dirent));
    if (n <= 0) break;
    /* Skip "." — we keep ".." for navigation. */
    if (de->d_name[0] == '.' && de->d_name[1] == '\0') continue;
    /* Hide dotfiles unless the pane has show_hidden set.  ".." stays
     * visible either way so the user can always navigate upward. */
    if (!pane->show_hidden && de->d_name[0] == '.' &&
        !(de->d_name[1] == '.' && de->d_name[2] == '\0')) continue;
    pile_entry_t *e = &pane->entries[pane->count++];
    int nlen = uc_strlen(de->d_name);
    if (nlen > (int)sizeof(e->name) - 1) nlen = (int)sizeof(e->name) - 1;
    uc_memcpy(e->name, de->d_name, nlen);
    e->name[nlen] = '\0';
    e->size = 0;
    e->mtime = 0;
    e->mode = 0;
    e->d_type = de->d_type;
    e->flags = 0;
    fill_stat(e, pane->path, path_buf);
  }
  /* Detect truncation: one more readable entry beyond our cap. */
  if (pane->count == PILE_MAX_ENTRIES) {
    if (getdents(fd, de, sizeof(struct dirent)) > 0) pane->truncated = 1;
  }
  close(fd);
  uc_free(path_buf);
  uc_free(de);

  sort_entries(pane->entries, pane->count, pane->sort_mode);
  return 0;
}

void pile_pane_resort(pile_pane_t *pane) {
  sort_entries(pane->entries, pane->count, pane->sort_mode);
}

/* ── Cursor ────────────────────────────────────────────────────────────── */

static void clamp_and_scroll(pile_pane_t *pane, int vrows) {
  if (pane->cursor < 0) pane->cursor = 0;
  if (pane->cursor >= pane->count)
    pane->cursor = pane->count > 0 ? pane->count - 1 : 0;
  if (pane->cursor < pane->scroll) pane->scroll = pane->cursor;
  if (pane->cursor >= pane->scroll + vrows)
    pane->scroll = pane->cursor - vrows + 1;
  if (pane->scroll < 0) pane->scroll = 0;
}

void pile_pane_move(pile_pane_t *pane, int delta, int vrows) {
  pane->cursor += delta;
  clamp_and_scroll(pane, vrows);
}

void pile_pane_home(pile_pane_t *pane) {
  pane->cursor = 0;
  pane->scroll = 0;
}

void pile_pane_end(pile_pane_t *pane, int vrows) {
  pane->cursor = pane->count > 0 ? pane->count - 1 : 0;
  clamp_and_scroll(pane, vrows);
}

/* ── Directory navigation ──────────────────────────────────────────────── */

/* Normalise `base` + "/" + `leaf` into `out`.  Resolves a trailing
 * ".." by dropping the last component of base.  Leaves other dot
 * sequences alone (the VFS lookup handles them).  Returns 0 on
 * success, -1 on overflow. */
static int path_descend(char *out, int cap, const char *base,
                        const char *leaf) {
  if (leaf[0] == '.' && leaf[1] == '.' && leaf[2] == '\0') {
    int blen = uc_strlen(base);
    /* At root: stay at root. */
    if (blen <= 1) {
      if (cap < 2) return -1;
      out[0] = '/';
      out[1] = '\0';
      return 0;
    }
    /* Drop trailing slash if any (other than the root slash), then
     * drop the last component. */
    int end = blen;
    if (base[end - 1] == '/') end--;
    while (end > 0 && base[end - 1] != '/') end--;
    /* Keep the leading slash even if end would now be 0. */
    if (end == 0) end = 1;
    if (end >= cap) return -1;
    for (int i = 0; i < end; i++) out[i] = base[i];
    /* Strip a trailing slash unless we're at the root. */
    if (end > 1 && out[end - 1] == '/') end--;
    out[end] = '\0';
    return 0;
  }
  return pile_path_join(out, cap, base, leaf);
}

int pile_pane_enter(pile_pane_t *pane) {
  if (pane->count == 0) return 0;
  pile_entry_t *e = &pane->entries[pane->cursor];
  if (e->d_type != DT_DIR) return 0;  /* file actions land in P3 */

  char *newpath = uc_malloc(PILE_PATH_MAX);
  if (!newpath) return 0;
  int rc = 0;
  if (path_descend(newpath, PILE_PATH_MAX, pane->path, e->name) == 0) {
    uc_strcpy(pane->path, newpath);
    pile_pane_load(pane);
    rc = 1;
  }
  uc_free(newpath);
  return rc;
}

int pile_pane_parent(pile_pane_t *pane) {
  char *newpath = uc_malloc(PILE_PATH_MAX);
  if (!newpath) return 0;
  int rc = 0;
  if (path_descend(newpath, PILE_PATH_MAX, pane->path, "..") == 0) {
    uc_strcpy(pane->path, newpath);
    pile_pane_load(pane);
    rc = 1;
  }
  uc_free(newpath);
  return rc;
}

/* ── Marking ───────────────────────────────────────────────────────────── */

/* Shell-style glob with '*' (0+ any) and '?' (1 any).  Anchored both
 * ends.  Returns 1 on match, 0 otherwise.
 *
 * Iterative two-cursor algorithm with mismatch backtrack: on '*',
 * remember the cursor positions; on a later mismatch, advance the
 * name cursor one char past that saved position and retry the tail.
 * Avoids the stack growth a recursive implementation would incur for
 * patterns with many '*' — Phase PS Rule 2. */
static int match_glob(const char *pat, const char *name) {
  const char *star_pat = 0;
  const char *star_name = 0;
  while (*name) {
    if (*pat == '*') {
      star_pat = ++pat;
      star_name = name;
      if (!*pat) return 1;  /* trailing '*' matches anything */
    } else if (*pat == '?' || *pat == *name) {
      pat++;
      name++;
    } else if (star_pat) {
      pat = star_pat;
      name = ++star_name;
    } else {
      return 0;
    }
  }
  /* Name consumed; pattern must be exhausted or only '*'s remain. */
  while (*pat == '*') pat++;
  return !*pat;
}

/* ".." is never markable — it's a navigation anchor, not a file. */
static int markable(const pile_entry_t *e) {
  return !(e->name[0] == '.' && e->name[1] == '.' && e->name[2] == '\0');
}

void pile_pane_mark_toggle(pile_pane_t *pane, int vrows) {
  if (pane->count == 0) return;
  pile_entry_t *e = &pane->entries[pane->cursor];
  if (markable(e)) e->flags ^= PILE_EFLAG_MARKED;
  pile_pane_move(pane, +1, vrows);
}

void pile_pane_mark_invert(pile_pane_t *pane) {
  for (int i = 0; i < pane->count; i++) {
    pile_entry_t *e = &pane->entries[i];
    if (markable(e)) e->flags ^= PILE_EFLAG_MARKED;
  }
}

int pile_pane_mark_glob(pile_pane_t *pane, const char *pattern, int mark) {
  int affected = 0;
  for (int i = 0; i < pane->count; i++) {
    pile_entry_t *e = &pane->entries[i];
    if (!markable(e)) continue;
    if (!match_glob(pattern, e->name)) continue;
    if (mark) {
      if (!(e->flags & PILE_EFLAG_MARKED)) {
        e->flags |= PILE_EFLAG_MARKED;
        affected++;
      }
    } else {
      if (e->flags & PILE_EFLAG_MARKED) {
        e->flags &= (uint8_t)~PILE_EFLAG_MARKED;
        affected++;
      }
    }
  }
  return affected;
}

int pile_pane_sel_count(const pile_pane_t *pane) {
  int n = 0;
  for (int i = 0; i < pane->count; i++) {
    if (pane->entries[i].flags & PILE_EFLAG_MARKED) n++;
  }
  return n;
}
