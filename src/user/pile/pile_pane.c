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
static int path_join(char *out, int cap, const char *dir, const char *name) {
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

/* Dirs first, then lexicographic by name.  ".." is pinned to the top so
 * navigation always sees it as entry 0 when present. */
static int entry_cmp(const pile_entry_t *a, const pile_entry_t *b) {
  int a_is_dotdot = (a->name[0] == '.' && a->name[1] == '.' && a->name[2] == '\0');
  int b_is_dotdot = (b->name[0] == '.' && b->name[1] == '.' && b->name[2] == '\0');
  if (a_is_dotdot != b_is_dotdot) return b_is_dotdot - a_is_dotdot;
  int a_is_dir = (a->d_type == DT_DIR);
  int b_is_dir = (b->d_type == DT_DIR);
  if (a_is_dir != b_is_dir) return b_is_dir - a_is_dir;
  return uc_strcmp(a->name, b->name);
}

static void sort_entries(pile_entry_t *a, int n) {
  /* Insertion sort: n ≤ 256, O(n²) is fine and keeps code small. */
  for (int i = 1; i < n; i++) {
    pile_entry_t tmp = a[i];
    int j = i;
    while (j > 0 && entry_cmp(&a[j - 1], &tmp) > 0) {
      a[j] = a[j - 1];
      j--;
    }
    a[j] = tmp;
  }
}

/* ── Load ──────────────────────────────────────────────────────────────── */

static void fill_stat(pile_entry_t *e, const char *dir) {
  char full[PILE_PATH_MAX + 64];
  if (path_join(full, (int)sizeof(full), dir, e->name) != 0) {
    e->flags |= PILE_EFLAG_STATFAIL;
    return;
  }
  struct stat st;
  if (stat(full, &st) != 0) {
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

  struct dirent de;
  while (pane->count < PILE_MAX_ENTRIES) {
    int n = getdents(fd, &de, sizeof(de));
    if (n <= 0) break;
    /* Skip "." — we keep ".." for navigation. */
    if (de.d_name[0] == '.' && de.d_name[1] == '\0') continue;
    pile_entry_t *e = &pane->entries[pane->count++];
    int nlen = uc_strlen(de.d_name);
    if (nlen > (int)sizeof(e->name) - 1) nlen = (int)sizeof(e->name) - 1;
    uc_memcpy(e->name, de.d_name, nlen);
    e->name[nlen] = '\0';
    e->size = 0;
    e->mtime = 0;
    e->mode = 0;
    e->d_type = de.d_type;
    e->flags = 0;
    fill_stat(e, pane->path);
  }
  /* Detect truncation: one more readable entry beyond our cap. */
  if (pane->count == PILE_MAX_ENTRIES) {
    if (getdents(fd, &de, sizeof(de)) > 0) pane->truncated = 1;
  }
  close(fd);

  sort_entries(pane->entries, pane->count);
  return 0;
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
  return path_join(out, cap, base, leaf);
}

int pile_pane_enter(pile_pane_t *pane) {
  if (pane->count == 0) return 0;
  pile_entry_t *e = &pane->entries[pane->cursor];
  if (e->d_type != DT_DIR) return 0;  /* file actions land in P3 */

  char newpath[PILE_PATH_MAX];
  if (path_descend(newpath, (int)sizeof(newpath), pane->path, e->name) != 0)
    return 0;
  uc_strcpy(pane->path, newpath);
  pile_pane_load(pane);
  return 1;
}

int pile_pane_parent(pile_pane_t *pane) {
  char newpath[PILE_PATH_MAX];
  if (path_descend(newpath, (int)sizeof(newpath), pane->path, "..") != 0)
    return 0;
  uc_strcpy(pane->path, newpath);
  pile_pane_load(pane);
  return 1;
}
